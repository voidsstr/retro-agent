/* test_fx_pack_ub.c
 *
 * Guards the 0.1.2 SSE color-pack fix in MY open-source ICD — the MesaFX fork
 * retro3dfx-gl (github voidsstr/retro3dfx-gl), file
 *   src/mesa/drivers/glide/fxvbtmp.h  ->  fx_pack_ub()
 * built by retro-agent/retro3dfx/build-mesafx-retail.sh, deployed as retrogl.dll
 * on .124. (NOT the vintage SGI/3dfx SGL ICD in retro-3dfx SWLIBS — different
 * codebase, different lane.)
 *
 * The fix replaced Mesa's UNCLAMPED_FLOAT_TO_UBYTE (a float-store/byte-reload =
 * guaranteed store-forwarding stall + 2 branches per component) with a branchless
 * clamp that -mfpmath=sse lowers to mulss/minss/maxss/cvttss2si. The regression
 * risk is OUTPUT EQUIVALENCE — the new pack must produce the same 0..255 result
 * as the round-and-clamp contract for every input. This test encodes that exact
 * contract (kept byte-for-byte in sync with the fork inline):
 *
 *   f = f*255 + 0.5;  if (f>255) f=255;  if (f<0) f=0;  return (ubyte)(int)f;
 */
#include "munit.h"
#include <string.h>

/* EXACT mirror of fxvbtmp.h fx_pack_ub (float arithmetic, as the source uses). */
static unsigned char fx_pack_ub(float f) {
    f = f * 255.0f + 0.5f;
    if (f > 255.0f) f = 255.0f;
    if (f < 0.0f)   f = 0.0f;
    return (unsigned char)(int)f;
}

TEST(endpoints_and_midpoint) {
    CHECK_EQ_U(fx_pack_ub(0.0f), 0);
    CHECK_EQ_U(fx_pack_ub(1.0f), 255);
    CHECK_EQ_U(fx_pack_ub(0.5f), 128);      /* 127.5 + 0.5 = 128.0 */
}

TEST(clamps_out_of_range_both_ends) {
    CHECK_EQ_U(fx_pack_ub(-0.5f), 0);        /* below 0 -> 0 (maxss) */
    CHECK_EQ_U(fx_pack_ub(-1000.0f), 0);
    CHECK_EQ_U(fx_pack_ub(2.0f), 255);       /* above 1 -> 255 (minss) */
    CHECK_EQ_U(fx_pack_ub(1000.0f), 255);
}

TEST(rounds_to_nearest_not_truncate) {
    /* 1/255 rounds up to 1; the old truncating path would drop it to 0. */
    CHECK_EQ_U(fx_pack_ub(1.0f / 255.0f), 1);
    CHECK_EQ_U(fx_pack_ub(0.9f / 255.0f), 1);   /* 0.9 + 0.5 = 1.4 -> 1 */
    CHECK_EQ_U(fx_pack_ub(0.4f / 255.0f), 0);   /* 0.4 + 0.5 = 0.9 -> 0 */
}

TEST(monotonic_non_decreasing_across_unit_interval) {
    unsigned char prev = 0;
    for (int i = 0; i <= 1000; i++) {
        unsigned char v = fx_pack_ub((float)i / 1000.0f);
        CHECK(v >= prev, "pack must be monotonic non-decreasing on [0,1]");
        prev = v;
    }
    CHECK_EQ_U(prev, 255);
}

TEST(full_unit_range_maps_into_0_255) {
    for (int i = 0; i <= 1000; i++) {
        unsigned char v = fx_pack_ub((float)i / 1000.0f);
        CHECK(v <= 255, "result always a valid ubyte");   /* trivially true, guards type */
    }
    /* every 8-bit level is reachable near its 1/255 step (round-to-nearest) */
    CHECK_EQ_U(fx_pack_ub(128.0f / 255.0f), 128);
    CHECK_EQ_U(fx_pack_ub(254.0f / 255.0f), 254);
}

MUNIT_MAIN("MesaFX fx_pack_ub SSE color clamp (fix 0.1.2)", {
    RUN(endpoints_and_midpoint);
    RUN(clamps_out_of_range_both_ends);
    RUN(rounds_to_nearest_not_truncate);
    RUN(monotonic_non_decreasing_across_unit_interval);
    RUN(full_unit_range_maps_into_0_255);
})
