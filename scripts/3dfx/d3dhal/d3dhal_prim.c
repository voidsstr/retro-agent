/*
 * d3dhal_prim.c - DP2 primitives (TL vertices) -> Glide draws.
 *
 * DX6/7 hands the driver transformed-and-lit vertices (screen-space XYZRHW +
 * ARGB + UV). Since the Voodoo has no hardware T&L and the runtime already
 * transformed them, we forward directly to the backend - no vertex processing.
 * Triangle strips/fans are expanded to lists here (the backend draws lists).
 *
 * Clean-room: pure format shuffle from the documented D3D TL-vertex to the
 * backend vertex; hardware submission is open Glide.
 */
#include "fxd3d.h"

static void tl_to_gb(gb_vtx_t *o, const fxd_tlvertex *i){
    o->x=i->x; o->y=i->y;
    o->ooz=i->z;              /* depth (W-buffer uses rhw below)               */
    o->w=i->rhw;             /* 1/w for perspective-correct ST                */
    o->b=(unsigned char)(i->color      &0xFF);
    o->g=(unsigned char)((i->color>> 8)&0xFF);
    o->r=(unsigned char)((i->color>>16)&0xFF);
    o->a=(unsigned char)((i->color>>24)&0xFF);
    o->s=i->tu; o->t=i->tv;
}

/* defined in d3dhal_tex.c; declared here at file scope (MSVC C rejects a
 * declaration after statements inside a block). */
extern void fxd__tex_bind(fxd_tex*);

void fxd_draw(fxd_device *dev, int prim, const fxd_tlvertex *v, int count){
    gb_vtx_t g[3];
    int i;
    fxd_flush(dev);
    fxd__tex_bind(dev->bound_tex);

    switch(prim){
        case FXD2_TRIANGLELIST:
            for(i=0;i+2<count;i+=3){
                tl_to_gb(&g[0],&v[i]); tl_to_gb(&g[1],&v[i+1]); tl_to_gb(&g[2],&v[i+2]);
                gb_draw_tris(g,3);
            }
            break;
        case FXD2_TRIANGLESTRIP:
            for(i=2;i<count;i++){
                /* alternate winding to keep facing consistent */
                if(i&1){ tl_to_gb(&g[0],&v[i-1]); tl_to_gb(&g[1],&v[i-2]); }
                else   { tl_to_gb(&g[0],&v[i-2]); tl_to_gb(&g[1],&v[i-1]); }
                tl_to_gb(&g[2],&v[i]);
                gb_draw_tris(g,3);
            }
            break;
        case FXD2_TRIANGLEFAN:
            for(i=2;i<count;i++){
                tl_to_gb(&g[0],&v[0]); tl_to_gb(&g[1],&v[i-1]); tl_to_gb(&g[2],&v[i]);
                gb_draw_tris(g,3);
            }
            break;
        case FXD2_LINELIST:
            for(i=0;i+1<count;i+=2){ tl_to_gb(&g[0],&v[i]); tl_to_gb(&g[1],&v[i+1]); gb_draw_lines(g,2);}
            break;
        case FXD2_POINTS:
            for(i=0;i<count;i++){ tl_to_gb(&g[0],&v[i]); gb_draw_points(g,1);}
            break;
        default: break;
    }
}
