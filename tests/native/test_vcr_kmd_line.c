/* test_vcr_kmd_line.c - TRUE-SOURCE test of voodoo-cleanroom/vcr-kmd/include/vcr_line.h,
 * the arithmetic behind the display driver's accelerated cosmetic lines and
 * 8x8 monochrome pattern fills (display/vcrdd_punt.c DrvStrokePath /
 * DrvLineTo / DrvRealizeBrush + DrvBitBlt -> display/vcrdd_2d.c
 * VcrDd2dPatFill), 2026-09-27.
 *
 * Both used to be GDI's alone (EngStrokePath / EngLineTo / EngBitBlt, the CPU
 * drawing into the frame buffer). With Diag\Accel2DLine = 1 a solid cosmetic
 * horizontal or vertical line is an engine rectangle fill, and with
 * Diag\Accel2DPattern = 1 an 8x8 1 bpp brush (hatches, the 50 % grey of drag
 * rectangles and focus frames) is an engine mono-pattern fill. Verified on the
 * 86Box Voodoo3 bed with gdilab's patline test at 8/16/32 bpp: 0 bad against
 * GDI's own software rendering (evidence/86box_v3/patline).
 *
 * What this proves, against per-pixel references:
 *   - a line's rectangle is EXACTLY the pixels GDI's cosmetic rule draws -
 *     every pixel from the start toward the end, the end excluded - in both
 *     directions on both axes, and a zero-length line draws nothing; the old
 *     "min..max inclusive" rectangle and the "min..max half-open" one are both
 *     wrong in one direction or the other;
 *   - a slanted line is refused (GDI's diamond-exit rule decides those);
 *   - 28.4 fixed point: only whole pixels are taken;
 *   - the brush's bytes land in the engine's two pattern dwords row 0 lowest,
 *     top-down and bottom-up bitmaps alike, and - with the pattern offset
 *     vcr_pat_offset gives for a brush origin, and the surface's base skew the
 *     driver subtracts - the engine (modelled as 86Box's Voodoo3 applies a mono
 *     pattern: row (y + paty) & 7, column (x + patx) & 7, MSB first) paints
 *     GDI's brush pixel (x - org) & 7 for every origin and skew; the offset
 *     without the negation, or without the skew, would not;
 *   - the ROP4s taken: PATCOPY/PATINVERT opaque, PATCOPY transparent
 *     (0xAAF0); everything else is GDI's.
 */
#define _DEFAULT_SOURCE
#include <stdlib.h>
#include <string.h>
#include "munit.h"
#include "../../voodoo-cleanroom/vcr-kmd/include/vcr_line.h"

#define N 24

/* GDI's cosmetic solid line, axis-aligned: step from the start toward the
 * end, the end pixel excluded */
static void gdi_line(unsigned char px[N][N], int x1, int y1, int x2, int y2)
{
    int dx = x2 > x1 ? 1 : x2 < x1 ? -1 : 0, dy = y2 > y1 ? 1 : y2 < y1 ? -1 : 0;
    int x = x1, y = y1;
    while (x != x2 || y != y2) {
        px[y][x] = 1;
        x += dx;
        y += dy;
    }
}

static void rect_px(unsigned char px[N][N], const vcr_hrect *r)
{
    long x, y;
    for (y = r->top; y < r->bottom; y++)
        for (x = r->left; x < r->right; x++)
            px[y][x] = 1;
}

TEST(an_axis_line_is_exactly_gdis_pixels_in_every_direction)
{
    int a, b, c, cases = 0;
    for (a = 0; a < N; a++)
        for (b = 0; b < N; b++)
            for (c = 0; c < N; c++) {
                int k;
                for (k = 0; k < 2; k++) {       /* horizontal, vertical */
                    int x1 = k ? c : a, y1 = k ? a : c, x2 = k ? c : b, y2 = k ? b : c;
                    unsigned char want[N][N], got[N][N];
                    vcr_hrect r;
                    memset(want, 0, sizeof want);
                    memset(got, 0, sizeof got);
                    gdi_line(want, x1, y1, x2, y2);
                    CHECK_EQ_I(vcr_line_rect(x1, y1, x2, y2, &r), 1);
                    rect_px(got, &r);
                    if (memcmp(want, got, sizeof want)) {
                        CHECK(0, "line rectangle differs from GDI's pixels");
                        return;
                    }
                    cases++;
                }
            }
    CHECK_EQ_I(cases, 2 * N * N * N);
}

TEST(the_old_rectangles_would_be_wrong)
{
    vcr_hrect r;
    /* left to right, (2,5)-(6,5): GDI draws x 2..5 */
    CHECK_EQ_I(vcr_line_rect(2, 5, 6, 5, &r), 1);
    CHECK_EQ_I((int)r.left, 2);
    CHECK_EQ_I((int)r.right, 6);            /* min..max INCLUSIVE would be 7 */
    /* right to left, (6,5)-(2,5): GDI draws x 6..3 */
    CHECK_EQ_I(vcr_line_rect(6, 5, 2, 5, &r), 1);
    CHECK_EQ_I((int)r.left, 3);             /* min..max half-open would be 2 */
    CHECK_EQ_I((int)r.right, 7);            /* ...and 6 */
    /* bottom to top, (4,9)-(4,1): y 9..2 */
    CHECK_EQ_I(vcr_line_rect(4, 9, 4, 1, &r), 1);
    CHECK_EQ_I((int)r.top, 2);
    CHECK_EQ_I((int)r.bottom, 10);
    CHECK_EQ_I((int)r.left, 4);
    CHECK_EQ_I((int)r.right, 5);
}

TEST(a_zero_length_line_draws_nothing_and_a_slanted_one_is_refused)
{
    vcr_hrect r;
    CHECK_EQ_I(vcr_line_rect(7, 7, 7, 7, &r), 1);
    CHECK(r.right <= r.left || r.bottom <= r.top, "empty");
    CHECK_EQ_I(vcr_line_rect(0, 0, 1, 1, &r), 0);
    CHECK_EQ_I(vcr_line_rect(0, 0, 5, 1, &r), 0);
    CHECK_EQ_I(vcr_line_rect(3, 0, 2, 9, &r), 0);
}

TEST(the_clip_keeps_the_intersection_and_says_when_nothing_is_left)
{
    vcr_hrect c = {10, 10, 20, 20}, r;
    r = (vcr_hrect){5, 12, 25, 13};
    CHECK_EQ_I(vcr_hrect_clip(&r, &c), 1);
    CHECK_EQ_I((int)r.left, 10);
    CHECK_EQ_I((int)r.right, 20);
    CHECK_EQ_I((int)r.top, 12);
    CHECK_EQ_I((int)r.bottom, 13);
    r = (vcr_hrect){0, 0, 10, 30};          /* touches the left edge only: nothing */
    CHECK_EQ_I(vcr_hrect_clip(&r, &c), 0);
    r = (vcr_hrect){12, 25, 13, 30};
    CHECK_EQ_I(vcr_hrect_clip(&r, &c), 0);
}

TEST(only_whole_pixels_in_28_4_are_taken)
{
    long p = 99;
    CHECK_EQ_I(vcr_fix_whole(0x30, &p), 1);
    CHECK_EQ_I((int)p, 3);
    CHECK_EQ_I(vcr_fix_whole(-0x20, &p), 1);
    CHECK_EQ_I((int)p, -2);
    CHECK_EQ_I(vcr_fix_whole(0, &p), 1);
    CHECK_EQ_I((int)p, 0);
    p = 99;
    CHECK_EQ_I(vcr_fix_whole(0x38, &p), 0);  /* 3.5 */
    CHECK_EQ_I((int)p, 99);
    CHECK_EQ_I(vcr_fix_whole(0x31, &p), 0);
    CHECK_EQ_I(vcr_fix_whole(-0x1f, &p), 0);
}

/* ---- patterns -------------------------------------------------------------- */

/* the engine: a mono pattern fill at engine x (the surface x plus the base
 * skew), pattern offsets as programmed */
static int eng_bit(unsigned long p0, unsigned long p1, long xe, long y, unsigned patx,
                   unsigned paty)
{
    unsigned row = (unsigned)((y + paty) & 7), col = (unsigned)((xe + patx) & 7);
    unsigned char b = (unsigned char)((row < 4 ? p0 >> (8 * row) : p1 >> (8 * (row - 4))) & 0xff);
    return (b >> (7 - col)) & 1;
}

/* GDI: pixel (x,y) takes brush pixel ((x - ox) & 7, (y - oy) & 7), a 1 bpp
 * row's leftmost pixel in bit 7 */
static int gdi_bit(const unsigned char brush[8], long x, long y, long ox, long oy)
{
    return (brush[(y - oy) & 7] >> (7 - ((x - ox) & 7))) & 1;
}

static const unsigned char k_brush[8] = {0x81, 0x42, 0x24, 0x18, 0xf0, 0x0f, 0xaa, 0x13};

TEST(the_brush_rows_land_in_the_pattern_dwords_row_0_lowest)
{
    unsigned char buf[8 * 12];
    unsigned long p0, p1;
    int i;
    memset(buf, 0xee, sizeof buf);
    for (i = 0; i < 8; i++)
        buf[i * 12] = k_brush[i];           /* top-down, 12-byte rows */
    vcr_pat_pack(buf, 12, &p0, &p1);
    CHECK_EQ_U(p0, 0x18244281u);
    CHECK_EQ_U(p1, 0x13aa0ff0u);
    memset(buf, 0xee, sizeof buf);
    for (i = 0; i < 8; i++)
        buf[(7 - i) * 12] = k_brush[i];     /* bottom-up: row 0 last in memory */
    vcr_pat_pack(buf + 7 * 12, -12, &p0, &p1);
    CHECK_EQ_U(p0, 0x18244281u);
    CHECK_EQ_U(p1, 0x13aa0ff0u);
}

TEST(the_engine_paints_gdis_brush_pixel_for_every_origin_and_skew)
{
    unsigned long p0, p1;
    long ox, oy, x, y;
    unsigned dsk;
    vcr_pat_pack(k_brush, 1, &p0, &p1);
    for (dsk = 0; dsk < 16; dsk++)
        for (ox = -19; ox <= 19; ox++)
            for (oy = -11; oy <= 11; oy += 3) {
                /* what vcrdd_punt.c accel_patfill + VcrDd2dPatFill program */
                unsigned patx = (vcr_pat_offset(ox, 0) - dsk) & 7, paty = vcr_pat_offset(oy, 0);
                /* and the same through vcr_pat_offset's own skew argument */
                CHECK_EQ_U(patx, vcr_pat_offset(ox, dsk));
                for (y = 0; y < 16; y++)
                    for (x = 0; x < 24; x++)
                        if (eng_bit(p0, p1, x + dsk, y, patx, paty) !=
                            gdi_bit(k_brush, x, y, ox, oy)) {
                            CHECK(0, "engine pattern pixel differs from GDI's brush pixel");
                            return;
                        }
            }
}

TEST(the_offset_without_the_negation_or_without_the_skew_would_be_wrong)
{
    unsigned long p0, p1;
    long x, y, bad_sign = 0, bad_skew = 0;
    vcr_pat_pack(k_brush, 1, &p0, &p1);
    for (y = 0; y < 8; y++)
        for (x = 0; x < 8; x++) {
            /* origin (3,5), no skew: org & 7 instead of -org & 7 */
            bad_sign += eng_bit(p0, p1, x, y, 3, 5) != gdi_bit(k_brush, x, y, 3, 5);
            /* origin (0,0), skew 4 (a 16 bpp surface 8 bytes into a 16-byte
             * unit): the skew not taken off */
            bad_skew += eng_bit(p0, p1, x + 4, y, vcr_pat_offset(0, 0), 0) !=
                        gdi_bit(k_brush, x, y, 0, 0);
        }
    CHECK(bad_sign > 0, "the un-negated origin paints other pixels");
    CHECK(bad_skew > 0, "the un-skewed offset paints other pixels");
}

TEST(the_rops_taken_are_patcopy_and_patinvert_and_transparent_patcopy)
{
    unsigned r3 = 0;
    int tr = -1;
    CHECK_EQ_I(vcr_pat_rop(0xf0f0, &r3, &tr), 1);
    CHECK_EQ_U(r3, 0xf0u);
    CHECK_EQ_I(tr, 0);
    CHECK_EQ_I(vcr_pat_rop(0x5a5a, &r3, &tr), 1);
    CHECK_EQ_U(r3, 0x5au);
    CHECK_EQ_I(tr, 0);
    CHECK_EQ_I(vcr_pat_rop(0xaaf0, &r3, &tr), 1);   /* TRANSPARENT background mode */
    CHECK_EQ_U(r3, 0xf0u);
    CHECK_EQ_I(tr, 1);
    CHECK_EQ_I(vcr_pat_rop(0xaa5a, &r3, &tr), 0);   /* not asked for, not taken */
    CHECK_EQ_I(vcr_pat_rop(0xcccc, &r3, &tr), 0);   /* SRCCOPY */
    CHECK_EQ_I(vcr_pat_rop(0x0f0f, &r3, &tr), 0);   /* PATCOPY inverted */
    CHECK_EQ_I(vcr_pat_rop(0xf0aa, &r3, &tr), 0);
    CHECK_EQ_I(vcr_pat_rop(0xa0a0, &r3, &tr), 0);   /* P AND D */
}

MUNIT_MAIN("vcr-kmd accelerated lines and mono patterns (include/vcr_line.h)", {
    RUN(an_axis_line_is_exactly_gdis_pixels_in_every_direction);
    RUN(the_old_rectangles_would_be_wrong);
    RUN(a_zero_length_line_draws_nothing_and_a_slanted_one_is_refused);
    RUN(the_clip_keeps_the_intersection_and_says_when_nothing_is_left);
    RUN(only_whole_pixels_in_28_4_are_taken);
    RUN(the_brush_rows_land_in_the_pattern_dwords_row_0_lowest);
    RUN(the_engine_paints_gdis_brush_pixel_for_every_origin_and_skew);
    RUN(the_offset_without_the_negation_or_without_the_skew_would_be_wrong);
    RUN(the_rops_taken_are_patcopy_and_patinvert_and_transparent_patcopy);
})
