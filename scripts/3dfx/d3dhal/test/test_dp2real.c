/*
 * test_dp2real.c - host unit test for the REAL DX7 DP2 translator (M4a).
 *
 * Builds command buffers in the REAL D3DHAL_DP2COMMAND format (4-byte packed
 * headers + per-op operands) plus a separate FVF vertex buffer, runs them
 * through fxd_dp2_execute_real against the stub backend, and asserts the
 * backend call log + primitive counts. Then feeds a battery of MALFORMED
 * buffers (truncated header/operands, vertex-buffer overrun, out-of-range
 * index, unknown opcode, bad FVF, zero-length) and asserts each fails cleanly
 * with the right code + err_off - no crash, no OOB, no hang.
 *
 * Runs on the Linux build host (native gcc).
 */
#include "fxd3d_dp2.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>

extern char g_log[]; extern int g_loglen, g_tris, g_points, g_lines;

/* documented D3D constant values reused from the HAL */
enum { RS_SRCBLEND=19, RS_CULLMODE=22, RS_ALPHABLENDENABLE=27 };
enum { BLEND_SRCALPHA=5 };
enum { CULL_NONE=1 };
enum { TSS_MAGFILTER=16 };
enum { TFN_LINEAR=2 };

/* XYZRHW|DIFFUSE|SPECULAR|TEX1 (the canned TLVERTEX code) - 32-byte stride */
#define FVF_TL  0x1C4u
/* XYZRHW|DIFFUSE|TEX1 - 28-byte stride (exercises the stride machinery)    */
#define FVF_28  0x144u

static unsigned char cbuf[1024];
static unsigned char vbuf[8*32];

static int put_cmd(int off,int op,int count){
    fxdp2_cmd h; h.bCommand=(BYTE)op; h.bReserved=0; h.wCount=(WORD)count;
    memcpy(cbuf+off,&h,sizeof(h)); return off+(int)sizeof(h);
}
static int put16(int off,unsigned v){ cbuf[off]=(unsigned char)(v&0xff);
    cbuf[off+1]=(unsigned char)((v>>8)&0xff); return off+2; }
static int put32(int off,DWORD v){ (void)put16(off,(unsigned)(v&0xffff));
    return put16(off+2,(unsigned)(v>>16)); }
static int putz(int off,int n){ memset(cbuf+off,0,n); return off+n; }

/* one 32-byte TL vertex (green, opaque) into vbuf slot i */
static void mkvert(int i,float x,float y){
    unsigned char *p=vbuf+i*32; float f;
    memset(p,0,32);
    memcpy(p,&x,4); memcpy(p+4,&y,4);
    f=0.5f; memcpy(p+8,&f,4); f=1.0f; memcpy(p+12,&f,4);
    p[16]=0x00;p[17]=0xFF;p[18]=0x00;p[19]=0xFF;  /* diffuse 0xFF00FF00 LE  */
}

/* write one 32-byte FVF_TL vertex INLINE into cbuf at off (for the IMM ops) */
static int put_tlvert(int off,float x,float y){
    float f;
    memset(cbuf+off,0,32);
    memcpy(cbuf+off,&x,4); memcpy(cbuf+off+4,&y,4);
    f=0.5f; memcpy(cbuf+off+8,&f,4); f=1.0f; memcpy(cbuf+off+12,&f,4);
    cbuf[off+16]=0x00;cbuf[off+17]=0xFF;cbuf[off+18]=0x00;cbuf[off+19]=0xFF;
    return off+32;
}

/* stub PFND3DPARSEUNKNOWNCOMMAND-style hook (Finding 5): "understands" opcode
 * 99 as an 8-byte command (4-byte header + 4-byte operand), skips it, and
 * reports where to resume. Refuses (non-zero) any other opcode. */
static int g_parse_calls;
static int stub_parse_unknown(void *ctx,const void *cmds,DWORD off,
                              DWORD cmd_len,DWORD *next_off){
    const unsigned char *b=(const unsigned char*)cmds;
    (void)ctx; (void)cmd_len;
    g_parse_calls++;
    if(b[off]!=99) return -1;        /* can't parse -> COMMAND_UNPARSED       */
    *next_off=off+8;                 /* 4-byte header + 4-byte operand         */
    return 0;
}

int main(void){
    fxd_device dev;
    DWORD rstates[768], err;
    int off, rc, k;

    fxd_device_init(&dev, 640, 480, 16);
    dev.bound_tex = fxd_tex_create(64,64, 23 /*R5G6B5*/);
    memset(rstates, 0, sizeof(rstates));
    for(k=0;k<8;k++) mkvert(k, 100.0f+k*40, 100.0f+k*30);

    /* ---- phase 1: a full REAL command stream --------------------------- */
    off=0;
    off=put_cmd(off, FXDP2OP_RENDERSTATE, 3);
    off=put32(off,RS_ALPHABLENDENABLE); off=put32(off,1);
    off=put32(off,RS_SRCBLEND);         off=put32(off,BLEND_SRCALPHA);
    off=put32(off,RS_CULLMODE);         off=put32(off,CULL_NONE);

    off=put_cmd(off, FXDP2OP_TEXTURESTAGESTATE, 1);
    off=put16(off,0); off=put16(off,TSS_MAGFILTER); off=put32(off,TFN_LINEAR);

    off=put_cmd(off, FXDP2OP_VIEWPORTINFO, 1);   /* consume-and-skip, 16 B  */
    off=putz(off,16);

    off=put_cmd(off, FXDP2OP_TRIANGLELIST, 2);   /* verts 0..5 -> 2 tris    */
    off=put16(off,0);

    off=put_cmd(off, FXDP2OP_INDEXEDTRIANGLELIST2, 2); /* base 1 -> 2 tris  */
    off=put16(off,1);                            /* wVStart prefix          */
    off=put16(off,0); off=put16(off,1); off=put16(off,2);
    off=put16(off,2); off=put16(off,1); off=put16(off,3);

    off=put_cmd(off, FXDP2OP_TRIANGLESTRIP, 3);  /* verts 0..4 -> 3 tris    */
    off=put16(off,0);

    off=put_cmd(off, FXDP2OP_TRIANGLEFAN, 2);    /* verts 2..5 -> 2 tris    */
    off=put16(off,2);

    off=put_cmd(off, FXDP2OP_CLEAR, 1);          /* skip: 16 hdr + 1 RECT   */
    off=putz(off,32);

    off=put_cmd(off, FXDP2OP_INDEXEDTRIANGLELIST, 1); /* abs {0,1,2} -> 1   */
    off=put16(off,0); off=put16(off,1); off=put16(off,2); off=put16(off,0);

    off=put_cmd(off, FXDP2OP_LINELIST, 2);       /* verts 0..3 -> 2 lines   */
    off=put16(off,0);

    off=put_cmd(off, FXDP2OP_POINTS, 1);         /* {count 3, start 1}      */
    off=put16(off,3); off=put16(off,1);

    rc = fxd_dp2_execute_real(&dev, cbuf, (DWORD)off, vbuf, 8, FVF_TL,
                              rstates, &err);
    printf("real: rc=%d tris=%d lines=%d points=%d err=%u\nlog=%s\n",
           rc, g_tris, g_lines, g_points, (unsigned)err, g_log);

    assert(rc==11);
    assert(g_tris==10);                 /* 2+2+3+2+1                        */
    assert(g_lines==2);
    assert(g_points==3);
    assert(rstates[RS_ALPHABLENDENABLE]==1);   /* lpdwRStates mirrored      */
    assert(rstates[RS_SRCBLEND]==BLEND_SRCALPHA);
    assert(rstates[RS_CULLMODE]==CULL_NONE);
    assert(strstr(g_log,"blend=1;"));
    assert(strstr(g_log,"bs=1;"));      /* GR_BLEND_SRC_ALPHA == 1          */
    assert(strstr(g_log,"cull=0;"));    /* GB_CULL_NONE == 0                */
    assert(strstr(g_log,"bilinear=1;"));/* mag filter linear -> bilinear    */
    assert(strstr(g_log,"texbind;"));   /* bound texture selected           */

    /* ---- phase 2: a non-TLVERTEX stride (28 B, no specular field) ------ */
    {
        unsigned char v28[3*28]; float f; int i, base;
        memset(v28,0,sizeof(v28));
        for(i=0;i<3;i++){
            unsigned char *p=v28+i*28;
            f=10.0f+i; memcpy(p,&f,4); memcpy(p+4,&f,4);
            f=0.5f; memcpy(p+8,&f,4); f=1.0f; memcpy(p+12,&f,4);
            p[19]=0xFF;                       /* diffuse alpha              */
        }
        off=put_cmd(0, FXDP2OP_TRIANGLELIST, 1); off=put16(off,0);
        base=g_tris;
        rc = fxd_dp2_execute_real(&dev, cbuf, (DWORD)off, v28, 3, FVF_28, 0, &err);
        assert(rc==1);
        assert(g_tris==base+1);               /* 28-byte stride fetched     */
    }

    /* ---- phase 1b: line strips, indexed lines, immediate-mode ops ------- *
     * Findings 2/3/4: LINESTRIP, INDEXEDLINELIST/2/STRIP, TRIANGLEFAN_IMM and
     * LINELIST_IMM must all render instead of aborting the batch. */
    {
        int tris0=g_tris, lines0=g_lines;
        off=0;
        /* LINESTRIP (16): 3 lines over verts 0..3 */
        off=put_cmd(off, FXDP2OP_LINESTRIP, 3); off=put16(off,0);
        /* INDEXEDLINELIST (2): {0,1},{2,3} absolute -> 2 lines */
        off=put_cmd(off, FXDP2OP_INDEXEDLINELIST, 2);
        off=put16(off,0); off=put16(off,1); off=put16(off,2); off=put16(off,3);
        /* INDEXEDLINELIST2 (27): base 1, {0,1},{2,3} -> lines {1,2},{3,4} */
        off=put_cmd(off, FXDP2OP_INDEXEDLINELIST2, 2); off=put16(off,1);
        off=put16(off,0); off=put16(off,1); off=put16(off,2); off=put16(off,3);
        /* INDEXEDLINESTRIP (17): base 0, indices {0,1,2,3} -> 3 lines */
        off=put_cmd(off, FXDP2OP_INDEXEDLINESTRIP, 3); off=put16(off,0);
        off=put16(off,0); off=put16(off,1); off=put16(off,2); off=put16(off,3);
        /* TRIANGLEFAN_IMM (23): 2 tris from 4 INLINE verts (after dwEdgeFlags) */
        off=put_cmd(off, FXDP2OP_TRIANGLEFAN_IMM, 2);
        off=put32(off,0);                                /* dwEdgeFlags        */
        off=put_tlvert(off,10,10); off=put_tlvert(off,20,10);
        off=put_tlvert(off,20,20); off=put_tlvert(off,10,20);
        /* LINELIST_IMM (24): 2 lines from 4 INLINE verts */
        off=put_cmd(off, FXDP2OP_LINELIST_IMM, 2);
        off=put_tlvert(off,0,0); off=put_tlvert(off,5,5);
        off=put_tlvert(off,5,0); off=put_tlvert(off,0,5);

        rc = fxd_dp2_execute_real(&dev, cbuf, (DWORD)off, vbuf, 8, FVF_TL, 0, &err);
        printf("lines/imm: rc=%d dtris=%d dlines=%d err=%u\n",
               rc, g_tris-tris0, g_lines-lines0, (unsigned)err);
        assert(rc==6);
        assert(g_tris  - tris0  == 2);           /* fan-imm -> 2 tris          */
        assert(g_lines - lines0 == 3+2+2+3+2);   /* strip+ill+ill2+istrip+llimm*/
    }

    /* ---- phase 3: malformed inputs must fail cleanly -------------------- */

    /* zero-length buffer: legal, zero commands */
    rc = fxd_dp2_execute_real(&dev, cbuf, 0, vbuf, 8, FVF_TL, 0, &err);
    assert(rc==0);

    /* truncated command header (3 of 4 bytes) */
    (void)put_cmd(0, FXDP2OP_RENDERSTATE, 1);
    rc = fxd_dp2_execute_real(&dev, cbuf, 3, vbuf, 8, FVF_TL, 0, &err);
    assert(rc==FXD_DP2E_MALFORMED && err==0);

    /* truncated operands: RENDERSTATE claims 4 pairs, only 1 present */
    off=put_cmd(0, FXDP2OP_RENDERSTATE, 4);
    off=put32(off,RS_CULLMODE); off=put32(off,CULL_NONE);
    rc = fxd_dp2_execute_real(&dev, cbuf, (DWORD)off, vbuf, 8, FVF_TL, 0, &err);
    assert(rc==FXD_DP2E_MALFORMED && err==0);

    /* truncated operand of a SECOND command: err_off points at IT */
    off=put_cmd(0, FXDP2OP_RENDERSTATE, 1);
    off=put32(off,RS_CULLMODE); off=put32(off,CULL_NONE);
    off=put_cmd(off, FXDP2OP_TRIANGLELIST, 1);   /* header only, no wVStart */
    rc = fxd_dp2_execute_real(&dev, cbuf, (DWORD)off, vbuf, 8, FVF_TL, 0, &err);
    assert(rc==FXD_DP2E_MALFORMED && err==12);

    /* wPrimitiveCount overruns the vertex buffer (needs 15 of 4 verts) */
    off=put_cmd(0, FXDP2OP_TRIANGLELIST, 5); off=put16(off,0);
    { int base=g_tris;
      rc = fxd_dp2_execute_real(&dev, cbuf, (DWORD)off, vbuf, 4, FVF_TL, 0, &err);
      assert(rc==FXD_DP2E_MALFORMED && err==0);
      assert(g_tris==base); }                    /* nothing drawn           */

    /* out-of-range index in an indexed list */
    off=put_cmd(0, FXDP2OP_INDEXEDTRIANGLELIST, 1);
    off=put16(off,0); off=put16(off,1); off=put16(off,999); off=put16(off,0);
    { int base=g_tris;
      rc = fxd_dp2_execute_real(&dev, cbuf, (DWORD)off, vbuf, 4, FVF_TL, 0, &err);
      assert(rc==FXD_DP2E_MALFORMED && err==0);
      assert(g_tris==base); }

    /* unknown opcode after a valid command: stops with err_off at IT,
     * and the state before it was still applied */
    memset(rstates, 0, sizeof(rstates));
    off=put_cmd(0, FXDP2OP_RENDERSTATE, 1);
    off=put32(off,RS_SRCBLEND); off=put32(off,BLEND_SRCALPHA);
    off=put_cmd(off, 99, 0);
    rc = fxd_dp2_execute_real(&dev, cbuf, (DWORD)off, vbuf, 8, FVF_TL, rstates, &err);
    assert(rc==FXD_DP2E_UNKNOWN_OP && err==12);
    assert(rstates[RS_SRCBLEND]==BLEND_SRCALPHA);

    /* draw with a NULL vertex buffer */
    off=put_cmd(0, FXDP2OP_TRIANGLELIST, 1); off=put16(off,0);
    rc = fxd_dp2_execute_real(&dev, cbuf, (DWORD)off, 0, 8, FVF_TL, 0, &err);
    assert(rc==FXD_DP2E_MALFORMED && err==0);

    /* unsupported FVF (XYZ, untransformed): states run, the draw errors */
    off=put_cmd(0, FXDP2OP_RENDERSTATE, 1);
    off=put32(off,RS_CULLMODE); off=put32(off,CULL_NONE);
    off=put_cmd(off, FXDP2OP_TRIANGLELIST, 1); off=put16(off,0);
    rc = fxd_dp2_execute_real(&dev, cbuf, (DWORD)off, vbuf, 8, 0x002, 0, &err);
    assert(rc==FXD_DP2E_BADFVF && err==12);

    /* Finding 1 (critical): a RENDERSTATE index >= FXD_MAX_RSTATES (256) is an
     * OVERRIDE/out-of-range token and must NOT be mirrored - writing it would
     * be a controlled OOB store past the runtime's 256-DWORD lpdwRStates. Size
     * the mirror to EXACTLY 256 with a canary right after: the OLD code
     * (st<768) would scribble the canary, the fix leaves it pristine. */
    {
        struct { DWORD rs[256]; DWORD canary[16]; } m;
        int ci;
        memset(&m, 0, sizeof(m));
        off=put_cmd(0, FXDP2OP_RENDERSTATE, 3);
        off=put32(off,RS_CULLMODE); off=put32(off,CULL_NONE);   /* 22: in-range */
        off=put32(off,256);         off=put32(off,0xDEADBEEF);  /* OVERRIDE_BIAS */
        off=put32(off,260);         off=put32(off,0xFEEDFACE);  /* > MAX_RSTATES */
        rc = fxd_dp2_execute_real(&dev, cbuf, (DWORD)off, vbuf, 8, FVF_TL, m.rs, &err);
        assert(rc==1);
        assert(m.rs[RS_CULLMODE]==CULL_NONE);                   /* in-range kept */
        for(ci=0;ci<16;ci++) assert(m.canary[ci]==0);          /* no OOB store  */
    }

    /* Finding 3: out-of-range index in an indexed LINE list fails cleanly */
    off=put_cmd(0, FXDP2OP_INDEXEDLINELIST, 1);
    off=put16(off,0); off=put16(off,999);
    { int base=g_lines;
      rc = fxd_dp2_execute_real(&dev, cbuf, (DWORD)off, vbuf, 4, FVF_TL, 0, &err);
      assert(rc==FXD_DP2E_MALFORMED && err==0);
      assert(g_lines==base); }

    /* Finding 3: truncated indexed LINE strip (claims 3 lines, too few bytes) */
    off=put_cmd(0, FXDP2OP_INDEXEDLINESTRIP, 3); off=put16(off,0);
    off=put16(off,0); off=put16(off,1);            /* only 2 of 4 indices */
    rc = fxd_dp2_execute_real(&dev, cbuf, (DWORD)off, vbuf, 8, FVF_TL, 0, &err);
    assert(rc==FXD_DP2E_MALFORMED && err==0);

    /* Finding 4: immediate fan claims 2 tris (4 inline verts) but the inline
     * vertex data is truncated -> clean MALFORMED, no OOB read */
    off=put_cmd(0, FXDP2OP_TRIANGLEFAN_IMM, 2);
    off=put32(off,0);                              /* dwEdgeFlags, no verts */
    { int base=g_tris;
      rc = fxd_dp2_execute_real(&dev, cbuf, (DWORD)off, vbuf, 8, FVF_TL, 0, &err);
      assert(rc==FXD_DP2E_MALFORMED && err==0);
      assert(g_tris==base); }

    /* Finding 4: immediate line list with truncated inline verts */
    off=put_cmd(0, FXDP2OP_LINELIST_IMM, 2);
    off=put_tlvert(off,0,0);                       /* only 1 of 4 verts */
    { int base=g_lines;
      rc = fxd_dp2_execute_real(&dev, cbuf, (DWORD)off, vbuf, 8, FVF_TL, 0, &err);
      assert(rc==FXD_DP2E_MALFORMED && err==0);
      assert(g_lines==base); }

    /* Finding 5: an opcode we don't implement is skipped via the parse-unknown
     * callback and the walk RESUMES - the state op AFTER it still applies. */
    memset(rstates, 0, sizeof(rstates));
    g_parse_calls=0;
    off=put_cmd(0, FXDP2OP_RENDERSTATE, 1);
    off=put32(off,RS_SRCBLEND); off=put32(off,BLEND_SRCALPHA);  /* cmd @0      */
    off=put_cmd(off, 99, 0); off=put32(off,0);                  /* unknown @12 */
    off=put_cmd(off, FXDP2OP_RENDERSTATE, 1);
    off=put32(off,RS_CULLMODE); off=put32(off,CULL_NONE);       /* cmd @20     */
    rc = fxd_dp2_execute_real_cb(&dev, cbuf, (DWORD)off, vbuf, 8, FVF_TL,
                                 rstates, &err, stub_parse_unknown, 0);
    assert(rc==3);                                     /* 2 states + 1 skipped */
    assert(g_parse_calls==1);
    assert(rstates[RS_SRCBLEND]==BLEND_SRCALPHA);
    assert(rstates[RS_CULLMODE]==CULL_NONE);           /* op after unknown ran */
    assert(err==0);

    /* Finding 5: a callback that REFUSES keeps the classic UNPARSED behavior */
    off=put_cmd(0, FXDP2OP_RENDERSTATE, 1);
    off=put32(off,RS_SRCBLEND); off=put32(off,BLEND_SRCALPHA);
    off=put_cmd(off, 77, 0);                            /* stub won't parse 77 */
    rc = fxd_dp2_execute_real_cb(&dev, cbuf, (DWORD)off, vbuf, 8, FVF_TL,
                                 0, &err, stub_parse_unknown, 0);
    assert(rc==FXD_DP2E_UNKNOWN_OP && err==12);

    /* Hardening (POINTS nested-count DoS cap, FXDP2_MAX_EMIT): a POINTS command
     * whose outer count x per-run wCount would emit more than the cap must fail
     * MALFORMED instead of driving billions of fetches (kernel TDR). 17 runs of
     * 65535 pts = 1,114,095 > 1,048,576 -> the cap trips on the last run.
     * Needs a real 65535-vertex buffer so vrange_ok passes on each run. */
    {
        DWORD nv = 65535, i2;
        unsigned char *vbig = (unsigned char*)calloc(nv, 32);
        assert(vbig);
        off = put_cmd(0, FXDP2OP_POINTS, 17);
        for(i2=0;i2<17;i2++){ off=put16(off,(unsigned)nv); off=put16(off,0); }
        { int base=g_points;
          rc = fxd_dp2_execute_real(&dev, cbuf, (DWORD)off, vbig, nv, FVF_TL, 0, &err);
          assert(rc==FXD_DP2E_MALFORMED && err==0);      /* cap tripped        */
          /* stopped near the cap, did NOT run the full 17th batch away         */
          assert((DWORD)(g_points-base) <= 17u*65535u);
          assert((DWORD)(g_points-base) >  16u*65535u - 64u); }
        free(vbig);
    }

    /* fxd_dp2_fvf_stride (M4c-2 review #4): the exported stride the DDI
     * caller (driver/nt/enable.c d3d_DrawPrimitives2) uses to clamp
     * dwVertexOffset + dwVertexLength*stride against the vertex SURFACE's
     * byte size before executing. Must agree with the walk's own
     * fvf_resolve: the canned TL code is 32 bytes, the no-specular variant
     * 28, and every code the walk would reject (no XYZRHW, reserved bits,
     * XYZ untransformed) reports 0 - the caller then bounds the offset
     * alone and lets the executor return BADFVF. Old-buggy behavior: no
     * stride was exported at all, so the caller COULD NOT bound the vertex
     * window and passed it through unchecked. */
    assert(fxd_dp2_fvf_stride(FVF_TL) == 32);
    assert(fxd_dp2_fvf_stride(FVF_28) == 28);
    assert(fxd_dp2_fvf_stride(0x044u) == 20);   /* XYZRHW|DIFFUSE           */
    assert(fxd_dp2_fvf_stride(0x004u) == 16);   /* bare XYZRHW              */
    assert(fxd_dp2_fvf_stride(0x002u) == 0);    /* XYZ untransformed: reject */
    assert(fxd_dp2_fvf_stride(0x005u) == 0);    /* RESERVED0 set: reject     */
    assert(fxd_dp2_fvf_stride(0) == 0);
    /* 2 texcoord sets of 2 floats each: 16 + 4 + 2*8 = 36 */
    assert(fxd_dp2_fvf_stride(0x244u) == 36);

    fxd_tex_destroy(dev.bound_tex);
    printf("PASS: REAL DP2 stream translation + malformed-input hardening verified on host\n");
    return 0;
}
