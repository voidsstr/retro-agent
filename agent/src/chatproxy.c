/*
 * chatproxy.c - User prompt queue, response log buffer, and proxy config
 *
 * Implements a simple message bus that turns the retro machine into a
 * Claude Code-style chat interface:
 *
 *   PROMPT_PUSH text  - user submits a prompt (called from Win98/XP UI)
 *   PROMPT_POP        - subagent pulls next pending prompt (returns empty if none)
 *   PROMPT_WAIT [ms]  - long-poll form of PROMPT_POP
 *   LOG_APPEND text   - subagent appends streamed response text to the log
 *   LOG_APPEND2 id text - the same, idempotent per chunk id ("OK dup" on a resend)
 *   LOG_READ offset   - UI client reads the log starting at ABSOLUTE byte offset
 *   LOG_WAIT offset [ms] - long-poll form of LOG_READ
 *   LOG_CLEAR         - reset the log buffer
 *   STATUS_SET/GET/WAIT - the subagent's one-line status
 *   PROXY_GET         - read configured Claude Code proxy host
 *   PROXY_SET host    - configure Claude Code proxy host (persisted in registry)
 *
 * A Claude Code background subagent on the developer machine discovers
 * retro agents via UDP broadcast (port 9899), then polls each one's
 * PROMPT_WAIT, processes the prompt with full Claude Code tools, and
 * streams the response back via LOG_APPEND. The proxy host setting lets
 * each agent declare which dev box owns it (so multiple dev boxes can
 * share a fleet without stepping on each other).
 *
 * Design:
 *   - Single prompt slot (only one in-flight prompt at a time, like Claude Code)
 *   - Growable log buffer up to LOG_MAX_SIZE bytes, ABSOLUTE offsets
 *   - Critical section protects all state
 *   - Buffers reset on agent restart, proxy host persists in registry
 *   - Long-polls BLOCK in threaded mode (NT) and PARK in multiplex mode
 *     (Win9x) - see chatproxy.h
 */

#include "handlers.h"
#include "protocol.h"
#include "util.h"
#include "log.h"
#include "chatproxy.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* The prompt-slot / log-ring / status-seq state machine is SHARED with the
 * DOS combined agent+chat: agent/shared/chatcore.[ch]. This file keeps the
 * NT-specific parts — locking, the change notifiers the blocking waiters
 * sleep on, parking, and the socket handlers. */
#include "../shared/chatcore.h"
#include "../shared/chatcore.c"

#define LOG_CHATPROXY "CHATPROXY"

#define PROMPT_MAX_SIZE   CHATCORE_PROMPT_MAX
#define LOG_MAX_SIZE      (256*1024) /* max log buffer (256KB) */
#define WAIT_MAX_TIMEOUT  CHAT_WAIT_MAX_TIMEOUT_MS
#define WAIT_DEFAULT_MS   30000
#define STATUS_MAX_SIZE   CHATCORE_STATUS_MAX

static CRITICAL_SECTION g_lock;
static int g_lock_initialized = 0;

/* All chat state (prompt slot, log buffer, status) lives here */
static chatcore_t g_core;

/* ---------------------------------------------------------------------------
 * CHANGE NOTIFIERS - how a blocked long-poll (threaded mode) is woken.
 *
 * These used to be three plain events, and the STATUS one spun the CPU:
 * g_status_event was MANUAL-reset and only the slow path reset it, so after
 * any STATUS_WAIT answered from its fast path the event stayed signalled, and
 * the next STATUS_WAIT with nothing new "woke" on it at once, saw no change,
 * and looped straight back into the same signalled wait until its 30 s
 * deadline - at HIGH_PRIORITY_CLASS. (LOG_WAIT and PROMPT_WAIT used
 * auto-reset events, so a stale signal made them return early and empty
 * instead, and a second waiter slept through a change the first consumed.)
 *
 * A notifier is a GENERATION: one manual-reset event that is signalled
 * exactly once, when the state next changes, and then discarded - never
 * reset. A waiter checks its condition and joins the current generation
 * (takes its own handle on that event) under ONE hold of g_lock, so:
 *   - it cannot miss a wake-up: any change after that check ends the very
 *     generation it holds, whether or not it has started waiting yet;
 *   - it cannot spin: a wake whose change did not satisfy it (someone else
 *     took the prompt) joins the NEXT generation, which is fresh and unset;
 *   - every waiter wakes (broadcast), and none can consume a signal another
 *     needed, because nobody ever resets anything.
 * The event is created lazily by the first waiter, so a box nobody is
 * long-polling pays nothing per LOG_APPEND.
 * ------------------------------------------------------------------------- */
typedef struct {
    HANDLE gen;   /* the current generation's event, or NULL if none joined */
} chat_notify_t;

static chat_notify_t g_log_notify;
static chat_notify_t g_prompt_notify;
static chat_notify_t g_status_notify;

/* g_lock held. End the current generation; returns its event (or NULL) for
 * notify_fire() to signal AFTER the lock is released. */
static HANDLE notify_advance_locked(chat_notify_t *n)
{
    HANDLE h = n->gen;
    n->gen = NULL;
    return h;
}

static void notify_fire(HANDLE h)
{
    if (h) {
        SetEvent(h);
        CloseHandle(h);   /* waiters hold their own duplicated handles */
    }
}

/* g_lock held. A private handle on the current generation's event (the
 * caller closes it), or NULL if one could not be made - the caller then
 * falls back to short sleeps, which is slower but still correct. */
static HANDLE notify_join_locked(chat_notify_t *n)
{
    HANDLE mine = NULL;
    if (!n->gen)
        n->gen = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (n->gen &&
        !DuplicateHandle(GetCurrentProcess(), n->gen, GetCurrentProcess(),
                         &mine, 0, FALSE, DUPLICATE_SAME_ACCESS))
        mine = NULL;
    return mine;
}

/* PROMPT_PUSH reports when nobody is polling for prompts (chatcore.h). */
static LONG  g_prompt_listeners = 0;  /* PROMPT_WAITs blocked or parked now */
static DWORD g_prompt_last_seen = 0;  /* tick of the last PROMPT_WAIT/POP */
static int   g_prompt_ever_seen = 0;
static DWORD g_proxy_started = 0;

/* multiplex mode: the connection whose command is being handled right now */
static chat_park_t *g_park_slot = NULL;

static void ensure_init(void)
{
    if (!g_lock_initialized) {
        InitializeCriticalSection(&g_lock);
        chatcore_init(&g_core, LOG_MAX_SIZE);
        g_proxy_started = GetTickCount();
        g_lock_initialized = 1;
    }
}

void chatproxy_init(void)
{
    ensure_init();
}

void chatproxy_set_park_slot(chat_park_t *slot)
{
    g_park_slot = slot;
}

/* g_lock held */
static void prompt_seen_locked(void)
{
    g_prompt_last_seen = GetTickCount();
    g_prompt_ever_seen = 1;
}

/* Has the peer closed (or reset) this connection? Never blocks: select with
 * a zero timeout, then a 1-byte MSG_PEEK only if something is readable. A
 * readable socket whose peek returns 0 (FIN) or an error is gone; one with a
 * real byte waiting is a live client that sent another command early. */
static int peer_gone(SOCKET s)
{
    fd_set r;
    struct timeval tv;
    char c;
    int n;

    FD_ZERO(&r);
    FD_SET(s, &r);
    tv.tv_sec = 0;
    tv.tv_usec = 0;
    n = select(0, &r, NULL, NULL, &tv);
    if (n < 0) return 1;
    if (n == 0) return 0;
    return recv(s, &c, 1, MSG_PEEK) <= 0;
}

/* Parse the long-poll timeout and apply the caps. Parked polls use the
 * requested length; only a BLOCKING wait in multiplex mode (never taken
 * while a park slot is set) still honours g_longpoll_max_ms. */
static DWORD wait_ms(DWORD requested, int blocking)
{
    DWORD t = requested;
    if (t == 0 || t > WAIT_MAX_TIMEOUT) t = WAIT_MAX_TIMEOUT;
    if (blocking && g_longpoll_max_ms > 0 && t > (DWORD)g_longpoll_max_ms)
        t = (DWORD)g_longpoll_max_ms;
    return t;
}

/* Threaded-mode blocking wait. ready() is evaluated under g_lock. Returns 1
 * once ready, 0 at the timeout. See CHANGE NOTIFIERS for why this can
 * neither miss a wake-up nor spin. */
typedef int (*chat_ready_fn)(const void *ctx);

static int chat_block_until(chat_notify_t *n, chat_ready_fn ready,
                            const void *ctx, DWORD timeout_ms)
{
    DWORD start = GetTickCount();
    for (;;) {
        HANDLE mine;
        DWORD elapsed;

        EnterCriticalSection(&g_lock);
        if (ready(ctx)) {
            LeaveCriticalSection(&g_lock);
            return 1;
        }
        mine = notify_join_locked(n);
        LeaveCriticalSection(&g_lock);

        elapsed = GetTickCount() - start;
        if (elapsed >= timeout_ms) {
            if (mine) CloseHandle(mine);
            return 0;
        }
        if (mine) {
            WaitForSingleObject(mine, timeout_ms - elapsed);
            CloseHandle(mine);
        } else {
            DWORD left = timeout_ms - elapsed;
            Sleep(left < 100 ? left : 100);
        }
    }
}

static int ready_log(const void *ctx)
{
    return chatcore_log_end(&g_core) != *(const DWORD *)ctx;
}

static int ready_status(const void *ctx)
{
    return g_core.status_seq != *(const DWORD *)ctx;
}

static int ready_prompt(const void *ctx)
{
    (void)ctx;
    return g_core.prompt_pending;
}

/* ---- response builders ---- */

/* "<absolute end>\n<bytes from the reader's offset to the end>". A reader
 * behind the ring's base resumes at the base; one past the end gets nothing
 * (and the smaller total tells it the log was cleared). */
static void send_log_response(SOCKET sock, DWORD offset)
{
    char *response;
    DWORD response_size, idx;
    unsigned long avail = 0;
    char header[64];
    int header_len, past_end = 0;

    EnterCriticalSection(&g_lock);
    idx = (DWORD)chatcore_log_window(&g_core, offset, &avail, &past_end);
    header_len = _snprintf(header, sizeof(header), "%lu\n",
                           (unsigned long)chatcore_log_end(&g_core));
    if (header_len < 0) header_len = 0;
    response_size = (DWORD)header_len + (DWORD)avail;
    response = (char *)malloc(response_size + 1);
    if (!response) {
        LeaveCriticalSection(&g_lock);
        send_error_response(sock, "Out of memory");
        return;
    }
    memcpy(response, header, header_len);
    if (avail)
        memcpy(response + header_len, g_core.log + idx, avail);
    response[response_size] = '\0';
    LeaveCriticalSection(&g_lock);

    send_text_response(sock, response);
    free(response);
}

static void send_status_response(SOCKET sock)
{
    char buf[STATUS_MAX_SIZE + 32];
    int header_len;

    EnterCriticalSection(&g_lock);
    header_len = _snprintf(buf, sizeof(buf), "%lu\n%s",
                           (unsigned long)g_core.status_seq, g_core.status);
    LeaveCriticalSection(&g_lock);
    if (header_len < 0) header_len = 0;
    if ((DWORD)header_len >= sizeof(buf)) header_len = sizeof(buf) - 1;
    buf[header_len] = '\0';
    send_text_response(sock, buf);
}

/* ---- handlers ---- */

/*
 * PROMPT_PUSH text
 *
 * Replies "OK", optionally followed by "replaced" (an earlier prompt was
 * still waiting and has been overwritten) and/or "no-listener <n>s" (nobody
 * has polled PROMPT_WAIT for n seconds). Every reply still starts with "OK",
 * which is all an older retro_chat checks.
 */
void handle_prompt_push(SOCKET sock, const char *args)
{
    DWORD len, now, idle;
    const char *p;
    int replaced, listening;
    HANDLE fire;
    char reply[64];

    ensure_init();

    /* The text arrives VERBATIM (handlers.c passes chat text after exactly
     * one separator space), so reject one that is only blanks here - the
     * daemon strips prompts and would treat it as no prompt at all. */
    for (p = args ? args : ""; *p == ' ' || *p == '\t' || *p == '\r' || *p == '\n'; p++)
        ;
    if (!*p) {
        send_error_response(sock, "PROMPT_PUSH requires text");
        return;
    }

    len = (DWORD)strlen(args);
    if (len >= PROMPT_MAX_SIZE) {
        send_error_response(sock, "Prompt too long");
        return;
    }

    EnterCriticalSection(&g_lock);
    replaced = g_core.prompt_pending;
    chatcore_prompt_push(&g_core, args);
    now = GetTickCount();
    idle = now - (g_prompt_ever_seen ? g_prompt_last_seen : g_proxy_started);
    listening = g_prompt_listeners > 0 ||
                (g_prompt_ever_seen && idle < CHATCORE_LISTENER_STALE_MS);
    fire = notify_advance_locked(&g_prompt_notify);
    LeaveCriticalSection(&g_lock);

    /* Wake every PROMPT_WAIT waiter */
    notify_fire(fire);

    chatcore_push_reply(reply, sizeof(reply), replaced, listening, idle);
    log_msg(LOG_CHATPROXY, "Prompt queued (%lu bytes): %s",
            (unsigned long)len, reply);
    send_text_response(sock, reply);
}

void handle_prompt_pop(SOCKET sock)
{
    char buf[PROMPT_MAX_SIZE];
    int has_prompt = 0;

    ensure_init();

    EnterCriticalSection(&g_lock);
    prompt_seen_locked();
    has_prompt = chatcore_prompt_take(&g_core, buf, sizeof(buf),
                                      (unsigned long)sock);
    LeaveCriticalSection(&g_lock);

    if (has_prompt) {
        log_msg(LOG_CHATPROXY, "Prompt delivered to subagent");
        send_text_response(sock, buf);
    } else {
        /* Empty response — subagent should poll again later */
        send_text_response(sock, "");
    }
}

/*
 * LOG_APPEND text
 *
 * Appends text verbatim (leading blanks included: the brain splits long
 * lines and indents code, and stripping them glued words together). Not
 * idempotent - a sender that resends after a timeout duplicates the chunk.
 * New senders use LOG_APPEND2.
 */
void handle_log_append(SOCKET sock, const char *args)
{
    DWORD len;
    unsigned long n;
    HANDLE fire = NULL;

    ensure_init();

    if (!args) args = "";
    len = (DWORD)strlen(args);

    EnterCriticalSection(&g_lock);
    n = chatcore_log_append(&g_core, args, len);
    if (n > 0) fire = notify_advance_locked(&g_log_notify);
    LeaveCriticalSection(&g_lock);

    /* Wake every LOG_WAIT waiter */
    notify_fire(fire);

    send_text_response(sock, "OK");
}

/*
 * LOG_APPEND2 <id> <text>
 *
 * The idempotent LOG_APPEND. <id> is a decimal uint32 the sender picks,
 * unique per chunk; a resend of a chunk the agent already appended (the
 * reply was lost, the sender timed out and tried again) answers "OK dup"
 * and appends nothing. The last CHATCORE_DEDUP_IDS ids are remembered.
 * <text> is everything after exactly one space following the id, verbatim.
 * An agent that predates this answers "Unknown command", which is how a
 * sender knows to fall back to LOG_APPEND.
 */
void handle_log_append2(SOCKET sock, const char *args)
{
    const char *p = args ? args : "";
    const char *text;
    unsigned long id = 0;
    unsigned long n;
    int digits = 0, dup = 0;
    HANDLE fire = NULL;

    ensure_init();

    while (*p >= '0' && *p <= '9') {
        id = id * 10UL + (unsigned long)(*p - '0');
        p++;
        digits++;
    }
    if (!digits || digits > 10 || (*p && *p != ' ')) {
        send_error_response(sock, "LOG_APPEND2 requires <id> <text>");
        return;
    }
    text = (*p == ' ') ? p + 1 : p;

    EnterCriticalSection(&g_lock);
    n = chatcore_log_append_once(&g_core, id, text,
                                 (unsigned long)strlen(text), &dup);
    if (n > 0) fire = notify_advance_locked(&g_log_notify);
    LeaveCriticalSection(&g_lock);

    notify_fire(fire);

    send_text_response(sock, dup ? "OK dup" : "OK");
}

void handle_log_read(SOCKET sock, const char *args)
{
    DWORD offset = 0;

    ensure_init();

    if (args && args[0])
        offset = (DWORD)strtoul(args, NULL, 10);
    send_log_response(sock, offset);
}

void handle_log_clear(SOCKET sock)
{
    HANDLE fire_log, fire_status;

    ensure_init();

    EnterCriticalSection(&g_lock);
    chatcore_log_clear(&g_core);
    fire_log = notify_advance_locked(&g_log_notify);
    fire_status = notify_advance_locked(&g_status_notify);
    LeaveCriticalSection(&g_lock);

    notify_fire(fire_log);
    notify_fire(fire_status);

    log_msg(LOG_CHATPROXY, "Log cleared");
    send_text_response(sock, "OK");
}

/* ---- long-polling waiters ---- */

/*
 * LOG_WAIT <last_offset> [timeout_ms]
 *
 * Answers as soon as the log's absolute end differs from <last_offset> (new
 * content, or the reader is past the end of a cleared log), or at the
 * timeout (default 30000ms, capped at WAIT_MAX_TIMEOUT). Same reply as
 * LOG_READ: "<total_size>\n<bytes_starting_at_offset>".
 */
void handle_log_wait(SOCKET sock, const char *args)
{
    DWORD offset = 0;
    DWORD timeout_ms = WAIT_DEFAULT_MS;

    ensure_init();

    if (args && args[0]) {
        char *endp = NULL;
        offset = (DWORD)strtoul(args, &endp, 10);
        if (endp && *endp)
            timeout_ms = (DWORD)strtoul(endp, NULL, 10);
    }

    EnterCriticalSection(&g_lock);
    if (!ready_log(&offset) && g_park_slot) {
        g_park_slot->kind = CHAT_PARK_LOG;
        g_park_slot->arg = offset;
        g_park_slot->deadline = GetTickCount() + wait_ms(timeout_ms, 0);
        LeaveCriticalSection(&g_lock);
        return;                                   /* answered later */
    }
    LeaveCriticalSection(&g_lock);

    chat_block_until(&g_log_notify, ready_log, &offset, wait_ms(timeout_ms, 1));
    send_log_response(sock, offset);
}

/*
 * PROMPT_WAIT [timeout_ms]
 *
 * Answers with the pending prompt as soon as there is one, or "" at the
 * timeout (default 30000ms, capped at WAIT_MAX_TIMEOUT).
 *
 * The prompt is TAKEN, not popped: it stays in flight for this connection
 * until the connection's next command, and goes back in the slot if the
 * connection dies first (chatproxy_conn_closed). A prompt used to vanish
 * when the taking connection was already dead - the reply "sent" into a
 * half-open socket, and nobody ever saw it.
 */
void handle_prompt_wait(SOCKET sock, const char *args)
{
    DWORD requested = WAIT_DEFAULT_MS;
    DWORD timeout_ms, start;
    char buf[PROMPT_MAX_SIZE];

    ensure_init();

    if (args && args[0])
        requested = (DWORD)strtoul(args, NULL, 10);

    EnterCriticalSection(&g_lock);
    prompt_seen_locked();
    /* Fast path: a prompt is already pending. This connection just sent us
     * a command, so it is alive. */
    if (chatcore_prompt_take(&g_core, buf, sizeof(buf), (unsigned long)sock)) {
        LeaveCriticalSection(&g_lock);
        log_msg(LOG_CHATPROXY, "Prompt delivered (immediate)");
        send_text_response(sock, buf);
        return;
    }
    if (g_park_slot) {
        g_park_slot->kind = CHAT_PARK_PROMPT;
        g_park_slot->arg = 0;
        g_park_slot->deadline = GetTickCount() + wait_ms(requested, 0);
        g_prompt_listeners++;
        LeaveCriticalSection(&g_lock);
        return;                                   /* answered later */
    }
    g_prompt_listeners++;
    LeaveCriticalSection(&g_lock);

    /* Threaded mode: block until a prompt arrives or the timeout */
    timeout_ms = wait_ms(requested, 1);
    start = GetTickCount();
    for (;;) {
        DWORD elapsed = GetTickCount() - start;
        int took = 0;

        if (elapsed >= timeout_ms ||
            !chat_block_until(&g_prompt_notify, ready_prompt, NULL,
                              timeout_ms - elapsed))
            break;                                /* timed out */

        /* A prompt is pending. Do not take it into a dead connection:
         * leave it for the next poller, and wake the others to look. */
        if (peer_gone(sock)) {
            HANDLE fire;
            EnterCriticalSection(&g_lock);
            g_prompt_listeners--;
            fire = notify_advance_locked(&g_prompt_notify);
            LeaveCriticalSection(&g_lock);
            notify_fire(fire);
            log_msg(LOG_CHATPROXY, "PROMPT_WAIT: peer gone - prompt left "
                    "for the next poller");
            return;                               /* no reply: it is gone */
        }

        EnterCriticalSection(&g_lock);
        took = chatcore_prompt_take(&g_core, buf, sizeof(buf),
                                    (unsigned long)sock);
        if (took) {
            g_prompt_listeners--;
            prompt_seen_locked();
        }
        LeaveCriticalSection(&g_lock);
        if (took) {
            log_msg(LOG_CHATPROXY, "Prompt delivered (after wait)");
            send_text_response(sock, buf);
            return;
        }
        /* another poller took it first: keep waiting */
    }

    EnterCriticalSection(&g_lock);
    g_prompt_listeners--;
    prompt_seen_locked();
    LeaveCriticalSection(&g_lock);
    /* Timeout — return empty so subagent can re-issue PROMPT_WAIT */
    send_text_response(sock, "");
}

/* ---- subagent status channel ---- */

/*
 * STATUS_SET text
 *
 * Stores a short string describing what the dev-box subagent is
 * currently doing. Examples:
 *   STATUS_SET reading config.cfg
 *   STATUS_SET EXEC dir C:\WINDOWS
 *   STATUS_SET idle
 *
 * Each call increments g_core.status_seq and wakes every STATUS_WAIT
 * waiter. Empty/missing arg clears the status (reported as empty string by
 * STATUS_GET).
 */
void handle_status_set(SOCKET sock, const char *args)
{
    HANDLE fire;

    ensure_init();

    if (!args) args = "";

    EnterCriticalSection(&g_lock);
    chatcore_status_set(&g_core, args);   /* truncates at STATUS_MAX_SIZE */
    fire = notify_advance_locked(&g_status_notify);
    LeaveCriticalSection(&g_lock);

    notify_fire(fire);

    send_text_response(sock, "OK");
}

/*
 * STATUS_GET
 *
 * Returns "<seq>\n<status_text>" so the caller can detect changes by
 * tracking the sequence number. The status text is empty when nothing
 * has been set yet.
 */
void handle_status_get(SOCKET sock)
{
    ensure_init();
    send_status_response(sock);
}

/*
 * STATUS_WAIT <last_seq> [timeout_ms]
 *
 * Long-polls until either:
 *   - g_core.status_seq advances past last_seq, OR
 *   - the timeout expires (default 30000ms, capped at WAIT_MAX_TIMEOUT)
 *
 * Returns the same format as STATUS_GET: "<seq>\n<status_text>".
 */
void handle_status_wait(SOCKET sock, const char *args)
{
    DWORD last_seq = 0;
    DWORD timeout_ms = WAIT_DEFAULT_MS;

    ensure_init();

    if (args && args[0]) {
        char *endp = NULL;
        last_seq = (DWORD)strtoul(args, &endp, 10);
        if (endp && *endp)
            timeout_ms = (DWORD)strtoul(endp, NULL, 10);
    }

    EnterCriticalSection(&g_lock);
    if (!ready_status(&last_seq) && g_park_slot) {
        g_park_slot->kind = CHAT_PARK_STATUS;
        g_park_slot->arg = last_seq;
        g_park_slot->deadline = GetTickCount() + wait_ms(timeout_ms, 0);
        LeaveCriticalSection(&g_lock);
        return;                                   /* answered later */
    }
    LeaveCriticalSection(&g_lock);

    chat_block_until(&g_status_notify, ready_status, &last_seq,
                     wait_ms(timeout_ms, 1));
    send_status_response(sock);
}

/* ---- connection hooks + parking (chatproxy.h) ---- */

void chatproxy_conn_command(SOCKET sock)
{
    ensure_init();
    EnterCriticalSection(&g_lock);
    chatcore_prompt_ack(&g_core, (unsigned long)sock);
    LeaveCriticalSection(&g_lock);
}

void chatproxy_conn_closed(SOCKET sock)
{
    HANDLE fire = NULL;
    int requeued;

    ensure_init();
    EnterCriticalSection(&g_lock);
    requeued = chatcore_prompt_requeue(&g_core, (unsigned long)sock);
    if (requeued) fire = notify_advance_locked(&g_prompt_notify);
    LeaveCriticalSection(&g_lock);
    notify_fire(fire);
    if (requeued)
        log_msg(LOG_CHATPROXY, "a prompt was taken by a connection that "
                "closed before acknowledging it - put back for the next poller");
}

void chatproxy_park_clear(chat_park_t *p)
{
    if (!p || p->kind == CHAT_PARK_NONE) return;
    if (p->kind == CHAT_PARK_PROMPT) {
        ensure_init();
        EnterCriticalSection(&g_lock);
        g_prompt_listeners--;
        prompt_seen_locked();
        LeaveCriticalSection(&g_lock);
    }
    p->kind = CHAT_PARK_NONE;
}

DWORD chatproxy_park_ms_left(const chat_park_t *p, DWORD now)
{
    if (!p || p->kind == CHAT_PARK_NONE) return INFINITE;
    if ((LONG)(p->deadline - now) <= 0) return 0;
    return p->deadline - now;
}

int chatproxy_park_service(SOCKET sock, chat_park_t *p, DWORD now, int force)
{
    int expired, ready;
    DWORD arg;

    if (!p || p->kind == CHAT_PARK_NONE) return 0;
    ensure_init();
    expired = force || (LONG)(now - p->deadline) >= 0;
    arg = p->arg;

    switch (p->kind) {
    case CHAT_PARK_LOG:
        EnterCriticalSection(&g_lock);
        ready = ready_log(&arg);
        LeaveCriticalSection(&g_lock);
        if (!ready && !expired) return 0;
        chatproxy_park_clear(p);
        send_log_response(sock, arg);
        return 1;

    case CHAT_PARK_STATUS:
        EnterCriticalSection(&g_lock);
        ready = ready_status(&arg);
        LeaveCriticalSection(&g_lock);
        if (!ready && !expired) return 0;
        chatproxy_park_clear(p);
        send_status_response(sock);
        return 1;

    case CHAT_PARK_PROMPT: {
        char buf[PROMPT_MAX_SIZE];
        int took = 0;

        EnterCriticalSection(&g_lock);
        ready = g_core.prompt_pending;
        LeaveCriticalSection(&g_lock);
        /* force = the client sent another command while this poll was
         * parked, i.e. it has moved on: answer "" rather than hand a prompt
         * to a connection that may never read it. */
        if (ready && !force) {
            /* Never take a prompt into a connection whose peer has gone:
             * leave it pending, and the next parked poll gets it. */
            if (peer_gone(sock)) {
                chatproxy_park_clear(p);
                log_msg(LOG_CHATPROXY, "parked PROMPT_WAIT: peer gone - "
                        "prompt left for the next poller");
                return -1;
            }
            EnterCriticalSection(&g_lock);
            took = chatcore_prompt_take(&g_core, buf, sizeof(buf),
                                        (unsigned long)sock);
            LeaveCriticalSection(&g_lock);
        }
        if (!took && !expired) return 0;
        chatproxy_park_clear(p);
        if (took) {
            log_msg(LOG_CHATPROXY, "Prompt delivered (parked)");
            send_text_response(sock, buf);
        } else {
            send_text_response(sock, "");
        }
        return 1;
    }

    default:
        p->kind = CHAT_PARK_NONE;
        return 0;
    }
}

/* ---- proxy host configuration (persisted in registry) ---- */

#define PROXY_REG_KEY  "Software\\RetroAgent"
#define PROXY_REG_NAME "ProxyHost"

void handle_proxy_get(SOCKET sock)
{
    HKEY hKey;
    char value[256] = {0};
    DWORD size = sizeof(value);
    DWORD type;

    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, PROXY_REG_KEY, 0,
                      KEY_READ, &hKey) == ERROR_SUCCESS) {
        if (RegQueryValueExA(hKey, PROXY_REG_NAME, NULL, &type,
                             (BYTE *)value, &size) != ERROR_SUCCESS
            || type != REG_SZ) {
            value[0] = '\0';
        }
        RegCloseKey(hKey);
    }

    send_text_response(sock, value);
}

void handle_proxy_set(SOCKET sock, const char *args)
{
    HKEY hKey;
    DWORD disposition;
    LONG rc;

    if (!args) args = "";

    rc = RegCreateKeyExA(HKEY_LOCAL_MACHINE, PROXY_REG_KEY, 0, NULL, 0,
                         KEY_WRITE, NULL, &hKey, &disposition);
    if (rc != ERROR_SUCCESS) {
        send_error_response(sock, "Cannot open registry key");
        return;
    }

    rc = RegSetValueExA(hKey, PROXY_REG_NAME, 0, REG_SZ,
                        (const BYTE *)args, (DWORD)strlen(args) + 1);
    RegCloseKey(hKey);

    if (rc != ERROR_SUCCESS) {
        send_error_response(sock, "Failed to write proxy host");
        return;
    }

    log_msg(LOG_CHATPROXY, "Proxy host set to: %s", args[0] ? args : "(cleared)");
    send_text_response(sock, "OK");
}
