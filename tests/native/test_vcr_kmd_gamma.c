/* test_vcr_kmd_gamma.c - TRUE-SOURCE test of
 * voodoo-cleanroom/vcr-kmd/include/vcr_gamma.h, what display/vcrdd.c
 * DrvIcmSetDeviceGammaRamp loads into the colour table (2026-09-28).
 *
 * Why it exists: Jedi Academy logged "SetDeviceGammaRamp failed." on .124 -
 * our display driver offered no GDI gamma at all, so the id Tech 3 family
 * fell back to software gamma with overbright forced to 0 (the dark picture).
 * What this pins:
 *   - an identity GDI ramp (v * 0x101) loads an identity colour table;
 *   - the DAC takes each word's HIGH byte - the low byte would turn a ramp
 *     inside out (0x80ff -> 0xff, not 0x80);
 *   - red, green and blue come from the right thirds of the ramp;
 *   - a Quake III overbright ramp (the gamma curve doubled, clamped) lands as
 *     the doubled table;
 *   - only 16/24/32 bpp take a ramp: at 8 bpp the table is the palette.
 */
#include "munit.h"
#include "../../voodoo-cleanroom/vcr-kmd/include/vcr_gamma.h"

static unsigned short ramp[3 * 256];

TEST(identity_ramp_is_an_identity_table)
{
    unsigned i;
    for (i = 0; i < 256; i++)
        ramp[i] = ramp[256 + i] = ramp[512 + i] = (unsigned short)(i * 0x101);
    for (i = 0; i < 256; i++)
        if (vcr_gamma_entry(ramp, i) != 0x010101ul * i)
            break;
    CHECK(i == 256, "entry i is 0x00iiiiii");
}

TEST(the_high_byte_of_each_word)
{
    ramp[10] = 0x80ff;
    ramp[256 + 10] = 0x4000;
    ramp[512 + 10] = 0x00ff;
    CHECK(vcr_gamma_entry(ramp, 10) == 0x804000ul, "0x80ff -> 0x80, 0x00ff -> 0x00");
    CHECK(vcr_gamma_entry(ramp, 10) != 0xff00fful, "the old low-byte reading");
}

TEST(channels_come_from_their_own_third)
{
    unsigned i;
    for (i = 0; i < 256; i++) {
        ramp[i] = 0xffff;       /* red full */
        ramp[256 + i] = 0;      /* green off */
        ramp[512 + i] = 0x7f00; /* blue half */
    }
    CHECK(vcr_gamma_entry(ramp, 0) == 0xff007ful, "red, green, blue in order");
    CHECK(vcr_gamma_entry(ramp, 255) == 0xff007ful, "the last entry too");
}

TEST(an_id_tech_3_overbright_ramp)
{
    /* R_SetColorMappings with r_gamma 1, overbright 1: min(i << 1, 255),
     * handed to GDI as v << 8 (GLimp_SetGamma) */
    unsigned i, v;
    for (i = 0; i < 256; i++) {
        v = i << 1;
        if (v > 255)
            v = 255;
        ramp[i] = ramp[256 + i] = ramp[512 + i] = (unsigned short)(v << 8);
    }
    CHECK(vcr_gamma_entry(ramp, 64) == 0x808080ul, "entry 64 doubled");
    CHECK(vcr_gamma_entry(ramp, 127) == 0xfefefeul, "entry 127");
    CHECK(vcr_gamma_entry(ramp, 128) == 0xfffffful, "128 and up clamp to white");
}

TEST(only_direct_colour_depths_take_a_ramp)
{
    CHECK(!vcr_gamma_depth_ok(8), "8 bpp: the table is the palette");
    CHECK(vcr_gamma_depth_ok(16) && vcr_gamma_depth_ok(32), "16/32 bpp");
    CHECK(vcr_gamma_depth_ok(24), "24 bpp");
    CHECK(!vcr_gamma_depth_ok(0) && !vcr_gamma_depth_ok(15), "anything else refused");
}

MUNIT_MAIN("vcr-kmd GDI gamma ramp -> colour table (include/vcr_gamma.h)", {
    RUN(identity_ramp_is_an_identity_table);
    RUN(the_high_byte_of_each_word);
    RUN(channels_come_from_their_own_third);
    RUN(an_id_tech_3_overbright_ramp);
    RUN(only_direct_colour_depths_take_a_ramp);
})
