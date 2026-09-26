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
    h.twox_above_khz = 262000;      /* the vendor's rule (H5 h3modeset.c) */
    h.twox_htotal_chars = 261;
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
        vcr_u32 vt = (vcr_u32)t->h * ((t->flags & VCR_T_DBLSCAN) ? 2 : 1) +
                     t->vfp + t->vsync + t->vbp;       /* porches are physical lines */
        /* the nominal refresh is within 1.5 Hz of what the numbers give */
        vcr_u32 mhz = (vcr_u32)((unsigned long long)t->pixclk_khz * 1000000ull /
                                ((unsigned long long)ht * vt));
        CHECK(mhz + 1500 >= t->refresh * 1000u && mhz <= t->refresh * 1000u + 1500,
              "nominal refresh disagrees with the timing");
        CHECK(t->w % 8 == 0 && ht % 8 == 0, "horizontal values in whole characters");
    }
}

TEST(the_640x480_60_crtc_is_the_vendors_not_textbook_vga) {
    vcr_hwcaps h = v5();
    vcr_modeset m;
    CHECK_EQ_I(vcr_mode_compute(&h, T(640, 480, 60), 8, &m), 0);
    /* htotal 800 -> 100 chars -> CR00 = 95; display 80 chars -> CR01 = 79 */
    CHECK_EQ_U(m.crtc[0x00], 0x5f);
    CHECK_EQ_U(m.crtc[0x01], 0x4f);
    CHECK_EQ_U(m.crtc[0x03], 0x80 | (99 & 0x1f));
    /* Sync start/end ONE UNIT EARLY - hsync starts at pixel 656 (char 82) and
     * the vendor writes 81; vsync starts on line 490 and it writes 489. The
     * textbook VGA values (82, 0xea) were this test's first expectation and
     * the V5 6000 overruled them (golden capture). */
    CHECK_EQ_U(m.crtc[0x04], 656 / 8 - 1);
    CHECK_EQ_U(m.crtc[0x05], 0x80 | ((752 / 8 - 1) & 0x1f));
    CHECK_EQ_U(m.crtc[0x06], 0x0b);
    CHECK_EQ_U(m.crtc[0x10], 0xe9);
    CHECK_EQ_U(m.crtc[0x11], 0x20 | (491 & 0x0f));
    CHECK_EQ_U(m.crtc[0x12], 0xdf);
    CHECK_EQ_U(m.crtc[0x07], 0x3e);
    /* CR1A bit 5 is the vendor's blank-end bit 6: (100-80) + (79 & 63) = 35 -> 0 */
    CHECK_EQ_U(m.crtc_ext[0], 0);
    CHECK_EQ_U(m.crtc_ext[1], 0);
    CHECK_EQ_U(m.misc, 0xcf);           /* -h -v, clock 3, no page bit */
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
    CHECK_EQ_U(m.misc, 0x0f);       /* 1024x768@85 is +h +v */
}

TEST(two_x_mode_follows_the_vendor_rule) {
    vcr_hwcaps h = v5();
    vcr_modeset m;
    /* above 262 MHz at width >= 1280: 1920x1440@75 is 297 MHz */
    CHECK_EQ_I(vcr_mode_compute(&h, T(1920, 1440, 75), 16, &m), 0);
    CHECK_EQ_U(m.twox, 1);
    CHECK(m.dacmode & VCR_DAC_MODE_2X, "dacMode 2X");
    CHECK(m.vidproccfg & VCR_VPC_2X_MODE_EN, "vidProcCfg 2X");
    /* htotal 2640 halved = 1320 px = 165 chars -> CR00 160, CR01 = 960/8-1 */
    CHECK_EQ_U(m.crtc[0x00], 160);
    CHECK_EQ_U(m.crtc[0x01], 119);
    CHECK(m.pix_khz_actual > 295000 && m.pix_khz_actual < 299000, "the PLL runs the full dot clock");
    /* OR htotal above 261 characters (blank end is only 6 bits): 1920x1080@60
     * is only 148.5 MHz but 2200 px = 275 chars */
    vcr_mode_compute(&h, T(1920, 1080, 60), 16, &m);
    CHECK_EQ_U(m.twox, 1);
    /* the vendor's 1600x1200 sits at exactly 261 chars: 1X at every refresh */
    vcr_mode_compute(&h, T(1600, 1200, 85), 16, &m);
    CHECK_EQ_U(m.twox, 0);
    vcr_mode_compute(&h, T(1600, 1200, 60), 16, &m);
    CHECK_EQ_U(m.twox, 0);
    CHECK_EQ_U(m.crtc[0x00], 0x00);                 /* 261 - 5 = 256 -> 0 + CR1A b0 */
    CHECK_EQ_U(m.crtc_ext[0] & 1, 1);
    /* a Voodoo 3 switches above 160 MHz at width >= 1280 */
    h.device_id = VCR_DEV_VOODOO3;
    h.twox_above_khz = 160000;
    h.twox_htotal_chars = 0;
    vcr_mode_compute(&h, T(1600, 1200, 70), 16, &m);
    CHECK_EQ_U(m.twox, 1);
}

TEST(doublescan_modes_use_half_mode_and_double_the_lines) {
    vcr_hwcaps h = v5();
    vcr_modeset m;
    CHECK_EQ_I(vcr_mode_compute(&h, T(320, 240, 60), 16, &m), 0);
    CHECK(m.vidproccfg & VCR_VPC_HALF_MODE, "HALF mode");
    CHECK(m.crtc[0x09] & 0x80, "CR09 doublescan");
    CHECK_EQ_U(m.vidscreensize, 320u | (240u << 13));
    CHECK_EQ_U(m.crtc[0x12], (480 - 1) & 0xff);
    /* offered at 32 bpp too, programmed identically (vendor vidProcCfg
     * 0x010c0091 vs 0x01040091 at 16 bpp: only the pixel format differs) */
    CHECK_EQ_I(vcr_mode_compute(&h, T(320, 240, 60), 32, &m), 0);
    CHECK(m.vidproccfg & VCR_VPC_HALF_MODE, "HALF at 32 bpp");
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


/* EVERY CRTC byte, CR1A/CR1B and misc, as the vendor driver programs them on
 * the V5 6000 - read with vcrprobe.sys next to AmigaMerlin 3.1-R11 (golden/
 * amigamerlin-3.1-r11_192.168.1.124.json; tools/golden_compare.py reports
 * 51/51 modes identical). This is what settled: sync start/end one unit
 * early, the vendor's blank-end bit in CR1A, its 1600x1200 (htotal 261
 * chars) and low-resolution timings, misc without the page bit. */
static const struct {
    unsigned w, h, hz, bpp;
    unsigned char misc, cr1a, cr1b, crtc[25];
} k_vendor[] = {
        { 320, 200, 70, 8, 0x4f, 0x80, 0x00,
          { 0x2d, 0x27, 0x27, 0x91, 0x28, 0x8e, 0xbf, 0x1f, 0x00, 0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x9c, 0x2e, 0x8f, 0x28, 0x00, 0x8f, 0xc0, 0x80, 0xff } },  /* 320x200x8@70 */
        { 320, 240, 60, 16, 0xcf, 0x80, 0x00,
          { 0x2d, 0x27, 0x27, 0x91, 0x28, 0x8e, 0x0b, 0x3e, 0x00, 0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xe9, 0x2b, 0xdf, 0x28, 0x00, 0xdf, 0x0c, 0x80, 0xff } },  /* 320x240x16@60 */
        { 400, 300, 60, 16, 0x0f, 0xa0, 0x00,
          { 0x3d, 0x31, 0x31, 0x81, 0x34, 0x1c, 0x72, 0xf0, 0x00, 0xe0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x58, 0x2c, 0x57, 0x28, 0x00, 0x57, 0x73, 0x80, 0xff } },  /* 400x300x16@60 */
        { 512, 384, 60, 16, 0xcf, 0x20, 0x00,
          { 0x4f, 0x3f, 0x3f, 0x93, 0x41, 0x09, 0x24, 0xf5, 0x00, 0xe0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x28, 0xff, 0x28, 0x00, 0xff, 0x25, 0x80, 0xff } },  /* 512x384x16@60 */
        { 640, 400, 70, 16, 0x4f, 0x00, 0x00,
          { 0x5f, 0x4f, 0x4f, 0x83, 0x51, 0x9d, 0xbf, 0x1f, 0x00, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x9c, 0x2e, 0x8f, 0x28, 0x00, 0x8f, 0xc0, 0x80, 0xff } },  /* 640x400x16@70 */
        { 640, 480, 60, 16, 0xcf, 0x00, 0x00,
          { 0x5f, 0x4f, 0x4f, 0x83, 0x51, 0x9d, 0x0b, 0x3e, 0x00, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xe9, 0x2b, 0xdf, 0x28, 0x00, 0xdf, 0x0c, 0x80, 0xff } },  /* 640x480x16@60 */
        { 800, 600, 56, 8, 0x0f, 0x80, 0x00,
          { 0x7b, 0x63, 0x63, 0x9f, 0x66, 0x8f, 0x6f, 0xf0, 0x00, 0x60, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x58, 0x2a, 0x57, 0x28, 0x00, 0x57, 0x70, 0x80, 0xff } },  /* 800x600x8@56 */
        { 800, 600, 60, 16, 0x0f, 0xa0, 0x00,
          { 0x7f, 0x63, 0x63, 0x83, 0x68, 0x18, 0x72, 0xf0, 0x00, 0x60, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x58, 0x2c, 0x57, 0x28, 0x00, 0x57, 0x73, 0x80, 0xff } },  /* 800x600x16@60 */
        { 1024, 768, 85, 16, 0x0f, 0x20, 0x00,
          { 0xa7, 0x7f, 0x7f, 0x8b, 0x85, 0x91, 0x26, 0xf5, 0x00, 0x60, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x23, 0xff, 0x28, 0x00, 0xff, 0x27, 0x80, 0xff } },  /* 1024x768x16@85 */
        { 1152, 864, 75, 16, 0x0f, 0xa0, 0x00,
          { 0xc3, 0x8f, 0x8f, 0x87, 0x97, 0x07, 0x82, 0xff, 0x00, 0x60, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x60, 0x23, 0x5f, 0x28, 0x00, 0x5f, 0x83, 0x80, 0xff } },  /* 1152x864x16@75 */
        { 1280, 1024, 85, 16, 0x0f, 0xa0, 0x41,
          { 0xd3, 0x9f, 0x9f, 0x97, 0xa7, 0x1b, 0x2e, 0x5a, 0x00, 0x60, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x23, 0xff, 0x28, 0x00, 0xff, 0x2f, 0x80, 0xff } },  /* 1280x1024x16@85 */
        { 1600, 1200, 60, 16, 0x0f, 0xa1, 0x55,
          { 0x00, 0xc7, 0xc7, 0x84, 0xcf, 0x07, 0xe0, 0x10, 0x00, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xb0, 0x23, 0xaf, 0x28, 0x00, 0xaf, 0xe1, 0x80, 0xff } },  /* 1600x1200x16@60 */
        { 1600, 1200, 70, 16, 0x0f, 0xa1, 0x55,
          { 0x00, 0xc7, 0xc7, 0x84, 0xcf, 0x07, 0xe0, 0x10, 0x00, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xb0, 0x23, 0xaf, 0x28, 0x00, 0xaf, 0xe1, 0x80, 0xff } },  /* 1600x1200x16@70 */
};

TEST(golden_vendor_crtc_byte_for_byte) {
    vcr_hwcaps h = v5();
    unsigned i, k;
    for (i = 0; i < sizeof k_vendor / sizeof k_vendor[0]; i++) {
        vcr_modeset m;
        int rc = vcr_mode_compute(&h, T(k_vendor[i].w, k_vendor[i].h, k_vendor[i].hz),
                                  k_vendor[i].bpp, &m);
        CHECK_EQ_I(rc, 0);
        if (rc)
            continue;
        CHECK_EQ_U(m.misc, k_vendor[i].misc);
        CHECK_EQ_U(m.crtc_ext[0], k_vendor[i].cr1a);
        CHECK_EQ_U(m.crtc_ext[1], k_vendor[i].cr1b);
        for (k = 0; k < 25; k++) {
            if (m.crtc[k] != k_vendor[i].crtc[k]) {
                fprintf(stderr, "    %ux%u@%u CR%02x ours %02x vendor %02x\n", k_vendor[i].w,
                        k_vendor[i].h, k_vendor[i].hz, k, m.crtc[k], k_vendor[i].crtc[k]);
                munit_fails++;
            }
        }
    }
}


/* The miniport keeps its mode list in a fixed array (vcrmp.h VCR_MAX_MODES,
 * 400). It once held 200 and the table outgrew it: the VM mode sweep passed
 * "200/200" while ~10 modes had been dropped. The largest list any supported
 * chip can produce must fit with room to spare. */
TEST(the_mode_list_fits_the_miniport_array) {
    vcr_hwcaps h = v5();
    static vcr_mode list[1024];
    vcr_u32 n;
    h.fb_bytes = 64u << 20;                 /* more memory than any VSA-100 has */
    h.max_pixclk_khz = 1000000;
    n = vcr_modes_build(&h, list, 1024);
    CHECK(n <= 400 - 40, "mode table within the miniport's VCR_MAX_MODES with headroom");
    CHECK_EQ_U(n, vcr_ntimings * 3);        /* every timing at every depth */
}


/* The desktop's place in video memory: the TOP, as the vendor driver puts it
 * (golden vidDesktopStartAddr: 1280x1024x32 0x01b00000, 640x480x16
 * 0x01f60000 for its tiled desktop; its linear 640x480x8 0x01fb5000). It
 * first sat at 1 MB, inside Glide's command FIFO (96 KB .. ~1116 KB): the
 * repaint after a game's mode switch corrupted the command stream and hung
 * the engine on the V5 6000. */
TEST(the_desktop_sits_at_the_top_clear_of_glides_fifo) {
    const vcr_u32 fb = 32u << 20;
    CHECK_EQ_U(vcr_desktop_offset(fb, 1280 * 4, 1024), 0x01b00000);
    CHECK_EQ_U(vcr_desktop_offset(fb, 640, 480), 0x01fb5000);
    CHECK_EQ_U(vcr_desktop_offset(fb, 640 * 2, 480), 0x01f6a000);  /* linear: 4 KB, not 64 KB */
    /* the largest mode we offer still leaves Glide's FIFO alone */
    CHECK(vcr_desktop_offset(fb, 1920 * 4, 1440) >= 96u * 1024 + 0xff000, "above the FIFO");
    CHECK_EQ_U(vcr_desktop_offset(fb, 8192 * 4, 2048), 0);        /* does not fit */
}

MUNIT_MAIN("vcr-kmd modes", {
    RUN(the_desktop_sits_at_the_top_clear_of_glides_fifo);
    RUN(the_mode_list_fits_the_miniport_array);
    RUN(golden_vendor_crtc_byte_for_byte);
    RUN(golden_vendor_capture_agrees);
    RUN(pll_formula_and_register_packing);
    RUN(every_timing_gets_a_pll_within_half_a_percent);
    RUN(the_timings_table_is_self_consistent);
    RUN(the_640x480_60_crtc_is_the_vendors_not_textbook_vga);
    RUN(depth_selects_format_stride_and_clut_bank);
    RUN(two_x_mode_follows_the_vendor_rule);
    RUN(doublescan_modes_use_half_mode_and_double_the_lines);
    RUN(limits_refuse_what_the_hardware_cannot_show);
    RUN(the_mode_list_and_lookup);
})
