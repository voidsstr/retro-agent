/* test_vcr_kmd_texlod.c
 *
 * Where the Voodoo TMU finds a Direct3D texture (voodoo-cleanroom/vcr-kmd/
 * common/vcr_texlod.c, used by display/vcrdd_d3d.c). The TMU addresses every
 * texture as a mipmap whose LOD 0 is 256 texels wide: a 64x64 texture is LOD
 * 2, found at texBaseAddr + size(LOD 0) + size(LOD 1). Programming the
 * texture's own address as the base samples 160 KB past it. Verified on the
 * 86Box Voodoo3 with d3dprobe render (tex, modulate, bigtex: 12/12).
 *
 * The VSA-100 texture path (vcr_texlod_compute_ext, Diag\D3DBigTex, default
 * OFF - 2026-09-28, clean-room lane, NOT yet run on silicon): textures up to
 * 2048 (tLOD TBIG, LODs counted from 2048, texBaseAddr still at the 256-level
 * - the levels wider than 256 lie BEFORE it), the 26-bit texture address
 * (bits 24:4 + address bit 25 in bit 1), ARGB8888 and DXT1/3/5 sizes. Every
 * expected base below is cross-checked against 3dfx's own GPL tables
 * (h5 glide3/src/ditex.c _grMipMapOffset / _grMipMapOffsetCmp4Bit /
 * _grMipMapOffsetDXT, copied here as the reference) and the old function's
 * answers are pinned so the Banshee/Voodoo3 path cannot move.
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

/* ---- the Banshee / Voodoo3 answers, pinned (the default path everywhere) --------- */

TEST(the_voodoo3_answers_do_not_move) {
    /* vcr_texlod_compute as it stood before the VSA-100 path (4a9793b..):
     * {w, h, offset} -> {lod, base, tlod}. The VSA-100 work must leave these
     * exactly: the HAL calls this function whenever Diag\D3DBigTex is off
     * and on every Banshee/Voodoo3. */
    static const struct { unsigned w, h, off, lod, base, tlod; } v3[] = {
        { 256, 256, 0x0100000, 0, 0x0100000, 0x00000000 },
        { 128, 128, 0x0100000, 1, 0x00e0000, 0x00000104 },
        {  64,  64, 0x0100000, 2, 0x00d8000, 0x00000208 },
        {   1,   1, 0x0100000, 8, 0x00d5550, 0x00000820 },
        { 128,  32, 0x0200000, 1, 0x01f8000, 0x00500104 },
        {  32, 128, 0x0200000, 1, 0x01f8000, 0x00400104 },
        { 256,  32, 0x0000000, 0, 0x0000000, 0x00700000 },
        {  64,  64, 0x0000000, 2, 0x0fd8000, 0x00000208 },   /* wraps in the 16 MB field */
        {  64,  64, 0x1400000, 2, 0x03d8000, 0x00000208 },   /* 20 MB: sampled 16 MB lower */
    };
    unsigned i;
    for (i = 0; i < sizeof v3 / sizeof v3[0]; i++) {
        vcr_texlod t;
        CHECK(vcr_texlod_compute(v3[i].w, v3[i].h, 2, v3[i].w * 2, v3[i].off, &t) == 0, "fits");
        CHECK_EQ_U(t.lod, v3[i].lod);
        CHECK_EQ_U(t.base, v3[i].base);
        CHECK_EQ_U(t.tlod, v3[i].tlod);
    }
}

TEST(the_ext_function_with_no_flags_is_the_voodoo3_function) {
    /* every power-of-two 16 bpp size to 256, every aspect to 8:1, both ways,
     * at offsets that do and do not wrap: field for field */
    static const unsigned offs[] = { 0, 0x10, 0x8000, 0x100000, 0xfffff0, 0x1400000, 0x3ff0000 };
    unsigned lw, lh, o, n = 0;
    for (lw = 0; lw <= 8; lw++)
        for (lh = 0; lh <= 8; lh++)
            for (o = 0; o < sizeof offs / sizeof offs[0]; o++) {
                unsigned w = 1u << lw, h = 1u << lh;
                vcr_texlod a;
                vcr_texlod_ext b;
                int ra = vcr_texlod_compute(w, h, 2, w * 2, offs[o], &a);
                int rb = vcr_texlod_compute_ext(w, h, VCR_TEXK_RGB16, w * 2, offs[o], 0, &b);
                CHECK_EQ_I(ra, rb);
                if (ra || rb)
                    continue;
                CHECK_EQ_U(a.lod, b.t.lod);
                CHECK_EQ_U(a.aspect, b.t.aspect);
                CHECK_EQ_U(a.s_is_wider, b.t.s_is_wider);
                CHECK_EQ_U(a.base, b.t.base);
                CHECK_EQ_U(a.tlod, b.t.tlod);
                CHECK_EQ_U(b.tbig, 0u);
                CHECK_EQ_U(b.lod_limit, 8u);
                CHECK_EQ_U(b.lin_base, a.base);
                n++;
            }
    CHECK_EQ_U(n, 51u * 7u);   /* 51 size pairs within 8:1, 7 offsets */
}

/* ---- 3dfx's own tables (GPL h5 glide3/src/ditex.c), the reference ------------------ */

/* _grMipMapOffset[aspect][lod + 1] in TEXELS: the offset of the largest level
 * from the 256-level; rows 8:1, 4:1, 2:1, 1:1 (mirrored for the tall ones) */
static const long mmo[4][13] = {
    { 10927, 10926, 10924, 10920, 10912, 10880, 10752, 10240, 8192, 0, -32768, -163840, -688128 },
    { 21847, 21846, 21844, 21840, 21824, 21760, 21504, 20480, 16384, 0, -65536, -327680, -1376256 },
    { 43691, 43690, 43688, 43680, 43648, 43520, 43008, 40960, 32768, 0, -131072, -655360, -2752512 },
    { 87381, 87380, 87376, 87360, 87296, 87040, 86016, 81920, 65536, 0, -262144, -1310720, -5505024 },
};
/* _grMipMapOffsetCmp4Bit, 4-bit compressed (FXT1 and DXT1): 8x4 minimum,
 * rows 8:1, 4:1, 2:1, 1:1, 1:2, 1:4, 1:8 - NOT mirrored */
static const long cmp4[7][13] = {
    { 11072, 11040, 11008, 10976, 10944, 10880, 10752, 10240, 8192, 0, -32768, -163840, -688128 },
    { 21952, 21920, 21888, 21856, 21824, 21760, 21504, 20480, 16384, 0, -65536, -327680, -1376256 },
    { 43776, 43744, 43712, 43680, 43648, 43520, 43008, 40960, 32768, 0, -131072, -655360, -2752512 },
    { 87456, 87424, 87392, 87360, 87296, 87040, 86016, 81920, 65536, 0, -262144, -1310720, -5505024 },
    { 43808, 43776, 43744, 43712, 43648, 43520, 43008, 40960, 32768, 0, -131072, -655360, -2752512 },
    { 22048, 22016, 21984, 21952, 21888, 21760, 21504, 20480, 16384, 0, -65536, -327680, -1376256 },
    { 11296, 11264, 11232, 11200, 11136, 11008, 10752, 10240, 8192, 0, -32768, -163840, -688128 },
};
/* _grMipMapOffsetDXT, 8-bit compressed (DXT2-5): 4x4 minimum, mirrored */
static const long dxt[4][13] = {
    { 11024, 11008, 10992, 10976, 10944, 10880, 10752, 10240, 8192, 0, -32768, -163840, -688128 },
    { 21904, 21888, 21872, 21856, 21824, 21760, 21504, 20480, 16384, 0, -65536, -327680, -1376256 },
    { 43728, 43712, 43696, 43680, 43648, 43520, 43008, 40960, 32768, 0, -131072, -655360, -2752512 },
    { 87408, 87392, 87376, 87360, 87296, 87040, 86016, 81920, 65536, 0, -262144, -1310720, -5505024 },
};

/* Glide's base for a texture whose largest level is 2^top on the wide side:
 * start - (table * bits / 8, rounded down to 16) (_grTexCalcBaseAddress) */
static unsigned glide_base(long texels, unsigned bits, unsigned start)
{
    long bytes = texels * (long)bits / 8;
    bytes &= ~0xfL;
    return (unsigned)((long)start - bytes);
}

TEST(big_and_small_16_and_32_bpp_bases_are_3dfxs) {
    /* every aspect, both ways, every top from 1 to 2048, VSA-100 with BIG:
     * the linear base must be Glide's wherever Glide's sum is a whole 16 bytes
     * (Glide shifts the data of the others - a 1x1 - by the remainder; the
     * HAL keeps the Voodoo3 function's rounding there, checked just below) */
    static const unsigned kinds[2] = { VCR_TEXK_RGB16, VCR_TEXK_ARGB32 }, bits[2] = { 16, 32 };
    unsigned k, a, tall, top, checked = 0;
    const unsigned start = 0x0800000;
    for (k = 0; k < 2; k++)
        for (a = 0; a <= 3; a++)
            for (tall = 0; tall <= 1; tall++)
                for (top = a; top <= 11; top++) {
                    unsigned big = 1u << top, small = big >> a;
                    unsigned w = tall ? small : big, h = tall ? big : small;
                    long texels = mmo[3 - a][top + 1];
                    vcr_texlod_ext t;
                    CHECK(vcr_texlod_compute_ext(w, h, kinds[k], w * bits[k] / 8, start,
                                                 VCR_TEXF_NAPALM | VCR_TEXF_BIG, &t) == 0, "fits");
                    CHECK_EQ_U(t.tbig, top > 8);
                    CHECK_EQ_U(t.t.lod, (top > 8 ? 11u : 8u) - top);
                    CHECK_EQ_U(t.lod_limit, top > 8 ? 11u : 8u);
                    CHECK_EQ_U(t.t.aspect, a);
                    CHECK_EQ_U(t.t.s_is_wider, !tall && a);
                    if (((texels * (long)bits[k] / 8) & 0xf) == 0) {
                        CHECK_EQ_U(t.lin_base, glide_base(texels, bits[k], start));
                        checked++;
                    } else {
                        CHECK_EQ_U(t.lin_base, (start - (unsigned)(texels * (long)bits[k] / 8)) & ~0xfu);
                    }
                    CHECK_EQ_U(t.t.base, vcr_tex_munge(t.lin_base));
                }
    CHECK(checked > 150, "most cases compared with Glide's table");
}

TEST(a_big_texture_is_based_after_its_big_levels) {
    vcr_texlod_ext t;
    /* 512x512 16 bpp at 8 MB: TBIG, lod 2, base past its 512 level (0x80000) */
    CHECK(vcr_texlod_compute_ext(512, 512, VCR_TEXK_RGB16, 1024, 0x800000,
                                 VCR_TEXF_NAPALM | VCR_TEXF_BIG, &t) == 0, "512 fits");
    CHECK_EQ_U(t.t.lod, 2u);
    CHECK_EQ_U(t.lin_base, 0x880000u);
    CHECK_EQ_U(t.t.tlod, (2u << 2) | (2u << 8) | VCR_TEX_TBIG);
    /* 1024x1024: lod 1, past 1024 + 512 levels */
    CHECK(vcr_texlod_compute_ext(1024, 1024, VCR_TEXK_RGB16, 2048, 0x800000,
                                 VCR_TEXF_NAPALM | VCR_TEXF_BIG, &t) == 0, "1024 fits");
    CHECK_EQ_U(t.t.lod, 1u);
    CHECK_EQ_U(t.lin_base, 0x800000u + 0x200000u + 0x80000u);
    /* 2048x2048: lod 0, past 8 + 2 + 0.5 MB - Glide's -5505024 texels x 2 */
    CHECK(vcr_texlod_compute_ext(2048, 2048, VCR_TEXK_RGB16, 4096, 0x800000,
                                 VCR_TEXF_NAPALM | VCR_TEXF_BIG, &t) == 0, "2048 fits");
    CHECK_EQ_U(t.t.lod, 0u);
    CHECK_EQ_U(t.lin_base, 0x800000u + 0xa80000u);
    CHECK_EQ_U(t.t.tlod, VCR_TEX_TBIG);
    CHECK(t.lin_base != 0x800000u, "the texture's own address would be LOD 3's (256) place");
    /* 2048x256 (8:1): lod 0, aspect 3, past 2048x256 + 1024x128 + 512x64 */
    CHECK(vcr_texlod_compute_ext(2048, 256, VCR_TEXK_RGB16, 4096, 0x800000,
                                 VCR_TEXF_NAPALM | VCR_TEXF_BIG, &t) == 0, "8:1 fits");
    CHECK_EQ_U(t.lin_base, 0x800000u + 688128u * 2);
    CHECK_EQ_U(t.t.tlod, VCR_TEX_TBIG | (3u << 21) | (1u << 20));
    /* 256 with BIG armed: not big - the Voodoo3 numbering, no TBIG */
    CHECK(vcr_texlod_compute_ext(256, 256, VCR_TEXK_RGB16, 512, 0x800000,
                                 VCR_TEXF_NAPALM | VCR_TEXF_BIG, &t) == 0, "256 fits");
    CHECK_EQ_U(t.tbig, 0u);
    CHECK_EQ_U(t.t.tlod, 0u);
    CHECK_EQ_U(t.lin_base, 0x800000u);
}

TEST(the_texture_address_has_26_bits_on_the_vsa100) {
    vcr_texlod_ext t;
    vcr_texlod v3;
    /* a 64x64 at 20 MB: the Voodoo3 field (bits 23:4) samples it at 4 MB -
     * the VSA-100 path keeps bit 24 */
    CHECK(vcr_texlod_compute(64, 64, 2, 128, 0x1400000, &v3) == 0, "v3");
    CHECK_EQ_U(v3.base, 0x03d8000u);                        /* the old answer: 16 MB low */
    CHECK(vcr_texlod_compute_ext(64, 64, VCR_TEXK_RGB16, 128, 0x1400000, VCR_TEXF_NAPALM, &t) == 0,
          "vsa");
    CHECK_EQ_U(t.lin_base, 0x13d8000u);
    CHECK_EQ_U(t.t.base, 0x13d8000u);
    CHECK(t.t.base != v3.base, "the Voodoo3 field drops bit 24");
    /* at 40 MB the address bit 25 goes to register bit 1 (SST_TEXTURE_MUNGE_ADDRESS) */
    CHECK(vcr_texlod_compute_ext(256, 256, VCR_TEXK_RGB16, 512, 0x2800000, VCR_TEXF_NAPALM, &t) == 0,
          "40 MB");
    CHECK_EQ_U(t.lin_base, 0x2800000u);
    CHECK_EQ_U(t.t.base, 0x0800002u);
    CHECK_EQ_U(vcr_tex_unmunge(t.t.base), 0x2800000u);
    CHECK_EQ_U(vcr_tex_munge(0x02345670u), 0x00345672u);
    CHECK_EQ_U(vcr_tex_unmunge(0x00345672u), 0x02345670u);
    CHECK_EQ_U(vcr_tex_munge(0x00fffff0u), 0x00fffff0u);   /* below 32 MB: the same number */
}

TEST(compressed_levels_are_sized_as_the_tmu_walks_them) {
    /* DXT1 in 8x4 units, DXT3/5 in 4x4 (bytes) */
    CHECK_EQ_U(vcr_tex_level_bytes(VCR_TEXK_DXT1, 256, 256), 32768u);
    CHECK_EQ_U(vcr_tex_level_bytes(VCR_TEXK_DXT1, 4, 4), 16u);     /* D3D stores 8 */
    CHECK_EQ_U(vcr_tex_level_bytes(VCR_TEXK_DXT1, 1, 1), 16u);
    CHECK_EQ_U(vcr_tex_level_bytes(VCR_TEXK_DXT1, 16, 2), 32u);
    CHECK_EQ_U(vcr_tex_level_bytes(VCR_TEXK_DXT35, 256, 256), 65536u);
    CHECK_EQ_U(vcr_tex_level_bytes(VCR_TEXK_DXT35, 2, 1), 16u);
    CHECK_EQ_U(vcr_tex_level_bytes(VCR_TEXK_ARGB32, 2048, 2048), 16777216u);
    CHECK_EQ_U(vcr_tex_level_bytes(VCR_TEXK_RGB16, 2048, 2048), 8388608u);
    CHECK_EQ_U(vcr_tex_level_bytes(9, 4, 4), 0u);
}

TEST(compressed_bases_are_3dfxs) {
    /* DXT1 against _grMipMapOffsetCmp4Bit (every row, tall ones included),
     * DXT3/5 against _grMipMapOffsetDXT, tops 8 (DXT1) / 4 (DXT3/5) to 2048 */
    unsigned a, tall, top, n = 0;
    const unsigned start = 0x0c00000;
    for (a = 0; a <= 3; a++)
        for (tall = 0; tall <= 1; tall++)
            for (top = a; top <= 11; top++) {
                unsigned big = 1u << top, small = big >> a;
                unsigned w = tall ? small : big, h = tall ? big : small;
                unsigned row4 = tall ? 3 + a : 3 - a;
                vcr_texlod_ext t;
                int r1 = vcr_texlod_compute_ext(w, h, VCR_TEXK_DXT1, 0, start,
                                                VCR_TEXF_NAPALM | VCR_TEXF_BIG, &t);
                if (w < 8) {
                    CHECK(r1 != 0, "DXT1 narrower than 8 is refused");
                } else {
                    CHECK(r1 == 0, "DXT1 fits");
                    CHECK_EQ_U(t.lin_base, glide_base(cmp4[row4][top + 1], 4, start));
                    n++;
                }
                if (!vcr_texlod_compute_ext(w, h, VCR_TEXK_DXT35, 0, start,
                                            VCR_TEXF_NAPALM | VCR_TEXF_BIG, &t)) {
                    CHECK_EQ_U(t.lin_base, glide_base(dxt[3 - a][top + 1], 8, start));
                    n++;
                }
            }
    CHECK_EQ_U(n, 66u + 84u);     /* DXT1 8 wide and up, and every DXT3/5 */
}

TEST(a_chain_is_packed_and_bounded_as_the_tmu_walks_it) {
    /* 16 bpp: the old mip_offset's numbers */
    CHECK_EQ_U(vcr_tex_chain_offset(VCR_TEXK_RGB16, 64, 64, 0), 0u);
    CHECK_EQ_U(vcr_tex_chain_offset(VCR_TEXK_RGB16, 64, 64, 1), 8192u);
    CHECK_EQ_U(vcr_tex_chain_offset(VCR_TEXK_RGB16, 64, 64, 7), 8192u + 2048 + 512 + 128 + 32 + 8 + 2);
    /* a 2048 chain: level 3 (256) sits exactly where TBIG's base points */
    {
        vcr_texlod_ext t;
        CHECK(vcr_texlod_compute_ext(2048, 2048, VCR_TEXK_RGB16, 4096, 0x400000,
                                     VCR_TEXF_NAPALM | VCR_TEXF_BIG, &t) == 0, "fits");
        CHECK_EQ_U(0x400000u + vcr_tex_chain_offset(VCR_TEXK_RGB16, 2048, 2048, 3), t.lin_base);
        CHECK_EQ_U(vcr_tex_chain_offset(VCR_TEXK_RGB16, 2048, 2048, 12),
                   (unsigned)((5505024L + 87381L) * 2));
    }
    /* DXT1: a 64x64 chain's levels 64, 32, 16, 8 are D3D's layout; 4, 2, 1 are not */
    CHECK_EQ_U(vcr_tex_chain_usable(VCR_TEXK_DXT1, 64, 64, 7), 4u);
    CHECK_EQ_U(vcr_tex_chain_usable(VCR_TEXK_DXT1, 64, 64, 2), 2u);
    CHECK_EQ_U(vcr_tex_chain_usable(VCR_TEXK_DXT35, 64, 64, 7), 7u);
    CHECK_EQ_U(vcr_tex_chain_usable(VCR_TEXK_RGB16, 64, 64, 7), 7u);
    CHECK_EQ_U(vcr_tex_chain_offset(VCR_TEXK_DXT1, 64, 64, 4), 2048u + 512 + 128 + 32);
}

TEST(what_the_vsa100_path_refuses) {
    vcr_texlod_ext t;
    const unsigned both = VCR_TEXF_NAPALM | VCR_TEXF_BIG;
    CHECK(vcr_texlod_compute_ext(512, 512, VCR_TEXK_RGB16, 1024, 0, VCR_TEXF_NAPALM, &t) != 0,
          "512 without BIG");
    CHECK(vcr_texlod_compute_ext(512, 512, VCR_TEXK_RGB16, 1024, 0, VCR_TEXF_BIG, &t) != 0,
          "BIG without NAPALM is not the VSA-100");
    CHECK(vcr_texlod_compute_ext(4096, 4096, VCR_TEXK_RGB16, 8192, 0, both, &t) != 0, "4096");
    CHECK(vcr_texlod_compute_ext(2048, 64, VCR_TEXK_RGB16, 4096, 0, both, &t) != 0, "32:1");
    CHECK(vcr_texlod_compute_ext(64, 64, VCR_TEXK_DXT1, 0, 0, 0, &t) != 0, "DXT on a Voodoo3");
    CHECK(vcr_texlod_compute_ext(64, 64, VCR_TEXK_ARGB32, 256, 0, 0, &t) != 0, "32-bit on a Voodoo3");
    CHECK(vcr_texlod_compute_ext(4, 4, VCR_TEXK_DXT1, 0, 0, both, &t) != 0, "DXT1 4 wide");
    CHECK(vcr_texlod_compute_ext(4, 4, VCR_TEXK_DXT35, 0, 0, both, &t) == 0, "DXT3/5 4 wide");
    CHECK(vcr_texlod_compute_ext(64, 64, VCR_TEXK_ARGB32, 128, 0, both, &t) != 0, "32-bit pitch");
    CHECK(vcr_texlod_compute_ext(64, 64, VCR_TEXK_ARGB32, 256, 0, both, &t) == 0, "32-bit pitch ok");
    CHECK(vcr_texlod_compute_ext(64, 64, 7, 0, 0, both, &t) != 0, "an unknown kind");
    CHECK(vcr_texlod_compute_ext(96, 64, VCR_TEXK_RGB16, 192, 0, both, &t) != 0, "not a power of two");
}

/* Integration review (2026-09-28): the flush's texture-port write-back on the
 * VSA-100 path fires only where the default path's did - a 16 bpp texture no
 * wider than 256 BELOW 16 MB. The default path's 24-bit base is 16 MB low
 * above that line, so its range test (VcrDd3dTexFlush: addr - lin < 2 MB)
 * never passed there; with the 26-bit base any D3DBigTex bit made it pass,
 * and a port write of a texture above 16 MB has never run on the card. */
TEST(the_write_back_stays_where_the_default_path_proved_it) {
    const unsigned both = VCR_TEXF_NAPALM | VCR_TEXF_BIG;
    vcr_texlod_ext x;
    vcr_texlod t;
    unsigned long lin;
    CHECK_EQ_U(VCR_TEX_WRITEBACK_LIMIT, 0x1000000u);
    CHECK_EQ_U(vcr_tex_writeback_addr(VCR_TEXK_RGB16, 0, 0x00100000u), 0x00100000u);
    CHECK_EQ_U(vcr_tex_writeback_addr(VCR_TEXK_RGB16, 0, 0x00fffff0u), 0x00fffff0u);
    /* the fix: at and above 16 MB, none */
    CHECK_EQ_U(vcr_tex_writeback_addr(VCR_TEXK_RGB16, 0, 0x01000000u), ~0u);
    CHECK_EQ_U(vcr_tex_writeback_addr(VCR_TEXK_RGB16, 0, 0x01a00000u), ~0u);
    /* never for TBIG, 32-bit or compressed, wherever they are */
    CHECK_EQ_U(vcr_tex_writeback_addr(VCR_TEXK_RGB16, 1, 0x00100000u), ~0u);
    CHECK_EQ_U(vcr_tex_writeback_addr(VCR_TEXK_ARGB32, 0, 0x00100000u), ~0u);
    CHECK_EQ_U(vcr_tex_writeback_addr(VCR_TEXK_DXT1, 0, 0x00100000u), ~0u);
    CHECK_EQ_U(vcr_tex_writeback_addr(VCR_TEXK_DXT35, 0, 0x00100000u), ~0u);
    /* why: a 64x64 16 bpp texture at 26 MB. The OLD guard (RGB16 and not
     * TBIG) handed its address on, and VcrDd3dTexFlush's range test - addr
     * against the unmunged 26-bit base - passed: the write would fire */
    CHECK(vcr_texlod_compute_ext(64, 64, VCR_TEXK_RGB16, 128, 0x01a00000u, both, &x) == 0, "fits");
    lin = vcr_tex_unmunge(x.t.base);
    CHECK(0x01a00000ul >= lin && 0x01a00000ul - lin < 0x200000ul, "the old guard let it through");
    CHECK_EQ_U(vcr_tex_writeback_addr(VCR_TEXK_RGB16, x.tbig, 0x01a00000u), ~0u);
    /* ...while the default path's 24-bit base puts it 16 MB away: no write */
    CHECK(vcr_texlod_compute(64, 64, 2, 128, 0x01a00000u, &t) == 0, "fits");
    CHECK(!(0x01a00000u >= t.base && 0x01a00000u - t.base < 0x200000u), "the default never wrote");
    /* below 16 MB both paths agree, base and write-back alike */
    CHECK(vcr_texlod_compute_ext(64, 64, VCR_TEXK_RGB16, 128, 0x00a00000u, both, &x) == 0, "fits");
    CHECK(vcr_texlod_compute(64, 64, 2, 128, 0x00a00000u, &t) == 0, "fits");
    CHECK_EQ_U(x.t.base, t.base);
    CHECK_EQ_U(vcr_tex_writeback_addr(VCR_TEXK_RGB16, x.tbig, 0x00a00000u), 0x00a00000u);
}

MUNIT_MAIN("vcr-kmd texture LOD / base address", {
    RUN(a_256_texture_is_lod_0_at_its_own_address);
    RUN(a_smaller_texture_is_based_before_itself);
    RUN(aspect_ratio_shrinks_the_missing_levels);
    RUN(what_the_chip_cannot_sample_is_refused);
    RUN(the_voodoo3_answers_do_not_move);
    RUN(the_ext_function_with_no_flags_is_the_voodoo3_function);
    RUN(big_and_small_16_and_32_bpp_bases_are_3dfxs);
    RUN(a_big_texture_is_based_after_its_big_levels);
    RUN(the_texture_address_has_26_bits_on_the_vsa100);
    RUN(compressed_levels_are_sized_as_the_tmu_walks_them);
    RUN(compressed_bases_are_3dfxs);
    RUN(a_chain_is_packed_and_bounded_as_the_tmu_walks_it);
    RUN(what_the_vsa100_path_refuses);
    RUN(the_write_back_stays_where_the_default_path_proved_it);
})
