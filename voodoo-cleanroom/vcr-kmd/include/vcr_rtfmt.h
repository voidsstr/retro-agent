/*
 * vcr_rtfmt.h - which Direct3D render targets the 3D engine can draw into,
 * and how a 32 bpp one is programmed. Integer only and Win32-free: the HAL
 * (vcrdd_d3d.c, -mgeneral-regs-only) decides with it, and the host test
 * compiles it as is.
 *
 * The chips:
 *   Banshee / Voodoo3  the 3D engine renders 16 bpp only (no renderMode
 *                      register: 0x1e0 is reserved). A 32 bpp target is
 *                      REFUSED - the caps never offer one, and a context or a
 *                      SETRENDERTARGET that names one draws nothing.
 *   VSA-100            renderMode[1:0] picks the 3D pixel size: 0 = 16 bpp,
 *                      2 = 32 bpp (h5 h3defs.h SST_RM_16BPP/SST_RM_32BPP,
 *                      _grRenderMode in gsst.c). There is no separate format
 *                      for the aux (depth) buffer: in 32 bpp it is 32 bits a
 *                      pixel, 24 of depth under 8 of stencil (gglide.c
 *                      grBufferClear: "the depth buffer is 24bpp"; clears put
 *                      (stencil << 24) | depth) - so a 32 bpp target needs a
 *                      32-bit Z (D24X8 / D24S8) and a 16 bpp one a 16-bit Z.
 *                      A mismatch is refused, as the DX7-DDI runtime itself
 *                      treats a DX7 driver's Z (D3D8's CheckDepthStencilMatch
 *                      wants equal depths without the DX8 format op).
 *
 * Depth values: 16 bpp iterates and clears Z in 0..0xffff, 32 bpp in
 * 0..0xffffff (diget.c GR_ZDEPTH_MIN_MAX: NAPALM_ZDEPTHVALUE_NEAREST for a
 * 4-byte pixel; MesaFX over Glide scales window Z by DepthMaxF = 2^24-1).
 */
#ifndef VCR_RTFMT_H
#define VCR_RTFMT_H

#include "vcr_3dregs.h"

#define VCR_RT_REFUSED  0u
#define VCR_RT_16       16u
#define VCR_RT_32       32u

/* the 3D pixel size for a colour target of rt_bits with a Z of z_bits
 * (0: no Z buffer), or VCR_RT_REFUSED */
static __inline unsigned vcr_rt_format(unsigned napalm, unsigned rt_bits, unsigned z_bits)
{
    if (rt_bits == 16)
        return (z_bits == 0 || z_bits == 16) ? VCR_RT_16 : VCR_RT_REFUSED;
    if (rt_bits == 32 && napalm)
        return (z_bits == 0 || z_bits == 32) ? VCR_RT_32 : VCR_RT_REFUSED;
    return VCR_RT_REFUSED;          /* 8/24 bpp, or 32 bpp on a Banshee/Voodoo3 */
}

/* the largest depth value the aux buffer holds (a clear to 1.0 writes this) */
static __inline unsigned vcr_rt_zmax(unsigned fmt)
{
    return fmt == VCR_RT_32 ? 0xffffffu : 0xffffu;
}

/* renderMode for the target: pixel size + every channel's write enable
 * (VSA-100 only - never written on a Banshee/Voodoo3) */
static __inline unsigned vcr_rt_rendermode(unsigned fmt)
{
    return (fmt == VCR_RT_32 ? RM_32BPP : RM_16BPP) | RM_RGBA_WRITE;
}

#endif /* VCR_RTFMT_H */
