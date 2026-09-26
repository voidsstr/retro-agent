/* test_vcr_kmd_cursor.c
 *
 * The hardware cursor's pattern and position (voodoo-cleanroom/vcr-kmd/
 * common/vcr_cursor.c). A hardware cursor never appears in a screenshot, so
 * what is pinned here is the translation from a Windows monochrome pointer to
 * the chip's two planes: every pixel's (AND, XOR) pair lands where the
 * layout says, pixels outside the pointer are transparent, and hwCurLoc is
 * offset by 64 so a pointer can hang off the top-left edge.
 */
#include <string.h>
#include "munit.h"
#include "../../voodoo-cleanroom/vcr-kmd/common/vcr_cursor.c"

static int px(const vcr_u8 *pat, int x, int y, int plane)
{
    const vcr_u8 *row = pat + y * VCR_CURSOR_ROW_BYTES + plane * 8;
    return (row[x / 8] >> (7 - x % 8)) & 1;
}

TEST(every_pixel_keeps_its_and_xor_pair) {
    /* a 16x3 pointer: row 0 black|white halves, row 1 transparent|invert,
     * row 2 a checker */
    vcr_u8 and_rows[3 * 2] = { 0x00, 0x00, 0xff, 0xff, 0xaa, 0xaa };
    vcr_u8 xor_rows[3 * 2] = { 0x00, 0xff, 0x00, 0xff, 0x55, 0x55 };
    vcr_u8 pat[VCR_CURSOR_BYTES];
    int x;
    CHECK(vcr_cursor_from_mono(and_rows, xor_rows, 16, 3, 2, pat), "fits");
    for (x = 0; x < 16; x++) {
        CHECK_EQ_I(px(pat, x, 0, 0), 0);
        CHECK_EQ_I(px(pat, x, 0, 1), x >= 8);          /* black then white */
        CHECK_EQ_I(px(pat, x, 1, 0), 1);
        CHECK_EQ_I(px(pat, x, 1, 1), x >= 8);          /* transparent then invert */
        CHECK_EQ_I(px(pat, x, 2, 0), !(x & 1));
        CHECK_EQ_I(px(pat, x, 2, 1), x & 1);
    }
}

TEST(outside_the_pointer_is_transparent) {
    vcr_u8 and_rows[2] = { 0x00, 0x00 }, xor_rows[2] = { 0xff, 0xff };
    vcr_u8 pat[VCR_CURSOR_BYTES];
    int x, y;
    CHECK(vcr_cursor_from_mono(and_rows, xor_rows, 11, 1, 2, pat), "fits");
    for (y = 0; y < 64; y++)
        for (x = 0; x < 64; x++) {
            int inside = y == 0 && x < 11;
            CHECK_EQ_I(px(pat, x, y, 0), !inside);     /* AND 1 outside */
            CHECK_EQ_I(px(pat, x, y, 1), inside);      /* XOR 0 outside */
        }
}

TEST(too_big_is_refused) {
    vcr_u8 m[9 * 65];
    vcr_u8 pat[VCR_CURSOR_BYTES];
    memset(m, 0, sizeof m);
    CHECK(!vcr_cursor_from_mono(m, m, 65, 8, 9, pat), "65 wide");
    CHECK(!vcr_cursor_from_mono(m, m, 32, 65, 4, pat), "65 high");
    CHECK(!vcr_cursor_from_mono(m, m, 32, 8, 3, pat), "stride shorter than the width");
    CHECK(vcr_cursor_from_mono(m, m, 64, 64, 8, pat), "64x64 fits");
}

TEST(position_is_offset_by_64_and_clamped) {
    CHECK_EQ_U(vcr_cursor_loc(0, 0), (64u << 16) | 64u);
    CHECK_EQ_U(vcr_cursor_loc(700, 500), (564u << 16) | 764u);
    CHECK_EQ_U(vcr_cursor_loc(-10, -63), (1u << 16) | 54u);     /* hanging off the edge */
    CHECK_EQ_U(vcr_cursor_loc(-100, -100), 0u);                 /* fully off: clamped */
}

MUNIT_MAIN("vcr-kmd hardware cursor", {
    RUN(every_pixel_keeps_its_and_xor_pair);
    RUN(outside_the_pointer_is_transparent);
    RUN(too_big_is_refused);
    RUN(position_is_offset_by_64_and_clamped);
})
