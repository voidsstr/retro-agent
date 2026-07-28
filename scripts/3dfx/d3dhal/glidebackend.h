/*
 * glidebackend.h - the one seam that knows Glide.
 *
 * Thin wrapper over the open Glide3 API (the DLLs built by
 * scripts/3dfx/build-glide.sh). Both the clean-room Direct3D HAL (fxD3D) and
 * the on-card test/benchmark harness (gfxbench) drive the hardware ONLY through
 * this interface, so:
 *   - the driver's hardware path is exercised by a plain user-mode EXE on the
 *     card long before it must work inside a Windows display driver, and
 *   - Glide is swappable (h3 Voodoo3 / h5 Voodoo4-5, or a future raw-register
 *     backend) behind a stable API.
 *
 * Clean-room: this file expresses fixed-function pipeline state in Glide terms
 * only, from public documentation. See docs/3dfx-d3d-hal-design.md.
 */
#ifndef FXD3D_GLIDEBACKEND_H
#define FXD3D_GLIDEBACKEND_H

#ifdef HAVE_DDK
/*
 * Kernel/DDK build (fxd3ddd.dll, GDI_DRIVER): do NOT pull the full user-mode
 * Glide3 SDK header into a native kernel translation unit. <glide.h> drops the
 * entire user-mode gr* prototype surface (grSstWinOpen/grDrawTriangles/...),
 * the floating-point LOD-bias macros, and (via glidesys.h) auto-selects the
 * __WIN32__ user-mode branch - none of which belong in a subsystem:native,
 * -nodefaultlib driver, and any stray gr* call would compile clean here and
 * then demand user-mode glide3x.dll at link time.
 *
 * The clean-room fxD3D core drives the board ONLY through the gb_* seam
 * declared below; the ONLY glide.h symbols any HAVE_DDK translation unit
 * actually references are the GR_CMP_ and GR_BLEND_ argument codes that
 * d3dhal_state.c maps D3D render state onto (these are the seam's numeric
 * convention, also documented in driver/nt/gbk/gbk.h and re-defined glide-free
 * as GBK_GR_* inside gbk_state.c). Provide exactly those, as plain integer
 * constants matching glide.h:214-221 (GR_CMP_*) and :160-169 (GR_BLEND_*), so
 * the core still builds without ever seeing glide.h.
 */
#ifndef GR_CMP_NEVER
#define GR_CMP_NEVER    0x0
#define GR_CMP_LESS     0x1
#define GR_CMP_EQUAL    0x2
#define GR_CMP_LEQUAL   0x3
#define GR_CMP_GREATER  0x4
#define GR_CMP_NOTEQUAL 0x5
#define GR_CMP_GEQUAL   0x6
#define GR_CMP_ALWAYS   0x7
#endif
#ifndef GR_BLEND_ZERO
#define GR_BLEND_ZERO                0x0
#define GR_BLEND_SRC_ALPHA           0x1
#define GR_BLEND_SRC_COLOR           0x2
#define GR_BLEND_DST_ALPHA           0x3
#define GR_BLEND_ONE                 0x4
#define GR_BLEND_ONE_MINUS_SRC_ALPHA 0x5
#define GR_BLEND_ONE_MINUS_SRC_COLOR 0x6
#define GR_BLEND_ONE_MINUS_DST_ALPHA 0x7
#endif
#else
#include <glide.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ---- lifecycle / mode ---------------------------------------------------- */

typedef struct {
    int   width, height;         /* logical resolution                        */
    int   depth;                 /* 16 or 32 (Napalm)                         */
    int   refresh_hz;            /* 60/75/85                                   */
    int   fsaa;                  /* 0 (off), 2, 4, 8 (VSA-100 sample count)    */
    int   double_buffer;         /* 1 double, 2 triple                        */
    int   z_buffer;              /* allocate an aux/z buffer                   */
} gb_mode_t;

/* Init Glide and report the board (chip count, memory, name). Call once. */
int  gb_startup(char *name_out, int name_len, int *num_chips_out);
void gb_shutdown(void);

/* Open/close a fullscreen context for a mode. Returns 0 on success.
 * Maps gb_mode_t onto grSstWinOpen (resolution/refresh/format/buffers) and the
 * VSA-100 FSAA path (grEnable(GR_AA_ORDERED) + sample count). */
int  gb_open(const gb_mode_t *m);
void gb_close(void);

/* Enumerate the resolutions the board reports as usable (bit set of gb indices). */
int  gb_enum_modes(gb_mode_t *out, int max);

/* ---- per-frame ----------------------------------------------------------- */
void gb_clear(unsigned r, unsigned g, unsigned b);   /* clear color+aux       */
void gb_swap(int vsync);                              /* buffer swap           */
void gb_finish(void);                                 /* drain FIFO (bench)    */

/* ---- fixed-function render state (the surface the D3D HAL maps onto) ------ */
/* These are the primitives d3dhal_state.c calls; each is 1-3 gr* calls. */

typedef enum { GB_SHADE_FLAT, GB_SHADE_GOURAUD } gb_shade_t;
typedef enum { GB_CULL_NONE, GB_CULL_CW, GB_CULL_CCW } gb_cull_t;

void gb_set_shade(gb_shade_t s);
void gb_set_depth(int enable, int func /*GR_CMP_* */, int write);
void gb_set_blend(int enable, int src /*GR_BLEND_* */, int dst);
void gb_set_alphatest(int enable, int func /*GR_CMP_* */, unsigned char ref);
void gb_set_fog(int enable, unsigned rgb, float density_linear);
void gb_set_cull(gb_cull_t c);
void gb_set_dither(int enable);
void gb_set_chromakey(int enable, unsigned rgb);
void gb_set_texfactor(unsigned argb);

/* ---- textures ------------------------------------------------------------ */
/* Format codes mirror the D3D formats the HAL will feed; backend picks the TMU
 * format (RGB565/ARGB1555/ARGB4444/P8). data is tightly packed, w/h powers of 2. */
typedef enum { GB_TF_RGB565, GB_TF_ARGB1555, GB_TF_ARGB4444, GB_TF_P8 } gb_texfmt_t;

typedef struct gb_tex gb_tex_t;
gb_tex_t *gb_tex_create(int w, int h, gb_texfmt_t fmt);
void      gb_tex_upload(gb_tex_t *t, const void *pixels /*+ optional palette*/);
void      gb_tex_bind(gb_tex_t *t);       /* select for subsequent draws       */
void      gb_tex_none(void);              /* untextured (gouraud only)          */
void      gb_tex_destroy(gb_tex_t *t);
void      gb_set_texfilter(int bilinear, int clamp_u, int clamp_v);

/* ---- geometry ------------------------------------------------------------ */
/* Screen-space vertex: XYZ (z as 1/w for W-buffering), diffuse RGBA, one UV.
 * This is exactly the DX6/7 TL-vertex the runtime hands the driver, so the HAL
 * forwards it with no transform. */
typedef struct {
    float x, y, ooz;      /* screen x,y ; ooz = 1/w (or z)                     */
    float w;              /* 1/w for perspective-correct ST (q)                */
    unsigned char r, g, b, a;
    float s, t;           /* 0..1 texcoords (scaled to tex size in backend)    */
} gb_vtx_t;

void gb_set_vertex_layout(void);      /* install the GR_PARAM_* layout once    */
void gb_draw_tris(const gb_vtx_t *v, int count /*multiple of 3*/);
void gb_draw_points(const gb_vtx_t *v, int count);
void gb_draw_lines(const gb_vtx_t *v, int count);

#ifdef __cplusplus
}
#endif
#endif /* FXD3D_GLIDEBACKEND_H */
