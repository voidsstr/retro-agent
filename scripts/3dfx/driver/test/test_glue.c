/*
 * test_glue.c - host test for the DDI glue (M3's portable bridge).
 *
 * Exercises the exact path the driver's DrawPrimitives2 callback takes:
 * create a context, bind a texture, feed a DP2 command buffer through
 * fxdglue_draw_primitives2, and assert the render happened. This proves the
 * bridge from "Windows handed us a command buffer" to fxD3D works, with no DDK
 * and no hardware (stub backend). Runs on the Linux build host.
 */
#include "ddi_glue.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>

extern int g_tris;

/* documented D3D constant values reused from the HAL */
enum { RS_ALPHABLENDENABLE=27, RS_SRCBLEND=19, RS_FOGENABLE=28 };
enum { BLEND_SRCALPHA=5 };

static unsigned char buf[256];
static int put_hdr(int off,int cmd,int count){
    fxd2_hdr h; memset(&h,0,sizeof(h));
    h.bCommand=(WORD)cmd; h.dwCount=(DWORD)count;
    memcpy(buf+off,&h,sizeof(h)); return off+(int)sizeof(h);
}
static int put32(int off,DWORD v){ buf[off]=v&0xff;buf[off+1]=(v>>8)&0xff;
    buf[off+2]=(v>>16)&0xff;buf[off+3]=(v>>24)&0xff; return off+4; }

int main(void){
    fxd_context *ctx = fxdglue_context_create(800,600,16);
    fxd_tex *tex = fxdglue_tex_create(64,64, 23 /*R5G6B5*/);
    int off=0, ops;
    assert(ctx && tex);

    fxdglue_set_texture(ctx, tex);

    /* build: enable blend + set src blend, then draw two triangles */
    off=put_hdr(off, FXD2_RENDERSTATE, 2);
    off=put32(off,RS_ALPHABLENDENABLE); off=put32(off,1);
    off=put32(off,RS_SRCBLEND);         off=put32(off,BLEND_SRCALPHA);

    off=put_hdr(off, FXD2_TRIANGLELIST, 6);
    {
        fxd_tlvertex v[6]; int k;
        memset(v,0,sizeof(v));
        for(k=0;k<6;k++){ v[k].x=k*40; v[k].y=k*30; v[k].z=.5f; v[k].rhw=1;
                          v[k].color=0x80FFFFFF; }
        memcpy(buf+off, v, sizeof(v)); off+=(int)sizeof(v);
    }

    ops = fxdglue_draw_primitives2(ctx, buf, off);
    printf("glue: ops=%d tris=%d\n", ops, g_tris);
    assert(ops==2);
    assert(g_tris==2);   /* 6 verts -> 2 triangles                            */

    fxdglue_tex_destroy(tex);
    fxdglue_context_destroy(ctx);
    printf("PASS: DDI glue (context + DrawPrimitives2 path) verified on host\n");
    return 0;
}
