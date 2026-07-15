/*
 * fxd3d.h - internal types for the clean-room Direct3D HAL (fxD3D).
 *
 * Shared by the HAL modules (d3dhal_*.c). The HAL translates DirectX 6/7
 * fixed-function DDI operations into calls on the Glide backend seam
 * (glidebackend.h). See docs/3dfx-d3d-hal-design.md.
 *
 * The Microsoft D3D DDI structures (D3DHAL_*, D3DDEVICEDESC, DP2 tokens) come
 * from the DDK. To keep this tree compilable WITHOUT the DDK on the build host,
 * we define the small subset we use behind -DHAVE_DDK: when the DDK is present
 * (the real driver build) its headers win; otherwise these minimal mirrors let
 * the translation logic compile and be unit-tested against synthetic buffers.
 *
 * Clean-room: ***REMOVED***. The DDI shapes below are the public, documented
 * Microsoft interface (the same any DDK driver implements).
 */
#ifndef FXD3D_H
#define FXD3D_H

#include "glidebackend.h"

#ifdef HAVE_DDK
#  include <ddrawi.h>
#  include <d3dhal.h>
#else
/* ---- minimal public DDI mirrors (documented MS shapes) ------------------- */
typedef unsigned long  DWORD;
typedef unsigned short WORD;
typedef unsigned char  BYTE;
typedef float          D3DVALUE;

/* DP2 command opcodes we handle (subset of D3DHAL_DP2OPERATION). */
enum {
    FXD2_RENDERSTATE      = 8,
    FXD2_TEXTURESTAGESTATE= 28,
    FXD2_POINTS           = 1,
    FXD2_LINELIST         = 2,
    FXD2_TRIANGLELIST     = 5,
    FXD2_TRIANGLESTRIP    = 6,
    FXD2_TRIANGLEFAN      = 7,
    FXD2_TEXBLT           = 31,
    FXD2_SETRENDERTARGET  = 32
};

/* A DP2 command header: opcode + count, followed by `count` operands. */
typedef struct { WORD bCommand; BYTE bReserved; BYTE wStateCount; DWORD dwCount; } fxd2_hdr;

/* One TL vertex as DX6/7 delivers it (screen space, pre-transformed). */
typedef struct {
    D3DVALUE x, y, z, rhw;      /* rhw = 1/w                                 */
    DWORD    color, specular;   /* ARGB                                       */
    D3DVALUE tu, tv;            /* stage-0 texcoords                          */
} fxd_tlvertex;

/* render-state / tss operand pair */
typedef struct { DWORD state; DWORD value; } fxd_statepair;
#endif /* HAVE_DDK */

/* ---- HAL device state ---------------------------------------------------- */

typedef struct fxd_tex fxd_tex;   /* opaque; wraps a gb_tex_t + D3D handle     */

typedef struct {
    /* shadow of the fixed-function state, flushed to the backend lazily */
    int  z_enable, z_func, z_write;
    int  blend_enable, src_blend, dst_blend;
    int  atest_enable, atest_func; unsigned atest_ref;
    int  fog_enable; unsigned fog_color; float fog_density;
    int  cull; int shade; int dither; int ckey_enable; unsigned ckey;
    unsigned tex_factor;
    /* stage 0 */
    int  color_op, alpha_op; int min_filter, mag_filter, addr_u, addr_v;
    fxd_tex *bound_tex;
    int  dirty;                 /* state changed since last draw             */
    int  width, height, depth;  /* current render target                     */
} fxd_device;

/* ---- module entry points ------------------------------------------------- */

/* caps: fill a device-description caps blob the runtime queries. Returns the
 * number of texture stages / formats advertised. */
int  fxd_fill_caps(void *d3ddevicedesc /*D3DDEVICEDESC* when HAVE_DDK*/);

/* state: apply one render-state or texture-stage-state pair to the device
 * shadow (does not touch hardware until fxd_flush). Returns 1 if recognized. */
int  fxd_set_renderstate(fxd_device *dev, DWORD state, DWORD value);
int  fxd_set_tss(fxd_device *dev, DWORD stage, DWORD state, DWORD value);

/* flush shadow -> Glide backend (called before a draw). */
void fxd_flush(fxd_device *dev);

/* textures */
fxd_tex *fxd_tex_create(int w, int h, DWORD d3dformat);
void     fxd_tex_load(fxd_tex *t, const void *pixels);
void     fxd_tex_destroy(fxd_tex *t);

/* primitives: convert a DP2 TL-vertex run to backend draws. */
void fxd_draw(fxd_device *dev, int prim_type, const fxd_tlvertex *v, int count);

/* ddi: walk a DP2 command buffer, dispatching to the above. Returns #ops. */
int  fxd_dp2_execute(fxd_device *dev, const void *cmdbuf, int len);

/* device lifecycle */
void fxd_device_init(fxd_device *dev, int w, int h, int depth);

#endif /* FXD3D_H */
