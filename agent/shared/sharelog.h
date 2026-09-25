/*
 * sharelog.h - WHEN the agent mirrors its log to the share.
 *
 * The agent copies C:\RETRO_AGENT\agent.log (and its rolled agent.log.1) to
 * \\...\agent logs\<host>-agent.log so a box that cannot be reached - or that
 * crashed - can still be read from one place. That is worth keeping. What was
 * not worth keeping is HOW it did it (agent <= 1.84.x):
 *
 *   - both files were copied every 60 s, for the life of the agent, whether or
 *     not a single byte had changed: 2 x 1,440 SMB copies a day per box, 512 KB
 *     each at the cap, over SMB1 from a Pentium 166;
 *   - and every copy logged "sharelog: mirrored to ..." - so the log ALWAYS
 *     changed, which guaranteed the next copy was never a no-op either.
 *
 * Now:
 *   - agent.log is copied only when log.c's write counter moved since the copy
 *     that last succeeded; agent.log.1 only when the rotation counter moved
 *     (it cannot change otherwise - it is only ever written by a rotation);
 *   - the first pass after startup copies both, so the PREVIOUS run's log -
 *     including a crash - still reaches the share straight after a reboot;
 *   - a failed copy is retried (the counters it was trying to reach are not
 *     recorded), with the pass interval doubling while the share stays
 *     unreachable, up to 8x;
 *   - the agent logs only when the outcome CHANGES (working <-> failing), so
 *     an unreachable share is still visible in the log, once, and a working
 *     one is not re-announced every minute.
 *
 * Header-only and free of Win32 so tests/native/test_sharelog.c compiles the
 * code the agent runs.
 */
#ifndef RETRO_SHARELOG_H
#define RETRO_SHARELOG_H

#ifdef __GNUC__
#define SHARELOG_UNUSED __attribute__((unused))
#else
#define SHARELOG_UNUSED
#endif

#define SHARELOG_COPY_LOG   1
#define SHARELOG_COPY_BAK   2
#define SHARELOG_MAX_BACKOFF 3        /* period << 3 = 8x while failing */

typedef struct {
    int           have_log;     /* agent.log reached the share at least once */
    unsigned long log_seq;      /* ...reflecting this many log writes        */
    int           have_bak;     /* agent.log.1 reached the share             */
    unsigned long bak_rot;      /* ...reflecting this many rotations         */
    int           last_ok;      /* -1 no outcome yet, 0 failing, 1 working   */
    int           fail_streak;  /* consecutive failed passes (backoff)       */
} sharelog_state_t;

SHARELOG_UNUSED static void sharelog_init(sharelog_state_t *s)
{
    s->have_log = 0;
    s->log_seq = 0;
    s->have_bak = 0;
    s->bak_rot = 0;
    s->last_ok = -1;
    s->fail_streak = 0;
}

/* What this pass should copy, given log.c's counters now. `bak_exists` is
 * whether agent.log.1 is on the local disk at all (a fresh box has none). */
SHARELOG_UNUSED static int sharelog_plan(const sharelog_state_t *s,
                                         unsigned long write_seq,
                                         unsigned long rotation_seq,
                                         int bak_exists)
{
    int plan = 0;
    if (!s->have_log || write_seq != s->log_seq)
        plan |= SHARELOG_COPY_LOG;
    if (bak_exists && (!s->have_bak || rotation_seq != s->bak_rot))
        plan |= SHARELOG_COPY_BAK;
    return plan;
}

/* Record what the pass achieved. Only a SUCCESSFUL copy advances the counters
 * it reflects, so a failure is retried on the next pass. Returns 1 when the
 * overall outcome differs from the previous pass's - the only time the agent
 * should say anything about the mirror. A pass that had nothing to copy has no
 * outcome and never logs. */
SHARELOG_UNUSED static int sharelog_record(sharelog_state_t *s, int plan,
                                           int log_ok, int bak_ok,
                                           unsigned long write_seq,
                                           unsigned long rotation_seq)
{
    int ok, changed;

    if (!plan)
        return 0;
    if ((plan & SHARELOG_COPY_LOG) && log_ok) {
        s->have_log = 1;
        s->log_seq = write_seq;
    }
    if ((plan & SHARELOG_COPY_BAK) && bak_ok) {
        s->have_bak = 1;
        s->bak_rot = rotation_seq;
    }
    ok = (!(plan & SHARELOG_COPY_LOG) || log_ok) &&
         (!(plan & SHARELOG_COPY_BAK) || bak_ok);
    s->fail_streak = ok ? 0 : s->fail_streak + 1;
    changed = (s->last_ok != ok);
    s->last_ok = ok;
    return changed;
}

/* How long to wait before the next pass: the period, doubled per consecutive
 * failure up to SHARELOG_MAX_BACKOFF doublings. */
SHARELOG_UNUSED static unsigned long sharelog_next_ms(const sharelog_state_t *s,
                                                      unsigned long period_ms)
{
    int k = s->fail_streak;
    if (k > SHARELOG_MAX_BACKOFF)
        k = SHARELOG_MAX_BACKOFF;
    return period_ms << k;
}

#endif /* RETRO_SHARELOG_H */
