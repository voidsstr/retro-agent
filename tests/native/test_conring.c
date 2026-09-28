/* test_conring.c - TRUE-SOURCE: compiles the REAL agent/shared/conring.h, the
 * queue between the agent's loggers and its console (agent 1.90.0,
 * agent/src/log.c echo_push_locked / log_echo_thread).
 *
 * THE OLD-BUGGY BEHAVIOUR (agent <= 1.89.1, log.c raw_out): every log line
 * was echoed with WriteFile(GetStdHandle(STD_ERROR_HANDLE)) while g_log_cs was
 * held, so a caller returned only when the CONSOLE accepted the bytes. A
 * frozen console (a QuickEdit/Mark selection, a hung conhost or display) kept
 * that caller - and every thread queued on the log lock behind it - forever.
 * ADMIN-PC (Win7) went deaf for 35 h that way on 2026-09-26. Modelled below as
 * old_echo() so the regression is asserted in both directions.
 *
 * THE FIX: a logger only copies the line into this bounded ring; one echo
 * thread drains it to the console with no lock held. A full ring drops and
 * counts whole lines, latches until fully drained, and reports the gap at the
 * exact point in the stream where the lines went missing.
 */
#include "munit.h"
#include <stdio.h>
#include <string.h>

#include "../../agent/shared/conring.h"

/* ---- the old rule, for contrast ----------------------------------------
 * A simulated console: `frozen` means it accepts nothing. The old raw_out
 * returned only after the console took the whole line, so with a frozen
 * console the caller never returns. We cannot run a call that never returns,
 * so the model reports whether it WOULD have returned, and how many callers
 * would be stuck behind the lock it holds. */
static int old_echo_returns(int console_frozen)
{
    return !console_frozen;      /* WriteFile(console) under g_log_cs */
}

/* A log-shaped line; every one is LINE_LEN bytes. */
#define LINE_LEN 31
static int line(char *buf, int cap, unsigned seq)
{
    return snprintf(buf, cap, "[12:00:00][MAIN ] line %06u\r\n", seq);
}

/* Take everything out, appending to `out`; returns the gap count seen. */
static unsigned drain(conring_t *r, char *out, unsigned *out_len,
                      unsigned out_cap, unsigned chunk)
{
    char tmp[4096];
    unsigned gaps = 0, n, g;
    if (chunk > sizeof(tmp)) chunk = sizeof(tmp);
    for (;;) {
        n = conring_take(r, tmp, chunk, &g);
        if (n) {
            if (*out_len + n <= out_cap) memcpy(out + *out_len, tmp, n);
            *out_len += n;
        }
        gaps += g;
        if (!n && !g) break;
    }
    return gaps;
}

TEST(lines_come_out_whole_and_in_order)
{
    static char store[256], out[4096];
    conring_t r;
    char l[64], expect[4096];
    unsigned out_len = 0, exp_len = 0, i, gaps;

    conring_init(&r, store, sizeof(store));
    CHECK_EQ_U(r.cap, 256);
    for (i = 0; i < 5; i++) {
        int k = line(l, sizeof(l), i);
        CHECK(conring_push(&r, l, (unsigned)k) & CONRING_QUEUED, "queued");
        memcpy(expect + exp_len, l, (size_t)k);
        exp_len += (unsigned)k;
    }
    gaps = drain(&r, out, &out_len, sizeof(out), 7);   /* odd chunk size */
    CHECK_EQ_U(gaps, 0);
    CHECK_EQ_U(out_len, exp_len);
    CHECK(memcmp(out, expect, exp_len) == 0, "stream differs from what was queued");
    CHECK_EQ_U(conring_used(&r), 0);
}

TEST(the_ring_wraps_without_corrupting_the_stream)
{
    /* 10,000 lines through a 128-byte ring, consumer taking odd amounts:
     * every byte arrives, in order, across hundreds of wrap-arounds. */
    static char store[128], out[600000], expect[600000];
    conring_t r;
    char l[64];
    unsigned out_len = 0, exp_len = 0, i, g;
    char tmp[64];

    conring_init(&r, store, sizeof(store));
    for (i = 0; i < 10000; i++) {
        int k = line(l, sizeof(l), i);
        if (!(conring_push(&r, l, (unsigned)k) & CONRING_QUEUED)) {
            /* the consumer below always leaves room: a drop here is a bug */
            CHECK(0, "a line was dropped although the consumer keeps up");
            return;
        }
        memcpy(expect + exp_len, l, (size_t)k);
        exp_len += (unsigned)k;
        /* consumer takes 13 bytes after every line, sometimes nothing */
        if (i % 3) {
            unsigned n = conring_take(&r, tmp, 13, &g);
            memcpy(out + out_len, tmp, n);
            out_len += n;
            CHECK_EQ_U(g, 0);
        }
        /* keep room for the next line */
        while (r.cap - conring_used(&r) < 40) {
            unsigned n = conring_take(&r, tmp, sizeof(tmp), &g);
            memcpy(out + out_len, tmp, n);
            out_len += n;
        }
    }
    drain(&r, out, &out_len, sizeof(out), 64);
    CHECK_EQ_U(out_len, exp_len);
    CHECK(memcmp(out, expect, exp_len) == 0, "wrap-around corrupted the stream");
    CHECK_EQ_U(r.total_dropped, 0);
}

TEST(the_position_counters_survive_2_to_the_32)
{
    /* head/tail are free-running 32-bit counters; the agent runs for months. */
    static char store[64], out[256];
    conring_t r;
    const char *msg = "0123456789abcdefghijklmnopqrstuvwxyz\r\n";
    unsigned out_len = 0;

    conring_init(&r, store, sizeof(store));
    r.head = r.tail = 0xFFFFFFF0u;           /* 16 bytes before the wrap */
    CHECK(conring_push(&r, msg, (unsigned)strlen(msg)) & CONRING_QUEUED, "queued");
    CHECK_EQ_U(conring_used(&r), strlen(msg));
    CHECK(r.head < r.tail, "head really did wrap past 2^32");
    drain(&r, out, &out_len, sizeof(out), 5);
    CHECK_EQ_U(out_len, strlen(msg));
    CHECK(memcmp(out, msg, strlen(msg)) == 0, "bytes differ across the 2^32 wrap");
}

TEST(a_frozen_console_never_blocks_a_logger)
{
    /* THE DEFECT. The console is frozen: the echo thread is stuck and never
     * takes anything. 100,000 lines are logged. */
    static char store[8192];
    conring_t r;
    char l[64];
    unsigned i, queued = 0, dropped = 0, bytes_queued = 0;
    unsigned long bytes_dropped = 0;

    conring_init(&r, store, sizeof(store));
    for (i = 0; i < 100000; i++) {
        int k = line(l, sizeof(l), i);
        unsigned res = conring_push(&r, l, (unsigned)k);   /* returns, always */
        if (res & CONRING_QUEUED) { queued++; bytes_queued += (unsigned)k; }
        else { dropped++; bytes_dropped += (unsigned long)k; }
        CHECK(conring_used(&r) <= r.cap, "the ring grew past its bound");
    }
    /* FIXED: every call returned; the ring is bounded; drops are counted */
    CHECK_EQ_U(queued + dropped, 100000);
    CHECK_EQ_U(line(l, sizeof(l), 0), LINE_LEN);
    CHECK_EQ_U(queued, 8192 / LINE_LEN);      /* 264 fit, the rest drop */
    CHECK_EQ_U(bytes_queued, conring_used(&r));
    CHECK_EQ_U(r.total_dropped, dropped);
    CHECK_EQ_U(r.total_dropped_bytes, bytes_dropped);
    /* OLD: the very first line would never have returned */
    CHECK_EQ_I(old_echo_returns(1), 0);
    CHECK_EQ_I(old_echo_returns(0), 1);
}

TEST(a_full_ring_drops_whole_lines_never_part_of_one)
{
    static char store[64];
    conring_t r;
    const char *a = "0123456789012345678901234567890123456789\r\n"; /* 42 */
    const char *b = "abcdefghijklmnopqrstuvwxyz\r\n";                 /* 28 */

    conring_init(&r, store, sizeof(store));
    CHECK(conring_push(&r, a, 42) & CONRING_QUEUED, "a fits");
    /* 22 bytes free: b (28) does not fit and must not be split */
    CHECK_EQ_U(conring_push(&r, b, 28), 0);
    CHECK_EQ_U(conring_used(&r), 42);
    CHECK_EQ_U(r.total_dropped, 1);
    CHECK_EQ_U(r.total_dropped_bytes, 28);
}

TEST(after_a_drop_the_ring_latches_until_fully_drained)
{
    /* One gap per stall, placed exactly where the ring's contents end. A
     * short line that WOULD fit after a drop is still refused, or the console
     * would show later lines before the gap notice. */
    static char store[64];
    conring_t r;
    char tmp[64];
    unsigned g, n;

    conring_init(&r, store, sizeof(store));
    conring_push(&r, "0123456789012345678901234567890123456789\r\n", 42);
    CHECK_EQ_U(conring_push(&r, "abcdefghijklmnopqrstuvwxyz\r\n", 28), 0);
    CHECK_EQ_U(conring_push(&r, "ok\r\n", 4), 0);     /* fits, but latched */
    CHECK_EQ_U(conring_push(&r, "ok\r\n", 4), 0);
    CHECK_EQ_U(r.total_dropped, 3);

    /* a partial take does not report the gap yet - bytes before it remain */
    n = conring_take(&r, tmp, 10, &g);
    CHECK_EQ_U(n, 10);
    CHECK_EQ_U(g, 0);
    CHECK_EQ_U(conring_push(&r, "ok\r\n", 4), 0);     /* still latched */

    /* the take that empties the ring reports all four, and unlatches */
    n = conring_take(&r, tmp, sizeof(tmp), &g);
    CHECK_EQ_U(n, 32);
    CHECK_EQ_U(g, 4);
    CHECK_EQ_U(r.gap_lines, 0);
    CHECK_EQ_U(conring_push(&r, "ok\r\n", 4), CONRING_QUEUED | CONRING_WAKE);

    /* the gap is reported once, not on every later take */
    n = conring_take(&r, tmp, sizeof(tmp), &g);
    CHECK_EQ_U(n, 4);
    CHECK_EQ_U(g, 0);
    CHECK_EQ_U(r.total_dropped, 4);                    /* lifetime total kept */
}

TEST(the_gap_notice_lands_between_the_right_lines)
{
    /* Frozen console with a 256-byte ring, then recovery: the echo shows the
     * lines that fit, the notice, then what came after - never interleaved. */
    static char store[256], out[8192];
    conring_t r;
    char l[64], tmp[100];
    unsigned i, g, n, out_len = 0, first_missing = 0, got_gap = 0;

    conring_init(&r, store, sizeof(store));
    for (i = 0; i < 20; i++) {              /* frozen: nothing taken */
        int k = line(l, sizeof(l), i);
        if (!(conring_push(&r, l, (unsigned)k) & CONRING_QUEUED) && !first_missing)
            first_missing = i;
    }
    CHECK_EQ_U(first_missing, 256 / LINE_LEN);   /* 8 fit, line 8 is lost */
    for (;;) {                              /* the console recovers */
        n = conring_take(&r, tmp, sizeof(tmp), &g);
        memcpy(out + out_len, tmp, n);
        out_len += n;
        if (g) {
            got_gap = g;
            out_len += (unsigned)snprintf(out + out_len, 64, "<gap %u>", g);
        }
        if (!n && !g) break;
    }
    for (i = 20; i < 22; i++) {             /* logging after recovery */
        int k = line(l, sizeof(l), i);
        CHECK(conring_push(&r, l, (unsigned)k) & CONRING_QUEUED, "accepted again");
    }
    n = conring_take(&r, tmp, sizeof(tmp), &g);
    memcpy(out + out_len, tmp, n);
    out_len += n;
    out[out_len] = '\0';

    CHECK_EQ_U(got_gap, 12);
    CHECK(strstr(out, "line 000007\r\n<gap 12>[12:00:00][MAIN ] line 000020")
          != NULL, "the notice must sit exactly where lines 8..19 went missing");
    CHECK(strstr(out, "line 000008") == NULL, "a dropped line reappeared");
    CHECK(strstr(out, "line 000000\r\n") == out + 18, "the stream starts at line 0");
}

TEST(wake_only_when_the_consumer_may_be_asleep)
{
    static char store[64];
    conring_t r;
    char tmp[64];
    unsigned g;

    conring_init(&r, store, sizeof(store));
    CHECK_EQ_U(conring_push(&r, "a\r\n", 3), CONRING_QUEUED | CONRING_WAKE);
    CHECK_EQ_U(conring_push(&r, "b\r\n", 3), CONRING_QUEUED);   /* no event per line */
    conring_take(&r, tmp, sizeof(tmp), &g);
    CHECK_EQ_U(conring_push(&r, "c\r\n", 3), CONRING_QUEUED | CONRING_WAKE);
    CHECK_EQ_U(conring_push(&r, "", 0), 0);                     /* nothing, no wake */
}

TEST(a_line_bigger_than_the_ring_cannot_wedge_the_latch)
{
    /* Dropped onto an EMPTY ring, the latch would never clear if nobody woke
     * the consumer: every later line would be refused forever. */
    static char store[32], big[100];
    conring_t r;
    char tmp[64];
    unsigned g, n;

    memset(big, 'x', sizeof(big));
    conring_init(&r, store, sizeof(store));
    CHECK_EQ_U(conring_push(&r, big, sizeof(big)), CONRING_WAKE);   /* dropped + wake */
    n = conring_take(&r, tmp, sizeof(tmp), &g);
    CHECK_EQ_U(n, 0);
    CHECK_EQ_U(g, 1);
    CHECK_EQ_U(conring_push(&r, "ok\r\n", 4), CONRING_QUEUED | CONRING_WAKE);
}

TEST(init_uses_a_power_of_two_and_tolerates_nothing)
{
    static char store[100];
    conring_t r;
    char tmp[8];
    unsigned g;

    conring_init(&r, store, 100);
    CHECK_EQ_U(r.cap, 64);
    conring_init(&r, store, 8192);
    CHECK_EQ_U(r.cap, 8192);
    conring_init(&r, NULL, 0);               /* echo disabled: drops, no crash */
    CHECK_EQ_U(r.cap, 0);
    CHECK_EQ_U(conring_push(&r, "x\r\n", 3), CONRING_WAKE);
    CHECK_EQ_U(conring_take(&r, tmp, sizeof(tmp), &g), 0);
    CHECK_EQ_U(g, 1);
}

MUNIT_MAIN("console echo ring (agent/shared/conring.h, agent 1.90.0)",
    RUN(lines_come_out_whole_and_in_order);
    RUN(the_ring_wraps_without_corrupting_the_stream);
    RUN(the_position_counters_survive_2_to_the_32);
    RUN(a_frozen_console_never_blocks_a_logger);
    RUN(a_full_ring_drops_whole_lines_never_part_of_one);
    RUN(after_a_drop_the_ring_latches_until_fully_drained);
    RUN(the_gap_notice_lands_between_the_right_lines);
    RUN(wake_only_when_the_consumer_may_be_asleep);
    RUN(a_line_bigger_than_the_ring_cannot_wedge_the_latch);
    RUN(init_uses_a_power_of_two_and_tolerates_nothing);
)
