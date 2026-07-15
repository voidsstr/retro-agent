/*
 * ddi_glue.h - the bridge between the Windows Direct3D DDI and fxD3D.
 *
 * The host display driver (NT miniport+DLL, or 9x minivdd+DDHAL) hands the
 * runtime a table of D3D callbacks. The single one that matters for rendering
 * is DrawPrimitives2. This glue is what the driver's DrawPrimitives2 callback
 * calls: it hands the DP2 command buffer to fxD3D's dispatcher (fxd_dp2_execute)
 * and manages the per-context fxd_device shadow.
 *
 * It is deliberately split out and written to compile WITHOUT the DDK, so the
 * bridge logic is testable on the build host; the DDK struct unpacking lives in
 * the driver's callback (nt/enable.c, win9x/d3dcb.c) behind -DHAVE_DDK.
 */
#ifndef FXD3D_DDI_GLUE_H
#define FXD3D_DDI_GLUE_H

#include "fxd3d.h"

#ifdef __cplusplus
extern "C" {
#endif

/* One rendering context (maps to a D3D device / DDraw local). */
typedef struct {
    fxd_device dev;
    int        opened;      /* backend mode opened for this context           */
} fxd_context;

/* Create/destroy a context for a render target of the given size/depth. */
fxd_context *fxdglue_context_create(int w, int h, int depth);
void         fxdglue_context_destroy(fxd_context *c);

/* Bind a texture (created via fxdglue_tex_*) as the context's current source. */
void         fxdglue_set_texture(fxd_context *c, fxd_tex *t);

/* THE hot path: run a DP2 command buffer for this context. `cmd`/`len` are the
 * raw command-buffer bytes the runtime supplied (the driver's DrawPrimitives2
 * extracts these from D3DHAL_DRAWPRIMITIVES2DATA). Returns #ops executed. */
int          fxdglue_draw_primitives2(fxd_context *c, const void *cmd, int len);

/* Fill the caps blob the runtime queries (D3DDEVICEDESC under HAVE_DDK). */
int          fxdglue_get_caps(void *devicedesc);

/* Texture create/load/destroy, forwarded to the HAL. */
fxd_tex     *fxdglue_tex_create(int w, int h, unsigned d3dformat);
void         fxdglue_tex_load(fxd_tex *t, const void *pixels);
void         fxdglue_tex_destroy(fxd_tex *t);

#ifdef __cplusplus
}
#endif
#endif /* FXD3D_DDI_GLUE_H */
