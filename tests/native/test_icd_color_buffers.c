/* test_icd_color_buffers.c
 *
 * Guards how MY open-source ICD (voodoo-cleanroom MesaFX, fork retro3dfx-gl,
 * src/mesa/drivers/glide/fxapi.c -> rgl_color_buffers_for()) picks the number
 * of colour buffers it opens the board with (grSstWinOpen[Ext] nColBuffers).
 *
 * 0.1.81 opened THREE whenever the swaps waited for the retrace
 * (FX_GLIDE_SWAPINTERVAL >= 1), to stop an 85 Hz CRT dropping to 42.5 fps when
 * a frame misses a refresh. Measured on .124 (V5 6000, Quake III demo four,
 * 1280x960x32, 2026-10-02) it bought nothing: vsync on 55.9 fps with two
 * buffers, 56.0 with three (vsync off 65.0 / 64.7) - the swap still holds the
 * command stream until the retrace. 0.1.82 keeps two by default and opens
 * three only on an explicit RETROGL_COLOR_BUFFERS=3. This mirrors the
 * function exactly (tests/python/test_icd_color_buffers_source.py checks the
 * fork still matches) and asserts both the shipped and the withdrawn rule.
 */
#include "munit.h"
#include <stdlib.h>

/* EXACT mirror of fxapi.c rgl_color_buffers_for() as of 0.1.82 */
static int rgl_color_buffers_for(const char *cb, const char *si)
{
    (void)si;
    if (cb && (cb[0] == '2' || cb[0] == '3') && cb[1] == '\0')
        return cb[0] - '0';
    return 2;
}

/* 0.1.81, withdrawn: three under vsync */
static int rgl_color_buffers_0181(const char *cb, const char *si)
{
    if (cb && (cb[0] == '2' || cb[0] == '3') && cb[1] == '\0')
        return cb[0] - '0';
    return (si && atoi(si) >= 1) ? 3 : 2;
}

TEST(default_is_two_buffers_with_or_without_vsync) {
    CHECK_EQ_U(rgl_color_buffers_for(NULL, NULL), 2);
    CHECK_EQ_U(rgl_color_buffers_for(NULL, "0"), 2);
    CHECK_EQ_U(rgl_color_buffers_for(NULL, "1"), 2);
    CHECK(rgl_color_buffers_0181(NULL, "1") == 3,
          "0.1.81 opened three under vsync - measured no gain, withdrawn");
}

TEST(explicit_override_still_opens_three_or_two) {
    CHECK_EQ_U(rgl_color_buffers_for("3", NULL), 3);
    CHECK_EQ_U(rgl_color_buffers_for("3", "0"), 3);
    CHECK_EQ_U(rgl_color_buffers_for("2", "1"), 2);
}

TEST(junk_override_is_ignored) {
    CHECK_EQ_U(rgl_color_buffers_for("4", "1"), 2);
    CHECK_EQ_U(rgl_color_buffers_for("33", NULL), 2);
    CHECK_EQ_U(rgl_color_buffers_for("", NULL), 2);
    CHECK_EQ_U(rgl_color_buffers_for("x", NULL), 2);
}

MUNIT_MAIN("MesaFX colour-buffer count (ICD 0.1.81 -> 0.1.82)", {
    RUN(default_is_two_buffers_with_or_without_vsync);
    RUN(explicit_override_still_opens_three_or_two);
    RUN(junk_override_is_ignored);
})
