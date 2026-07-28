/*
 * gbkernel.h - the chassis <-> kernel-Glide-backend entry (fxD3D M4b-2).
 *
 * The gb_* render seam (glidebackend.h) is stateless from the chassis' point
 * of view; the ONE call that brings the board up is gbkernel_attach, made by
 * the M4c chassis from DrvEnableSurface / DrvEnableDirectDraw once the paired
 * video miniport has mapped BAR0 (register aperture) and BAR1 (linear
 * framebuffer). See docs/3dfx-gbkernel-design.md sec 6 ("Single entry").
 */
#ifndef FXD3D_GBKERNEL_H
#define FXD3D_GBKERNEL_H

#ifdef __cplusplus
extern "C" {
#endif

/* Bring the Voodoo3 up for a mode. bar0va/bar1va are the driver-mapped BAR
 * virtual addresses (BAR1 MUST be mapped NON-CACHED - gbk_mmio.h hard
 * requirement); vramBytes is total board memory; desktopEnd is the first byte
 * above the GDI primary surface (so the 3D layout lands above it);
 * desktopStride is the GDI primary's pitch in BYTES (lDeltaScreen) - the
 * blit-present dst uses it, NOT the 16bpp 3D color-buffer stride (M4b-2 minor
 * #2); desktopBpp is the REAL GDI primary depth, stored verbatim (review #7) -
 * 32 makes gb_swap program a 32bpp blit dst so the 2D engine expands 565->8888
 * during the present (the design 5a convert), 16 is the plain copy, and any
 * other depth makes gb_swap SKIP the present (gbk_present_dstformat returns 0)
 * rather than program a corrupt 2D format; depthBytes>0 requests an aux/z
 * buffer. Returns 0 on success, negative if the mode doesn't fit or the FIFO
 * faulted during init. */
int  gbkernel_attach(void *bar0va, void *bar1va, unsigned vramBytes,
                     unsigned desktopEnd, unsigned desktopStride,
                     unsigned desktopBpp, int w, int h, int depthBytes);

/* Disable the CMDFIFO and detach (chassis DrvDisableSurface / mode change). */
void gbkernel_detach(void);

/* Board-memory placement the DDraw surface path (enable.c, M4c-2) builds
 * surfaces on. All offsets are byte offsets into BAR1 / board memory, the
 * same space DDraw surface fpVidMem values live in (the GDI primary is
 * offset 0). surfOffset/surfSize is the region gbkernel RESERVES out of the
 * top of tram for DDraw offscreen surfaces (app-locked textures, the 5a
 * 32bpp RT proxy) - gb_tex_create's own bump allocator never enters it. */
typedef struct gbk_meminfo {
    unsigned vramBytes;      /* total board memory                          */
    unsigned desktopEnd;     /* first byte above the GDI primary            */
    unsigned stride3d;       /* 16bpp 3D color/aux buffer pitch in bytes    */
    unsigned backOffset;     /* back color buffer - the 3D draw target      */
    unsigned auxOffset;      /* 16-bit aux/z buffer offset (0 if none)      */
    unsigned surfOffset;     /* DDraw offscreen-surface region start        */
    unsigned surfSize;       /* DDraw offscreen-surface region length       */
    unsigned width, height;  /* 3D target geometry in pixels                */
    unsigned desktopBpp;     /* GDI primary depth the present targets       */
} gbk_meminfo_t;

/* Fill *out from the live layout. Returns 0 when attached, negative when the
 * backend is down (2D-only degrade) - the caller must then refuse vidmem
 * surface placement rather than invent offsets. */
int  gbkernel_get_meminfo(gbk_meminfo_t *out);

#ifdef __cplusplus
}
#endif
#endif /* FXD3D_GBKERNEL_H */
