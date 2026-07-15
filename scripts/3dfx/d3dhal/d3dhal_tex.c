/*
 * d3dhal_tex.c - Direct3D texture/surface -> Glide TMU.
 *
 * Maps D3D pixel formats to the nearest VSA-100 TMU format and uploads via the
 * backend. Clean-room: D3D format constants are the documented public values;
 * the hardware side is the open Glide texture API.
 */
#include "fxd3d.h"
#include <stdlib.h>

/* D3DFORMAT FourCC-free codes we accept (documented D3DFMT_* ordinals). */
enum {
    FXFMT_R5G6B5=23, FXFMT_X1R5G5B5=24, FXFMT_A1R5G5B5=25,
    FXFMT_A4R4G4B4=26, FXFMT_A8R8G8B8=21, FXFMT_X8R8G8B8=22, FXFMT_P8=41
};

struct fxd_tex {
    gb_tex_t *g;
    int w, h;
    gb_texfmt_t gfmt;
    int src_bpp;             /* bytes/pixel of the D3D source                 */
    DWORD d3dfmt;
};

static gb_texfmt_t map_fmt(DWORD f, int *src_bpp){
    switch(f){
        case FXFMT_A1R5G5B5: case FXFMT_X1R5G5B5: *src_bpp=2; return GB_TF_ARGB1555;
        case FXFMT_A4R4G4B4:                      *src_bpp=2; return GB_TF_ARGB4444;
        case FXFMT_P8:                            *src_bpp=1; return GB_TF_P8;
        case FXFMT_A8R8G8B8: case FXFMT_X8R8G8B8: *src_bpp=4; return GB_TF_RGB565; /* dithered down */
        case FXFMT_R5G6B5: default:               *src_bpp=2; return GB_TF_RGB565;
    }
}

fxd_tex *fxd_tex_create(int w, int h, DWORD d3dformat){
    fxd_tex *t=calloc(1,sizeof(*t));
    t->w=w; t->h=h; t->d3dfmt=d3dformat;
    t->gfmt=map_fmt(d3dformat,&t->src_bpp);
    t->g=gb_tex_create(w,h,t->gfmt);
    return t;
}

/* Convert an ARGB8888 texel run down to RGB565 (the one non-trivial repack).
 * Other formats already match a TMU format and pass through. */
static void argb8888_to_565(const unsigned char *src, unsigned short *dst, int n){
    int i;
    for(i=0;i<n;i++){
        unsigned b=src[i*4+0], g=src[i*4+1], r=src[i*4+2];
        dst[i]=(unsigned short)(((r&0xF8)<<8)|((g&0xFC)<<3)|(b>>3));
    }
}

void fxd_tex_load(fxd_tex *t, const void *pixels){
    if(t->d3dfmt==FXFMT_A8R8G8B8 || t->d3dfmt==FXFMT_X8R8G8B8){
        unsigned short *tmp=malloc(t->w*t->h*2);
        argb8888_to_565((const unsigned char*)pixels, tmp, t->w*t->h);
        gb_tex_upload(t->g, tmp);
        free(tmp);
    } else {
        gb_tex_upload(t->g, pixels); /* already a TMU-native layout           */
    }
}

void fxd_tex_destroy(fxd_tex *t){ if(t){ gb_tex_destroy(t->g); free(t);} }

/* Bind helper used by the primitive path (keeps the device's filter/addr). */
void fxd__tex_bind(fxd_tex *t){ if(t) gb_tex_bind(t->g); else gb_tex_none(); }
