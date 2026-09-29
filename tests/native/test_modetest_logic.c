/* test_modetest_logic.c - TRUE-SOURCE test of tools/modetest/modetest_logic.h,
 * the checks tools/modetest/modetest.exe makes before it touches a display.
 *
 * modetest measures what a fullscreen mode change that names NO refresh lands
 * on (the question behind "every Quake-era title at the monitor's best refresh
 * at its own resolution"). It switches modes on real monitors - two of them
 * 1998-2000 CRTs - so every refusal below is a way a mode test could hurt a
 * box, and each is asserted in BOTH directions: the request that must pass and
 * the one that must not. The mode lists are the fleet's own, from GAMERES /
 * the vcr-kmd timing table / DISPLAYCFG reads of 2026-09-29.
 */
#include "munit.h"
#include "../../tools/modetest/modetest_logic.h"

/* .124 (V5 6000 on vcr-kmd, HP P1120, EDID vmax 160): a slice of what the
 * driver lists, several rates per mode (vcr_modes.c's table) */
static const mt_mode_t L124[] = {
    { 640, 480, 32, 60 }, { 640, 480, 32, 75 }, { 640, 480, 32, 85 },
    { 1024, 768, 32, 60 }, { 1024, 768, 32, 70 }, { 1024, 768, 32, 75 }, { 1024, 768, 32, 85 },
    { 1152, 864, 32, 75 }, { 1152, 864, 32, 60 }, { 1152, 864, 32, 85 },
    { 1280, 960, 32, 60 }, { 1280, 960, 32, 85 }, { 1280, 960, 32, 75 },
    { 1280, 1024, 32, 60 }, { 1280, 1024, 32, 75 }, { 1280, 1024, 32, 85 },
    { 1600, 1200, 32, 60 }, { 1600, 1200, 32, 75 }, { 1600, 1200, 32, 85 },
    { 1280, 720, 32, 60 },
};
#define N124 ((int)(sizeof L124 / sizeof L124[0]))
static const mt_mode_t REG124 = { 1280, 1024, 32, 85 };

/* .243 (Win98 SE, Cirrus 5436, no EDID): Win9x lists every mode at 0 Hz */
static const mt_mode_t L243[] = {
    { 640, 480, 8, 0 }, { 800, 600, 8, 0 }, { 1024, 768, 8, 0 },
    { 640, 480, 16, 0 }, { 800, 600, 16, 0 },
};
#define N243 ((int)(sizeof L243 / sizeof L243[0]))
static const mt_mode_t REG243 = { 1024, 768, 8, 75 };

TEST(a_mode_must_be_listed_with_its_depth)
{
    int hz;
    CHECK_EQ_I(mt_check_request(L124, N124, &REG124, 160, 0, 0, 1280, 960, 32, 0, &hz),
               MT_OK);
    /* 1280x960 exists, but not at 16 bpp in this list: a depth the driver
     * never offered is not asked for */
    CHECK_EQ_I(mt_check_request(L124, N124, &REG124, 160, 0, 0, 1280, 960, 16, 0, &hz),
               MT_E_NOT_LISTED);
    CHECK_EQ_I(mt_check_request(L124, N124, &REG124, 160, 0, 0, 1366, 768, 32, 0, &hz),
               MT_E_NOT_LISTED);
    CHECK_EQ_I(mt_check_request(L124, N124, &REG124, 160, 0, 0, 640, 480, 12, 0, &hz),
               MT_E_BPP);
}

TEST(never_bigger_than_the_persisted_desktop)
{
    int hz;
    /* 1600x1200 is listed, but the box is set up for 1280x1024 */
    CHECK_EQ_I(mt_check_request(L124, N124, &REG124, 160, 0, 0, 1600, 1200, 32, 0, &hz),
               MT_E_BIGGER);
    CHECK_EQ_I(mt_check_request(L124, N124, &REG124, 160, 0, 0, 1280, 1024, 32, 0, &hz),
               MT_OK);
}

TEST(an_explicit_rate_must_be_listed_real_and_under_the_ceiling)
{
    int hz = -9;
    CHECK_EQ_I(mt_check_request(L124, N124, &REG124, 160, 0, 0, 1280, 960, 32, 85, &hz),
               MT_OK);
    CHECK_EQ_I(hz, 85);
    /* 70 Hz is not listed at 1280x960 */
    CHECK_EQ_I(mt_check_request(L124, N124, &REG124, 160, 0, 0, 1280, 960, 32, 70, &hz),
               MT_E_RATE_NOT_LISTED);
    /* 1 Hz is the "driver default" sentinel, never a rate */
    CHECK_EQ_I(mt_check_request(L124, N124, &REG124, 160, 0, 0, 1280, 960, 32, 1, &hz),
               MT_E_RATE_NOT_REAL);
    /* listed, but past a panel that says it tops out at 76 Hz */
    CHECK_EQ_I(mt_check_request(L124, N124, &REG124, 76, 0, 0, 1280, 960, 32, 85, &hz),
               MT_E_RATE_OVER_CEILING);
    /* an operator's -maxhz lowers it further; EDID and -maxhz: the lower wins */
    CHECK_EQ_I(mt_ceiling(160, 75), 75);
    CHECK_EQ_I(mt_ceiling(76, 100), 76);
    CHECK_EQ_I(mt_ceiling(0, 0), 0);
}

TEST(max_needs_a_ceiling_no_edid_no_claim)
{
    int hz = -9;
    /* the gameres.h rule: with no EDID there is no measurement, so "max" is
     * refused rather than guessed - the old unclamped answer would be 85 */
    CHECK_EQ_I(mt_check_request(L124, N124, &REG124, 0, 0, 0, 1280, 960, 32, -1, &hz),
               MT_E_NO_CEILING);
    CHECK_EQ_I(hz, 0);
    /* an operator who read the tube's manual can supply one */
    CHECK_EQ_I(mt_check_request(L124, N124, &REG124, 0, 0, 75, 1280, 960, 32, -1, &hz),
               MT_OK);
    CHECK_EQ_I(hz, 75);
    /* with the EDID: the best listed under it */
    CHECK_EQ_I(mt_check_request(L124, N124, &REG124, 160, 0, 0, 1280, 960, 32, -1, &hz),
               MT_OK);
    CHECK_EQ_I(hz, 85);
    /* Win9x lists no rate at all: "max" finds nothing real, and says so */
    CHECK_EQ_I(mt_check_request(L243, N243, &REG243, 0, 0, 85, 800, 600, 8, -1, &hz),
               MT_E_NO_RATE);
}

TEST(the_horizontal_limit_refuses_what_the_vertical_one_allows)
{
    int hz = -9;
    /* .124's previous tube, the Sony CPD-G200: 96 kHz. 1600x1200@85 is inside
     * any 160 Hz vertical range and is ~106 kHz - out of range. The estimate
     * must refuse it and pick 75 (95.4 kHz by the 1.06 estimate, 93.75 real). */
    mt_mode_t reg = { 1600, 1200, 32, 75 };
    CHECK(!mt_hfreq_ok(1200, 85, 96), "1600x1200@85 must not fit a 96 kHz tube");
    CHECK(mt_hfreq_ok(1200, 75, 96), "1600x1200@75 fits a 96 kHz tube");
    CHECK(mt_hfreq_ok(1024, 85, 96), "1280x1024@85 (91 kHz) fits a 96 kHz tube");
    CHECK(mt_hfreq_ok(1200, 85, 0), "no horizontal limit known: nothing refused by it");
    CHECK_EQ_I(mt_check_request(L124, N124, &reg, 160, 96, 0, 1600, 1200, 32, -1, &hz),
               MT_OK);
    CHECK_EQ_I(hz, 75);
    /* the old, vertical-only answer would have been 85 */
    CHECK_EQ_I(mt_best_rate(L124, N124, 1600, 1200, 32, 160, 0), 85);
    CHECK_EQ_I(mt_check_request(L124, N124, &reg, 160, 96, 0, 1600, 1200, 32, 85, &hz),
               MT_E_RATE_OVER_CEILING);
}

TEST(the_screen_must_be_in_its_persisted_mode_before_a_test)
{
    /* Win9x reports 0 for the live rate (.243: refresh 0, registry 75): that
     * is "unknown", not "different" */
    mt_mode_t live243 = { 1024, 768, 8, 0 };
    mt_mode_t live124 = { 1280, 1024, 32, 85 };
    mt_mode_t game124 = { 1280, 1024, 32, 60 };    /* same size, a game's 60 Hz */
    mt_mode_t strand  = { 640, 480, 32, 60 };      /* a game left the desktop here */
    CHECK(mt_live_is_persisted(&live243, &REG243), "Win9x 0 Hz live = unknown, still ours");
    CHECK(mt_live_is_persisted(&live124, &REG124), "live == persisted");
    CHECK(!mt_live_is_persisted(&game124, &REG124), "a real, different rate = in use");
    CHECK(!mt_live_is_persisted(&strand, &REG124), "stranded at 640x480 = in use");
}

TEST(a_test_A_request_on_win98_passes_without_any_rate)
{
    int hz = -9;
    CHECK_EQ_I(mt_check_request(L243, N243, &REG243, 0, 0, 0, 800, 600, 8, 0, &hz), MT_OK);
    CHECK_EQ_I(hz, 0);
    /* and an explicit rate cannot be checked against a list of zeros */
    CHECK_EQ_I(mt_check_request(L243, N243, &REG243, 0, 0, 0, 800, 600, 8, 75, &hz),
               MT_E_RATE_NOT_LISTED);
}

TEST(vblank_median_turns_timestamps_into_a_refresh)
{
    /* QueryPerformanceFrequency 3579545 (the ACPI PM timer XP uses) */
    const double f = 3579545.0, per60 = f / 60.0, per85 = f / 85.0;
    double iv[40];
    int i;
    for (i = 0; i < 40; i++)
        iv[i] = per60 + ((i % 3) - 1) * 30.0;       /* +-30 ticks of jitter */
    CHECK_EQ_I(mt_mhz_to_hz(mt_vblank_mhz(iv, 40, f)), 60);

    /* a few MISSED blanks (double intervals) do not move the median */
    for (i = 0; i < 40; i++)
        iv[i] = (i % 8 == 0) ? 2.0 * per85 : per85;
    CHECK_EQ_I(mt_mhz_to_hz(mt_vblank_mhz(iv, 40, f)), 85);

    /* a WaitForVerticalBlank that returns at once is not a measurement */
    for (i = 0; i < 40; i++)
        iv[i] = 5.0 + i;
    CHECK_EQ_I(mt_vblank_mhz(iv, 40, f), 0);

    /* scattered samples (under 70% near the median) are refused */
    for (i = 0; i < 40; i++)
        iv[i] = per60 * (0.6 + 0.02 * i);
    CHECK_EQ_I(mt_vblank_mhz(iv, 40, f), 0);

    /* too few samples */
    for (i = 0; i < 9; i++)
        iv[i] = per60;
    CHECK_EQ_I(mt_vblank_mhz(iv, 9, f), 0);

    /* Win98's QueryPerformanceFrequency is the 8254 PIT, 1193182 Hz */
    for (i = 0; i < 20; i++)
        iv[i] = 1193182.0 / 75.0;
    CHECK_EQ_I(mt_mhz_to_hz(mt_vblank_mhz(iv, 20, 1193182.0)), 75);
}

TEST(a_reported_rate_that_disagrees_with_the_scanout_is_flagged)
{
    CHECK_EQ_I(mt_rate_agreement(60, 60012), 2);
    CHECK_EQ_I(mt_rate_agreement(85, 84980), 2);
    CHECK_EQ_I(mt_rate_agreement(85, 60010), -1);    /* reports 85, scans out 60 */
    CHECK_EQ_I(mt_rate_agreement(0, 75020), 1);      /* Win9x: only the scanout */
    CHECK_EQ_I(mt_rate_agreement(60, 0), 1);         /* no vblank support */
    CHECK_EQ_I(mt_rate_agreement(1, 0), 0);          /* a sentinel and nothing else */
}

TEST(every_refusal_names_its_reason)
{
    int r;
    for (r = MT_E_NOT_LISTED; r <= MT_E_LAST; r++)
        CHECK(mt_reason(r)[0] && mt_reason(r)[0] != 'u', "a refusal the log cannot name");
    CHECK(mt_reason(MT_E_NO_CEILING)[1] == 'm', "'max' needs a ceiling");
    CHECK(MT_E_LAST >= MT_E_NOBPP_DEPTH, "MT_E_LAST must name the last refusal");
}

/* Review 2026-09-29: -nobpp sends W|H and the OS fills in the PERSISTED depth,
 * so a BPP other than that one validated a mode the OS never sets. */
TEST(nobpp_is_only_allowed_at_the_persisted_depth)
{
    CHECK_EQ_I(mt_check_nobpp(0, 16, &REG124), MT_OK);          /* not -nobpp */
    CHECK_EQ_I(mt_check_nobpp(1, 32, &REG124), MT_OK);          /* = persisted 32 */
    CHECK_EQ_I(mt_check_nobpp(1, 16, &REG124), MT_E_NOBPP_DEPTH);
    CHECK_EQ_I(mt_check_nobpp(1, 8, &REG243), MT_OK);           /* .243 is 8 bpp */
    CHECK_EQ_I(mt_check_nobpp(1, 16, &REG243), MT_E_NOBPP_DEPTH);
}

/* Review 2026-09-29: the old single counter refused EVERY switch after
 * MT_MAX_SWITCHES + 2 - restores included - so a long run could end with
 * "RESTORE NOT ATTEMPTED", and on Win9x nothing reverts the mode at exit. */
TEST(a_give_back_is_never_refused_by_the_switch_count)
{
    CHECK(mt_switch_allowed(0, 0), "the first test switch");
    CHECK(mt_switch_allowed(MT_MAX_SWITCHES - 1, 0), "the last test switch");
    CHECK(!mt_switch_allowed(MT_MAX_SWITCHES, 0), "one test switch too many");
    CHECK(mt_switch_allowed(MT_MAX_SWITCHES, 1), "a restore after the limit");
    CHECK(mt_switch_allowed(MT_MAX_SWITCHES + 50, 1), "a restore long after it");
}

/* Review 2026-09-29: the watchdog and the final check used to restore whenever
 * the screen was off its persisted mode - including when THIS run had switched
 * nothing, i.e. a mode some game (the 1080p workflow's) had set mid-run. */
TEST(only_what_this_run_switched_is_given_back)
{
    mt_mode_t game = { 640, 480, 32, 60 }, home = { 1280, 1024, 32, 85 };
    mt_mode_t unread = { 0, 0, 0, 0 };
    CHECK(!mt_should_giveback(0, &game, &REG124), "switched nothing: not ours to undo");
    CHECK(mt_should_giveback(3, &game, &REG124), "switched and left off: give it back");
    CHECK(!mt_should_giveback(3, &home, &REG124), "switched and home again: nothing to do");
    CHECK(mt_should_giveback(1, &unread, &REG124), "an unreadable mode after a switch: restore");
    CHECK(!mt_should_giveback(0, &unread, &REG124), "an unreadable mode, no switch: hands off");
}

/* Review 2026-09-29: "DID NOT KEEP" was printed from the REQUESTED pre-switch
 * rate, so a driver that turned "@75" into 60 produced a verdict about a
 * pre-switch that never happened. */
TEST(a_keep_verdict_needs_a_pre_switch_that_landed)
{
    CHECK_EQ_I(mt_observed_hz(60, 74990), 75);      /* the scanout wins        */
    CHECK_EQ_I(mt_observed_hz(85, 0), 85);          /* else a real report      */
    CHECK_EQ_I(mt_observed_hz(0, 0), 0);            /* Win9x, no timing        */
    CHECK_EQ_I(mt_observed_hz(1, 0), 0);            /* a sentinel is not a rate */
    CHECK_EQ_I(mt_keep_verdict(75, 75, 75), MT_V_KEPT);
    CHECK_EQ_I(mt_keep_verdict(75, 75, 60), MT_V_NOT_KEPT);
    CHECK_EQ_I(mt_keep_verdict(75, 60, 60), MT_V_PRE_MISSED);   /* the old code: "DID NOT KEEP" */
    CHECK_EQ_I(mt_keep_verdict(75, 0, 60), MT_V_UNMEASURED);
    CHECK_EQ_I(mt_keep_verdict(75, 75, 0), MT_V_UNMEASURED);
    CHECK(mt_keep_verdict_name(MT_V_PRE_MISSED)[0] == 'I', "INVALID, said so");
}

TEST(a_run_plans_a_bounded_number_of_switches)
{
    CHECK_EQ_I(mt_plan_switches(MT_T_A), 2);
    CHECK_EQ_I(mt_plan_switches(MT_T_A | MT_T_B), 5);
    CHECK_EQ_I(mt_plan_switches(MT_T_A | MT_T_B | MT_T_C), 8);
    CHECK(mt_plan_switches(MT_T_A | MT_T_B | MT_T_C | MT_T_CB | MT_T_D) > MT_MAX_SWITCHES,
          "every test at once is over the per-run limit and must be refused");
    CHECK(MT_PACE_MIN_S >= 5, "the pace floor between switches is at least 5 s");
}

MUNIT_MAIN("modetest refresh-probe safety logic (tools/modetest/modetest_logic.h)", {
    RUN(a_mode_must_be_listed_with_its_depth);
    RUN(never_bigger_than_the_persisted_desktop);
    RUN(an_explicit_rate_must_be_listed_real_and_under_the_ceiling);
    RUN(max_needs_a_ceiling_no_edid_no_claim);
    RUN(the_horizontal_limit_refuses_what_the_vertical_one_allows);
    RUN(the_screen_must_be_in_its_persisted_mode_before_a_test);
    RUN(a_test_A_request_on_win98_passes_without_any_rate);
    RUN(vblank_median_turns_timestamps_into_a_refresh);
    RUN(a_reported_rate_that_disagrees_with_the_scanout_is_flagged);
    RUN(every_refusal_names_its_reason);
    RUN(a_run_plans_a_bounded_number_of_switches);
    RUN(nobpp_is_only_allowed_at_the_persisted_depth);
    RUN(a_give_back_is_never_refused_by_the_switch_count);
    RUN(only_what_this_run_switched_is_given_back);
    RUN(a_keep_verdict_needs_a_pre_switch_that_landed);
})
