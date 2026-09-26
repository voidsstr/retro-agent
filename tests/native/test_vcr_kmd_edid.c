/* test_vcr_kmd_edid.c
 *
 * The monitor's EDID and the mode filter it drives (voodoo-cleanroom/vcr-kmd/
 * common/vcr_edid.c, vcr_modes.c vcr_mode_check). Without it our driver
 * listed every timing to 85 Hz whatever was plugged in: the ICD asks for the
 * highest refresh listed, so a game ran 1600x1200 at 75 Hz (93.75 kHz) where
 * the vendor ran 60 - measured 4-5 % slower at fill-bound resolutions on the
 * V5 6000 (2026-09-26) - and a monitor whose range stops short of a listed
 * rate is driven out of range. Fixtures: the three monitors .124's registry
 * holds EDIDs for (serials blanked).
 */
#include <string.h>
#include "munit.h"
#include "../../voodoo-cleanroom/vcr-kmd/common/vcr_edid.c"
#include "../../voodoo-cleanroom/vcr-kmd/common/vcr_modes.c"

/* Dell E2414H (DEL4090): the EDID XP stored under Enum\\DISPLAY on .124, serial
 * numbers blanked and the checksum recomputed */
static const vcr_u8 k_dell_e2414h[128] = {
    0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00, 0x10, 0xac, 0x90, 0x40, 0x00, 0x00, 0x00, 0x00,
    0x17, 0x18, 0x01, 0x03, 0x0e, 0x35, 0x1e, 0x78, 0xee, 0x0e, 0xf5, 0xa5, 0x55, 0x50, 0x9e, 0x26,
    0x10, 0x50, 0x54, 0xa5, 0x4b, 0x00, 0x71, 0x4f, 0x81, 0x80, 0xa9, 0xc0, 0xd1, 0xc0, 0x01, 0x01,
    0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x02, 0x3a, 0x80, 0x18, 0x71, 0x38, 0x2d, 0x40, 0x58, 0x2c,
    0x45, 0x00, 0x13, 0x2b, 0x21, 0x00, 0x00, 0x1e, 0x00, 0x00, 0x00, 0xff, 0x00, 0x30, 0x0a, 0x20,
    0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x00, 0x00, 0x00, 0xfc, 0x00, 0x44,
    0x45, 0x4c, 0x4c, 0x20, 0x45, 0x32, 0x34, 0x31, 0x34, 0x48, 0x0a, 0x20, 0x00, 0x00, 0x00, 0xfd,
    0x00, 0x38, 0x4c, 0x1e, 0x53, 0x11, 0x00, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x00, 0xb3,
};

/* Gateway VX1120 (GWY0460): the EDID XP stored under Enum\\DISPLAY on .124, serial
 * numbers blanked and the checksum recomputed */
static const vcr_u8 k_gateway_vx1120[128] = {
    0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00, 0x1e, 0xf9, 0x60, 0x04, 0x00, 0x00, 0x00, 0x00,
    0x03, 0x0b, 0x01, 0x01, 0x0e, 0x28, 0x1e, 0x78, 0xe9, 0x04, 0x82, 0xa0, 0x57, 0x4a, 0x9b, 0x26,
    0x12, 0x48, 0x4f, 0xff, 0xff, 0x80, 0xe1, 0x4f, 0xa9, 0x4f, 0xc9, 0x4f, 0xc1, 0x4f, 0xa9, 0x59,
    0x81, 0x99, 0x61, 0x59, 0x45, 0x59, 0x04, 0x74, 0x80, 0xd0, 0x72, 0xa0, 0x3c, 0x50, 0x90, 0xe0,
    0x13, 0x00, 0x89, 0x27, 0x11, 0x00, 0x00, 0x1c, 0x00, 0x00, 0x00, 0xfd, 0x00, 0x32, 0xa0, 0x1e,
    0x79, 0x24, 0x00, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x00, 0x00, 0x00, 0xfc, 0x00, 0x47,
    0x61, 0x74, 0x65, 0x77, 0x61, 0x79, 0x56, 0x58, 0x31, 0x31, 0x32, 0x30, 0x00, 0x00, 0x00, 0xff,
    0x00, 0x30, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x00, 0x60,
};

/* Sony CPD-G200 (SNY1270): the EDID XP stored under Enum\\DISPLAY on .124, serial
 * numbers blanked and the checksum recomputed */
static const vcr_u8 k_sony_cpd_g200[128] = {
    0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00, 0x4d, 0xd9, 0x70, 0x12, 0x00, 0x00, 0x00, 0x00,
    0x33, 0x09, 0x01, 0x02, 0x0e, 0x21, 0x18, 0x96, 0xeb, 0x0c, 0xc9, 0xa0, 0x57, 0x47, 0x9b, 0x27,
    0x12, 0x48, 0x4c, 0xff, 0xff, 0x80, 0x31, 0x59, 0x45, 0x59, 0x61, 0x59, 0x71, 0x59, 0x81, 0x99,
    0x81, 0x4f, 0xa9, 0x4f, 0x01, 0x01, 0xea, 0x24, 0x00, 0x60, 0x41, 0x00, 0x28, 0x30, 0x30, 0x60,
    0x13, 0x00, 0x38, 0xea, 0x10, 0x00, 0x00, 0x1e, 0x00, 0x00, 0x00, 0xfd, 0x00, 0x30, 0x78, 0x1e,
    0x60, 0x1a, 0x00, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x00, 0x00, 0x00, 0xfc, 0x00, 0x43,
    0x50, 0x44, 0x2d, 0x47, 0x32, 0x30, 0x30, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x00, 0x00, 0x00, 0xff,
    0x00, 0x30, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x00, 0xd1,
};

static const vcr_hwcaps k_v5 = { VCR_DEV_VSA100, 350000, 262000, 261, 32u << 20, 0x118000, 0,
                                 0, 0, 0, 0, 0 };

static int offered(const vcr_hwcaps *h, unsigned w, unsigned hh, unsigned hz, unsigned bpp)
{
    vcr_mode m[400];
    vcr_u32 n = vcr_modes_build(h, m, 400), i;
    for (i = 0; i < n; i++) {
        const vcr_timing *t = &vcr_timings[m[i].timing];
        if (t->w == w && t->h == hh && t->refresh == hz && m[i].bpp == bpp)
            return 1;
    }
    return 0;
}

TEST(the_three_monitors_parse) {
    vcr_edid_info d, g, s;
    CHECK(vcr_edid_parse(k_dell_e2414h, 128, &d), "Dell");
    CHECK(vcr_edid_parse(k_gateway_vx1120, 128, &g), "Gateway");
    CHECK(vcr_edid_parse(k_sony_cpd_g200, 128, &s), "Sony");
    CHECK(!strcmp(d.pnpid, "DEL") && d.product == 0x4090, "DEL4090");
    CHECK(!strcmp(g.pnpid, "GWY") && g.product == 0x0460, "GWY0460");
    CHECK(!strcmp(s.pnpid, "SNY") && s.product == 0x1270, "SNY1270");
    CHECK(!strcmp(d.name, "DELL E2414H"), "Dell name");
    CHECK(!strcmp(g.name, "GatewayVX1120"), "Gateway name");
    CHECK(!strcmp(s.name, "CPD-G200"), "Sony name");
    /* range limits, as the 0xFD descriptors state them */
    CHECK(d.has_range && s.has_range && g.has_range, "every one declares ranges");
    CHECK_EQ_U(d.vmin_hz, 56); CHECK_EQ_U(d.vmax_hz, 76);
    CHECK_EQ_U(d.hmin_khz, 30); CHECK_EQ_U(d.hmax_khz, 83);
    CHECK_EQ_U(d.max_pixclk_khz, 170000);
    CHECK_EQ_U(s.vmin_hz, 48); CHECK_EQ_U(s.vmax_hz, 120);
    CHECK_EQ_U(s.hmin_khz, 30); CHECK_EQ_U(s.hmax_khz, 96);
    CHECK_EQ_U(s.max_pixclk_khz, 260000);
    CHECK_EQ_U(g.vmin_hz, 50); CHECK_EQ_U(g.vmax_hz, 160);
    CHECK_EQ_U(g.hmin_khz, 30); CHECK_EQ_U(g.hmax_khz, 121);
    CHECK_EQ_U(g.max_pixclk_khz, 360000);
    /* preferred timings: the Dell panel's 1080p, the Sony's 1024x768@85 */
    CHECK_EQ_U(d.pref_w, 1920); CHECK_EQ_U(d.pref_h, 1080);
    CHECK_EQ_U(d.pref_pixclk_khz, 148500);
    CHECK_EQ_U(s.pref_w, 1024); CHECK_EQ_U(s.pref_h, 768);
    CHECK_EQ_U(s.pref_pixclk_khz, 94500);
    CHECK(s.pref_refresh_mhz > 84500 && s.pref_refresh_mhz < 85500, "Sony prefers 85 Hz");
    CHECK(!d.digital && !s.digital && !g.digital, "all three on the analog input");
}

TEST(a_broken_edid_filters_nothing) {
    vcr_u8 bad[128];
    vcr_edid_info e;
    vcr_hwcaps h = k_v5;
    memcpy(bad, k_sony_cpd_g200, 128);
    bad[100] ^= 1;                          /* checksum no longer 0 */
    CHECK(!vcr_edid_parse(bad, 128, &e) && !e.valid, "checksum failure refused");
    CHECK(!vcr_hwcaps_set_monitor(&h, &e), "no limits from it");
    CHECK_EQ_U(h.mon_hmax_khz, 0);
    CHECK(!vcr_edid_parse(k_sony_cpd_g200, 64, &e), "short buffer refused");
    CHECK(offered(&h, 1600, 1200, 85, 16), "unfiltered: the pre-EDID list");
}

TEST(the_sony_keeps_what_its_96_khz_allow) {
    vcr_edid_info e;
    vcr_hwcaps h = k_v5;
    vcr_mode all[400], mon[400];
    vcr_u32 na = vcr_modes_build(&h, all, 400), nm, i;
    vcr_edid_parse(k_sony_cpd_g200, 128, &e);
    CHECK(vcr_hwcaps_set_monitor(&h, &e), "limits applied");
    nm = vcr_modes_build(&h, mon, 400);
    CHECK(nm > 60 && nm < na, "some modes, fewer than unfiltered");
    CHECK(offered(&h, 1600, 1200, 75, 16), "93.75 kHz <= 96");
    CHECK(!offered(&h, 1600, 1200, 85, 16), "106 kHz > 96");
    CHECK(offered(&h, 1280, 1024, 85, 32), "91.1 kHz");
    CHECK(offered(&h, 640, 480, 60, 8), "31.5 kHz");
    CHECK(offered(&h, 1024, 768, 85, 16), "its preferred mode");
    /* nothing kept is outside the ranges */
    for (i = 0; i < nm; i++) {
        const vcr_timing *t = &vcr_timings[mon[i].timing];
        CHECK(vcr_timing_hfreq_hz(t) <= 96500 && vcr_timing_hfreq_hz(t) + 500 >= 30000, "H range");
        CHECK(vcr_timing_vfreq_mhz(t) <= 120500 && vcr_timing_vfreq_mhz(t) + 500 >= 48000, "V range");
        CHECK(t->pixclk_khz <= 260000, "dot clock");
    }
}

TEST(the_dell_panel_stops_at_76_hz_and_170_mhz) {
    vcr_edid_info e;
    vcr_hwcaps h = k_v5;
    vcr_edid_parse(k_dell_e2414h, 128, &e);
    vcr_hwcaps_set_monitor(&h, &e);
    CHECK(offered(&h, 1280, 1024, 75, 16), "80 kHz, 75 Hz");
    CHECK(!offered(&h, 1280, 1024, 85, 16), "91 kHz is above 83");
    CHECK(offered(&h, 1024, 768, 75, 16), "60 kHz");
    CHECK(!offered(&h, 1024, 768, 85, 16), "85 Hz is above 76");
    CHECK(offered(&h, 1600, 1200, 60, 16), "156.6 MHz <= 170");
    CHECK(!offered(&h, 1600, 1200, 70, 16), "182.7 MHz > 170");
}

TEST(a_75_hz_timing_at_75_03_is_not_refused_by_rounding) {
    /* the slack: EDID stores whole Hz and kHz */
    vcr_hwcaps h = k_v5;
    int i = vcr_timing_find(1024, 768, 75);
    CHECK(i >= 0, "1024x768@75 exists");
    h.mon_hmin_khz = 30; h.mon_hmax_khz = vcr_timing_hfreq_hz(&vcr_timings[i]) / 1000;
    h.mon_vmin_hz = 50;  h.mon_vmax_hz = 75;
    CHECK_EQ_I(vcr_mode_check(&h, &vcr_timings[i], 16), 0);
    h.mon_vmax_hz = 74;
    CHECK_EQ_I(vcr_mode_check(&h, &vcr_timings[i], 16), VCR_MODE_E_MONITOR);
}

MUNIT_MAIN("vcr-kmd EDID + monitor mode filter", {
    RUN(the_three_monitors_parse);
    RUN(a_broken_edid_filters_nothing);
    RUN(the_sony_keeps_what_its_96_khz_allow);
    RUN(the_dell_panel_stops_at_76_hz_and_170_mhz);
    RUN(a_75_hz_timing_at_75_03_is_not_refused_by_rounding);
})
