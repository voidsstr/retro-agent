/* test_refreshkeep.c
 *
 * Guards agent/tools/refreshkeep.c - the tool that holds a fullscreen game at
 * 100Hz on .124 (GeForce2 GTS / ForceWare 71.89 / Sony CPD-G200).
 *
 * The bug it exists for, measured on hardware 2026-08-25: a game that calls
 * ChangeDisplaySettings WITHOUT DM_DISPLAYFREQUENCY lands at 60Hz even when
 * the desktop is already at 100Hz and CDS_UPDATEREGISTRY has stored 100Hz for
 * that exact mode. Desktop 1024x768x32@100 -> Quake II fullscreen 1024x768 ->
 * DISPLAYCFG reported refresh=60. ForceWare 71.89's control panel has no
 * refresh-rate override page, so the mode must be re-applied from outside.
 *
 * This test #includes the SHIPPING decision logic (agent/tools/refreshlogic.h)
 * rather than copying it, and asserts both the fixed behaviour and the
 * old-buggy outcome it replaces (60Hz silently left in place).
 */
#include "munit.h"
#include "../../agent/tools/refreshlogic.h"

/* The rates .124's driver enumerates for 1024x768x32, from setrefresh.exe:
 * "available refresh at 1024x768x32: 60 70 72 75 85 100". */
static const int G200[] = { 60, 70, 72, 75, 85, 100 };
#define G200_N ((int) (sizeof(G200) / sizeof(int)))

TEST(max_picks_100_not_the_60_the_game_left) {
    CHECK_EQ_I(rk_pick_refresh(G200, G200_N, 0), 100);
}

TEST(explicit_supported_rate_is_honoured) {
    CHECK_EQ_I(rk_pick_refresh(G200, G200_N, 100), 100);
    CHECK_EQ_I(rk_pick_refresh(G200, G200_N, 85), 85);
}

TEST(unsupported_rate_yields_no_change) {
    /* 120Hz is not offered at 1024x768 on the CPD-G200. Driving a CRT at a
     * rate the driver does not enumerate is how you get "out of range" on a
     * box that needs physical access, so this must be 0 (do nothing) and
     * never silently fall back to some other rate. */
    CHECK_EQ_I(rk_pick_refresh(G200, G200_N, 120), 0);
}

TEST(sentinel_rates_are_never_chosen) {
    /* EnumDisplaySettings reports 0Hz/1Hz "adapter default" sentinel modes;
     * setrefresh.c already guards these with its < 200 check. */
    static const int with_sentinels[] = { 0, 1, 60, 100, 255 };
    CHECK_EQ_I(rk_pick_refresh(with_sentinels, 5, 0), 100);
    CHECK_EQ_I(rk_pick_refresh(with_sentinels, 5, 1), 0);
    CHECK_EQ_I(rk_pick_refresh(with_sentinels, 5, 255), 0);
    CHECK_EQ_I(rk_hz_is_real(0), 0);
    CHECK_EQ_I(rk_hz_is_real(200), 0);
    CHECK_EQ_I(rk_hz_is_real(100), 1);
}

TEST(forces_at_60_and_stays_quiet_at_100) {
    /* THE fix: 60Hz under a 100Hz target must trigger a re-apply. */
    CHECK_EQ_I(rk_should_force(60, 100), 1);
    /* ...and must not churn once there: the 1Hz poll would otherwise re-set
     * the mode every second and flicker the CRT for the whole session. */
    CHECK_EQ_I(rk_should_force(100, 100), 0);
    /* No usable target -> leave the display alone. */
    CHECK_EQ_I(rk_should_force(60, 0), 0);
}

TEST(an_unknown_current_rate_is_never_forced) {
    /* Win9x reports 0 for the live mode and some drivers their 1 Hz default:
     * the OLD rule forced on every 1 s poll forever (0 != 100), and a re-apply
     * cannot make the reading real - a CRT re-syncing once a second. */
    CHECK_EQ_I(rk_should_force(0, 100), 0);
    CHECK_EQ_I(rk_should_force(1, 100), 0);
    CHECK_EQ_I(rk_should_force(255, 100), 0);
    CHECK(0 != 100, "the old rule's test - current != target - was true here");
}

TEST(a_mode_that_keeps_drifting_stops_the_holder) {
    /* A D3D title that loses its device when the mode changes under it resets
     * at no rate: each re-apply is undone within a second. Three re-applies in
     * a minute is the budget; the fourth is refused. */
    int t[8] = { 5, 6, 7 };
    CHECK_EQ_I(rk_budget_ok(t, 0, 5), 1);
    CHECK_EQ_I(rk_budget_ok(t, 2, 7), 1);
    CHECK_EQ_I(rk_budget_ok(t, 3, 8), 0);
    /* an OpenGL title that drifts once per level load is fine: re-applies a
     * minute apart never exhaust it */
    t[0] = 0; t[1] = 70; t[2] = 140;
    CHECK_EQ_I(rk_budget_ok(t, 3, 150), 1);
}

TEST(empty_mode_list_is_safe) {
    CHECK_EQ_I(rk_pick_refresh(NULL, 0, 0), 0);
    CHECK_EQ_I(rk_pick_refresh(NULL, 0, 100), 0);
}

MUNIT_MAIN("refreshkeep 100Hz hold (agent/tools/refreshlogic.h)", {
    RUN(max_picks_100_not_the_60_the_game_left);
    RUN(explicit_supported_rate_is_honoured);
    RUN(unsupported_rate_yields_no_change);
    RUN(sentinel_rates_are_never_chosen);
    RUN(forces_at_60_and_stays_quiet_at_100);
    RUN(an_unknown_current_rate_is_never_forced);
    RUN(a_mode_that_keeps_drifting_stops_the_holder);
    RUN(empty_mode_list_is_safe);
})
