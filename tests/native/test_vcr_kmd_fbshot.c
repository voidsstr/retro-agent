/* test_vcr_kmd_fbshot.c - TRUE-SOURCE test of
 * voodoo-cleanroom/vcr-kmd/include/vcr_fbshot.h, the decoding behind
 * `vcrctl fbshot` (2026-09-28): read the memory the video processor is
 * scanning out, because a GDI screenshot of a fullscreen Glide/GL/D3D game on
 * our driver photographs the old desktop memory instead.
 *
 * The register values are .124's own, from the flight recorder's MODESET_DONE
 * lines that night: 1280x1024x32@85 = vidProcCfg 000c0081, vidScreenSize
 * 00400500, stride 00001400; Warcraft II's 640x480x8 = 00000081, 001e0280,
 * 00000280. What this pins:
 *   - the format field and bytes per pixel those registers mean;
 *   - a linear offset is y * stride + x, and a TILED one is not (128 x 32
 *     tiles, stride counted in tiles) - both directions, so a linear formula
 *     used for tiled memory fails here;
 *   - the extent the tool bounds its reads with;
 *   - the pixel formats decode to the right colours, and 8 bpp goes through the
 *     CLUT it is given.
 */
#include <string.h>
#include "munit.h"
#include "../../voodoo-cleanroom/vcr-kmd/include/vcr_regs.h"
#include "../../voodoo-cleanroom/vcr-kmd/include/vcr_fbshot.h"

static unsigned fmt_of(unsigned long vpc)
{
    return (unsigned)((vpc & VCR_VPC_DESKTOP_FMT_MASK) >> VCR_VPC_DESKTOP_FMT_SHIFT);
}

TEST(the_124_desktop_registers_decode)
{
    unsigned long vpc = 0x000c0081, ss = 0x00400500, stride = 0x1400;
    CHECK(fmt_of(vpc) == VCR_VPC_FMT_RGB32, "1280x1024x32 is RGB32");
    CHECK(vcr_fb_bytespp(fmt_of(vpc)) == 4, "4 bytes a pixel");
    CHECK((ss & 0xfff) == 1280 && (ss >> 12 & 0xfff) == 1024, "screen size");
    CHECK(!(vpc & VCR_VPC_DESKTOP_TILED_EN), "the desktop is linear");
    CHECK(vcr_fb_pitch(stride, 0) == 5120, "pitch in bytes");
    CHECK(vcr_fb_extent(1280, 1024, 4, stride, 0) == 1023ul * 5120 + 5120, "extent");
    vpc = 0x00000081;
    CHECK(fmt_of(vpc) == VCR_VPC_FMT_PAL8 && vcr_fb_bytespp(fmt_of(vpc)) == 1,
          "Warcraft II's 640x480x8 is palettized");
    CHECK(vcr_fb_extent(640, 480, 1, 0x280, 0) == 479ul * 640 + 640, "8 bpp extent");
}

TEST(linear_offsets_are_y_times_stride_plus_x)
{
    CHECK(vcr_fb_offset(0, 0, 0x1400, 0) == 0, "origin");
    CHECK(vcr_fb_offset(12, 3, 0x1400, 0) == 3 * 5120 + 12, "a pixel");
    CHECK(vcr_fb_offset(12, 3, 0x81400, 0) == 3 * 5120 + 12, "overlay stride bits ignored");
}

TEST(tiled_offsets_walk_128x32_tiles)
{
    /* 10 tiles a row (1280 bytes) */
    unsigned long st = 10;
    CHECK(vcr_fb_pitch(st, 1) == 1280, "stride counts tiles");
    CHECK(vcr_fb_offset(0, 0, st, 1) == 0, "origin");
    CHECK(vcr_fb_offset(127, 0, st, 1) == 127, "end of the first tile's first line");
    CHECK(vcr_fb_offset(128, 0, st, 1) == 4096, "the next tile starts 4 KB on");
    CHECK(vcr_fb_offset(0, 1, st, 1) == 128, "line 1 is 128 bytes into the tile");
    CHECK(vcr_fb_offset(0, 32, st, 1) == 10 * 4096, "line 32 is the next tile row");
    CHECK(vcr_fb_offset(130, 33, st, 1) == (10 + 1) * 4096 + 128 + 2, "both at once");
    /* the linear formula on the same numbers is a different place */
    CHECK(vcr_fb_offset(128, 0, st, 1) != vcr_fb_offset(128, 0, 1280, 0), "tiled != linear");
    CHECK(vcr_fb_extent(640, 480, 2, st, 1) == 15ul * 10 * 4096, "15 tile rows");
    CHECK(vcr_fb_extent(640, 481, 2, st, 1) == 16ul * 10 * 4096, "a partial tile row counts");
}

TEST(pixels_decode_to_rgb)
{
    unsigned char p565[2] = { 0x00, 0xf8 };            /* pure red */
    unsigned char g565[2] = { 0xe0, 0x07 };            /* pure green */
    unsigned char w565[2] = { 0xff, 0xff };
    unsigned char p32[4] = { 0x33, 0x22, 0x11, 0x00 }; /* BGRX */
    unsigned char p24[3] = { 0x33, 0x22, 0x11 };
    unsigned char i8[1] = { 7 };
    unsigned long clut[256];
    int i;
    for (i = 0; i < 256; i++)
        clut[i] = 0x010203ul * (unsigned long)i;
    CHECK(vcr_fb_rgb(1, p565, NULL) == 0xff0000ul, "565 red");
    CHECK(vcr_fb_rgb(1, g565, NULL) == 0x00ff00ul, "565 green");
    CHECK(vcr_fb_rgb(1, w565, NULL) == 0xfffffful, "565 white is white, not 0xf8fcf8");
    CHECK(vcr_fb_rgb(3, p32, NULL) == 0x112233ul, "32 bpp is BGRX in memory");
    CHECK(vcr_fb_rgb(2, p24, NULL) == 0x112233ul, "24 bpp");
    CHECK(vcr_fb_rgb(0, i8, clut) == 0x070e15ul, "8 bpp goes through the CLUT");
    CHECK(vcr_fb_rgb(0, i8, NULL) == 0x070707ul, "no CLUT: grey ramp, never black");
    CHECK(vcr_fb_bytespp(4) == 0 && vcr_fb_rgb(4, p32, NULL) == 0, "an unknown format is refused");
}

MUNIT_MAIN("vcr-kmd fbshot decoding (include/vcr_fbshot.h)", {
    RUN(the_124_desktop_registers_decode);
    RUN(linear_offsets_are_y_times_stride_plus_x);
    RUN(tiled_offsets_walk_128x32_tiles);
    RUN(pixels_decode_to_rgb);
})
