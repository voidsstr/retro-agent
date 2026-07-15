/*
 * ddi_glue.c - portable bridge from the D3D DDI to fxD3D (see .h).
 *
 * Compiles without the DDK. The driver's DrawPrimitives2 callback (which DOES
 * need the DDK to unpack D3DHAL_DRAWPRIMITIVES2DATA) reduces to: find the
 * command-buffer pointer+length, then call fxdglue_draw_primitives2(). Keeping
 * that reduction here means the whole render path is one testable unit and the
 * DDK-specific code in the driver is only a few lines of struct access.
 */
#include "ddi_glue.h"
#include <stdlib.h>

fxd_context *fxdglue_context_create(int w, int h, int depth){
    fxd_context *c = calloc(1, sizeof(*c));
    if(!c) return 0;
    fxd_device_init(&c->dev, w, h, depth);
    /* NOTE: the backend mode (gb_open) is opened by the host driver's modeset
     * path, not here - the display driver already owns the resolution. In the
     * standalone test/86Box path, the caller opens the backend first. */
    c->opened = 0;
    return c;
}

void fxdglue_context_destroy(fxd_context *c){ free(c); }

void fxdglue_set_texture(fxd_context *c, fxd_tex *t){
    if(c) c->dev.bound_tex = t;
}

int fxdglue_draw_primitives2(fxd_context *c, const void *cmd, int len){
    if(!c || !cmd || len <= 0) return 0;
    return fxd_dp2_execute(&c->dev, cmd, len);
}

int fxdglue_get_caps(void *devicedesc){
    return fxd_fill_caps(devicedesc);
}

fxd_tex *fxdglue_tex_create(int w, int h, unsigned d3dformat){
    return fxd_tex_create(w, h, (DWORD)d3dformat);
}
void fxdglue_tex_load(fxd_tex *t, const void *pixels){ fxd_tex_load(t, pixels); }
void fxdglue_tex_destroy(fxd_tex *t){ fxd_tex_destroy(t); }
