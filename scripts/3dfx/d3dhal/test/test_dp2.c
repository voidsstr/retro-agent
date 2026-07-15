/*
 * test_dp2.c - host unit test for the fxD3D translation core.
 *
 * Builds a synthetic DP2 command buffer (render states + a texture-stage state
 * + a textured triangle), runs it through fxd_dp2_execute against the stub
 * backend, and asserts the resulting backend call sequence + draw counts. This
 * validates the DDI dispatch + D3D->Glide mapping WITHOUT hardware.
 *
 * Runs on the Linux build host (native gcc).
 */
#include "fxd3d.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>

extern char g_log[]; extern int g_loglen, g_tris, g_points, g_lines;

/* mirror the enums the test needs (same values as d3dhal_state.c) */
enum { RS_ZENABLE=7, RS_SHADEMODE=9, RS_ALPHATESTENABLE=15, RS_SRCBLEND=19,
       RS_DESTBLEND=20, RS_CULLMODE=22, RS_ALPHABLENDENABLE=27, RS_FOGENABLE=28 };
enum { CMP_LESSEQUAL=4 };
enum { BLEND_SRCALPHA=5, BLEND_INVSRCALPHA=6 };
enum { CULL_NONE=1 };
enum { SHADE_GOURAUD=2 };
enum { TSS_MAGFILTER=16 };
enum { TFN_LINEAR=2 };

static unsigned char buf[512];
static int put_hdr(int off,int cmd,int count){
    fxd2_hdr h; memset(&h,0,sizeof(h));
    h.bCommand=(WORD)cmd; h.dwCount=(DWORD)count;
    memcpy(buf+off,&h,sizeof(h)); return off+(int)sizeof(h);
}
static int put32(int off,DWORD v){ buf[off]=v&0xff;buf[off+1]=(v>>8)&0xff;
    buf[off+2]=(v>>16)&0xff;buf[off+3]=(v>>24)&0xff; return off+4; }

int main(void){
    fxd_device dev; int off=0, ops;
    fxd_device_init(&dev, 640, 480, 16);

    /* --- command 1: 4 render states --- */
    off=put_hdr(off, FXD2_RENDERSTATE, 4);
    off=put32(off,RS_ALPHABLENDENABLE); off=put32(off,1);
    off=put32(off,RS_SRCBLEND);         off=put32(off,BLEND_SRCALPHA);
    off=put32(off,RS_DESTBLEND);        off=put32(off,BLEND_INVSRCALPHA);
    off=put32(off,RS_CULLMODE);         off=put32(off,CULL_NONE);

    /* --- command 2: one texture-stage state (mag filter = linear) --- */
    off=put_hdr(off, FXD2_TEXTURESTAGESTATE, 1);
    off=put32(off,0); off=put32(off,TSS_MAGFILTER); off=put32(off,TFN_LINEAR);

    /* --- command 3: a triangle (3 TL vertices) --- */
    off=put_hdr(off, FXD2_TRIANGLELIST, 3);
    {
        fxd_tlvertex v[3]; int k;
        memset(v,0,sizeof(v));
        for(k=0;k<3;k++){ v[k].x=100+k*50; v[k].y=100+k*40; v[k].z=0.5f;
                          v[k].rhw=1.0f; v[k].color=0xFF00FF00; v[k].tu=k; v[k].tv=0; }
        memcpy(buf+off, v, sizeof(v)); off+=(int)sizeof(v);
    }

    /* bind a texture so the draw path exercises texture bind */
    dev.bound_tex = fxd_tex_create(64,64, 23 /*R5G6B5*/);

    ops = fxd_dp2_execute(&dev, buf, off);

    printf("ops=%d tris=%d\nlog=%s\n", ops, g_tris, g_log);

    /* assertions */
    assert(ops==3);
    assert(g_tris==1);
    assert(strstr(g_log,"blend=1;"));
    assert(strstr(g_log,"bs=1;"));       /* GR_BLEND_SRC_ALPHA == 1           */
    assert(strstr(g_log,"cull=0;"));     /* GB_CULL_NONE == 0                 */
    assert(strstr(g_log,"bilinear=1;")); /* mag filter linear -> bilinear     */
    assert(strstr(g_log,"texbind;"));    /* bound texture selected            */

    fxd_tex_destroy(dev.bound_tex);
    printf("PASS: DP2 dispatch + D3D->Glide state mapping verified on host\n");
    return 0;
}
