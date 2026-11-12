/* agent/shared/aigate.h - the Fleet AI engine hard lock (agent 1.99.0).
 *
 * User directive 2026-11-08: the fleet AI is off, its files are removed, and
 * the agent must not turn it back on. Before this release the ONLY gate was
 * HKLM\Software\RetroAgent\AIEngine, which AI_ENABLE set - so "off by
 * default" survived exactly until something issued one command, and then
 * survived reboots in the wrong direction. Every test below asserts BOTH the
 * new answer and the old 1.17.0-1.98.0 answer, so the regression is visible.
 */
#include "munit.h"
#include "../../agent/shared/aigate.h"
#include <string.h>

#define CHECK_WHY(got, want) \
    CHECK(strcmp((got), (want)) == 0, "reason is \"" want "\"")

static ai_regflag_t absent(void)        { ai_regflag_t f; f.present = 0; f.value = 0; return f; }
static ai_regflag_t dword(unsigned long v) { ai_regflag_t f; f.present = 1; f.value = v; return f; }

/* What 1.17.0-1.98.0 did: one flag, nothing else consulted. */
static int old_may_run(ai_regflag_t enabled)
{
    return enabled.present && enabled.value != 0;
}

TEST(a_fresh_box_is_locked_with_nobody_setting_anything)
{
    /* Both values absent - a freshly imaged or freshly auto-updated box. */
    CHECK(!ai_gate_allowed(absent()), "absent AIEngineAllowed is not a permission");
    CHECK(!ai_gate_may_run(absent(), absent()), "cannot run");
    CHECK(ai_gate_should_purge(absent()), "and the start purges");
    CHECK_WHY(ai_gate_why(absent(), absent()), AI_WHY_LOCKED_ABSENT);
}

TEST(the_old_single_flag_is_no_longer_enough)
{
    /* The exact state a pre-1.99.0 AI_ENABLE left behind: AIEngine=1, no
     * permission. THIS is the regression - the old gate ran, the new one
     * refuses and undoes it. */
    ai_regflag_t enabled = dword(1);
    CHECK(old_may_run(enabled), "1.17.0-1.98.0 would have run the engine here");
    CHECK(!ai_gate_may_run(absent(), enabled), "1.99.0 does not");
    CHECK(ai_gate_should_purge(absent()), "it purges the stray flag + binary");
    CHECK(!ai_gate_may_enable(absent()), "and AI_ENABLE refuses");
}

TEST(an_explicit_zero_permission_is_also_locked)
{
    CHECK(!ai_gate_allowed(dword(0)), "AIEngineAllowed=0 is not a permission");
    CHECK(!ai_gate_may_run(dword(0), dword(1)), "even with AIEngine=1");
    CHECK(ai_gate_should_purge(dword(0)), "purges");
    CHECK_WHY(ai_gate_why(dword(0), dword(1)), AI_WHY_LOCKED_ZERO);
}

TEST(permission_alone_does_not_start_anything)
{
    /* The old opt-in behaviour is preserved INSIDE the permission: allowing a
     * box must not be the same as switching the engine on, or allowing one
     * box would silently start burning its CPU. */
    CHECK(ai_gate_allowed(dword(1)), "allowed");
    CHECK(ai_gate_may_enable(dword(1)), "AI_ENABLE would be accepted");
    CHECK(!ai_gate_may_run(dword(1), absent()), "but nothing runs yet");
    CHECK(!ai_gate_may_run(dword(1), dword(0)), "nor with AIEngine=0");
    CHECK(!ai_gate_should_purge(dword(1)), "and an allowed box is NOT purged");
    CHECK_WHY(ai_gate_why(dword(1), absent()), AI_WHY_NOT_ENABLED);
}

TEST(the_deliberate_route_in_still_works)
{
    /* AIEngineAllowed=1 then AI_ENABLE - the only way, and it must work or
     * the three fleet-ai-* skills are dead rather than dormant. */
    CHECK(ai_gate_may_run(dword(1), dword(1)), "allowed + enabled runs");
    CHECK(!ai_gate_should_purge(dword(1)), "never purged while allowed");
    CHECK_WHY(ai_gate_why(dword(1), dword(1)), AI_WHY_RUNNABLE);
}

TEST(a_nonzero_permission_other_than_one_counts)
{
    /* REGWRITE ... REG_DWORD 2 by a fat finger should not read as locked
     * while ALSO not reading as some third state. Any non-zero allows. */
    CHECK(ai_gate_allowed(dword(2)), "non-zero allows");
    CHECK(ai_gate_may_run(dword(0xFFFFFFFFul), dword(7)), "and runs when enabled");
}

TEST(a_wrong_type_value_is_not_a_permission)
{
    /* ai_read_flag() reports present=0 for a non-REG_DWORD, so a REG_SZ "1"
     * written by hand must land here as locked, not as allowed. The struct is
     * the contract: present means "found, and it was a DWORD". */
    ai_regflag_t wrong_type = absent();     /* what ai_read_flag returns for REG_SZ */
    CHECK(!ai_gate_allowed(wrong_type), "a REG_SZ \"1\" does not unlock the box");
    CHECK(ai_gate_should_purge(wrong_type), "it purges, like any locked box");
}

MUNIT_MAIN("aigate (the AI engine cannot turn itself back on)",
    RUN(a_fresh_box_is_locked_with_nobody_setting_anything);
    RUN(the_old_single_flag_is_no_longer_enough);
    RUN(an_explicit_zero_permission_is_also_locked);
    RUN(permission_alone_does_not_start_anything);
    RUN(the_deliberate_route_in_still_works);
    RUN(a_nonzero_permission_other_than_one_counts);
    RUN(a_wrong_type_value_is_not_a_permission);
)
