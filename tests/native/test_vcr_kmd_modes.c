/* test_vcr_kmd_modes.c
 *
 * The mode-set recipe of the vcr-kmd kernel driver
 * (voodoo-cleanroom/vcr-kmd/common/vcr_modes.c): timings table, pllCtrl0
 * search and the VGA/3dfx CRTC values. A wrong number here is a monitor that
 * will not sync - or a box that will not come back - so every property the
 * miniport relies on is pinned before the code runs on the Voodoo.
 */
#include <string.h>
#include "munit.h"
#include "../../voodoo-cleanroom/vcr-kmd/common/vcr_modes.c"

static vcr_hwcaps v5(void)
{
    vcr_hwcaps h;
    memset(&h, 0, sizeof h);
    h.device_id = VCR_DEV_VSA100;
    h.max_pixclk_khz = 350000;
    h.twox_above_khz = 262000;      /* the vendor's threshold (golden: no 2X at 189 MHz) */
    h.fb_bytes = 32u << 20;         /* .124: 128 MB board = 32 MB per chip */
    h.fb_reserved = 64u << 10;
    return h;
}

static const vcr_timing *T(unsigned w, unsigned h, unsigned hz)
{
    int i = vcr_timing_find(w, h, hz);
    return i < 0 ? NULL : &vcr_timings[i];
}

TEST(pll_formula_and_register_packing) {
    /* f = 14.31818 MHz * (N+2) / ((M+2) * 2^K) */
    CHECK_EQ_U(vcr_pll_khz(VCR_PLL(0, 0, 0)), 14318);
    CHECK_EQ_U(vcr_pll_khz(VCR_PLL(2, 0, 0)), 28636);
    CHECK_EQ_U(vcr_pll_khz(VCR_PLL(2, 0, 1)), 14318);
    CHECK_EQ_U(VCR_PLL(0x12, 0x3, 0x1), 0x120d);
}

TEST(every_timing_gets_a_pll_within_half_a_percent) {
    vcr_u32 i;
    for (i = 0; i < vcr_ntimings; i++) {
        vcr_u32 got, want = vcr_timings[i].pixclk_khz;
        vcr_u32 reg = vcr_pll_calc(want, &got);
        vcr_u32 err = got > want ? got - want : want - got;
        CHECK(err * 200 <= want, "pll error > 0.5%");
        CHECK_EQ_U(vcr_pll_khz(reg), got);
    }
}

TEST(the_timings_table_is_self_consistent) {
    vcr_u32 i;
    for (i = 0; i < vcr_ntimings; i++) {
        const vcr_timing *t = &vcr_timings[i];
        vcr_u32 ht = (vcr_u32)t->w + t->hfp + t->hsync + t->hbp;
        vcr_u32 vt = ((vcr_u32)t->h + t->vfp + t->vsync + t->vbp) *
                     ((t->flags & VCR_T_DBLSCAN) ? 2 : 1);
        /* the nominal refresh is within 1.5 Hz of what the numbers give */
        vcr_u32 mhz = (vcr_u32)((unsigned long long)t->pixclk_khz * 1000000ull /
                                ((unsigned long long)ht * vt));
        CHECK(mhz + 1500 >= t->refresh * 1000u && mhz <= t->refresh * 1000u + 1500,
              "nominal refresh disagrees with the timing");
        CHECK(t->w % 8 == 0 && ht % 8 == 0, "horizontal values in whole characters");
    }
}

TEST(vga_640x480_60_matches_the_standard_crtc_values) {
    vcr_hwcaps h = v5();
    vcr_modeset m;
    CHECK_EQ_I(vcr_mode_compute(&h, T(640, 480, 60), 8, &m), 0);
    /* htotal 800 -> 100 chars -> CR00 = 95; display 80 chars -> CR01 = 79 */
    CHECK_EQ_U(m.crtc[0x00], 0x5f);
    CHECK_EQ_U(m.crtc[0x01], 0x4f);
    CHECK_EQ_U(m.crtc[0x02], 0x4f);
    CHECK_EQ_U(m.crtc[0x03], 0x80 | (99 & 0x1f));
    CHECK_EQ_U(m.crtc[0x04], 656 / 8);          /* sync starts at pixel 656 */
    CHECK_EQ_U(m.crtc[0x05], 0x80 | ((752 / 8) & 0x1f));
    /* vtotal 525 -> CR06 = 523 & 0xff; vsync starts on line 490 */
    CHECK_EQ_U(m.crtc[0x06], 0x0b);
    CHECK_EQ_U(m.crtc[0x10], 0xea);
    CHECK_EQ_U(m.crtc[0x11], 0x20 | (492 & 0x0f));
    CHECK_EQ_U(m.crtc[0x12], 0xdf);             /* 479 */
    CHECK_EQ_U(m.crtc[0x15], 0xdf);
    CHECK_EQ_U(m.crtc[0x16], 524 & 0xff);
    /* overflow, the standard VGA 640x480 value: vt 523 has bit 9 (b5), vd
     * 479 / vs 490 / vbs 479 have bit 8 (b1, b2, b3), line compare b4 */
    CHECK_EQ_U(m.crtc[0x07], 0x3e);
    /* CR1A b5 = hblank end bit 6: hbe = 99 */
    CHECK_EQ_U(m.crtc_ext[0], 0x20);
    CHECK_EQ_U(m.crtc_ext[1], 0);
    /* 640x480 DMT is -hsync -vsync; clock select 3 = the PLL */
    CHECK_EQ_U(m.misc, 0xef);
    CHECK_EQ_U(m.vidscreensize, 640u | (480u << 12));
    CHECK_EQ_U(m.stride, 640);
    CHECK_EQ_U(m.twox, 0);
    CHECK_EQ_U(m.vidproccfg & VCR_VPC_DESKTOP_FMT_MASK, 0);
    CHECK_EQ_U(m.vidproccfg & VCR_VPC_DESKTOP_CLUT_SELECT, 0);   /* bank 0 */
    CHECK(m.refresh_mhz > 59500 && m.refresh_mhz < 60500, "~60 Hz");
}

TEST(depth_selects_format_stride_and_clut_bank) {
    vcr_hwcaps h = v5();
    vcr_modeset m;
    vcr_mode_compute(&h, T(1024, 768, 85), 16, &m);
    CHECK_EQ_U((m.vidproccfg >> VCR_VPC_DESKTOP_FMT_SHIFT) & 7, VCR_VPC_FMT_RGB565);
    CHECK_EQ_U(m.stride, 2048);
    CHECK_EQ_U(m.vidproccfg & (VCR_VPC_DESKTOP_CLUT_SELECT | VCR_VPC_DESKTOP_CLUT_BYPASS), 0);
    CHECK(m.vidproccfg & VCR_VPC_VIDEO_PROCESSOR_EN, "processor on");
    CHECK(m.vidproccfg & VCR_VPC_DESKTOP_EN, "desktop on");
    vcr_mode_compute(&h, T(1024, 768, 85), 32, &m);
    CHECK_EQ_U((m.vidproccfg >> VCR_VPC_DESKTOP_FMT_SHIFT) & 7, VCR_VPC_FMT_RGB32);
    CHECK_EQ_U(m.stride, 4096);
    CHECK_EQ_U(m.misc, 0x2f);       /* 1024x768@85 is +h +v */
}

TEST(high_dot_clocks_use_2x_mode_with_halved_horizontal_timing) {
    vcr_hwcaps h = v5();
    vcr_modeset m;
    /* the tdfxfb threshold (max/2 = 175 MHz) - the vendor's is higher, below */
    h.twox_above_khz = 175000;
    /* 1600x1200@85: 229.5 MHz > 175 MHz threshold */
    CHECK_EQ_I(vcr_mode_compute(&h, T(1600, 1200, 85), 16, &m), 0);
    CHECK_EQ_U(m.twox, 1);
    CHECK(m.dacmode & VCR_DAC_MODE_2X, "dacMode 2X");
    CHECK(m.vidproccfg & VCR_VPC_2X_MODE_EN, "vidProcCfg 2X");
    /* htotal 2160 halved = 1080 px = 135 chars -> CR00 130, CR01 = 800/8-1 */
    CHECK_EQ_U(m.crtc[0x00], 130);
    CHECK_EQ_U(m.crtc[0x01], 99);
    /* 1200 lines: vd = 1199 -> bit 10 set in CR1B b2; vt = 1248 -> b0 */
    CHECK_EQ_U(m.crtc_ext[1] & 0x05, 0x05);
    CHECK_EQ_U(m.vidscreensize, 1600u | (1200u << 12));
    /* the PLL still runs at the full dot clock */
    CHECK(m.pix_khz_actual > 228000 && m.pix_khz_actual < 231000, "full dot clock");
    /* 1600x1200@60 (162 MHz) stays in 1X */
    vcr_mode_compute(&h, T(1600, 1200, 60), 16, &m);
    CHECK_EQ_U(m.twox, 0);
}

TEST(doublescan_modes_use_half_mode_and_double_the_lines) {
    vcr_hwcaps h = v5();
    vcr_modeset m;
    CHECK_EQ_I(vcr_mode_compute(&h, T(320, 240, 60), 16, &m), 0);
    CHECK(m.vidproccfg & VCR_VPC_HALF_MODE, "HALF mode");
    CHECK(m.crtc[0x09] & 0x80, "CR09 doublescan");
    CHECK_EQ_U(m.vidscreensize, 320u | (240u << 13));
    CHECK_EQ_U(m.crtc[0x12], (480 - 1) & 0xff);
    /* not offered at 32 bpp (the vendor driver only halves at <= 16 bpp) */
    CHECK_EQ_I(vcr_mode_compute(&h, T(320, 240, 60), 32, &m), VCR_MODE_E_BPP);
}

TEST(limits_refuse_what_the_hardware_cannot_show) {
    vcr_hwcaps h = v5();
    vcr_modeset m;
    CHECK_EQ_I(vcr_mode_compute(&h, T(640, 480, 60), 24, &m), VCR_MODE_E_BPP);
    h.max_pixclk_khz = 270000;              /* a Banshee RAMDAC */
    CHECK_EQ_I(vcr_mode_check(&h, T(1920, 1440, 75), 16), VCR_MODE_E_PIXCLK);
    CHECK_EQ_I(vcr_mode_check(&h, T(1920, 1440, 60), 16), 0);
    h = v5();
    h.fb_bytes = 8u << 20;                  /* an 8 MB board */
    CHECK_EQ_I(vcr_mode_check(&h, T(1920, 1440, 60), 32), VCR_MODE_E_MEMORY);
    CHECK_EQ_I(vcr_mode_check(&h, T(1920, 1440, 60), 16), 0);
}

TEST(the_mode_list_and_lookup) {
    vcr_hwcaps h = v5();
    vcr_mode list[256];
    vcr_u32 n = vcr_modes_build(&h, list, 256), i, got8 = 0, got32 = 0;
    CHECK(n > 60, "a full list on a 32 MB chip");
    for (i = 0; i < n; i++) {
        const vcr_timing *t = &vcr_timings[list[i].timing];
        if (t->w == 1600 && t->h == 1200 && list[i].bpp == 8) got8 = 1;
        if (t->w == 1600 && t->h == 1200 && list[i].bpp == 32) got32 = 1;
        CHECK(!(t->flags & VCR_T_DBLSCAN) || list[i].bpp <= 16, "no 32 bpp doublescan");
    }
    /* Glide asks DirectDraw for 8 bpp at its own resolution (win_mode.c) */
    CHECK(got8 && got32, "1600x1200 at 8 and 32 bpp");
    /* default refresh = lowest listed; an exact request wins */
    CHECK_EQ_U(vcr_timings[vcr_timing_find(1024, 768, 0)].refresh, 60);
    CHECK_EQ_U(vcr_timings[vcr_timing_find(1024, 768, 85)].refresh, 85);
    CHECK_EQ_I(vcr_timing_find(1024, 768, 99), -1);
    CHECK_EQ_I(vcr_timing_find(1234, 567, 0), -1);
}

/* What the vendor driver (AmigaMerlin 3.1-R11) programs on the V5 6000,
 * captured by tools/golden_capture.py through its own HWCEXT mapping on .124
 * (golden/amigamerlin-3.1-r11_192.168.1.124.json). Where it uses the DMT
 * timing, our PLL must land on the same frequency, and our register choices
 * must match its. */
TEST(golden_vendor_capture_agrees) {
    vcr_hwcaps h = v5();
    vcr_modeset m;
    static const struct { unsigned w, hh, hz; unsigned vendor_pll; } g[] = {
        { 640, 480, 60, 0xd137 },   /* 25.18 MHz */
        { 800, 600, 60, 0xf427 },   /* 40.03 */
        { 1024, 768, 60, 0x6b07 },  /* 65.03 */
        { 1024, 768, 85, 0x820e },  /* 94.50 */
        { 320, 240, 60, 0xd173 },   /* 12.59, doublescanned */
    };
    unsigned i;
    for (i = 0; i < sizeof g / sizeof g[0]; i++) {
        unsigned want = vcr_pll_khz(g[i].vendor_pll);
        CHECK_EQ_I(vcr_mode_compute(&h, T(g[i].w, g[i].hh, g[i].hz), 16, &m), 0);
        CHECK(m.pix_khz_actual + 30 >= want && m.pix_khz_actual <= want + 30,
              "PLL lands where the vendor's does");
        CHECK_EQ_U(m.dacmode, 0);
    }
    /* 320x240: HALF mode and a doubled height field (vendor 0x001e0140) */
    vcr_mode_compute(&h, T(320, 240, 60), 16, &m);
    CHECK_EQ_U(m.vidscreensize, 0x001e0140);
    CHECK(m.vidproccfg & VCR_VPC_HALF_MODE, "HALF");
    /* 16 bpp desktop, processor on, bank 0 (vendor 0x09040081 minus its
     * tiled-desktop and hardware-cursor bits, which we do not use yet) */
    vcr_mode_compute(&h, T(1024, 768, 85), 16, &m);
    CHECK_EQ_U(m.vidproccfg, 0x09040081u & ~(VCR_VPC_DESKTOP_TILED_EN | VCR_VPC_CURSOR_EN));
    vcr_mode_compute(&h, T(1024, 768, 60), 32, &m);
    CHECK_EQ_U(m.vidproccfg, 0x090c0081u & ~(VCR_VPC_DESKTOP_TILED_EN | VCR_VPC_CURSOR_EN));
    /* no 2X at 1600x1200@70 (189 MHz): vendor dacMode 0 */
    vcr_mode_compute(&h, T(1600, 1200, 70), 16, &m);
    CHECK_EQ_U(m.twox, 0);
    CHECK_EQ_U(m.vgainit0_set, 0x1140);
}

MUNIT_MAIN("vcr-kmd modes", {
    RUN(golden_vendor_capture_agrees);
    RUN(pll_formula_and_register_packing);
    RUN(every_timing_gets_a_pll_within_half_a_percent);
    RUN(the_timings_table_is_self_consistent);
    RUN(vga_640x480_60_matches_the_standard_crtc_values);
    RUN(depth_selects_format_stride_and_clut_bank);
    RUN(high_dot_clocks_use_2x_mode_with_halved_horizontal_timing);
    RUN(doublescan_modes_use_half_mode_and_double_the_lines);
    RUN(limits_refuse_what_the_hardware_cannot_show);
    RUN(the_mode_list_and_lookup);
})
