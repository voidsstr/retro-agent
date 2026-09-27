/*
 * vcr_rtfmt.h - which Direct3D render targets the 3D engine can draw into,
 * and how a 32 bpp one is programmed. Integer only and Win32-free: the HAL
 * (vcrdd_d3d.c, -mgeneral-regs-only) decides with it, and the host tests
 * (tests/python/test_vcr_kmd_d3d.py, tests/native/test_vcr_kmd_d3dseq.c)
 * compile it as is.
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
 *                      pixel, 24 of depth under 8 of STENCIL (gglide.c
 *                      grBufferClear: "the depth buffer is 24bpp") - so a
 *                      32 bpp target needs a 32-bit Z (D24X8 / D24S8) and a
 *                      16 bpp one a 16-bit Z. A mismatch is refused, as the
 *                      DX7-DDI runtime itself treats a DX7 driver's Z (D3D8's
 *                      CheckDepthStencilMatch wants equal depths without the
 *                      DX8 format op).
 *                      32 bpp targets are offered only when the miniport's
 *                      Diag\D3D32 = 1 (default OFF, VCR_INFO_F_D3D32): the
 *                      32 bpp path has not run on silicon, and .124's desktop
 *                      is 32 bpp, so without the switch every windowed D3D
 *                      client there would take it. Off, a VSA-100 answers
 *                      exactly as the proven 16 bpp HAL did (DDBD_16, D16).
 *
 * STENCIL IS NOT SUPPORTED. The stencil byte of a 24+8 aux buffer is written
 * and tested only through stencilMode/stencilOp (0x1e4/0x1e8, h3defs.h
 * SST_STENCIL_*): with SST_STENCIL_ENABLE and the stencil write mask clear
 * the chip neither reads nor writes those planes. The HAL writes both 0 at
 * every 32 bpp target setup (vcr_3dseq.h), so a stencil state a
 * Glide/OpenGL session left behind cannot fail or overwrite our draws. A
 * 16 bpp target has no stencil byte and keeps the proven sequence, write for
 * write (the pair is also in the Diag\Reset3D release reset).
 * zaColor[31:24] is NOT the stencil clear value - it is the ALPHA field
 * (h3defs.h SST_ZACOLOR_ALPHA, [23:0] SST_ZACOLOR_DEPTH); a stencil clear
 * would be a fastfill with stencilMode's REF + WMASK set, which we never do:
 * D3DCLEAR_STENCIL is dropped and dwStencilCaps is 0. D24S8 stays LISTED at
 * 32 bpp anyway: D3D8 applications that ask for a stencil format by name
 * (and CheckDepthStencilMatch) would otherwise find no 24-bit Z with the
 * layout 3dfx's own V5 HAL publishes, and a D24S8 surface IS the same 32-bit
 * storage as D24X8 - its stencil bits simply never change.
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
 * (0: no Z buffer), or VCR_RT_REFUSED. rt32: 32 bpp targets are allowed - a
 * VSA-100 with Diag\D3D32 = 1 (never a Banshee/Voodoo3) */
static __inline unsigned vcr_rt_format(unsigned rt32, unsigned rt_bits, unsigned z_bits)
{
    if (rt_bits == 16)
        return (z_bits == 0 || z_bits == 16) ? VCR_RT_16 : VCR_RT_REFUSED;
    if (rt_bits == 32 && rt32)
        return (z_bits == 0 || z_bits == 32) ? VCR_RT_32 : VCR_RT_REFUSED;
    return VCR_RT_REFUSED;          /* 8/24 bpp, or 32 bpp not allowed here */
}

/* may the register writer program a target of this format? Anything but
 * 16, or 32 where 32 is allowed, is refused on EVERY chip - VSA-100 too:
 * renderMode would otherwise be written for a size nobody decided on */
static __inline int vcr_rt_programmable(unsigned rt32, unsigned fmt)
{
    return fmt == VCR_RT_16 || (fmt == VCR_RT_32 && rt32);
}

/* why a colour/Z pair is not a target (VCR_RT_OK: it is) - the HAL logs it */
#define VCR_RT_OK           0u
#define VCR_RT_WHY_FORMAT   1u      /* the size pair: vcr_rt_format refused it */
#define VCR_RT_WHY_ZOFF     2u      /* a Z surface with no video-memory offset */
#define VCR_RT_WHY_ZPITCH   3u      /* Z pitch under a target row, not 16-aligned, or past 0x3fff */
#define VCR_RT_WHY_ZSIZE    4u      /* the Z smaller than the target */
#define VCR_RT_WHY_RTPITCH  5u      /* the target's own pitch: under a row, not 16-aligned, or past 0x3fff */

/* the colour target's own pitch, by the same rule as its Z (vcr_rt_zcheck):
 * at least a row, a multiple of 16 bytes, inside the 14-bit stride field.
 * The DP2 walk has refused to draw into an unaligned target since the first
 * HAL (drawable needs (rt_pitch & 0xf) == 0), while Clear2 still fastfilled
 * it and ContextCreate accepted it - three answers for one surface. Here it
 * is ONE answer, at target_of, so ContextCreate, SETRENDERTARGET, DP2 and
 * Clear2 all refuse the same target, and the refusal is logged (event 513
 * what 15) instead of a device that silently draws nothing. The heap asks for
 * no pitch alignment (vcrdd_ddraw.c), so a windowed client whose width is not
 * a multiple of 8 pixels at 16 bpp reaches this; every proven size (the
 * fullscreen modes, d3dprobe's 256x256 window) is a multiple of 16 bytes. */
static __inline unsigned vcr_rt_rtcheck(unsigned fmt, unsigned width, unsigned rt_pitch)
{
    unsigned bytes = fmt == VCR_RT_32 ? 4u : 2u;
    if (rt_pitch < width * bytes || (rt_pitch & 0xfu) || rt_pitch > 0x3fffu)
        return VCR_RT_WHY_RTPITCH;
    return VCR_RT_OK;
}

/* the Z buffer against the target it serves. z_set: a Z surface is
 * attached (whether or not it is usable); z_off: its offset in video memory
 * (0 when it is not in video memory - the aux buffer would land at 0);
 * z_pitch: bytes a row; z_w/z_h: its size. A target of width x height
 * writes width * (fmt / 8) bytes of every Z row, over height rows. */
static __inline unsigned vcr_rt_zcheck(unsigned fmt, unsigned width, unsigned height,
                                       unsigned z_set, unsigned z_off, unsigned z_pitch,
                                       unsigned z_w, unsigned z_h)
{
    unsigned bytes = fmt == VCR_RT_32 ? 4u : 2u;
    if (!z_set)
        return VCR_RT_OK;
    if (!z_off)
        return VCR_RT_WHY_ZOFF;
    if (z_pitch < width * bytes || (z_pitch & 0xfu) || z_pitch > 0x3fffu)
        return VCR_RT_WHY_ZPITCH;
    if (z_w < width || z_h < height)
        return VCR_RT_WHY_ZSIZE;
    return VCR_RT_OK;
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

/* the Z formats GUID_ZPixelFormats answers: the Z of every RENDER depth the
 * HAL offers, not of the desktop. D16 always - 16 bpp targets exist on every
 * chip, and a 16 bpp fullscreen device made from .124's 32 bpp desktop is the
 * proven lane; the two 32-bit entries beside it with rt32 (DDBD_32 offered).
 * The D3D8 runtime checks an application's depth format against the list it
 * read AT THE DESKTOP MODE, before the fullscreen switch, so a list that
 * followed the desktop (4a9793b: the 32-bit pair alone at a 32 bpp desktop
 * with Diag\D3D32) took D16 away from exactly that proven device the moment
 * the switch was armed. A pair of the wrong sizes an application may still
 * pick costs nothing on the chip: D3D8's CheckDepthStencilMatch answers no
 * for a DX7-DDI HAL, and ContextCreate refuses it (vcr_rt_format) before any
 * register write. Without rt32: D16 alone, as the proven 16 bpp HAL. */
#define VCR_ZL_D16      1u
#define VCR_ZL_D24X8    2u
#define VCR_ZL_D24S8    4u
static __inline unsigned vcr_rt_zlist(unsigned rt32)
{
    return VCR_ZL_D16 | (rt32 ? (VCR_ZL_D24X8 | VCR_ZL_D24S8) : 0u);
}

#endif /* VCR_RTFMT_H */
