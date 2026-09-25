/* test_chatproxy.c — TRUE-SOURCE test: compiles the REAL agent/src/chatproxy.c
 * (and through it agent/shared/chatcore.c) against a fake, single-threaded
 * Win32/Winsock (stubs/netfake_env.h) and drives the chat bus the way the
 * agent's two client loops do. Covers the chat-path review fixes (agent
 * 1.85.0):
 *
 *   1  STATUS_WAIT spun the CPU for up to 30 s: a manual-reset event left set
 *      by a fast-path answer made every later slow-path wait return at once.
 *      The fake counts WaitForSingleObject calls and aborts a spinner.
 *   2  Multiplex (Win9x) long-polls PARK instead of blocking the one thread:
 *      no reply until the condition or deadline, full requested length
 *      despite the old 1 s clamp, and never a blocking wait.
 *   4  A prompt taken by a connection that is dead, or dies before its next
 *      command, is not lost.
 *   5  LOG offsets are absolute: the total a reader sees never shrinks when
 *      the ring drops its oldest half, so a reader is never sent back to 0.
 *   6  LOG_APPEND2 is idempotent per chunk id.
 *  10  PROMPT_PUSH says when it replaced a prompt or nobody is listening.
 */
#include "munit.h"
#include "stubs/netfake_env.h"

/* ---- what chatproxy.c needs from the rest of the agent ---- */
int g_longpoll_max_ms = 0;

void log_msg(const char *tag, const char *fmt, ...) { (void)tag; (void)fmt; }

#define NSOCK FAKE_MAX_SOCKS
static char resp[NSOCK][300000];      /* last reply text per socket */
static int  resp_status[NSOCK];
static int  resp_count[NSOCK];

int send_text_response(SOCKET s, const char *text)
{
    snprintf(resp[s], sizeof(resp[s]), "%s", text);
    resp_status[s] = 0x00;
    resp_count[s]++;
    return 0;
}
int send_error_response(SOCKET s, const char *text)
{
    snprintf(resp[s], sizeof(resp[s]), "%s", text);
    resp_status[s] = 0xFF;
    resp_count[s]++;
    return 0;
}

/* the real module under test */
#include "../../agent/src/chatproxy.c"

/* sockets: UI = the local retro_chat, D = the daemon's wait connection,
 * D2 = a second poller, SEND = the daemon's send connection */
enum { UI = 3, D = 4, D2 = 5, SEND = 6 };

static void notify_reset(chat_notify_t *n)
{
    if (n->gen) { CloseHandle(n->gen); n->gen = NULL; }
}

static void reset_all(void)
{
    ensure_init();
    chatcore_free(&g_core);
    chatcore_init(&g_core, LOG_MAX_SIZE);
    notify_reset(&g_log_notify);
    notify_reset(&g_prompt_notify);
    notify_reset(&g_status_notify);
    g_prompt_listeners = 0;
    g_prompt_ever_seen = 0;
    g_proxy_started = fake_now;
    g_park_slot = NULL;
    g_longpoll_max_ms = 0;
    fake_waits = 0;
    fake_during_wait = NULL;
    fake_sock_reset();
    memset(resp_count, 0, sizeof(resp_count));
}

/* "another thread" actions, run while a waiter blocks */
static void other_sets_status(void)   { handle_status_set(SEND, "reading config.cfg"); }
static void other_appends_log(void)   { handle_log_append(SEND, "hello"); }
static void other_pushes_prompt(void) { handle_prompt_push(UI, "what time is it"); }

/* ================================================================ fix 1 */

TEST(status_wait_does_not_spin_on_a_stale_signal) {
    DWORD t0;
    reset_all();
    handle_status_set(SEND, "EXEC dir C:\\");
    handle_status_wait(UI, "0 30000");         /* fast path: seq 0 -> 1 */
    CHECK(strcmp(resp[UI], "1\nEXEC dir C:\\") == 0, "fast path answers");

    /* THE BUG: the old manual-reset event was still set here, so this wait
     * "woke" immediately, saw no change, and looped - thousands of times. */
    fake_waits = 0;
    t0 = fake_now;
    handle_status_wait(UI, "1 30000");
    CHECK(fake_waits <= 2, "a wait with nothing new must block, not spin");
    CHECK_EQ_U(fake_now - t0, 30000);          /* it slept its whole timeout */
    CHECK(strcmp(resp[UI], "1\nEXEC dir C:\\") == 0, "timeout re-sends current");
    CHECK_EQ_I(fake_lock_errors, 0);
}

TEST(status_wait_cannot_miss_a_set_that_lands_while_it_waits) {
    DWORD t0;
    reset_all();
    handle_status_set(SEND, "idle");           /* seq 1 */
    t0 = fake_now;
    fake_during_wait = other_sets_status;      /* seq 2 arrives mid-wait */
    handle_status_wait(UI, "1 30000");
    CHECK(strcmp(resp[UI], "2\nreading config.cfg") == 0,
          "the waiter must see the set that happened while it waited");
    CHECK_EQ_U(fake_now - t0, 0);              /* ...immediately, not at 30 s */
}

TEST(two_status_waiters_both_wake_on_one_set) {
    /* broadcast: one waiter consuming the signal must not starve another */
    reset_all();
    fake_during_wait = other_sets_status;
    handle_status_wait(UI, "0 30000");
    CHECK(strcmp(resp[UI], "1\nreading config.cfg") == 0, "first waiter");
    fake_waits = 0;
    handle_status_wait(D2, "0 30000");         /* a second, behind the same seq */
    CHECK(strcmp(resp[D2], "1\nreading config.cfg") == 0,
          "second waiter answered at once (fast path), nothing consumed it");
    CHECK_EQ_I(fake_waits, 0);
}

TEST(log_wait_wakes_on_append_and_does_not_spin) {
    DWORD t0;
    reset_all();
    handle_log_append(SEND, "x");              /* a signal nobody waited for */
    handle_log_wait(UI, "0 30000");            /* fast path */
    CHECK(strcmp(resp[UI], "1\nx") == 0, "fast path returns the byte");
    fake_waits = 0;
    t0 = fake_now;
    fake_during_wait = other_appends_log;
    handle_log_wait(UI, "1 30000");
    CHECK(strcmp(resp[UI], "6\nhello") == 0, "woken by the append");
    CHECK_EQ_U(fake_now - t0, 0);
    CHECK(fake_waits <= 2, "no spin");
    /* the old auto-reset event returned EARLY and EMPTY on a stale signal;
     * this one waits out its timeout when nothing arrives */
    t0 = fake_now;
    handle_log_wait(UI, "6 30000");
    CHECK_EQ_U(fake_now - t0, 30000);
    CHECK(strcmp(resp[UI], "6\n") == 0, "timeout: total, no bytes");
}

TEST(no_event_objects_leak) {
    reset_all();
    fake_during_wait = other_sets_status;
    handle_status_wait(UI, "0 30000");
    handle_status_wait(UI, "1 10");
    fake_during_wait = other_appends_log;
    handle_log_wait(UI, "0 30000");
    notify_reset(&g_log_notify);
    notify_reset(&g_prompt_notify);
    notify_reset(&g_status_notify);
    CHECK_EQ_I(fake_events_live, 0);
}

/* ================================================================ fix 2 */

TEST(multiplex_log_wait_parks_and_is_answered_by_the_append) {
    chat_park_t park = { CHAT_PARK_NONE, 0, 0 };
    DWORD t0;
    reset_all();
    g_longpoll_max_ms = 1000;                  /* the old Win9x clamp */
    t0 = fake_now;

    chatproxy_set_park_slot(&park);
    handle_log_wait(UI, "0 30000");
    chatproxy_set_park_slot(NULL);

    CHECK_EQ_I(resp_count[UI], 0);             /* no reply yet: parked */
    CHECK_EQ_I(park.kind, CHAT_PARK_LOG);
    CHECK_EQ_U(fake_now - t0, 0);              /* and it did NOT block */
    CHECK_EQ_I(fake_waits, 0);
    CHECK_EQ_U(park.deadline - t0, 30000);     /* full length, not 1 s */
    CHECK_EQ_U(chatproxy_park_ms_left(&park, fake_now), 30000);

    CHECK_EQ_I(chatproxy_park_service(UI, &park, fake_now, 0), 0);
    handle_log_append(SEND, "reply text");     /* another client's command */
    CHECK_EQ_I(chatproxy_park_service(UI, &park, fake_now, 0), 1);
    CHECK(strcmp(resp[UI], "10\nreply text") == 0, "answered with the append");
    CHECK_EQ_I(park.kind, CHAT_PARK_NONE);
    CHECK_EQ_U(chatproxy_park_ms_left(&park, fake_now), INFINITE);
}

TEST(multiplex_status_wait_answers_at_its_deadline) {
    chat_park_t park = { CHAT_PARK_NONE, 0, 0 };
    DWORD t0;
    reset_all();
    t0 = fake_now;
    chatproxy_set_park_slot(&park);
    handle_status_wait(UI, "0 30000");
    chatproxy_set_park_slot(NULL);
    CHECK_EQ_I(park.kind, CHAT_PARK_STATUS);
    CHECK_EQ_I(chatproxy_park_service(UI, &park, t0 + 29999, 0), 0);
    CHECK_EQ_I(resp_count[UI], 0);
    CHECK_EQ_I(chatproxy_park_service(UI, &park, t0 + 30000, 0), 1);
    CHECK(strcmp(resp[UI], "0\n") == 0, "deadline answer = current status");
}

TEST(multiplex_prompt_wait_parks_then_delivers_to_first_live_poller) {
    chat_park_t p1 = { CHAT_PARK_NONE, 0, 0 }, p2 = { CHAT_PARK_NONE, 0, 0 };
    reset_all();
    chatproxy_set_park_slot(&p1);
    handle_prompt_wait(D, "30000");
    chatproxy_set_park_slot(&p2);
    handle_prompt_wait(D2, "30000");
    chatproxy_set_park_slot(NULL);
    CHECK_EQ_I(p1.kind, CHAT_PARK_PROMPT);
    CHECK_EQ_I(g_prompt_listeners, 2);

    handle_prompt_push(UI, "hello fleet");
    CHECK(strcmp(resp[UI], "OK") == 0, "two listeners parked: plain OK");
    CHECK_EQ_I(chatproxy_park_service(D, &p1, fake_now, 0), 1);
    CHECK(strcmp(resp[D], "hello fleet") == 0, "first parked poller gets it");
    CHECK_EQ_I(chatproxy_park_service(D2, &p2, fake_now, 0), 0);
    CHECK_EQ_I(resp_count[D2], 0);             /* the other stays parked */
    CHECK_EQ_I(g_prompt_listeners, 1);
}

TEST(a_forced_answer_never_hands_out_a_prompt) {
    chat_park_t p = { CHAT_PARK_NONE, 0, 0 };
    reset_all();
    chatproxy_set_park_slot(&p);
    handle_prompt_wait(D, "30000");
    chatproxy_set_park_slot(NULL);
    handle_prompt_push(UI, "keep me");
    /* the client sent another command while parked: it has moved on */
    CHECK_EQ_I(chatproxy_park_service(D, &p, fake_now, 1), 1);
    CHECK(strcmp(resp[D], "") == 0, "forced answer is the timeout answer");
    CHECK(g_core.prompt_pending, "the prompt is still there for a real poll");
}

/* ================================================================ fix 4 */

TEST(parked_prompt_wait_on_a_dead_peer_leaves_the_prompt) {
    chat_park_t dead = { CHAT_PARK_NONE, 0, 0 }, live = { CHAT_PARK_NONE, 0, 0 };
    reset_all();
    chatproxy_set_park_slot(&dead);
    handle_prompt_wait(D, "30000");
    chatproxy_set_park_slot(&live);
    handle_prompt_wait(D2, "30000");
    chatproxy_set_park_slot(NULL);

    fake_socks[D].peer_closed = 1;             /* daemon went away, FIN queued */
    handle_prompt_push(UI, "do not lose me");
    CHECK_EQ_I(chatproxy_park_service(D, &dead, fake_now, 0), -1);
    CHECK_EQ_I(resp_count[D], 0);              /* nothing sent into the void */
    CHECK(g_core.prompt_pending, "the prompt was NOT taken by the dead socket");
    CHECK_EQ_I(chatproxy_park_service(D2, &live, fake_now, 0), 1);
    CHECK(strcmp(resp[D2], "do not lose me") == 0, "the live poller gets it");
}

TEST(threaded_prompt_wait_on_a_dead_peer_leaves_the_prompt) {
    reset_all();
    fake_socks[D].peer_closed = 1;
    fake_during_wait = other_pushes_prompt;
    handle_prompt_wait(D, "30000");
    CHECK_EQ_I(resp_count[D], 0);
    CHECK(g_core.prompt_pending, "left pending for the next poller");
    CHECK_EQ_I(g_prompt_listeners, 0);
    handle_prompt_wait(D2, "30000");
    CHECK(strcmp(resp[D2], "what time is it") == 0, "next poller gets it");
}

TEST(a_prompt_taken_by_a_connection_that_dies_is_put_back) {
    reset_all();
    handle_prompt_push(UI, "first");
    handle_prompt_wait(D, "30000");            /* taken: in flight for D */
    CHECK(strcmp(resp[D], "first") == 0, "delivered");
    CHECK(!g_core.prompt_pending && g_core.prompt_inflight, "in flight");

    /* the daemon's connection drops before its next PROMPT_WAIT */
    chatproxy_conn_closed(D);
    CHECK(g_core.prompt_pending, "put back");
    handle_prompt_wait(D2, "30000");
    CHECK(strcmp(resp[D2], "first") == 0, "re-delivered to the reconnect");
}

TEST(the_next_command_acknowledges_the_prompt) {
    reset_all();
    handle_prompt_push(UI, "first");
    handle_prompt_wait(D, "30000");
    chatproxy_conn_command(D);                 /* its next PROMPT_WAIT arrives */
    chatproxy_conn_closed(D);
    CHECK(!g_core.prompt_pending && !g_core.prompt_inflight,
          "acknowledged: a later drop does not resurrect it");
    /* another connection's commands do not acknowledge D's prompt */
    handle_prompt_push(UI, "second");
    handle_prompt_wait(D, "30000");
    chatproxy_conn_command(D2);
    chatproxy_conn_closed(D);
    CHECK(g_core.prompt_pending, "D2's command did not ack D's prompt");
}

TEST(a_newer_prompt_wins_over_a_requeue) {
    reset_all();
    handle_prompt_push(UI, "old");
    handle_prompt_wait(D, "30000");
    handle_prompt_push(UI, "new");             /* user typed again meanwhile */
    chatproxy_conn_closed(D);
    CHECK(strcmp(g_core.prompt, "new") == 0 && g_core.prompt_pending,
          "the newer prompt is not overwritten by the requeue");
}

/* ================================================================ fix 5 */

/* A reader exactly as retro_chat <= 0.15 is written: offset += body bytes,
 * back to 0 when the total is smaller than its offset. */
typedef struct { unsigned long offset; unsigned long printed; int resets; } reader_t;

static void reader_poll(reader_t *r)
{
    char cmd[32];
    const char *nl;
    unsigned long total, body;
    snprintf(cmd, sizeof(cmd), "%lu", r->offset);
    handle_log_read(UI, cmd);
    nl = strchr(resp[UI], '\n');
    total = strtoul(resp[UI], NULL, 10);
    body = (unsigned long)strlen(nl + 1);
    r->printed += body;
    r->offset += body;
    if (total < r->offset) { r->offset = 0; r->resets++; }
}

TEST(ring_truncation_does_not_send_a_reader_back_to_zero) {
    reader_t r = { 0, 0, 0 };
    char chunk[1001];
    int i;
    reset_all();
    memset(chunk, 'a', 1000);
    chunk[1000] = '\0';
    /* 600 KB through a 256 KB ring: the oldest half is dropped twice */
    for (i = 0; i < 600; i++) {
        handle_log_append(SEND, chunk);
        reader_poll(&r);
    }
    CHECK(g_core.log_base > 0, "the ring really did drop its oldest half");
    CHECK_EQ_I(r.resets, 0);                   /* the old agent: reset here */
    CHECK_EQ_U(r.printed, 600000);             /* every byte printed ONCE */
    CHECK_EQ_U(r.offset, chatcore_log_end(&g_core));
    /* what the OLD relative numbering reported at this point: the buffer
     * size, which is below the reader's offset -> reset + reprint */
    CHECK(g_core.log_size < r.offset, "old total would have shrunk under it");
}

TEST(log_read_behind_the_base_resumes_at_the_base) {
    char chunk[1001];
    int i;
    unsigned long total;
    reset_all();
    memset(chunk, 'b', 1000);
    chunk[1000] = '\0';
    for (i = 0; i < 300; i++) handle_log_append(SEND, chunk);
    handle_log_read(UI, "0");
    total = strtoul(resp[UI], NULL, 10);
    CHECK_EQ_U(total, 300000);
    CHECK_EQ_U(strlen(strchr(resp[UI], '\n') + 1), g_core.log_size);
}

TEST(log_read_past_the_end_after_a_clear_says_so) {
    reset_all();
    handle_log_append(SEND, "0123456789");
    handle_log_clear(UI);
    handle_log_append(SEND, "abc");
    handle_log_read(UI, "10");                 /* reader from before the clear */
    CHECK(strcmp(resp[UI], "3\n") == 0, "empty body, smaller total -> reset");
    handle_log_wait(UI, "10 30000");           /* and LOG_WAIT answers at once */
    CHECK(strcmp(resp[UI], "3\n") == 0, "LOG_WAIT past the end returns now");
}

/* ================================================================ fix 6 */

TEST(log_append2_is_idempotent_and_verbatim) {
    reset_all();
    handle_log_append2(SEND, "7 hello");
    CHECK(strcmp(resp[SEND], "OK") == 0, "first append");
    handle_log_append2(SEND, "7 hello");       /* the daemon's resend */
    CHECK(strcmp(resp[SEND], "OK dup") == 0, "resend is recognised");
    CHECK_EQ_U(g_core.log_size, 5);            /* and NOT appended again */
    handle_log_append2(SEND, "8  indented");   /* text after ONE space */
    CHECK(memcmp(g_core.log + 5, " indented", 9) == 0, "leading blank kept");
    handle_log_append2(SEND, "9");             /* empty chunk is fine */
    CHECK(strcmp(resp[SEND], "OK") == 0, "empty text");
    handle_log_append2(SEND, "x7 hi");
    CHECK_EQ_I(resp_status[SEND], 0xFF);
    handle_log_append2(SEND, "");
    CHECK_EQ_I(resp_status[SEND], 0xFF);
    handle_log_append2(SEND, "12345678901 hi"); /* not a uint32 */
    CHECK_EQ_I(resp_status[SEND], 0xFF);
}

TEST(log_append_keeps_leading_blanks) {
    reset_all();
    handle_log_append(SEND, "two");
    handle_log_append(SEND, " words");
    CHECK(g_core.log_size == 9 && memcmp(g_core.log, "two words", 9) == 0,
          "LOG_APPEND must not glue words together");
}

/* ================================================================ fix 10 */

TEST(prompt_push_reports_replaced_and_no_listener) {
    chat_park_t p = { CHAT_PARK_NONE, 0, 0 };
    reset_all();
    fake_now += 42000;                          /* nobody has ever polled */
    handle_prompt_push(UI, "anyone there");
    CHECK(strcmp(resp[UI], "OK no-listener 42s") == 0, "no listener, ever");
    handle_prompt_push(UI, "second");
    CHECK(strcmp(resp[UI], "OK replaced no-listener 42s") == 0,
          "the unpicked first prompt was overwritten");

    handle_prompt_wait(D, "30000");             /* a poller takes "second" */
    chatproxy_set_park_slot(&p);
    handle_prompt_wait(D, "30000");             /* and parks again */
    chatproxy_set_park_slot(NULL);
    fake_now += 25000;                          /* long parked is still listening */
    handle_prompt_push(UI, "third");
    CHECK(strcmp(resp[UI], "OK") == 0, "a parked poller is a listener");
    CHECK_EQ_I(chatproxy_park_service(D, &p, fake_now, 0), 1);

    fake_now += 20000;                          /* the poller never came back */
    handle_prompt_push(UI, "fourth");
    CHECK(strcmp(resp[UI], "OK no-listener 20s") == 0, "listener gone 20 s");
    CHECK(strncmp(resp[UI], "OK", 2) == 0, "every reply still starts OK");
}

TEST(prompt_push_rejects_blank_but_keeps_text_verbatim) {
    reset_all();
    handle_prompt_push(UI, "   ");
    CHECK_EQ_I(resp_status[UI], 0xFF);
    handle_prompt_push(UI, "  indented question");
    CHECK(strcmp(g_core.prompt, "  indented question") == 0, "verbatim");
}

MUNIT_MAIN("chatproxy chat bus: long-polls, parking, prompt safety (true-source)", {
    RUN(status_wait_does_not_spin_on_a_stale_signal);
    RUN(status_wait_cannot_miss_a_set_that_lands_while_it_waits);
    RUN(two_status_waiters_both_wake_on_one_set);
    RUN(log_wait_wakes_on_append_and_does_not_spin);
    RUN(no_event_objects_leak);
    RUN(multiplex_log_wait_parks_and_is_answered_by_the_append);
    RUN(multiplex_status_wait_answers_at_its_deadline);
    RUN(multiplex_prompt_wait_parks_then_delivers_to_first_live_poller);
    RUN(a_forced_answer_never_hands_out_a_prompt);
    RUN(parked_prompt_wait_on_a_dead_peer_leaves_the_prompt);
    RUN(threaded_prompt_wait_on_a_dead_peer_leaves_the_prompt);
    RUN(a_prompt_taken_by_a_connection_that_dies_is_put_back);
    RUN(the_next_command_acknowledges_the_prompt);
    RUN(a_newer_prompt_wins_over_a_requeue);
    RUN(ring_truncation_does_not_send_a_reader_back_to_zero);
    RUN(log_read_behind_the_base_resumes_at_the_base);
    RUN(log_read_past_the_end_after_a_clear_says_so);
    RUN(log_append2_is_idempotent_and_verbatim);
    RUN(log_append_keeps_leading_blanks);
    RUN(prompt_push_reports_replaced_and_no_listener);
    RUN(prompt_push_rejects_blank_but_keeps_text_verbatim);
})
