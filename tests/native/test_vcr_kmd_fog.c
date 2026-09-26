/* test_vcr_kmd_fog.c
 *
 * The Voodoo fog table as the Direct3D HAL builds it (voodoo-cleanroom/
 * vcr-kmd/common/vcr_fog.c). The chip indexes the table by 1/W, entry i
 * standing for w = 2^(3+i/4)/(8-i%4) (Glide guFogTableIndexToW), so a table
 * built on a straight index would fog at the wrong distances; and a kernel
 * driver has no libm, so exp is our own and is checked here against math.h.
 */
#include <math.h>
#include "munit.h"
#include "../../voodoo-cleanroom/vcr-kmd/common/vcr_fog.c"

static int near(double a, double b, double tol) { return fabs(a - b) <= tol; }

TEST(index_to_w_is_glides_curve) {
    CHECK(near(vcr_fog_index_w(0), 1.0, 1e-6), "entry 0 = 1");
    CHECK(near(vcr_fog_index_w(3), 8.0 / 5.0, 1e-6), "entry 3 = 1.6");
    CHECK(near(vcr_fog_index_w(4), 2.0, 1e-6), "entry 4 = 2");
    CHECK(near(vcr_fog_index_w(63), 262144.0 / 5.0, 1e-2), "entry 63 = 52428.8");
}

TEST(our_exp_matches_libm) {
    float x;
    for (x = 0.0f; x > -20.0f; x -= 0.37f)
        CHECK(near(vcr_fog_exp(x), exp(x), 2e-6 + 2e-5 * exp(x)), "e^x");
    CHECK(vcr_fog_exp(-100.0f) == 0.0f, "underflow is 0");
}

TEST(linear_table_fogs_between_start_and_end) {
    unsigned char t[64];
    int i;
    vcr_fog_table(VCR_FOG_LINEAR, 2.0f, 8.0f, 0.0f, t);
    for (i = 0; i < 64; i++) {
        double w = vcr_fog_index_w(i), want = w <= 2.0 ? 0 : w >= 8.0 ? 255 : (w - 2.0) / 6.0 * 255.0;
        CHECK(fabs(t[i] - want) <= 1.0, "linear");
    }
    CHECK_EQ_U(t[0], 0u);           /* w = 1: in front of the fog */
    CHECK_EQ_U(t[63], 255u);        /* far: all fog */
}

TEST(exp_tables) {
    unsigned char t[64];
    vcr_fog_table(VCR_FOG_EXP, 0, 0, 0.1f, t);
    CHECK(fabs(t[8] - (1.0 - exp(-0.1 * 4.0)) * 255.0) <= 1.0, "exp at w=4");
    vcr_fog_table(VCR_FOG_EXP2, 0, 0, 0.1f, t);
    CHECK(fabs(t[8] - (1.0 - exp(-0.16)) * 255.0) <= 1.0, "exp2 at w=4");
}

TEST(registers_pack_two_entries_with_deltas) {
    unsigned char t[64];
    int i;
    for (i = 0; i < 64; i++)
        t[i] = (unsigned char)(i * 4);
    /* reg 0: e0=0 d0=(4-0)<<2=16, e1=4 d1=(8-4)<<2=16 */
    CHECK_EQ_U(vcr_fog_reg(t, 0), (4u << 24) | (16u << 16) | (0u << 8) | 16u);
    /* the last entry's delta is 0 */
    CHECK_EQ_U(vcr_fog_reg(t, 31) & 0x00ff0000u, 0u);
}

TEST(vertex_fog_lands_on_its_own_amount) {
    unsigned char t[64];
    int k;
    vcr_fog_table(VCR_FOG_RAMP, 0, 0, 0, t);
    for (k = 0; k <= 10; k++) {
        double a = k / 10.0, w = 1.0 / vcr_fog_ramp_oow((float)a);
        int i;
        /* the table entry the chip reads for that w */
        for (i = 0; i < 63 && vcr_fog_index_w(i + 1) <= w; i++)
            ;
        CHECK(fabs(t[i] - a * 255.0) <= 255.0 / 63.0 + 1.0, "ramp");
    }
}

MUNIT_MAIN("vcr-kmd fog table", {
    RUN(index_to_w_is_glides_curve);
    RUN(our_exp_matches_libm);
    RUN(linear_table_fogs_between_start_and_end);
    RUN(exp_tables);
    RUN(registers_pack_two_entries_with_deltas);
    RUN(vertex_fog_lands_on_its_own_amount);
})
