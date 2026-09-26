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

#endif /* VCR_TEXLOD_H */
