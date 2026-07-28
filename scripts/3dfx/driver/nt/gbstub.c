/*
 * gbstub.c - driver-side Glide backend PLACEHOLDER for the fxD3D NT driver.
 *
 * The fxD3D core (d3dhal_state.c / d3dhal_tex.c / d3dhal_prim.c) drives the
 * hardware exclusively through the gb_* seam declared in
 * d3dhal/glidebackend.h. On the user-mode bring-up / gfxbench path that seam is
 * satisfied by d3dhal/glidebackend.c, which opens a fullscreen Glide context
 * through a GDI window and calls glide3x.dll.
 *
 * That user-mode backend is NOT linkable into a native (subsystem:native,
 * -nodefaultlib) kernel display driver: it pulls user32 window creation and the
 * glide3x import. The real driver-side backend is Glide's own kernel init
 * (minihwc / cinit) compiled directly into the driver - a later milestone (see
 * driver/nt/SOURCES). Until that lands, this file provides the gb_* seam as
 * link-time placeholders so the DDI chassis + fxD3D core link into a loadable
 * fxd3ddd.dll. Every body is a no-op that returns a safe value.
 *
 * TODO(fxd3d M4): replace this file with the Glide minihwc/cinit driver-side
 *                 backend so gb_* actually programs the Voodoo.
 *
 * Clean-room: matches only the public gb_* seam in glidebackend.h.
 */
#include "../../d3dhal/glidebackend.h"
#include "gbkernel.h"     /* gbkernel_attach/detach - stubbed for the no-hw link */
#include "gbkdebug.h"     /* gbkernel_dbg_* bring-up ladder - stubbed here too    */

#ifdef HAVE_DDK

/* Concrete texture handle. The core treats gb_tex_t as opaque, so a minimal
 * placeholder body is enough to link; the real backend stores TMU addresses. */
struct gb_tex {
    int   w, h;
    int   fmt;
};

/* ---- fixed-function render state (placeholders) -------------------------- */
void gb_set_shade(gb_shade_t s)                              { (void)s; }
void gb_set_depth(int enable, int func, int write)           { (void)enable; (void)func; (void)write; }
void gb_set_blend(int enable, int src, int dst)              { (void)enable; (void)src; (void)dst; }
void gb_set_alphatest(int enable, int func, unsigned char r) { (void)enable; (void)func; (void)r; }
void gb_set_fog(int enable, unsigned rgb, float density)     { (void)enable; (void)rgb; (void)density; }
void gb_set_cull(gb_cull_t c)                                { (void)c; }
void gb_set_dither(int enable)                               { (void)enable; }
void gb_set_chromakey(int enable, unsigned rgb)             { (void)enable; (void)rgb; }
void gb_set_texfactor(unsigned argb)                        { (void)argb; }
void gb_set_texfilter(int bilinear, int clamp_u, int clamp_v){ (void)bilinear; (void)clamp_u; (void)clamp_v; }

/* ---- textures (placeholders) --------------------------------------------- */
gb_tex_t *gb_tex_create(int w, int h, gb_texfmt_t fmt)
{
    /* TODO(fxd3d M4): allocate a TMU texture via Glide (grTexTextureMemRequired
     * + grTexDownloadMipMap). Return NULL for now - no backend memory. */
    (void)w; (void)h; (void)fmt;
    return (gb_tex_t *)0;
}
void gb_tex_upload(gb_tex_t *t, const void *pixels)          { (void)t; (void)pixels; }
void gb_tex_bind(gb_tex_t *t)                                { (void)t; }
void gb_tex_none(void)                                       { }
void gb_tex_destroy(gb_tex_t *t)                             { (void)t; }

/* ---- geometry (placeholders) --------------------------------------------- */
void gb_draw_tris(const gb_vtx_t *v, int count)             { (void)v; (void)count; }
void gb_draw_points(const gb_vtx_t *v, int count)           { (void)v; (void)count; }
void gb_draw_lines(const gb_vtx_t *v, int count)            { (void)v; (void)count; }

/* ---- gbkernel entry + bring-up-ladder placeholders ----------------------- *
 * chassis.c (DrvEnableSurface) now calls gbkernel_attach, and the DrvEscape
 * ladder (gbkdebug.c) calls gbkernel_dbg_*; provide inert bodies so the no-hw
 * smoke link (gbstub.c in place of gbkernel.c + gbk\*.c) still resolves. attach
 * FAILS (-1) so the chassis degrades to a 2D-only surface, exactly as when the
 * real backend can't bring the board up. */
int gbkernel_attach(void *bar0va, void *bar1va, unsigned vramBytes,
                    unsigned desktopEnd, unsigned desktopStride,
                    unsigned desktopBpp, int w, int h, int depthBytes)
{
    (void)bar0va; (void)bar1va; (void)vramBytes; (void)desktopEnd;
    (void)desktopStride; (void)desktopBpp; (void)w; (void)h; (void)depthBytes;
    return -1;
}
void gbkernel_detach(void) { }

/* backend down -> the DDraw surface path refuses vidmem placement */
int gbkernel_get_meminfo(gbk_meminfo_t *out)
{
    if (out != 0) {
        out->vramBytes = 0;
        out->surfSize  = 0;
    }
    return -1;
}

int gbkernel_dbg_probe(fxdbg_probe_t *out)
{
    if (out != 0) {
        char *p = (char *)out;
        unsigned i;
        for (i = 0; i < sizeof(*out); i++)
            p[i] = 0;
        out->magic = FXDBG_MAGIC;
    }
    return 0;
}
int gbkernel_dbg_clear(unsigned long r, unsigned long g, unsigned long b)
{ (void)r; (void)g; (void)b; return 0; }
int gbkernel_dbg_tri(void) { return 0; }
int gbkernel_dbg_tex(void) { return 0; }
unsigned long gbkernel_dbg_readback(unsigned long which,
                                    unsigned long x, unsigned long y,
                                    unsigned long w, unsigned long h,
                                    void *out, unsigned long outMax)
{ (void)which; (void)x; (void)y; (void)w; (void)h; (void)out; (void)outMax; return 0; }
int gbkernel_dbg_faulted(void) { return 0; }

#endif /* HAVE_DDK */
