/* test_vcr_kmd_ics307.c
 *
 * The Voodoo 5 6000's external clock (voodoo-cleanroom/vcr-kmd/common/
 * vcr_ics307.c): the vendor driver programs it for every analog 4-chip SLI
 * session (measured: the HiNT bridge GPIO moves 0x111101 -> 0x222201), and
 * without it the four chips' scanout is not locked. Our integer search must
 * choose exactly what Glide GPL's gpio_6k_clock() chooses with doubles - the
 * reference is re-implemented here in floating point and compared over every
 * pixel clock the driver can set - and every choice must obey the part's
 * limits (VCO 55-400 MHz, Fref/(R+2) > 200 kHz).
 */
#include <math.h>
#include <string.h>
#include "munit.h"
#include "../../voodoo-cleanroom/vcr-kmd/common/vcr_ics307.c"
#include "../../voodoo-cleanroom/vcr-kmd/common/vcr_modes.c"

/* gpio_6k_clock()'s search, verbatim in structure, floating point */
static int ref_calc(double ic, unsigned *b_vdw, unsigned *b_rdw, unsigned *b_od)
{
    static const unsigned OD[] = { 2, 3, 4, 5, 6, 7, 8, 10 };
    double d_diff = 500.0e6;
    unsigned i, rdw, vdw, found = 0;
    for (i = 0; i < 8; i++)
        for (rdw = 1; rdw < 128; rdw++)
            for (vdw = 4; vdw < 512; vdw++) {
                double clk1 = 14318180.0 * 2.0 * (vdw + 8.0) / ((rdw + 2.0) * OD[i]);
                if (fabs(clk1 - ic) < d_diff) {
                    double part = 14318180.0 * 2.0 * (vdw + 8.0) / (rdw + 2.0);
                    if (part > 55e6 && part < 400e6 && 14318180 / (rdw + 2) > 200000) {
                        d_diff = fabs(clk1 - ic);
                        found = 1;
                        *b_vdw = vdw;
                        *b_rdw = rdw;
                        *b_od = OD[i];
                    }
                }
            }
    return found;
}

TEST(word_layout_is_the_datasheets) {
    vcr_ics307 c;
    CHECK(vcr_ics307_calc(39375000u, &c), "39.375 MHz is in range");
    /* byte 0: C1 C0 TTL F1 F0 S2 S1 S0 with TTL = 1 */
    CHECK_EQ_U(c.byte[0] & 0xf8, 0x20);
    CHECK_EQ_U(c.byte[0] & 7, c.s);
    CHECK_EQ_U(((unsigned)c.byte[1] << 1) | (c.byte[2] >> 7), c.vdw);
    CHECK_EQ_U(c.byte[2] & 0x7f, c.rdw);
    CHECK_EQ_U(c.s, c.od == 2 ? 1 : c.od == 3 ? 6 : c.od == 4 ? 3 : c.od == 5 ? 4 :
                    c.od == 6 ? 7 : c.od == 7 ? 5 : c.od == 8 ? 2 : 0);
}

TEST(the_target_is_the_masters_pixel_clock_over_4) {
    /* the vendor's 640x480@75 SLI session ran pllCtrl0 0x560f = 31.5 MHz */
    vcr_u32 t = vcr_ics307_target_from_pll(0x560f);
    CHECK(t > 7870000 && t < 7880000, "31.5 MHz / 4");
    /* 1600x1200@60: 0xad19 = 156.6 MHz */
    t = vcr_ics307_target_from_pll(0xad19);
    CHECK(t > 39140000 && t < 39160000, "156.6 MHz / 4");
}

TEST(every_driver_clock_matches_glides_choice_and_the_parts_limits) {
    vcr_hwcaps h = { VCR_DEV_VSA100, 350000, 262000, 261, 32u << 20, 0x118000, 0 };
    unsigned i, mismatches = 0, n = 0;
    for (i = 0; i < vcr_ntimings; i++) {
        vcr_modeset m;
        vcr_ics307 c;
        unsigned rv = 0, rr = 0, ro = 0;
        double ic;
        if (vcr_mode_compute(&h, &vcr_timings[i], 16, &m))
            continue;
        ic = (double)vcr_ics307_target_from_pll(m.pllctrl0);
        n++;
        CHECK(vcr_ics307_calc((vcr_u32)ic, &c), "a setting exists");
        CHECK(ref_calc(ic, &rv, &rr, &ro), "the reference finds one too");
        if (c.vdw != rv || c.rdw != rr || c.od != ro) {
            mismatches++;
            fprintf(stderr, "    %ux%u@%u: ours V%u R%u OD%u, Glide V%u R%u OD%u\n",
                    vcr_timings[i].w, vcr_timings[i].h, vcr_timings[i].refresh,
                    c.vdw, c.rdw, c.od, rv, rr, ro);
        }
        {
            double vco = 2.0 * 14318180.0 * (c.vdw + 8.0) / (c.rdw + 2.0);
            CHECK(vco > 55e6 && vco < 400e6, "VCO within 55-400 MHz");
            CHECK(14318180 / (c.rdw + 2) > 200000, "reference divider limit");
            /* the part bottoms out at ~5.5 MHz: doublescan low-res modes
             * (target 3-5 MHz) cannot be followed - by any driver - and
             * must be flagged so SLI refuses them rather than mis-clock */
            if (ic >= 5.6e6)
                CHECK(vcr_ics307_accurate(&c), "within 0.5 % where the part can reach");
            else
                CHECK(!vcr_ics307_accurate(&c) && c.actual_hz > 5.4e6,
                      "below the part's floor: flagged, clamped to its minimum");
        }
    }
    CHECK(n > 60, "every timing tried");
    CHECK_EQ_U(mismatches, 0);
}

/* ---- the wire: a model of the bridge GPIO with an ICS307 behind it --------
 * Input bits follow the pin level (the output value while the output is
 * enabled, the pull-up otherwise); the part shifts DATA in on each rising
 * SCLK and latches its register on a rising STRB. `hold` keeps SCLK low for
 * that many reads after it is raised (clock stretching); ~0u = forever. */
typedef struct gpio_model {
    vcr_u32 reg, shift, nbits, latched, nlatch, hold, reads_low, stalls;
} gpio_model;

static vcr_u32 pin(vcr_u32 reg, int in_bit)
{
    vcr_u32 oe = 1u << (in_bit + 1), out = 1u << (in_bit + 2);
    return (reg & oe) ? ((reg & out) ? 1 : 0) : 1;
}

static vcr_u32 g_rd(void *ctx)
{
    gpio_model *g = ctx;
    vcr_u32 v = g->reg & ~((1u << 8) | (1u << 12) | (1u << 16) | (1u << 20));
    int b;
    for (b = 8; b <= 20; b += 4)
        v |= pin(g->reg, b) << b;
    if (g->reads_low && (g->reg & (1u << 18))) {
        v &= ~(1u << 16);           /* the part holds SCLK low */
        if (g->reads_low != ~0u)
            g->reads_low--;
    }
    return v;
}

static void g_wr(void *ctx, vcr_u32 v)
{
    gpio_model *g = ctx;
    vcr_u32 old = g->reg;
    g->reg = v & ~((1u << 8) | (1u << 12) | (1u << 16) | (1u << 20));
    if (!pin(old, 16) && pin(g->reg, 16)) {         /* SCLK rising */
        g->shift = (g->shift << 1) | pin(g->reg, 8);
        g->nbits++;
        g->reads_low = g->hold;
    }
    if (!pin(old, 12) && pin(g->reg, 12)) {         /* STRB rising */
        g->latched = g->shift & 0xffffffu;
        g->nlatch++;
    }
}

static void g_stall(void *ctx, vcr_u32 us)
{
    ((gpio_model *)ctx)->stalls += us;
}

static vcr_gpio_io gio(gpio_model *g, vcr_u32 idle)
{
    vcr_gpio_io io = { g, g_rd, g_wr, g_stall };
    memset(g, 0, sizeof *g);
    g->reg = idle;
    return io;
}

TEST(the_word_reaches_the_part_msb_first_and_is_latched_once) {
    gpio_model g;
    vcr_gpio_io io = gio(&g, 0x00111101u);
    vcr_ics307 c;
    vcr_u32 before, after, stretch;
    CHECK(vcr_ics307_calc(vcr_ics307_target_from_pll(0xad19), &c), "1600x1200 clock");
    stretch = vcr_ics307_send(&io, &c, &before, &after);
    CHECK_EQ_U(g.nlatch, 1);
    CHECK_EQ_U(g.nbits, 24);
    CHECK_EQ_U(g.latched, ((vcr_u32)c.byte[0] << 16) | ((vcr_u32)c.byte[1] << 8) | c.byte[2]);
    CHECK_EQ_U(stretch, 0);
    CHECK_EQ_U(before, 0x00111101u);
    CHECK(g.stalls >= 24 * 3 * VCR_ICS307_DTIME_US, "every edge is spaced by DTIME");
}

TEST(after_programming_the_gpio_reads_what_the_vendor_driver_leaves) {
    /* .124, 640x480@75 4-chip SLI (pllCtrl0 0x560f): bridge 0xC4 0x00111101
     * -> 0x00222201 under AmigaMerlin 3.1-R11 AND under vcr-kmd (golden
     * sli_*_cfg5 captures): outputs enabled, every line driven low, the
     * untouched bits (0xff0000ff) as they were */
    gpio_model g;
    vcr_gpio_io io = gio(&g, 0x00111101u);
    vcr_ics307 c;
    vcr_u32 after;
    CHECK(vcr_ics307_calc(vcr_ics307_target_from_pll(0x560f), &c), "640x480@75 clock");
    CHECK_EQ_U(c.byte[2] & 1, 0);           /* the last bit sent: DATA ends low */
    vcr_ics307_send(&io, &c, NULL, &after);
    CHECK_EQ_U(after, 0x00222201u);
    g.reg = 0xa5111101u;                    /* bits outside the GPIO field survive */
    vcr_ics307_send(&io, &c, NULL, &after);
    CHECK_EQ_U(after & VCR_ICS307_KEEP, 0xa5000001u);
}

TEST(a_stretched_clock_is_waited_for_and_a_stuck_one_cannot_hang) {
    gpio_model g;
    vcr_gpio_io io = gio(&g, 0x00111101u);
    vcr_ics307 c;
    CHECK(vcr_ics307_calc(39375000u, &c), "a clock");
    g.hold = 3;
    CHECK_EQ_U(vcr_ics307_send(&io, &c, NULL, NULL), 24 * 3);
    CHECK_EQ_U(g.latched, ((vcr_u32)c.byte[0] << 16) | ((vcr_u32)c.byte[1] << 8) | c.byte[2]);
    io = gio(&g, 0x00111101u);
    g.hold = ~0u;                           /* SCLK never comes back */
    CHECK_EQ_U(vcr_ics307_send(&io, &c, NULL, NULL), 24 * VCR_ICS307_STRETCH_MAX);
    CHECK_EQ_U(g.nlatch, 1);                /* bounded, and it still finishes */
}

MUNIT_MAIN("vcr-kmd V5 6000 clock (ICS307)", {
    RUN(word_layout_is_the_datasheets);
    RUN(the_target_is_the_masters_pixel_clock_over_4);
    RUN(every_driver_clock_matches_glides_choice_and_the_parts_limits);
    RUN(the_word_reaches_the_part_msb_first_and_is_latched_once);
    RUN(after_programming_the_gpio_reads_what_the_vendor_driver_leaves);
    RUN(a_stretched_clock_is_waited_for_and_a_stuck_one_cannot_hang);
})
