/*
 * vcr_texlod.h - where the TMU of a Banshee / Voodoo3 / VSA-100 looks for a
 * texture, as the Direct3D HAL programs it (display/vcrdd_d3d.c). Pure
 * arithmetic, host-tested (tests/native/test_vcr_kmd_texlod.c).
 *
 * The TMU addresses a mipmap as if LOD 0 - 256 texels on the wide side -
 * started at texBaseAddr, and finds level n by adding the sizes of levels
 * 0 .. n-1 (for the texture's aspect ratio). A texture whose largest level is
 * smaller than 256 therefore gets a base that lies BEFORE its first texel:
 * its own address minus the levels it does not have. (Glide computes the
 * same, _grTexCalcBaseAddress; 86Box's voodoo_recalc_tex12 walks it back.)
 */
#ifndef VCR_TEXLOD_H
#define VCR_TEXLOD_H

#include "vcr_types.h"

typedef struct vcr_texlod {
    vcr_u32 lod;            /* 0 = 256 on the wide side ... 8 = 1 */
    vcr_u32 aspect;         /* log2(wide / narrow), 0..3 */
    vcr_u32 s_is_wider;     /* width > height */
    vcr_u32 base;           /* texBaseAddr: address of the would-be LOD 0, 16-byte field */
    vcr_u32 tlod;           /* tLOD: LODMIN = LODMAX = lod, aspect, S_IS_WIDER */
} vcr_texlod;

/* 0 on success; -1 if the chip cannot sample it (not a power of two, larger
 * than 256, an aspect beyond 8:1, a row pitch that is not width * bytes) */
int vcr_texlod_compute(vcr_u32 w, vcr_u32 h, vcr_u32 bytes, vcr_u32 pitch, vcr_u32 offset,
                       vcr_texlod *out);

/* ---- the VSA-100 texture path (Diag\D3DBigTex, default OFF) ------------------------
 * vcr_texlod_compute above is the Banshee / Voodoo3 answer and stays exactly
 * that: the HAL uses it whenever the switch is off, and on every Banshee /
 * Voodoo3. The VSA-100 path adds, each from 3dfx's GPL h5 Glide:
 *
 *  - textures up to 2048 (grTexSource, _g3LodXlat): a texture wider than 256
 *    sets tLOD TBIG (bit 30) and its LODs count from 2048 (lod = 11 - log2 of
 *    the wide side), and texBaseAddr STILL points at where the 256-level
 *    would be - "even on Napalm texBaseAddr *always* points at the 256x256
 *    mipmap level" (gtex.c _grTexCalcBaseAddressTiled). The levels above 256
 *    lie before it: base = the texture's address + the sizes of its levels
 *    wider than 256 (ditex.c _grMipMapOffset, the negative entries). S and T
 *    still span 256 on the wide side (grTexSource's s_scale, MesaFX's
 *    fxTexGetInfo): nothing changes in the vertex path;
 *  - the whole texture address: h5 h3defs.h SST_TEXTURE_ADDRESS is bits 24:4
 *    with address bit 25 in bit 1 (SST_TEXTURE_MUNGE_ADDRESS) - a 64 MB chip.
 *    The Voodoo3's field is bits 23:4 (h3 h3defs.h), which is what
 *    vcr_texlod_compute masks to: on a VSA-100 a texture above 16 MB is
 *    sampled 16 MB lower (.124's DirectDraw heap is 0 - 27 MB);
 *  - two more storages: ARGB8888 (4 bytes a texel, textureMode format 15) and
 *    the compressed ones, with textureMode bit 31 (SST_COMPRESSED_TEXTURES)
 *    and format 1 = DXT1, 2 = DXT2/3, 3 = DXT4/5 (h5 h3defs.h SST_DXT*).
 *    Their data is the D3D/S3TC block stream as it is (MesaFX hands Glide the
 *    application's blocks with a memcpy, and Glide's download writes them
 *    linearly), but the TMU sizes a level by its own rule: DXT1 in 8x4 units -
 *    two 4x4 blocks side by side - so a DXT1 level narrower than 8 is laid out
 *    differently from D3D's (3dfx's own HAL pads those, pad_dxt1), and the
 *    8-bit ones in 4x4 units (_grMipMapOffsetCmp4Bit / _grMipMapOffsetDXT,
 *    _grMipMapHostWHCmp4Bit / DXT).
 *
 * Pure arithmetic, host-tested (tests/native/test_vcr_kmd_texlod.c). */

/* how the texels of a level are stored */
#define VCR_TEXK_RGB16      0u      /* 2 bytes a texel: RGB565, ARGB1555, ARGB4444 */
#define VCR_TEXK_ARGB32     1u      /* 4 bytes a texel: ARGB8888 (VSA-100 only) */
#define VCR_TEXK_DXT1       2u      /* 4 bits a texel, 8-byte 4x4 blocks; the TMU reads 8x4 */
#define VCR_TEXK_DXT35      3u      /* 8 bits a texel, 16-byte 4x4 blocks (DXT2/3/4/5) */

/* the flags of the path */
#define VCR_TEXF_NAPALM     0x1u    /* texBaseAddr as the VSA-100 takes it (26 bits, munged) */
#define VCR_TEXF_BIG        0x2u    /* textures up to 2048 (TBIG) - VSA-100 only */

#define VCR_TEX_TBIG        (1u << 30)  /* tLOD SST_TBIG */

typedef struct vcr_texlod_ext {
    vcr_texlod t;           /* lod (0 = 256 wide, or 2048 with tbig), aspect, s_is_wider,
                             * base (the REGISTER value) and tlod (TBIG included) */
    vcr_u32 tbig;           /* the texture is wider than 256: LODs count from 2048 */
    vcr_u32 lod_limit;      /* the smallest level the chip has: 8 (1 texel), 11 with tbig */
    vcr_u32 lin_base;       /* base as a video-memory offset (not munged) */
} vcr_texlod_ext;

/* bytes of one w x h level as the TMU sizes (and walks) it */
vcr_u32 vcr_tex_level_bytes(vcr_u32 kind, vcr_u32 w, vcr_u32 h);
/* bytes before level k of a chain whose top is w x h (levels halve to 1 x 1) */
vcr_u32 vcr_tex_chain_offset(vcr_u32 kind, vcr_u32 w, vcr_u32 h, vcr_u32 k);
/* of the `have` levels a chain from w x h has, how many the TMU may sample:
 * the levels whose layout is D3D's - a DXT1 level narrower than 8 is not */
vcr_u32 vcr_tex_chain_usable(vcr_u32 kind, vcr_u32 w, vcr_u32 h, vcr_u32 have);
/* texBaseAddr's encoding: a video-memory offset -> the register (VSA-100) */
vcr_u32 vcr_tex_munge(vcr_u32 lin);
vcr_u32 vcr_tex_unmunge(vcr_u32 reg);

/* The flush's texture-port write-back target on this path (display/vcrdd_d3d.c
 * flush_written -> vcrdd_3d.c VcrDd3dTexFlush): `addr`, or ~0 for none. Only
 * what the default path has always been proven on gets it - a 16 bpp texture
 * no wider than 256 whose video-memory offset is below 16 MB. Above 16 MB the
 * default path never writes back (its 24-bit base is 16 MB lower, so the
 * write-back's range test fails), and the port write of a VSA-100 texture
 * there has never run on the card: with a D3DBigTex bit set it must not start
 * firing for the old kind of texture only because the base now has 26 bits
 * (integration review, 2026-09-28). */
#define VCR_TEX_WRITEBACK_LIMIT 0x1000000u
vcr_u32 vcr_tex_writeback_addr(vcr_u32 kind, vcr_u32 tbig, vcr_u32 addr);

/* 0 on success; -1 if the chip cannot sample it: not a power of two, wider
 * than 256 without VCR_TEXF_BIG (2048 with it), an aspect beyond 8:1, a row
 * pitch that is not width * bytes (uncompressed; a compressed level's pitch
 * is not looked at), a DXT1 texture narrower than 8, a compressed or 32-bit
 * texture without VCR_TEXF_NAPALM, or an unknown kind. With flags 0 and
 * VCR_TEXK_RGB16 the answer is vcr_texlod_compute's, field for field. */
int vcr_texlod_compute_ext(vcr_u32 w, vcr_u32 h, vcr_u32 kind, vcr_u32 pitch, vcr_u32 offset,
                           vcr_u32 flags, vcr_texlod_ext *out);

#endif /* VCR_TEXLOD_H */
