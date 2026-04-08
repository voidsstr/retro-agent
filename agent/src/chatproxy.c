/*
 * chatproxy.c - User prompt queue, response log buffer, and proxy config
 *
 * Implements a simple message bus that turns the retro machine into a
 * Claude Code-style chat interface:
 *
 *   PROMPT_PUSH text  - user submits a prompt (called from Win98/XP UI)
 *   PROMPT_POP        - subagent pulls next pending prompt (returns empty if none)
 *   LOG_APPEND text   - subagent appends streamed response text to the log
 *   LOG_READ offset   - UI client reads the log starting at byte offset
 *   LOG_CLEAR         - reset the log buffer
 *   PROXY_GET         - read configured Claude Code proxy host
 *   PROXY_SET host    - configure Claude Code proxy host (persisted in registry)
 *
 * A Claude Code background subagent on the developer machine discovers
 * retro agents via UDP broadcast (port 9899), then polls each one's
 * PROMPT_POP, processes the prompt with full Claude Code tools, and
 * streams the response back via LOG_APPEND. The proxy host setting lets
 * each agent declare which dev box owns it (so multiple dev boxes can
 * share a fleet without stepping on each other).
 *
 * Design:
 *   - Single prompt slot (only one in-flight prompt at a time, like Claude Code)
 *   - Growable log buffer up to LOG_MAX_SIZE bytes
 *   - Critical section protects all state
 *   - Buffers reset on agent restart, proxy host persists in registry
 */

#include "handlers.h"
#include "protocol.h"
#include "util.h"
#include "log.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define LOG_CHATPROXY "CHATPROXY"

#define PROMPT_MAX_SIZE   8192       /* max prompt length */
#define LOG_MAX_SIZE      (256*1024) /* max log buffer (256KB) */
#define WAIT_MAX_TIMEOUT  60000      /* cap LOG_WAIT/PROMPT_WAIT at 60s */
#define STATUS_MAX_SIZE   512        /* max status string */

static CRITICAL_SECTION g_lock;
static int g_lock_initialized = 0;

/* Pending prompt slot — empty when not set */
static char g_prompt[PROMPT_MAX_SIZE] = {0};
static int  g_prompt_pending = 0;

/* Log buffer — grows as the subagent streams response chunks */
static char *g_log = NULL;
static DWORD g_log_size = 0;
static DWORD g_log_capacity = 0;

/* Manual-reset events used by long-polling waiters.
 *
 * - g_log_event: signaled when LOG_APPEND grows the buffer; LOG_WAIT
 *   waits on this. Reset before re-waiting.
 * - g_prompt_event: signaled when PROMPT_PUSH queues a new prompt;
 *   PROMPT_WAIT waits on this.
 *
 * Each is auto-reset so a single waiter wakes per signal. With the
 * thread-per-client model on NT, this gives sub-100ms latency for new
 * data with zero polling overhead.
 */
static HANDLE g_log_event = NULL;
static HANDLE g_prompt_event = NULL;

/* Subagent status: a single short string describing what the dev-box
 * subagent is currently doing (e.g. "EXEC dir C:\WINDOWS", "reading
 * config.cfg", "idle"). The retro_chat client polls STATUS_WAIT and
 * displays this above its input area so the user knows what's happening
 * even when no log output has streamed yet.
 *
 * g_status_seq increments on every STATUS_SET so STATUS_WAIT can detect
 * any change (including "set to same value as before") via cheap
 * sequence comparison instead of string comparison.
 */
static char  g_status[STATUS_MAX_SIZE] = {0};
static DWORD g_status_seq = 0;
static HANDLE g_status_event = NULL;

static void ensure_init(void)
{
    if (!g_lock_initialized) {
        InitializeCriticalSection(&g_lock);
        g_log_event = CreateEvent(NULL, FALSE, FALSE, NULL);   /* auto-reset */
        g_prompt_event = CreateEvent(NULL, FALSE, FALSE, NULL); /* auto-reset */
        g_status_event = CreateEvent(NULL, TRUE, FALSE, NULL); /* manual-reset, broadcast */
        g_lock_initialized = 1;
    }
}

static void log_append_locked(const char *text, DWORD len)
{
    DWORD new_size;

    if (len == 0) return;

    new_size = g_log_size + len;
    if (new_size > LOG_MAX_SIZE) {
        /* Drop oldest half when full */
        DWORD keep = LOG_MAX_SIZE / 2;
        if (g_log_size > keep) {
            memmove(g_log, g_log + (g_log_size - keep), keep);
            g_log_size = keep;
        }
        new_size = g_log_size + len;
        if (new_size > LOG_MAX_SIZE) {
            /* Truncate the new chunk if it's still too big */
            len = LOG_MAX_SIZE - g_log_size;
            new_size = LOG_MAX_SIZE;
        }
    }

    /* Grow buffer if needed */
    if (new_size > g_log_capacity) {
        DWORD new_cap = g_log_capacity ? g_log_capacity * 2 : 4096;
        char *new_buf;
        while (new_cap < new_size) new_cap *= 2;
        if (new_cap > LOG_MAX_SIZE) new_cap = LOG_MAX_SIZE;
        new_buf = (char *)realloc(g_log, new_cap);
        if (!new_buf) return;
        g_log = new_buf;
        g_log_capacity = new_cap;
    }

    memcpy(g_log + g_log_size, text, len);
    g_log_size = new_size;
}

/* ---- handlers ---- */

void handle_prompt_push(SOCKET sock, const char *args)
{
    DWORD len;

    ensure_init();

    if (!args || !args[0]) {
        send_error_response(sock, "PROMPT_PUSH requires text");
        return;
    }

    len = (DWORD)strlen(args);
    if (len >= PROMPT_MAX_SIZE) {
        send_error_response(sock, "Prompt too long");
        return;
    }

    EnterCriticalSection(&g_lock);
    memcpy(g_prompt, args, len);
    g_prompt[len] = '\0';
    g_prompt_pending = 1;
    LeaveCriticalSection(&g_lock);

    /* Wake any PROMPT_WAIT waiter */
    if (g_prompt_event) SetEvent(g_prompt_event);

    log_msg(LOG_CHATPROXY, "Prompt queued (%lu bytes)", (unsigned long)len);
    send_text_response(sock, "OK");
}

void handle_prompt_pop(SOCKET sock)
{
    char buf[PROMPT_MAX_SIZE];
    int has_prompt = 0;

    ensure_init();

    EnterCriticalSection(&g_lock);
    if (g_prompt_pending) {
        strncpy(buf, g_prompt, sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';
        g_prompt_pending = 0;
        g_prompt[0] = '\0';
        has_prompt = 1;
    }
    LeaveCriticalSection(&g_lock);

    if (has_prompt) {
        log_msg(LOG_CHATPROXY, "Prompt delivered to subagent");
        send_text_response(sock, buf);
    } else {
        /* Empty response — subagent should poll again later */
        send_text_response(sock, "");
    }
}

void handle_log_append(SOCKET sock, const char *args)
{
    DWORD len;

    ensure_init();

    if (!args) args = "";
    len = (DWORD)strlen(args);

    EnterCriticalSection(&g_lock);
    log_append_locked(args, len);
    LeaveCriticalSection(&g_lock);

    /* Wake any LOG_WAIT waiter */
    if (g_log_event && len > 0) SetEvent(g_log_event);

    send_text_response(sock, "OK");
}

void handle_log_read(SOCKET sock, const char *args)
{
    DWORD offset = 0;
    char *response;
    DWORD response_size;
    char header[64];
    int header_len;

    ensure_init();

    if (args && args[0]) {
        offset = (DWORD)strtoul(args, NULL, 10);
    }

    EnterCriticalSection(&g_lock);
    if (offset > g_log_size) offset = g_log_size;

    /* Format: "<total_size>\n<bytes_starting_at_offset>" */
    header_len = _snprintf(header, sizeof(header), "%lu\n",
                           (unsigned long)g_log_size);
    response_size = header_len + (g_log_size - offset);
    response = (char *)malloc(response_size + 1);
    if (!response) {
        LeaveCriticalSection(&g_lock);
        send_error_response(sock, "Out of memory");
        return;
    }

    memcpy(response, header, header_len);
    if (g_log_size > offset) {
        memcpy(response + header_len, g_log + offset, g_log_size - offset);
    }
    response[response_size] = '\0';
    LeaveCriticalSection(&g_lock);

    send_text_response(sock, response);
    free(response);
}

void handle_log_clear(SOCKET sock)
{
    ensure_init();

    EnterCriticalSection(&g_lock);
    g_log_size = 0;
    g_prompt_pending = 0;
    g_prompt[0] = '\0';
    g_status[0] = '\0';
    g_status_seq++;
    LeaveCriticalSection(&g_lock);

    if (g_status_event) SetEvent(g_status_event);

    log_msg(LOG_CHATPROXY, "Log cleared");
    send_text_response(sock, "OK");
}

/* ---- long-polling waiters ---- */

/*
 * LOG_WAIT <last_offset> [timeout_ms]
 *
 * Blocks until either:
 *   - The log buffer grows past <last_offset>, OR
 *   - The timeout expires (default 30000ms, capped at WAIT_MAX_TIMEOUT)
 *
 * Returns the same format as LOG_READ: "<total_size>\n<bytes_starting_at_offset>"
 *
 * If new content was already available before the wait started, returns
 * immediately. This makes the chat client's wait loop trivial: it can
 * always call LOG_WAIT and be sure to get content as soon as any exists.
 */
void handle_log_wait(SOCKET sock, const char *args)
{
    DWORD offset = 0;
    DWORD timeout_ms = 30000;
    DWORD wait_result;
    char *response;
    DWORD response_size;
    char header[64];
    int header_len;

    ensure_init();

    if (args && args[0]) {
        char *endp = NULL;
        offset = (DWORD)strtoul(args, &endp, 10);
        if (endp && *endp) {
            timeout_ms = (DWORD)strtoul(endp, NULL, 10);
        }
    }
    if (timeout_ms == 0 || timeout_ms > WAIT_MAX_TIMEOUT)
        timeout_ms = WAIT_MAX_TIMEOUT;

    /* Fast path: if there's already content past the offset, return now */
    EnterCriticalSection(&g_lock);
    if (g_log_size > offset || (offset > 0 && g_log_size < offset)) {
        /* Has new content, OR client is past the end of a cleared buffer */
        LeaveCriticalSection(&g_lock);
    } else {
        LeaveCriticalSection(&g_lock);
        /* Block until LOG_APPEND signals or timeout */
        if (g_log_event) {
            wait_result = WaitForSingleObject(g_log_event, timeout_ms);
            (void)wait_result;  /* either signaled or timeout — both OK */
        } else {
            Sleep(timeout_ms);
        }
    }

    /* Now read the (possibly new) state and respond like LOG_READ */
    EnterCriticalSection(&g_lock);
    if (offset > g_log_size) offset = g_log_size;

    header_len = _snprintf(header, sizeof(header), "%lu\n",
                           (unsigned long)g_log_size);
    response_size = header_len + (g_log_size - offset);
    response = (char *)malloc(response_size + 1);
    if (!response) {
        LeaveCriticalSection(&g_lock);
        send_error_response(sock, "Out of memory");
        return;
    }
    memcpy(response, header, header_len);
    if (g_log_size > offset) {
        memcpy(response + header_len, g_log + offset, g_log_size - offset);
    }
    response[response_size] = '\0';
    LeaveCriticalSection(&g_lock);

    send_text_response(sock, response);
    free(response);
}

/*
 * PROMPT_WAIT [timeout_ms]
 *
 * Blocks until either:
 *   - A new prompt is queued via PROMPT_PUSH, OR
 *   - The timeout expires (default 30000ms, capped at WAIT_MAX_TIMEOUT)
 *
 * On wake, behaves identically to PROMPT_POP: returns the prompt text
 * and clears the slot, or returns empty string on timeout.
 *
 * Used by the dev-box subagent to receive prompts immediately instead
 * of polling every few seconds.
 */
void handle_prompt_wait(SOCKET sock, const char *args)
{
    DWORD timeout_ms = 30000;
    char buf[PROMPT_MAX_SIZE];
    int has_prompt = 0;

    ensure_init();

    if (args && args[0]) {
        timeout_ms = (DWORD)strtoul(args, NULL, 10);
    }
    if (timeout_ms == 0 || timeout_ms > WAIT_MAX_TIMEOUT)
        timeout_ms = WAIT_MAX_TIMEOUT;

    /* Fast path: if a prompt is already pending, return it now */
    EnterCriticalSection(&g_lock);
    if (g_prompt_pending) {
        strncpy(buf, g_prompt, sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';
        g_prompt_pending = 0;
        g_prompt[0] = '\0';
        has_prompt = 1;
    }
    LeaveCriticalSection(&g_lock);

    if (has_prompt) {
        log_msg(LOG_CHATPROXY, "Prompt delivered (immediate)");
        send_text_response(sock, buf);
        return;
    }

    /* Block until PROMPT_PUSH signals or timeout */
    if (g_prompt_event) {
        WaitForSingleObject(g_prompt_event, timeout_ms);
    } else {
        Sleep(timeout_ms);
    }

    /* Recheck after wake */
    EnterCriticalSection(&g_lock);
    if (g_prompt_pending) {
        strncpy(buf, g_prompt, sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';
        g_prompt_pending = 0;
        g_prompt[0] = '\0';
        has_prompt = 1;
    }
    LeaveCriticalSection(&g_lock);

    if (has_prompt) {
        log_msg(LOG_CHATPROXY, "Prompt delivered (after wait)");
        send_text_response(sock, buf);
    } else {
        /* Timeout — return empty so subagent can re-issue PROMPT_WAIT */
        send_text_response(sock, "");
    }
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
 * Each call increments g_status_seq and signals g_status_event so any
 * STATUS_WAIT waiters wake immediately. Empty/missing arg clears the
 * status (reported as empty string by STATUS_GET).
 */
void handle_status_set(SOCKET sock, const char *args)
{
    DWORD len;

    ensure_init();

    if (!args) args = "";
    len = (DWORD)strlen(args);
    if (len >= STATUS_MAX_SIZE) len = STATUS_MAX_SIZE - 1;

    EnterCriticalSection(&g_lock);
    memcpy(g_status, args, len);
    g_status[len] = '\0';
    g_status_seq++;
    LeaveCriticalSection(&g_lock);

    /* Wake all STATUS_WAIT waiters (manual-reset event, broadcast) */
    if (g_status_event) SetEvent(g_status_event);

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
    char buf[STATUS_MAX_SIZE + 32];
    int header_len;

    ensure_init();

    EnterCriticalSection(&g_lock);
    header_len = _snprintf(buf, sizeof(buf), "%lu\n%s",
                           (unsigned long)g_status_seq, g_status);
    LeaveCriticalSection(&g_lock);

    if (header_len < 0) header_len = 0;
    if ((DWORD)header_len >= sizeof(buf)) header_len = sizeof(buf) - 1;
    buf[header_len] = '\0';
    send_text_response(sock, buf);
}

/*
 * STATUS_WAIT <last_seq> [timeout_ms]
 *
 * Long-polls until either:
 *   - g_status_seq advances past last_seq, OR
 *   - the timeout expires (default 30000ms, capped at WAIT_MAX_TIMEOUT)
 *
 * Returns the same format as STATUS_GET: "<seq>\n<status_text>".
 *
 * The retro_chat client uses this to refresh its status line as soon as
 * the subagent reports a new activity, with sub-100ms latency and zero
 * polling overhead.
 *
 * Uses a manual-reset event so multiple waiters can be supported, but
 * the typical case is a single retro_chat client per agent.
 */
void handle_status_wait(SOCKET sock, const char *args)
{
    DWORD last_seq = 0;
    DWORD timeout_ms = 30000;
    DWORD deadline;
    char buf[STATUS_MAX_SIZE + 32];
    int header_len;

    ensure_init();

    if (args && args[0]) {
        char *endp = NULL;
        last_seq = (DWORD)strtoul(args, &endp, 10);
        if (endp && *endp) {
            timeout_ms = (DWORD)strtoul(endp, NULL, 10);
        }
    }
    if (timeout_ms == 0 || timeout_ms > WAIT_MAX_TIMEOUT)
        timeout_ms = WAIT_MAX_TIMEOUT;

    /* Fast path: status already advanced */
    EnterCriticalSection(&g_lock);
    if (g_status_seq != last_seq) {
        header_len = _snprintf(buf, sizeof(buf), "%lu\n%s",
                               (unsigned long)g_status_seq, g_status);
        LeaveCriticalSection(&g_lock);
        if (header_len < 0) header_len = 0;
        if ((DWORD)header_len >= sizeof(buf)) header_len = sizeof(buf) - 1;
        buf[header_len] = '\0';
        send_text_response(sock, buf);
        return;
    }
    LeaveCriticalSection(&g_lock);

    /* Slow path: block on the status event with manual reset.
     * The event is set whenever STATUS_SET runs. We loop because
     * a manual-reset event can wake multiple waiters and any one
     * of them may consume the change before we check. */
    deadline = GetTickCount() + timeout_ms;
    if (g_status_event) {
        for (;;) {
            DWORD now = GetTickCount();
            DWORD remaining = (now < deadline) ? (deadline - now) : 0;
            DWORD cur_seq;
            DWORD wait_result;

            wait_result = WaitForSingleObject(g_status_event,
                                              remaining ? remaining : 1);

            EnterCriticalSection(&g_lock);
            cur_seq = g_status_seq;
            LeaveCriticalSection(&g_lock);

            if (cur_seq != last_seq) break;
            if (wait_result == WAIT_TIMEOUT) break;
            if (GetTickCount() >= deadline) break;
        }
        /* Reset the manual-reset event so we don't spin if no other
         * waiter consumed the signal. Safe to reset before reading
         * status because seq/string are protected by g_lock. */
        ResetEvent(g_status_event);
    } else {
        Sleep(timeout_ms);
    }

    EnterCriticalSection(&g_lock);
    header_len = _snprintf(buf, sizeof(buf), "%lu\n%s",
                           (unsigned long)g_status_seq, g_status);
    LeaveCriticalSection(&g_lock);
    if (header_len < 0) header_len = 0;
    if ((DWORD)header_len >= sizeof(buf)) header_len = sizeof(buf) - 1;
    buf[header_len] = '\0';
    send_text_response(sock, buf);
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
