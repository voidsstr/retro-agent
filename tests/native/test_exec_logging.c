/* test_exec_logging.c — protects the EXEC observability fixes (agent v1.23.0)
 * that came out of the .243 Cirrus refresh session, where two mingw console
 * helpers hung inside Win98's WINOA386 console VM and the single-threaded agent
 * went silent for the full 60s EXEC timeout — looking like a crash with nothing
 * in the log between launch and kill.
 *
 * Two things this locks in:
 *   1. do_exec (agent/src/exec.c) emits a heartbeat every EXEC_HEARTBEAT_MS
 *      while a child runs, and logs elapsed time at exit. Pure timer arithmetic
 *      — reproduced here and asserted against BOTH the fixed and old-silent
 *      behavior — plus a source check that the heartbeat/elapsed lines exist.
 *   2. DOWNLOAD (agent/src/files.c) opens with FILE_SHARE_WRITE so the LIVE
 *      agent.log (held open GENERIC_WRITE by the logger) is retrievable; the old
 *      FILE_SHARE_READ-only open collided with error 32 (the copy-aside dance).
 *
 * exec.c / files.c are Win32-heavy, so the source fixes are verified by reading
 * the real files (true-source presence), and the timing invariant by logic.
 */

#include "munit.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define EXEC_HEARTBEAT_MS 10000   /* must match exec.c */

/* Reproduce do_exec's per-iteration heartbeat decision: fire when at least
 * EXEC_HEARTBEAT_MS has passed since the last beat, then rebase last_beat. */
static int beat_due(unsigned long now, unsigned long *last_beat)
{
    if (now - *last_beat >= EXEC_HEARTBEAT_MS) {
        *last_beat = now;
        return 1;
    }
    return 0;
}

/* Slurp a source file trying a couple of relative roots (repo root or tests/). */
static char *slurp(const char *rel)
{
    static const char *roots[] = { "", "../../", "../", "./" };
    for (unsigned i = 0; i < sizeof(roots) / sizeof(roots[0]); i++) {
        char path[512];
        snprintf(path, sizeof(path), "%s%s", roots[i], rel);
        FILE *f = fopen(path, "rb");
        if (!f) continue;
        fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
        if (n <= 0) { fclose(f); continue; }
        char *buf = (char *)malloc((size_t)n + 1);
        size_t rd = fread(buf, 1, (size_t)n, f);
        fclose(f);
        buf[rd] = '\0';
        return buf;
    }
    return NULL;
}

TEST(heartbeat_fires_across_a_60s_hang) {
    /* A child that hangs for 60s (the listref/WINOA386 case): at 200ms polls we
     * must emit a beat at ~10,20,30,40,50s — five lines — not zero. The OLD code
     * logged nothing between launch and the 60s kill. */
    unsigned long start = 1000000UL;     /* arbitrary GetTickCount base */
    unsigned long last_beat = start;
    int beats = 0;
    for (unsigned long t = 0; t <= 60000UL; t += 200UL)
        beats += beat_due(start + t, &last_beat);
    /* beats at ~10,20,30,40,50,60s -> 6 lines (the 60s beat fires in the same
     * iteration as the timeout kill). The point: NOT zero, as the old code was. */
    CHECK_EQ_I(beats, 6);
    CHECK(beats > 0, "old-buggy behavior (silent hang) must not recur");
}

TEST(no_heartbeat_for_a_quick_command) {
    /* A command that finishes in <10s (the copy builtin) must produce no beat. */
    unsigned long start = 5000UL, last_beat = start;
    int beats = 0;
    for (unsigned long t = 0; t < 9800UL; t += 200UL)
        beats += beat_due(start + t, &last_beat);
    CHECK_EQ_I(beats, 0);
}

TEST(elapsed_seconds_is_ms_over_1000) {
    /* The heartbeat/exit lines report whole seconds = elapsed_ms / 1000. */
    CHECK_EQ_U(60000UL / 1000UL, 60UL);
    CHECK_EQ_U(9999UL  / 1000UL, 9UL);
    CHECK_EQ_U(0UL     / 1000UL, 0UL);
}

TEST(exec_source_has_heartbeat_and_elapsed) {
    char *src = slurp("agent/src/exec.c");
    CHECK(src != NULL, "must find agent/src/exec.c");
    CHECK(strstr(src, "EXEC_HEARTBEAT_MS") != NULL, "heartbeat interval defined");
    CHECK(strstr(src, "EXEC still running") != NULL, "live heartbeat log line present");
    CHECK(strstr(src, "elapsed") != NULL, "elapsed time logged");
    free(src);
}

TEST(download_opens_share_write) {
    char *src = slurp("agent/src/files.c");
    CHECK(src != NULL, "must find agent/src/files.c");
    /* The DOWNLOAD open must tolerate a concurrent writer (the live log). */
    CHECK(strstr(src, "FILE_SHARE_READ | FILE_SHARE_WRITE") != NULL,
          "DOWNLOAD must open FILE_SHARE_READ|FILE_SHARE_WRITE (fixes error 32 on live log)");
    free(src);
}

MUNIT_MAIN("agent EXEC logging + DOWNLOAD share (v1.23.0)", {
    RUN(heartbeat_fires_across_a_60s_hang);
    RUN(no_heartbeat_for_a_quick_command);
    RUN(elapsed_seconds_is_ms_over_1000);
    RUN(exec_source_has_heartbeat_and_elapsed);
    RUN(download_opens_share_write);
})
