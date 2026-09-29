/* voodoo-cleanroom/vcr-kmd/include/vcr_clock.h - the VSA-100 core clock set
 * live (IOCTL_VCR_CLOCK, miniport/vcrmp_clock.c VcrCoreClock, 2026-09-29).
 *
 * Measured on .124 (V5 6000, chip 0, as its VBIOS left it): pllCtrl1 0xE721 =
 * 166.8 MHz, pllCtrl0 0x4005 = 157.5 MHz (1280x1024@85), pllCtrl2 0xBF01 =
 * 691 MHz by the same formula - not a memory clock. Before this nothing in
 * our stack could change the clock at all: Glide's SSTH3_GRXCLOCK path is
 * compiled out of the Windows build and the kernel never wrote pllCtrl1 (the
 * "old" answer below: no word). */
#include "munit.h"
#include "../../voodoo-cleanroom/vcr-kmd/include/vcr_clock.h"

static vcr_u32 old_pll(vcr_u32 khz) { (void)khz; return 0; }

TEST(the_boards_own_words_decode_to_what_they_are)
{
    CHECK_EQ_U(vcr_clock_pll_khz(0xE721), 166806u);    /* the VBIOS core clock */
    CHECK_EQ_U(vcr_clock_pll_khz(0x4005), 157499u);    /* the 1280x1024@85 dot clock */
    CHECK(vcr_clock_pll_khz(0xBF01) > 600000u, "pllCtrl2 is no memory clock");
}

TEST(a_core_clock_word_is_k1_and_within_a_mhz)
{
    static const vcr_u32 mhz[] = { 120, 133, 143, 150, 155, 160, 166, 170, 175,
                                   180, 183, 190, 200, 210, 219 };
    unsigned i;
    for (i = 0; i < sizeof mhz / sizeof mhz[0]; i++) {
        vcr_u32 got = 0, w = vcr_clock_pll(mhz[i] * 1000u, &got);
        vcr_u32 err = got > mhz[i] * 1000u ? got - mhz[i] * 1000u : mhz[i] * 1000u - got;
        vcr_u32 m = (w >> 2) & 0x3f;
        CHECK(w != 0, "a word for every clock in range");
        CHECK_EQ_U(w & 3u, 1u);                         /* K = 1: VCO = 2 f */
        CHECK(m >= 1 && m <= 10, "M 1..10 (never 0)");
        CHECK(err <= 800u, "within 0.8 MHz of the target");
        /* the vendor's own table runs K = 1 from 51 to 219 MHz: VCO 100-440 */
        CHECK(2u * got >= 100000u && 2u * got <= 440000u, "VCO inside the vendor's range");
    }
    CHECK_EQ_U(old_pll(166000u), 0u);                   /* before: no way to ask */
}

TEST(the_stock_clock_comes_back_to_the_boards_value)
{
    vcr_u32 got = 0, w = vcr_clock_pll(166806u, &got);
    CHECK_EQ_U(got, 166806u);
    CHECK_EQ_U(w, 0xE721u);          /* exactly the VBIOS word: M 8 was the finest fit */
}

TEST(out_of_range_is_refused_not_clamped)
{
    vcr_u32 got = 123;
    CHECK_EQ_U(vcr_clock_pll(119999u, &got), 0u);
    CHECK_EQ_U(got, 0u);
    CHECK_EQ_U(vcr_clock_pll(219001u, &got), 0u);    /* above the vendor's ceiling */
    CHECK_EQ_U(vcr_clock_pll(0u, NULL), 0u);
    CHECK(vcr_clock_in_range(120000u) && vcr_clock_in_range(219000u), "the ends are in");
}

TEST(a_ramp_never_moves_more_than_a_step)
{
    vcr_u32 s[VCR_CLOCK_MAX_STEPS];
    int i, n = vcr_clock_plan(166000u, 183000u, s, VCR_CLOCK_MAX_STEPS);
    CHECK_EQ_I(n, 4);                                   /* 171, 176, 181, 183 */
    CHECK_EQ_U(s[0], 171000u);
    CHECK_EQ_U(s[n - 1], 183000u);
    for (i = 1; i < n; i++)
        CHECK(s[i] - s[i - 1] <= VCR_CLOCK_STEP_KHZ, "each step <= 5 MHz");
    n = vcr_clock_plan(183000u, 150000u, s, VCR_CLOCK_MAX_STEPS);
    CHECK_EQ_U(s[n - 1], 150000u);
    CHECK_EQ_U(s[0], 178000u);                          /* down as gently as up */
    CHECK_EQ_I(vcr_clock_plan(166000u, 166000u, s, VCR_CLOCK_MAX_STEPS), 0);
    CHECK_EQ_I(vcr_clock_plan(120000u, 219000u, s, 3), -1);   /* never a silent cut */
}

MUNIT_MAIN("vcr_clock (the VSA-100 core clock, set live)",
    RUN(the_boards_own_words_decode_to_what_they_are);
    RUN(a_core_clock_word_is_k1_and_within_a_mhz);
    RUN(the_stock_clock_comes_back_to_the_boards_value);
    RUN(out_of_range_is_refused_not_clamped);
    RUN(a_ramp_never_moves_more_than_a_step);
)
