/* test_sharelog.c - TRUE-SOURCE: compiles the REAL agent/shared/sharelog.h,
 * the decision behind the agent's share log mirror (agent 1.85.0).
 *
 * THE OLD-BUGGY BEHAVIOUR (agent <= 1.84.x, main.c sharelog_thread): copy
 * agent.log AND agent.log.1 to the share every 60 s for the life of the agent,
 * and log "sharelog: mirrored to ..." after every copy. That is 2 x 1,440 SMB
 * copies a day per box - up to 512 KB each - from a Pentium 166 over SMB1, and
 * because each copy wrote a log line, the log always HAD changed, so no copy
 * was ever skippable either. Modelled below as old_plan() so the regression is
 * asserted in both directions.
 *
 * THE FIX: copy agent.log only when log.c's write counter moved since the last
 * SUCCESSFUL copy, agent.log.1 only when the rotation counter moved, retry a
 * failure with a growing interval, and log only when the outcome flips.
 */
#include "munit.h"
#include <string.h>

#include "../../agent/shared/sharelog.h"

/* the old rule, for contrast: everything, every pass */
static int old_plan(int bak_exists)
{
    return SHARELOG_COPY_LOG | (bak_exists ? SHARELOG_COPY_BAK : 0);
}

TEST(the_first_pass_uploads_the_previous_runs_log)
{
    /* After a reboot the local agent.log still holds the previous run -
     * including a crash. That must reach the share straight away. */
    sharelog_state_t s;
    sharelog_init(&s);
    CHECK_EQ_I(sharelog_plan(&s, 5, 0, 1), SHARELOG_COPY_LOG | SHARELOG_COPY_BAK);
    CHECK_EQ_I(sharelog_plan(&s, 5, 0, 0), SHARELOG_COPY_LOG);
}

TEST(an_unchanged_log_is_not_copied_again)
{
    sharelog_state_t s;
    int plan;
    sharelog_init(&s);
    plan = sharelog_plan(&s, 7, 1, 1);
    sharelog_record(&s, plan, 1, 1, 7, 1);

    /* nothing written, nothing rotated: the FIX copies nothing */
    CHECK_EQ_I(sharelog_plan(&s, 7, 1, 1), 0);
    /* the OLD rule copied both files anyway, every minute */
    CHECK_EQ_I(old_plan(1), SHARELOG_COPY_LOG | SHARELOG_COPY_BAK);
}

TEST(a_new_line_copies_the_log_but_not_the_backup)
{
    /* agent.log.1 is only ever written by a rotation, so a new line cannot
     * have changed it. */
    sharelog_state_t s;
    int plan;
    sharelog_init(&s);
    plan = sharelog_plan(&s, 7, 1, 1);
    sharelog_record(&s, plan, 1, 1, 7, 1);
    CHECK_EQ_I(sharelog_plan(&s, 8, 1, 1), SHARELOG_COPY_LOG);
}

TEST(a_rotation_copies_both)
{
    sharelog_state_t s;
    int plan;
    sharelog_init(&s);
    plan = sharelog_plan(&s, 7, 1, 1);
    sharelog_record(&s, plan, 1, 1, 7, 1);
    CHECK_EQ_I(sharelog_plan(&s, 9, 2, 1), SHARELOG_COPY_LOG | SHARELOG_COPY_BAK);
}

TEST(a_failed_copy_is_retried_not_forgotten)
{
    /* The counters a failed copy was trying to reach are NOT recorded, so the
     * next pass tries again even if nothing new was written. */
    sharelog_state_t s;
    int plan;
    sharelog_init(&s);
    plan = sharelog_plan(&s, 3, 0, 0);
    sharelog_record(&s, plan, 0, 0, 3, 0);
    CHECK_EQ_I(sharelog_plan(&s, 3, 0, 0), SHARELOG_COPY_LOG);

    /* half a pass: the log landed, the backup did not */
    sharelog_init(&s);
    plan = sharelog_plan(&s, 3, 1, 1);
    sharelog_record(&s, plan, 1, 0, 3, 1);
    CHECK_EQ_I(sharelog_plan(&s, 3, 1, 1), SHARELOG_COPY_BAK);
}

TEST(only_a_change_of_outcome_is_logged)
{
    sharelog_state_t s;
    sharelog_init(&s);
    /* first outcome is always news */
    CHECK_EQ_I(sharelog_record(&s, SHARELOG_COPY_LOG, 1, 0, 1, 0), 1);
    /* the same outcome again is not - the OLD code logged every copy */
    CHECK_EQ_I(sharelog_record(&s, SHARELOG_COPY_LOG, 1, 0, 2, 0), 0);
    CHECK_EQ_I(sharelog_record(&s, SHARELOG_COPY_LOG, 1, 0, 3, 0), 0);
    /* the share went away: say so, once */
    CHECK_EQ_I(sharelog_record(&s, SHARELOG_COPY_LOG, 0, 0, 4, 0), 1);
    CHECK_EQ_I(sharelog_record(&s, SHARELOG_COPY_LOG, 0, 0, 4, 0), 0);
    /* and when it comes back */
    CHECK_EQ_I(sharelog_record(&s, SHARELOG_COPY_LOG, 1, 0, 5, 0), 1);
    /* a pass with nothing to do has no outcome at all */
    CHECK_EQ_I(sharelog_record(&s, 0, 0, 0, 5, 0), 0);
    CHECK_EQ_I(s.last_ok, 1);
}

TEST(the_retry_interval_backs_off_and_is_bounded)
{
    sharelog_state_t s;
    int i;
    sharelog_init(&s);
    CHECK_EQ_U(sharelog_next_ms(&s, 60000), 60000);
    sharelog_record(&s, SHARELOG_COPY_LOG, 0, 0, 1, 0);
    CHECK_EQ_U(sharelog_next_ms(&s, 60000), 120000);
    for (i = 0; i < 20; i++)
        sharelog_record(&s, SHARELOG_COPY_LOG, 0, 0, 1, 0);
    CHECK_EQ_U(sharelog_next_ms(&s, 60000), 480000);   /* capped at 8x */
    sharelog_record(&s, SHARELOG_COPY_LOG, 1, 0, 1, 0);
    CHECK_EQ_U(sharelog_next_ms(&s, 60000), 60000);    /* success resets */
}

TEST(a_day_of_idle_costs_one_copy_not_2880)
{
    /* 1,440 one-minute passes over an idle log with a backup present. */
    sharelog_state_t s;
    int pass, copies_new = 0, copies_old = 0;
    sharelog_init(&s);
    for (pass = 0; pass < 1440; pass++) {
        int plan = sharelog_plan(&s, 100, 1, 1);
        copies_new += !!(plan & SHARELOG_COPY_LOG) + !!(plan & SHARELOG_COPY_BAK);
        sharelog_record(&s, plan, 1, 1, 100, 1);
        plan = old_plan(1);
        copies_old += !!(plan & SHARELOG_COPY_LOG) + !!(plan & SHARELOG_COPY_BAK);
    }
    CHECK_EQ_I(copies_new, 2);      /* the first pass: log + backup */
    CHECK_EQ_I(copies_old, 2880);
}

MUNIT_MAIN("share log mirror (agent/shared/sharelog.h, agent 1.85.0)",
    RUN(the_first_pass_uploads_the_previous_runs_log);
    RUN(an_unchanged_log_is_not_copied_again);
    RUN(a_new_line_copies_the_log_but_not_the_backup);
    RUN(a_rotation_copies_both);
    RUN(a_failed_copy_is_retried_not_forgotten);
    RUN(only_a_change_of_outcome_is_logged);
    RUN(the_retry_interval_backs_off_and_is_bounded);
    RUN(a_day_of_idle_costs_one_copy_not_2880);
)
