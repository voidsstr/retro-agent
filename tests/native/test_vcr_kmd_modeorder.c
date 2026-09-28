/* test_vcr_kmd_modeorder.c - TRUE-SOURCE test of
 * voodoo-cleanroom/vcr-kmd/include/vcr_modeorder.h, the order
 * display/vcrdd.c DrvGetModes lists the modes in (2026-09-28).
 *
 * GLQuake keeps the first 30 entries of its mode list (gl_vidnt.c
 * MAX_MODE_LIST; 3 are its windowed modes), filled from EnumDisplaySettings in
 * the driver's order, >= 15 bpp, duplicates skipped. With the depth-major
 * order .124 listed (below, read with vcrctl's `modes` on 2026-09-28: the P1120
 * EDID-gated list, 179 modes) 1280x960x32 was the 31st distinct mode, and the
 * fleet's "Quake" shortcut (-width 1280 -height 960 -bpp 32) died with
 * "Specified video mode not available". What this proves, on that list:
 *   - the new order is a permutation (every mode listed exactly once);
 *   - every standard (4:3 / 5:4) resolution the list has at 16 and at 32 bpp
 *     is inside GLQuake's window;
 *   - the old order was not (1280x960x32 outside), so the test can fail;
 *   - the tier rule itself on a few known shapes.
 */
#include <string.h>
#include "munit.h"
#include "../../voodoo-cleanroom/vcr-kmd/include/vcr_modeorder.h"

typedef struct { unsigned w, h, bpp, hz; } md;
static const md k_124[] = {
    {320,200,8,70}, {320,240,8,60}, {400,300,8,60}, {512,384,8,60}, {640,400,8,70}, {640,480,8,60},
    {640,480,8,72}, {640,480,8,75}, {640,480,8,85}, {800,600,8,56}, {800,600,8,60}, {800,600,8,72},
    {800,600,8,75}, {800,600,8,85}, {1024,768,8,60}, {1024,768,8,70}, {1024,768,8,75}, {1024,768,8,85},
    {1152,864,8,75}, {1280,960,8,60}, {1280,960,8,85}, {1280,1024,8,60}, {1280,1024,8,75}, {1280,1024,8,85},
    {1600,1200,8,60}, {1600,1200,8,65}, {1600,1200,8,70}, {1600,1200,8,75}, {1600,1200,8,85}, {320,200,8,85},
    {320,240,8,72}, {320,240,8,75}, {320,240,8,85}, {400,300,8,72}, {400,300,8,75}, {400,300,8,85},
    {512,384,8,70}, {512,384,8,75}, {512,384,8,85}, {640,400,8,85}, {720,480,8,60}, {720,480,8,72},
    {720,480,8,85}, {720,576,8,60}, {720,576,8,72}, {960,720,8,60}, {960,720,8,75}, {1152,864,8,60},
    {1152,864,8,70}, {1152,864,8,85}, {1280,960,8,75}, {1600,1024,8,60}, {1600,1024,8,76}, {1600,1024,8,85},
    {1280,720,8,60}, {1280,800,8,60}, {1360,768,8,60}, {1440,900,8,60}, {1680,1050,8,60}, {320,200,16,70},
    {320,240,16,60}, {400,300,16,60}, {512,384,16,60}, {640,400,16,70}, {640,480,16,60}, {640,480,16,72},
    {640,480,16,75}, {640,480,16,85}, {800,600,16,56}, {800,600,16,60}, {800,600,16,72}, {800,600,16,75},
    {800,600,16,85}, {1024,768,16,60}, {1024,768,16,70}, {1024,768,16,75}, {1024,768,16,85}, {1152,864,16,75},
    {1280,960,16,60}, {1280,960,16,85}, {1280,1024,16,60}, {1280,1024,16,75}, {1280,1024,16,85}, {1600,1200,16,60},
    {1600,1200,16,65}, {1600,1200,16,70}, {1600,1200,16,75}, {1600,1200,16,85}, {320,200,16,85}, {320,240,16,72},
    {320,240,16,75}, {320,240,16,85}, {400,300,16,72}, {400,300,16,75}, {400,300,16,85}, {512,384,16,70},
    {512,384,16,75}, {512,384,16,85}, {640,400,16,85}, {720,480,16,60}, {720,480,16,72}, {720,480,16,85},
    {720,576,16,60}, {720,576,16,72}, {960,720,16,60}, {960,720,16,75}, {1152,864,16,60}, {1152,864,16,70},
    {1152,864,16,85}, {1280,960,16,75}, {1600,1024,16,60}, {1600,1024,16,76}, {1600,1024,16,85}, {1280,720,16,60},
    {1280,800,16,60}, {1360,768,16,60}, {1440,900,16,60}, {1680,1050,16,60}, {320,200,32,70}, {320,240,32,60},
    {400,300,32,60}, {512,384,32,60}, {640,400,32,70}, {640,480,32,60}, {640,480,32,72}, {640,480,32,75},
    {640,480,32,85}, {800,600,32,56}, {800,600,32,60}, {800,600,32,72}, {800,600,32,75}, {800,600,32,85},
    {1024,768,32,60}, {1024,768,32,70}, {1024,768,32,75}, {1024,768,32,85}, {1152,864,32,75}, {1280,960,32,60},
    {1280,960,32,85}, {1280,1024,32,60}, {1280,1024,32,75}, {1280,1024,32,85}, {1600,1200,32,60}, {1600,1200,32,65},
    {1600,1200,32,70}, {1600,1200,32,75}, {1600,1200,32,85}, {320,200,32,85}, {320,240,32,72}, {320,240,32,75},
    {320,240,32,85}, {400,300,32,72}, {400,300,32,75}, {400,300,32,85}, {512,384,32,70}, {512,384,32,75},
    {512,384,32,85}, {640,400,32,85}, {720,480,32,60}, {720,480,32,72}, {720,480,32,85}, {720,576,32,60},
    {720,576,32,72}, {960,720,32,60}, {960,720,32,75}, {1152,864,32,60}, {1152,864,32,70}, {1152,864,32,85},
    {1280,960,32,75}, {1600,1024,32,60}, {1600,1024,32,76}, {1600,1024,32,85}, {1280,720,32,60}, {1280,800,32,60},
    {1360,768,32,60}, {1440,900,32,60}, {1680,1050,32,60}, {640,480,4,1}, {800,600,4,1}
};
#define N124 ((int)(sizeof k_124 / sizeof k_124[0]))

/* the listing order DrvGetModes produces */
static int listed(int *out)
{
    unsigned r;
    int i, o = 0;
    for (r = 0; r < VCR_MODE_RANKS; r++)
        for (i = 0; i < N124; i++)
            if (vcr_mode_rank(k_124[i].w, k_124[i].h, k_124[i].bpp) == r)
                out[o++] = i;
    return o;
}

/* GLQuake's VID_InitFullDIB over a listing: the distinct >= 15 bpp modes it keeps */
static int glquake_window(const int *order, int n, md *keep)
{
    int k = 0, i, j;
    for (i = 0; i < n && k < 30 - 3; i++) {
        const md *m = &k_124[order[i]];
        int dup = 0;
        if (m->bpp < 15)
            continue;
        for (j = 0; j < k; j++)
            if (keep[j].w == m->w && keep[j].h == m->h && keep[j].bpp == m->bpp)
                dup = 1;
        if (!dup)
            keep[k++] = *m;
    }
    return k;
}

static int in_window(const md *keep, int k, unsigned w, unsigned h, unsigned bpp)
{
    int j;
    for (j = 0; j < k; j++)
        if (keep[j].w == w && keep[j].h == h && keep[j].bpp == bpp)
            return 1;
    return 0;
}

static int has_mode(unsigned w, unsigned h, unsigned bpp)
{
    int i;
    for (i = 0; i < N124; i++)
        if (k_124[i].w == w && k_124[i].h == h && k_124[i].bpp == bpp)
            return 1;
    return 0;
}

TEST(the_listing_is_a_permutation_of_the_modes)
{
    int order[N124], seen[N124], i;
    CHECK_EQ_I(listed(order), N124);
    memset(seen, 0, sizeof seen);
    for (i = 0; i < N124; i++)
        seen[order[i]]++;
    for (i = 0; i < N124; i++)
        CHECK_EQ_I(seen[i], 1);
}

TEST(every_standard_mode_at_16_and_32_bpp_is_inside_glquakes_window)
{
    int order[N124], i, k, checked = 0;
    md keep[30];
    k = glquake_window(order, listed(order), keep);
    for (i = 0; i < N124; i++) {
        const md *m = &k_124[i];
        if ((m->bpp == 16 || m->bpp == 32) && vcr_mode_standard(m->w, m->h)) {
            if (!in_window(keep, k, m->w, m->h, m->bpp)) {
                CHECK(0, "a standard mode is outside GLQuake's window");
                return;
            }
            checked++;
        }
    }
    CHECK(checked > 20, "the list has the standard modes");
    CHECK(in_window(keep, k, 1280, 960, 32), "the Quake shortcut's mode");
    CHECK(in_window(keep, k, 640, 480, 16), "the Voodoo Quake shortcut's mode");
}

TEST(the_old_depth_major_order_left_1280x960x32_out)
{
    int order[N124], i, k;
    md keep[30];
    for (i = 0; i < N124; i++)
        order[i] = i;                   /* the miniport's order, as listed before */
    k = glquake_window(order, N124, keep);
    CHECK(has_mode(1280, 960, 32), "the list has the mode");
    CHECK(!in_window(keep, k, 1280, 960, 32), "the old order reached it");
}

TEST(standard_means_4_3_or_5_4)
{
    CHECK(vcr_mode_standard(640, 480), "4:3");
    CHECK(vcr_mode_standard(1280, 960), "4:3");
    CHECK(vcr_mode_standard(1280, 1024), "5:4");
    CHECK(vcr_mode_standard(1600, 1200), "4:3");
    CHECK(!vcr_mode_standard(320, 200), "16:10 VGA");
    CHECK(!vcr_mode_standard(1280, 720), "16:9");
    CHECK(!vcr_mode_standard(720, 480), "3:2 NTSC");
    CHECK(vcr_mode_standard(720, 576), "PAL 720x576 is 5:4 by pixel count");
    CHECK(vcr_mode_rank(640, 480, 32) < vcr_mode_rank(1280, 720, 8), "every standard mode first");
    CHECK(vcr_mode_rank(640, 480, 16) < vcr_mode_rank(320, 240, 32), "then by depth");
}

MUNIT_MAIN("vcr-kmd mode listing order (include/vcr_modeorder.h)", {
    RUN(the_listing_is_a_permutation_of_the_modes);
    RUN(every_standard_mode_at_16_and_32_bpp_is_inside_glquakes_window);
    RUN(the_old_depth_major_order_left_1280x960x32_out);
    RUN(standard_means_4_3_or_5_4);
})
