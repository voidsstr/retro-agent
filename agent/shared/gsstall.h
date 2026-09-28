/*
 * gsstall.h - is GAMESYNC making progress, and if not, is it STARVED OF CPU?
 *
 * WHY. GAMESYNC's worker runs at THREAD_PRIORITY_IDLE on purpose (bgwork.h):
 * it must never take CPU from the game the box exists to play. The cost of
 * that choice is that a busy CPU stops it - and until now nothing said so. On
 * .110 (XP, one P4 core) a minimized ioquake3 held 96% of the CPU and a run sat
 * in state=sizing, message "enumerating library", elapsed_s climbing past an
 * hour with titles_total 0, looking exactly like a hung share.
 *
 * WHAT IT MEASURES. The worker marks a "beat" at every progress point (a file
 * listed, a file copied, a status message). Normally beats are milliseconds
 * apart. A gap of GSST_GAP_MS or more is a STALL. Whether a stall is
 * STARVATION is decided by the CPU, not guessed: an idle-priority thread that
 * is ready to run gets the processor whenever ANY of it is idle, so if the
 * system had idle time during the gap the worker was waiting on something else
 * (the share, the disk) - and if the CPU was GSST_BUSY_PCT or more busy for the
 * whole gap, it was starved. (NT's balance-set manager lets a starved thread
 * run for a quantum every ~4 s, which is why .110's log still crept forward a
 * line every 5-20 s: the gaps, not a single hang, are the signature.)
 *
 * The busy figure comes from GetSystemTimes deltas (XP SP1+, resolved at run
 * time - ntdyn.c). Where it does not exist (Win9x, Windows 2000) busy is -1 and
 * a stall is reported as a stall, never as starvation: no measurement, no claim.
 *
 * WHEN IT SPEAKS. Over a sliding window (two GSST_WINDOW_MS buckets), once at
 * least GSST_REPORT_MS has been lost AND that is at least half the window - so
 * a four-hour sync with a minute of hiccups stays quiet, and a run that is
 * starved NOW is reported now even if its first hour was fine.
 *
 * Win32-free so tests/native/test_desk_sweep_order.c compiles THIS file.
 */
#ifndef RETRO_GSSTALL_H
#define RETRO_GSSTALL_H

#ifdef __GNUC__
#define GSST_UNUSED __attribute__((unused))
#else
#define GSST_UNUSED
#endif

#define GSST_GAP_MS      3000UL     /* a gap this long between beats = stall  */
#define GSST_BUSY_PCT    85         /* ...starved if the CPU was this busy     */
#define GSST_WINDOW_MS   300000UL   /* one bucket of the sliding window        */
#define GSST_REPORT_MS   60000UL    /* say nothing until this much is lost     */

enum { GSST_OK = 0, GSST_SLOW = 1, GSST_STARVED = 2 };

typedef struct {
    unsigned long total_stall_ms;   /* whole run                               */
    unsigned long total_starved_ms;
    unsigned long win_start;        /* tick the current bucket began           */
    unsigned long cur_stall, cur_starved;
    unsigned long prev_len, prev_stall, prev_starved;
} gsst_t;

GSST_UNUSED static void gsst_reset(gsst_t *s, unsigned long now)
{
    s->total_stall_ms = s->total_starved_ms = 0;
    s->win_start = now;
    s->cur_stall = s->cur_starved = 0;
    s->prev_len = s->prev_stall = s->prev_starved = 0;
}

/* CPU busy percentage over an interval, from GetSystemTimes deltas. Kernel
 * time INCLUDES idle time, so the total is kernel + user. -1 = unknown. */
GSST_UNUSED static int gsst_busy_pct(unsigned long long idle_d,
                                     unsigned long long kernel_d,
                                     unsigned long long user_d)
{
    unsigned long long total = kernel_d + user_d;
    if (total == 0)
        return -1;
    if (idle_d > total)
        idle_d = total;
    return (int)(100 - (idle_d * 100) / total);
}

GSST_UNUSED static void gsst_roll(gsst_t *s, unsigned long now)
{
    if (now - s->win_start >= GSST_WINDOW_MS) {
        s->prev_len = now - s->win_start;
        s->prev_stall = s->cur_stall;
        s->prev_starved = s->cur_starved;
        s->cur_stall = s->cur_starved = 0;
        s->win_start = now;
    }
}

/* A gap of `gap_ms` between two beats has just closed at `now`, with the CPU
 * `busy_pct` busy across it (-1 = unknown). Returns 1 if it counted as
 * starvation. Short gaps are the normal case and only roll the window. */
GSST_UNUSED static int gsst_note_gap(gsst_t *s, unsigned long now,
                                     unsigned long gap_ms, int busy_pct)
{
    gsst_roll(s, now);
    if (gap_ms < GSST_GAP_MS)
        return 0;
    s->total_stall_ms += gap_ms;
    s->cur_stall += gap_ms;
    if (busy_pct >= GSST_BUSY_PCT) {
        s->total_starved_ms += gap_ms;
        s->cur_starved += gap_ms;
        return 1;
    }
    return 0;
}

/* GSST_OK, GSST_SLOW (stalled, not attributable to the CPU) or GSST_STARVED,
 * over the recent window. *lost_ms / *span_ms say how much of how long. */
GSST_UNUSED static int gsst_verdict(const gsst_t *s, unsigned long now,
                                    unsigned long *lost_ms,
                                    unsigned long *span_ms)
{
    unsigned long span = s->prev_len + (now - s->win_start);
    unsigned long stall = s->prev_stall + s->cur_stall;
    unsigned long starved = s->prev_starved + s->cur_starved;
    int v = GSST_OK;

    if (stall > span)
        span = stall;           /* one gap longer than the window itself */
    if (starved >= GSST_REPORT_MS && starved * 2 >= span) {
        v = GSST_STARVED;
        if (lost_ms) *lost_ms = starved;
    } else if (stall >= GSST_REPORT_MS && stall * 2 >= span) {
        v = GSST_SLOW;
        if (lost_ms) *lost_ms = stall;
    } else if (lost_ms) {
        *lost_ms = stall;
    }
    if (span_ms)
        *span_ms = span;
    return v;
}

#endif /* RETRO_GSSTALL_H */
