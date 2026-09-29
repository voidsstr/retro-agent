/* test_monpower.c - TRUE-SOURCE: compiles the REAL agent/shared/monpower.h,
 * the decisions behind agent/src/monpower.c (agent 1.96.0: the monitor never
 * powers off; the screensaver is what an idle box shows).
 *
 * THE OLD BEHAVIOUR (<= 1.95.x): nothing touched power management. .243's
 * active scheme (read off the box 2026-09-29, below) turned the monitor off
 * after 900 s and put the system to sleep after 1200 s; .184 (XP) switched the
 * monitor off after 20 minutes - under the 10-minute Starfield screensaver.
 */
#include "munit.h"
#include <string.h>

#include "../../agent/shared/monpower.h"

/* HKCU\Control Panel\PowerCfg\PowerPolicies\0\Policies on .243 (Win98 SE),
 * the USER_POWER_POLICY of "Home/Office Desk", verbatim. */
static const unsigned char upp_243[] = {
    0x01,0x00,0x00,0x00, 0x02,0x00,0x00,0x00, 0x01,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00, 0x02,0x00,0x00,0x00, 0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00, 0xB0,0x04,0x00,0x00, 0x3C,0x00,0x00,0x00,
    0x32,0x32,0x00,0x00, 0x04,0x00,0x00,0x00, 0x05,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x84,0x03,0x00,0x00,
    0x78,0x00,0x00,0x00, 0x08,0x07,0x00,0x00, 0x58,0x02,0x00,0x00,
    0x01,0x01,0x64,0x50, 0x64,0x64,0x00,0x00
};

TEST(the_243_scheme_decodes_to_what_the_box_did)
{
    CHECK(sizeof(upp_243) >= MP_UPP_MIN_SIZE, "blob shorter than the fields");
    CHECK_EQ_I((int)mp_le32(upp_243 + MP_UPP_VIDEO_AC), 900);   /* monitor off at 15 min */
    CHECK_EQ_I((int)mp_le32(upp_243 + MP_UPP_VIDEO_DC), 120);
    CHECK_EQ_I((int)mp_le32(upp_243 + MP_UPP_IDLE_AC), 1200);   /* standby at 20 min */
    CHECK_EQ_I((int)mp_le32(upp_243 + MP_UPP_IDLE_DC), 60);
}

TEST(an_unsettled_scheme_is_zeroed_and_a_settled_one_is_left_alone)
{
    unsigned long v[6];
    unsigned long *f[6];
    int i;
    v[0] = mp_le32(upp_243 + MP_UPP_VIDEO_AC);
    v[1] = mp_le32(upp_243 + MP_UPP_VIDEO_DC);
    v[2] = mp_le32(upp_243 + MP_UPP_IDLE_AC);
    v[3] = mp_le32(upp_243 + MP_UPP_IDLE_DC);
    v[4] = 0; v[5] = 0;                 /* hibernate: never, already */
    for (i = 0; i < 6; i++) f[i] = &v[i];
    /* old behaviour: the scheme is left as it was, the monitor sleeps */
    CHECK_EQ_I(mp_all_never(v, 6), 0);
    /* fixed: exactly the four non-zero timeouts are written... */
    CHECK_EQ_I(mp_zero_fields(f, 6), 4);
    CHECK_EQ_I(mp_all_never(v, 6), 1);
    /* ...and on the next start nothing is written at all */
    CHECK_EQ_I(mp_zero_fields(f, 6), 0);
}

TEST(an_unreadable_value_is_never_written)
{
    CHECK_EQ_I(mp_needs_write(1, 1200), 1);
    CHECK_EQ_I(mp_needs_write(1, 0), 0);
    CHECK_EQ_I(mp_needs_write(0, 1200), 0);
    CHECK_EQ_I(mp_needs_write(0, 0), 0);
}

TEST(the_api_follows_the_windows)
{
    /* Win98: old API (9x reports major 4 and is not NT) */
    CHECK_EQ_I(mp_pick_api(4, 0, 0, 1), MP_API_OLD);
    /* XP: old API, and it never has the new one */
    CHECK_EQ_I(mp_pick_api(5, 1, 0, 1), MP_API_OLD);
    /* Windows 7: the GUID API even though the XP shims are still exported */
    CHECK_EQ_I(mp_pick_api(6, 1, 1, 1), MP_API_NEW);
    /* an NT6 box without the new API falls back rather than giving up */
    CHECK_EQ_I(mp_pick_api(6, 1, 0, 1), MP_API_OLD);
    /* no powrprof at all */
    CHECK_EQ_I(mp_pick_api(4, 0, 0, 0), MP_API_NONE);
}

TEST(spi_flags_are_cleared_only_where_they_exist_and_are_on)
{
    CHECK_EQ_I(mp_spi_should_clear(4, 0, 1, 1), 1);    /* Win98, on */
    CHECK_EQ_I(mp_spi_should_clear(5, 1, 1, 1), 1);    /* XP, on */
    CHECK_EQ_I(mp_spi_should_clear(5, 1, 1, 0), 0);    /* XP, already off */
    CHECK_EQ_I(mp_spi_should_clear(5, 1, 0, 1), 0);    /* GET failed: not "on" */
    CHECK_EQ_I(mp_spi_should_clear(6, 1, 1, 1), 0);    /* Vista+: no such flag */
}

TEST(the_off_switch)
{
    CHECK_EQ_I(mp_enabled(0, 0), 1);                   /* absent = enforce */
    CHECK_EQ_I(mp_enabled(1, 1), 1);
    CHECK_EQ_I(mp_enabled(1, 0), 0);                   /* MonitorNeverSleep=0 */
}

TEST(the_vista_guids_are_the_documented_ones)
{
    static const mp_guid_t v = MP_GUID_VIDEOIDLE, sub = MP_GUID_VIDEO_SUBGROUP;
    static const mp_guid_t s = MP_GUID_STANDBYIDLE, h = MP_GUID_HIBERNATEIDLE;
    CHECK(v.d1 == 0x3c0bc021UL && v.d2 == 0xc8a8 && v.d4[7] == 0x7e, "VIDEOIDLE");
    CHECK(sub.d1 == 0x7516b95fUL && sub.d4[0] == 0x8c, "VIDEO subgroup");
    CHECK(s.d1 == 0x29f6c1dbUL && s.d4[7] == 0xda, "STANDBYIDLE");
    CHECK(h.d1 == 0x9d7815a6UL && h.d4[7] == 0x64, "HIBERNATEIDLE");
}

MUNIT_MAIN("monitor never sleeps (agent/shared/monpower.h, agent 1.96.0)",
    RUN(the_243_scheme_decodes_to_what_the_box_did);
    RUN(an_unsettled_scheme_is_zeroed_and_a_settled_one_is_left_alone);
    RUN(an_unreadable_value_is_never_written);
    RUN(the_api_follows_the_windows);
    RUN(spi_flags_are_cleared_only_where_they_exist_and_are_on);
    RUN(the_off_switch);
    RUN(the_vista_guids_are_the_documented_ones);
)
