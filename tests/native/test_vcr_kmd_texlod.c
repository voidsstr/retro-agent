/* test_vcr_kmd_texlod.c
 *
 * Where the Voodoo TMU finds a Direct3D texture (voodoo-cleanroom/vcr-kmd/
 * common/vcr_texlod.c, used by display/vcrdd_d3d.c). The TMU addresses every
 * texture as a mipmap whose LOD 0 is 256 texels wide: a 64x64 texture is LOD
 * 2, found at texBaseAddr + size(LOD 0) + size(LOD 1). Programming the
 * texture's own address as the base samples 160 KB past it. Verified on the
 * 86Box Voodoo3 with d3dprobe render (tex, modulate, bigtex: 12/12).
 */
#include "munit.h"
#include "../../voodoo-cleanroom/vcr-kmd/common/vcr_texlod.c"

TEST(a_256_texture_is_lod_0_at_its_own_address) {
    vcr_texlod t;
    CHECK(vcr_texlod_compute(256, 256, 2, 512, 0x100000, &t) == 0, "fits");
    CHECK_EQ_U(t.lod, 0u);
    CHECK_EQ_U(t.base, 0x100000u);
    CHECK_EQ_U(t.tlod, 0u);
}

TEST(a_smaller_texture_is_based_before_itself) {
    vcr_texlod t;
    /* 64x64 16 bpp: LOD 2; the missing LOD 0 (256x256x2 = 0x20000) and LOD 1
     * (128x128x2 = 0x8000) come off the address */
    CHECK(vcr_texlod_compute(64, 64, 2, 128, 0x100000, &t) == 0, "fits");
    CHECK_EQ_U(t.lod, 2u);
    CHECK_EQ_U(t.base, 0x100000u - 0x28000u);
    CHECK_EQ_U(t.tlod, (2u << 2) | (2u << 8));
    CHECK(t.base != 0x100000u, "the old, wrong answer: the texture's own address");
}

TEST(aspect_ratio_shrinks_the_missing_levels) {
    vcr_texlod t;
    /* 128x32: wide side 128 -> LOD 1, aspect 4:1 (2), S wider; the missing
     * LOD 0 is 256x64x2 = 0x8000 */
    CHECK(vcr_texlod_compute(128, 32, 2, 256, 0x200000, &t) == 0, "fits");
    CHECK_EQ_U(t.lod, 1u);
    CHECK_EQ_U(t.aspect, 2u);
    CHECK_EQ_U(t.s_is_wider, 1u);
    CHECK_EQ_U(t.base, 0x200000u - 0x8000u);
    CHECK_EQ_U(t.tlod, (1u << 2) | (1u << 8) | (2u << 21) | (1u << 20));
    /* the tall one: T wider, no S_IS_WIDER */
    CHECK(vcr_texlod_compute(32, 128, 2, 64, 0x200000, &t) == 0, "fits");
    CHECK_EQ_U(t.s_is_wider, 0u);
}

TEST(what_the_chip_cannot_sample_is_refused) {
    vcr_texlod t;
    CHECK(vcr_texlod_compute(96, 64, 2, 192, 0, &t) != 0, "not a power of two");
    CHECK(vcr_texlod_compute(512, 512, 2, 1024, 0, &t) != 0, "larger than 256");
    CHECK(vcr_texlod_compute(256, 16, 2, 512, 0, &t) != 0, "16:1");
    CHECK(vcr_texlod_compute(64, 64, 2, 256, 0, &t) != 0, "a padded row pitch");
    CHECK(vcr_texlod_compute(1, 1, 2, 2, 0x100000, &t) == 0, "1x1 is LOD 8");
    CHECK_EQ_U(t.lod, 8u);
}

MUNIT_MAIN("vcr-kmd texture LOD / base address", {
    RUN(a_256_texture_is_lod_0_at_its_own_address);
    RUN(a_smaller_texture_is_based_before_itself);
    RUN(aspect_ratio_shrinks_the_missing_levels);
    RUN(what_the_chip_cannot_sample_is_refused);
})
