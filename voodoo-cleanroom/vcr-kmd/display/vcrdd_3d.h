/*
 * vcrdd_3d.h - the 3D engine interface between the Direct3D HAL
 * (vcrdd_d3d.c, integer only) and the register writer (vcrdd_3d.c, FPU).
 *
 * vcr3d_draw carries floats: the HAL never reads or writes its float fields
 * itself (it is compiled -mgeneral-regs-only) - VcrDd3dDrawInit and
 * VcrDd3dTexScale fill them, inside the HAL's EngSaveFloatingPointState bracket.
 */
#ifndef VCRDD_3D_H
#define VCRDD_3D_H

#include "../include/vcr_3dregs.h"

typedef struct vcr3d_target {
    ULONG rt_off, rt_pitch;         /* colour buffer: offset in video memory, bytes/row */
    ULONG z_off, z_pitch;           /* depth buffer (z_off 0: none) */
    ULONG width, height;
} vcr3d_target;

typedef struct vcr3d_regs {
    ULONG fbzColorPath, fbzMode, alphaMode, fogMode, fogColor, c0, c1;
    ULONG fog_table[4];             /* VCR_FOG_* mode, start, end, density (float bits) */
    ULONG setupMode;
    ULONG textured;
    ULONG textureMode, tLOD, texBaseAddr;           /* TMU0 */
    ULONG textured1;                                /* two stages: TMU1 samples stage 0 */
    ULONG textureMode1, tLOD1, texBaseAddr1;        /* TMU1 */
} vcr3d_regs;

typedef struct vcr3d_draw {
    ULONG textured;
    ULONG diff_off;                 /* byte offset of the diffuse colour in a vertex (0: none) */
    ULONG tex_off;                  /* byte offset of TMU0's texture coordinates */
    ULONG textured1;                /* TMU1 in use (two stages) */
    ULONG tex1_off;                 /* byte offset of TMU1's texture coordinates */
    ULONG spec_off;                 /* byte offset of the specular colour (0: none) */
    ULONG fog_vertex;               /* Wfbi carries the fog factor (specular alpha) */
    float s_scale, t_scale;         /* u, v (0..1) -> S, T (the wider side spans 256) */
    float s_scale1, t_scale1;       /* the same for TMU1 */
    float xy_bias;                  /* added to x and y: the pixel-centre convention */
} vcr3d_draw;

#define VCR3D_CLEAR_COLOR   1
#define VCR3D_CLEAR_Z       2

BOOL VcrDd3dTarget(VCR_PDEV *pd, const vcr3d_target *t);
BOOL VcrDd3dState(VCR_PDEV *pd, const vcr3d_regs *r);
BOOL VcrDd3dClear(VCR_PDEV *pd, const vcr3d_target *t, ULONG what, ULONG argb, ULONG zbits,
                  const RECTL *rc, ULONG n);
BOOL VcrDd3dTriangle(VCR_PDEV *pd, const vcr3d_draw *d, const UCHAR *a, const UCHAR *b,
                     const UCHAR *c);
void VcrDd3dTexScale(vcr3d_draw *d, ULONG w, ULONG h);
BOOL VcrDd3dTexFlush(VCR_PDEV *pd, ULONG base, ULONG addr);
void VcrDd3dTexScale1(vcr3d_draw *d, ULONG w, ULONG h);
void VcrDd3dDrawInit(vcr3d_draw *d);

#endif /* VCRDD_3D_H */
