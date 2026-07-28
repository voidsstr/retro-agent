/*
 * d3dhal_dp2real.c - REAL DX7 DrawPrimitives2 stream -> fxD3D core (M4a).
 *
 * fxd_dp2_execute_real walks the runtime's actual DP2 command buffer - packed
 * 4-byte D3DHAL_DP2COMMAND headers each followed by their operands - and
 * translates it DIRECTLY into fxd_set_renderstate / fxd_set_tss / fxd_draw
 * calls (no intermediate fxd2 buffer). Draw ops reference a SEPARATE vertex
 * buffer laid out per the dwVertexType FVF code; each referenced vertex is
 * converted to fxd_tlvertex on the fly. Strips/fans/indexed lists decompose
 * to triangle triples in a fixed stack scratch (no heap) with the documented
 * winding preserved.
 *
 * Safety: every stream read is memcpy/byte-composed (DP2 streams have NO
 * alignment guarantee) and bounds-checked against cmd_len / vert_count with
 * 32-bit-safe arithmetic (counts are WORDs, so no product here can overflow a
 * DWORD); the walk always advances by at least the 4-byte header, so
 * malformed input can neither read out of bounds nor loop forever. Unknown
 * or unsupported opcodes are handed to the runtime's parse-unknown callback
 * (PFND3DPARSEUNKNOWNCOMMAND) so it parses/skips that one command and the walk
 * resumes; only when no callback is supplied (or it too cannot parse) does the
 * walk STOP with *err_off at the offending header - the D3DERR_COMMAND_UNPARSED
 * protocol.
 *
 * Clean-room: opcode values, operand layouts and the FVF stride rule are the
 * public Microsoft DX7 DDK contract (d3dhal.h / d3dtypes.h shapes).
 */
#include "fxd3d_dp2.h"
#include <string.h>

/* --- unaligned little-endian stream reads --------------------------------- */

static DWORD rd_u32(const BYTE *p){
    return (DWORD)p[0]|((DWORD)p[1]<<8)|((DWORD)p[2]<<16)|((DWORD)p[3]<<24);
}
static DWORD rd_u16(const BYTE *p){ return (DWORD)p[0]|((DWORD)p[1]<<8); }
static D3DVALUE rd_f32(const BYTE *p){
    DWORD u = rd_u32(p); D3DVALUE f;
    memcpy(&f, &u, 4);  /* same bit pattern; D3DVALUE is a 32-bit float      */
    return f;
}

/* --- FVF vertex layout ----------------------------------------------------
 * Field ORDER per the documented canned-vertex layouts: position(x,y,z,rhw),
 * [reserved DWORD], [diffuse], [specular], texcoord sets. Stride = sum of the
 * present fields; texcoord set i's float count comes from the two size bits
 * at position 16+2i (0->2, 1->3, 2->4, 3->1). */

typedef struct {
    DWORD stride;
    DWORD diff_off, spec_off, tex_off;
    int   has_diff, has_spec, tex_floats; /* tex_floats=0: no texcoords       */
} fvf_layout;

/* TL path only: XYZRHW required; blend weights / normals / reserved bits are
 * never valid in a pre-transformed stream. Returns 1 if usable. */
static int fvf_resolve(DWORD fvf, fvf_layout *L){
    DWORD off = 16, tc, i;
    if((fvf & FXDP2FVF_POSITION_MASK) != FXDP2FVF_XYZRHW) return 0;
    if(fvf & (FXDP2FVF_RESERVED0|FXDP2FVF_NORMAL|FXDP2FVF_RESERVED2)) return 0;
    if(fvf & FXDP2FVF_RESERVED1) off += 4;
    L->has_diff = (fvf & FXDP2FVF_DIFFUSE)!=0;
    L->diff_off = off; if(L->has_diff) off += 4;
    L->has_spec = (fvf & FXDP2FVF_SPECULAR)!=0;
    L->spec_off = off; if(L->has_spec) off += 4;
    tc = (fvf & FXDP2FVF_TEXCOUNT_MASK) >> FXDP2FVF_TEXCOUNT_SHIFT;
    if(tc > 8) return 0;
    L->tex_off = off; L->tex_floats = 0;
    for(i=0;i<tc;i++){
        DWORD code = (fvf >> (16 + 2*i)) & 3;
        DWORD nfl  = (code==0)?2:(code==1)?3:(code==2)?4:1;
        if(i==0) L->tex_floats = (int)nfl;  /* draw uses texture set 0       */
        off += nfl*4;
    }
    L->stride = off;
    return 1;
}

/* Fetch vertex `idx` as an fxd_tlvertex (defaults: opaque white diffuse, zero
 * specular/UV). Returns 0 if idx is out of range - the caller's error. */
static int vfetch(const BYTE *vb, DWORD nverts, const fvf_layout *L,
                  DWORD idx, fxd_tlvertex *o){
    const BYTE *v;
    if(idx >= nverts) return 0;
    v = vb + idx*L->stride;
    o->x = rd_f32(v);   o->y   = rd_f32(v+4);
    o->z = rd_f32(v+8); o->rhw = rd_f32(v+12);
    o->color    = L->has_diff ? rd_u32(v+L->diff_off) : (DWORD)0xFFFFFFFF;
    o->specular = L->has_spec ? rd_u32(v+L->spec_off) : 0;
    o->tu = 0.0f; o->tv = 0.0f;
    if(L->tex_floats >= 1) o->tu = rd_f32(v+L->tex_off);
    if(L->tex_floats >= 2) o->tv = rd_f32(v+L->tex_off+4);
    return 1;
}

/* contiguous vertex run [start, start+n) in range? (overflow-safe) */
static int vrange_ok(DWORD nverts, DWORD start, DWORD n){
    return n==0 || (start < nverts && n <= nverts - start);
}

/* Byte stride of one FVF vertex, 0 when the code is unusable (needs XYZRHW
 * etc. - the same fvf_resolve rule the walk applies). Exported so the DDI
 * caller can bound dwVertexOffset + dwVertexLength*stride against the vertex
 * SURFACE's byte size BEFORE the walk fetches anything (M4c-2 review #4):
 * vfetch validates indices only against nverts, so the byte window must be
 * proven by the caller. */
DWORD fxd_dp2_fvf_stride(DWORD fvf){
    fvf_layout L;
    return fvf_resolve(fvf, &L) ? L.stride : 0;
}

/* --- walk state + triangle gatherer ---------------------------------------
 * Every triangle source decomposes to triples in a fixed scratch (multiple of
 * 3 vertices, ~2KB on the stack - fine for a display-driver thread, no heap)
 * flushed as FXD2_TRIANGLELIST draws. */

#define FXDP2_SCRATCH 63
/* SECURITY cap: POINTS nests an outer count x an inner per-run wCount, so one
 * ~256 KB command could otherwise drive ~4.3e9 vertex fetches and stall the
 * kernel display lock (TDR-class DoS). No legitimate frame on a 16bpp Voodoo3
 * emits anywhere near this many points; beyond it we treat the command as
 * malformed. (All other draw ops are linear in wCount.) */
#define FXDP2_MAX_EMIT (1u << 20)

typedef struct {
    fxd_device  *dev;
    const BYTE  *vb;
    DWORD        nverts;
    fvf_layout   lay;
    int          lay_state;   /* -1 unresolved, 0 bad, 1 good                 */
    int          n;           /* vertices pending in s[]                      */
    fxd_tlvertex s[FXDP2_SCRATCH];
} dp2_walk;

static void tri_flush(dp2_walk *w){
    if(w->n) fxd_draw(w->dev, FXD2_TRIANGLELIST, w->s, w->n);
    w->n = 0;
}
static int tri_emit(dp2_walk *w, DWORD a, DWORD b, DWORD c){
    if(w->n + 3 > FXDP2_SCRATCH) tri_flush(w);
    if(!vfetch(w->vb, w->nverts, &w->lay, a, &w->s[w->n]))   return 0;
    if(!vfetch(w->vb, w->nverts, &w->lay, b, &w->s[w->n+1])) return 0;
    if(!vfetch(w->vb, w->nverts, &w->lay, c, &w->s[w->n+2])) return 0;
    w->n += 3;
    return 1;
}

/* Line gatherer - decomposes every line source (list/strip, indexed or
 * immediate) into discrete vertex PAIRS flushed as FXD2_LINELIST draws, the
 * same way tri_* handles triangles. n stays even (increments of 2), so the
 * FXDP2_SCRATCH flush check never splits a line pair. */
static void line_flush(dp2_walk *w){
    if(w->n) fxd_draw(w->dev, FXD2_LINELIST, w->s, w->n);
    w->n = 0;
}
static int line_emit(dp2_walk *w, DWORD a, DWORD b){
    if(w->n + 2 > FXDP2_SCRATCH) line_flush(w);
    if(!vfetch(w->vb, w->nverts, &w->lay, a, &w->s[w->n]))   return 0;
    if(!vfetch(w->vb, w->nverts, &w->lay, b, &w->s[w->n+1])) return 0;
    w->n += 2;
    return 1;
}

static int dp2_fail(DWORD *err_off, DWORD at, int code){
    if(err_off) *err_off = at;
    return code;
}

/* --- the translator -------------------------------------------------------- */

int fxd_dp2_execute_real_cb(fxd_device *dev, const void *cmds, DWORD cmd_len,
                            const void *verts, DWORD vert_count, DWORD fvf,
                            DWORD *rstates, DWORD *err_off,
                            fxd_parse_unknown_fn parse, void *parse_ctx)
{
    const BYTE *base = (const BYTE*)cmds;
    DWORD off = 0;
    int ncmd = 0;
    dp2_walk w;

    if(err_off) *err_off = 0;
    if(cmd_len == 0) return 0;
    if(!base) return dp2_fail(err_off, 0, FXD_DP2E_MALFORMED);

    w.dev = dev;
    w.vb = (const BYTE*)verts;
    w.nverts = verts ? vert_count : 0;
    w.lay_state = -1;
    w.n = 0;

    while(off < cmd_len){
        fxdp2_cmd h;
        DWORD hdr = off, opnd, avail, count, need;

        if(cmd_len - off < 4) return dp2_fail(err_off, hdr, FXD_DP2E_MALFORMED);
        memcpy(&h, base + off, sizeof(h));
        opnd  = off + 4;
        avail = cmd_len - opnd;
        count = h.wCount;

        /* draw ops need the vertex layout; resolved lazily so state-only
         * buffers run whatever the FVF says */
        switch(h.bCommand){
            case FXDP2OP_POINTS: case FXDP2OP_LINELIST:
            case FXDP2OP_LINESTRIP:
            case FXDP2OP_TRIANGLELIST: case FXDP2OP_TRIANGLESTRIP:
            case FXDP2OP_TRIANGLEFAN:
            case FXDP2OP_INDEXEDLINELIST: case FXDP2OP_INDEXEDLINELIST2:
            case FXDP2OP_INDEXEDLINESTRIP:
            case FXDP2OP_INDEXEDTRIANGLELIST: case FXDP2OP_INDEXEDTRIANGLELIST2:
            case FXDP2OP_INDEXEDTRIANGLESTRIP: case FXDP2OP_INDEXEDTRIANGLEFAN:
            case FXDP2OP_TRIANGLEFAN_IMM: case FXDP2OP_LINELIST_IMM:
                if(w.lay_state < 0) w.lay_state = fvf_resolve(fvf, &w.lay);
                if(!w.lay_state) return dp2_fail(err_off, hdr, FXD_DP2E_BADFVF);
                break;
            default: break;
        }

        switch(h.bCommand){

        /* ---- state ops ---------------------------------------------------- */

        case FXDP2OP_RENDERSTATE: {          /* count x {DWORD state, value}  */
            DWORD i;
            need = count*8;
            if(need > avail) return dp2_fail(err_off, hdr, FXD_DP2E_MALFORMED);
            for(i=0;i<count;i++){
                const BYTE *r = base + opnd + i*8;
                DWORD st = rd_u32(r), va = rd_u32(r+4);
                /* SECURITY: st comes straight from the untrusted stream. The
                 * runtime's lpdwRStates is exactly FXD_MAX_RSTATES (256) DWORDs
                 * - the size the DDK reference rasterizer allocates and bounds
                 * (see FXD_MAX_RSTATES). Indices >= 256 are OVERRIDE tokens
                 * (D3DSTATE_OVERRIDE_BIAS = 256), control markers - not array
                 * slots; the DDK samples skip them (IS_OVERRIDE -> continue).
                 * Writing them would be a controlled OOB kernel-mode store into
                 * a caller array we were never told the length of, so skip. */
                if(st >= FXD_MAX_RSTATES) continue;
                if(rstates) rstates[st] = va;
                fxd_set_renderstate(dev, st, va);
            }
            off = opnd + need;
            break;
        }

        case FXDP2OP_TEXTURESTAGESTATE: {    /* count x {WORD stage,state; DWORD value} */
            DWORD i;
            need = count*8;
            if(need > avail) return dp2_fail(err_off, hdr, FXD_DP2E_MALFORMED);
            for(i=0;i<count;i++){
                const BYTE *r = base + opnd + i*8;
                fxd_set_tss(dev, rd_u16(r), rd_u16(r+2), rd_u32(r+4));
            }
            off = opnd + need;
            break;
        }

        /* ---- non-indexed primitives -------------------------------------- */

        case FXDP2OP_POINTS: {               /* count x {WORD wCount,wVStart} */
            DWORD i, j, emitted = 0;
            need = count*4;
            if(need > avail) return dp2_fail(err_off, hdr, FXD_DP2E_MALFORMED);
            for(i=0;i<count;i++){
                const BYTE *r = base + opnd + i*4;
                DWORD pc = rd_u16(r), ps = rd_u16(r+2);
                if(!vrange_ok(w.nverts, ps, pc))
                    return dp2_fail(err_off, hdr, FXD_DP2E_MALFORMED);
                emitted += pc;               /* nested-count DoS guard */
                if(emitted > FXDP2_MAX_EMIT)
                    return dp2_fail(err_off, hdr, FXD_DP2E_MALFORMED);
                for(j=0;j<pc;j++){
                    if(w.n == FXDP2_SCRATCH){ fxd_draw(dev, FXD2_POINTS, w.s, w.n); w.n=0; }
                    (void)vfetch(w.vb, w.nverts, &w.lay, ps+j, &w.s[w.n]);
                    w.n++;
                }
                if(w.n){ fxd_draw(dev, FXD2_POINTS, w.s, w.n); w.n=0; }
            }
            off = opnd + need;
            break;
        }

        case FXDP2OP_LINELIST: {             /* {WORD wVStart}; 2*count verts */
            DWORD vstart, t;
            need = 2;
            if(need > avail) return dp2_fail(err_off, hdr, FXD_DP2E_MALFORMED);
            vstart = rd_u16(base+opnd);
            if(!vrange_ok(w.nverts, vstart, count*2))
                return dp2_fail(err_off, hdr, FXD_DP2E_MALFORMED);
            for(t=0;t<count;t++){
                if(w.n + 2 > FXDP2_SCRATCH){ fxd_draw(dev, FXD2_LINELIST, w.s, w.n); w.n=0; }
                (void)vfetch(w.vb, w.nverts, &w.lay, vstart+2*t,   &w.s[w.n]);
                (void)vfetch(w.vb, w.nverts, &w.lay, vstart+2*t+1, &w.s[w.n+1]);
                w.n += 2;
            }
            if(w.n){ fxd_draw(dev, FXD2_LINELIST, w.s, w.n); w.n=0; }
            off = opnd + need;
            break;
        }

        case FXDP2OP_LINESTRIP: {            /* {WORD wVStart}; count+1 verts, count lines */
            DWORD vstart, t;
            need = 2;
            if(need > avail) return dp2_fail(err_off, hdr, FXD_DP2E_MALFORMED);
            vstart = rd_u16(base+opnd);
            if(count){
                if(!vrange_ok(w.nverts, vstart, count+1))
                    return dp2_fail(err_off, hdr, FXD_DP2E_MALFORMED);
                for(t=0;t<count;t++)
                    (void)line_emit(&w, vstart+t, vstart+t+1);
                line_flush(&w);
            }
            off = opnd + need;
            break;
        }

        case FXDP2OP_TRIANGLELIST: {         /* {WORD wVStart}; 3*count verts */
            DWORD vstart, t;
            need = 2;
            if(need > avail) return dp2_fail(err_off, hdr, FXD_DP2E_MALFORMED);
            vstart = rd_u16(base+opnd);
            if(!vrange_ok(w.nverts, vstart, count*3))
                return dp2_fail(err_off, hdr, FXD_DP2E_MALFORMED);
            for(t=0;t<count;t++)
                (void)tri_emit(&w, vstart+3*t, vstart+3*t+1, vstart+3*t+2);
            tri_flush(&w);
            off = opnd + need;
            break;
        }

        case FXDP2OP_TRIANGLESTRIP: {        /* {WORD wVStart}; count+2 verts */
            DWORD vstart, t;
            need = 2;
            if(need > avail) return dp2_fail(err_off, hdr, FXD_DP2E_MALFORMED);
            vstart = rd_u16(base+opnd);
            if(count){
                if(!vrange_ok(w.nverts, vstart, count+2))
                    return dp2_fail(err_off, hdr, FXD_DP2E_MALFORMED);
                for(t=0;t<count;t++){
                    /* swap odd triangles so facing stays consistent          */
                    if(t&1) (void)tri_emit(&w, vstart+t+1, vstart+t,   vstart+t+2);
                    else    (void)tri_emit(&w, vstart+t,   vstart+t+1, vstart+t+2);
                }
                tri_flush(&w);
            }
            off = opnd + need;
            break;
        }

        case FXDP2OP_TRIANGLEFAN: {          /* {WORD wVStart}; count+2 verts */
            DWORD vstart, t;
            need = 2;
            if(need > avail) return dp2_fail(err_off, hdr, FXD_DP2E_MALFORMED);
            vstart = rd_u16(base+opnd);
            if(count){
                if(!vrange_ok(w.nverts, vstart, count+2))
                    return dp2_fail(err_off, hdr, FXD_DP2E_MALFORMED);
                for(t=0;t<count;t++)
                    (void)tri_emit(&w, vstart, vstart+t+1, vstart+t+2);
                tri_flush(&w);
            }
            off = opnd + need;
            break;
        }

        /* ---- indexed primitives ------------------------------------------ */

        case FXDP2OP_INDEXEDTRIANGLELIST: {  /* count x {WORD v1,v2,v3,flags}; absolute */
            DWORD t;
            need = count*8;
            if(need > avail) return dp2_fail(err_off, hdr, FXD_DP2E_MALFORMED);
            for(t=0;t<count;t++){
                const BYTE *r = base + opnd + t*8;
                /* wFlags (edge/strip hints) are irrelevant to filled draws   */
                if(!tri_emit(&w, rd_u16(r), rd_u16(r+2), rd_u16(r+4)))
                    { w.n=0; return dp2_fail(err_off, hdr, FXD_DP2E_MALFORMED); }
            }
            tri_flush(&w);
            off = opnd + need;
            break;
        }

        case FXDP2OP_INDEXEDTRIANGLELIST2: { /* {WORD wVStart} + count x {WORD v1,v2,v3} */
            DWORD vstart, t;
            need = 2 + count*6;
            if(need > avail) return dp2_fail(err_off, hdr, FXD_DP2E_MALFORMED);
            vstart = rd_u16(base+opnd);
            for(t=0;t<count;t++){
                const BYTE *r = base + opnd + 2 + t*6;
                if(!tri_emit(&w, vstart+rd_u16(r), vstart+rd_u16(r+2), vstart+rd_u16(r+4)))
                    { w.n=0; return dp2_fail(err_off, hdr, FXD_DP2E_MALFORMED); }
            }
            tri_flush(&w);
            off = opnd + need;
            break;
        }

        case FXDP2OP_INDEXEDTRIANGLESTRIP:   /* {WORD wVStart} + count+2 WORD indices */
        case FXDP2OP_INDEXEDTRIANGLEFAN: {   /* same shape; fan pivots on index 0     */
            DWORD vstart, t;
            const BYTE *ix;
            need = 2 + (count+2)*2;
            if(need > avail) return dp2_fail(err_off, hdr, FXD_DP2E_MALFORMED);
            vstart = rd_u16(base+opnd);
            ix = base + opnd + 2;            /* indices relative to wVStart   */
            for(t=0;t<count;t++){
                DWORD a, b, c;
                if(h.bCommand == FXDP2OP_INDEXEDTRIANGLEFAN){
                    a = rd_u16(ix); b = rd_u16(ix+(t+1)*2); c = rd_u16(ix+(t+2)*2);
                }else if(t&1){               /* strip: swap odd for winding   */
                    a = rd_u16(ix+(t+1)*2); b = rd_u16(ix+t*2); c = rd_u16(ix+(t+2)*2);
                }else{
                    a = rd_u16(ix+t*2); b = rd_u16(ix+(t+1)*2); c = rd_u16(ix+(t+2)*2);
                }
                if(!tri_emit(&w, vstart+a, vstart+b, vstart+c))
                    { w.n=0; return dp2_fail(err_off, hdr, FXD_DP2E_MALFORMED); }
            }
            tri_flush(&w);
            off = opnd + need;
            break;
        }

        /* ---- indexed line primitives ------------------------------------- */

        case FXDP2OP_INDEXEDLINELIST: {      /* count x {WORD v1,v2}; absolute */
            DWORD t;
            need = count*4;
            if(need > avail) return dp2_fail(err_off, hdr, FXD_DP2E_MALFORMED);
            for(t=0;t<count;t++){
                const BYTE *r = base + opnd + t*4;
                if(!line_emit(&w, rd_u16(r), rd_u16(r+2)))
                    { w.n=0; return dp2_fail(err_off, hdr, FXD_DP2E_MALFORMED); }
            }
            line_flush(&w);
            off = opnd + need;
            break;
        }

        case FXDP2OP_INDEXEDLINELIST2: {     /* {WORD wVStart} + count x {WORD v1,v2}; relative */
            DWORD vstart, t;
            need = 2 + count*4;
            if(need > avail) return dp2_fail(err_off, hdr, FXD_DP2E_MALFORMED);
            vstart = rd_u16(base+opnd);
            for(t=0;t<count;t++){
                const BYTE *r = base + opnd + 2 + t*4;
                if(!line_emit(&w, vstart+rd_u16(r), vstart+rd_u16(r+2)))
                    { w.n=0; return dp2_fail(err_off, hdr, FXD_DP2E_MALFORMED); }
            }
            line_flush(&w);
            off = opnd + need;
            break;
        }

        case FXDP2OP_INDEXEDLINESTRIP: {     /* {WORD wVStart} + count+1 WORD indices; relative */
            DWORD vstart, t;
            const BYTE *ix;
            need = 2 + (count+1)*2;
            if(need > avail) return dp2_fail(err_off, hdr, FXD_DP2E_MALFORMED);
            vstart = rd_u16(base+opnd);
            ix = base + opnd + 2;
            for(t=0;t<count;t++){
                if(!line_emit(&w, vstart+rd_u16(ix+t*2), vstart+rd_u16(ix+(t+1)*2)))
                    { w.n=0; return dp2_fail(err_off, hdr, FXD_DP2E_MALFORMED); }
            }
            line_flush(&w);
            off = opnd + need;
            break;
        }

        /* ---- immediate-mode primitives (vertices INLINE in the stream) ----
         * The DX7 runtime emits these for post-clip geometry: the FVF vertex
         * data follows the operand header IN the command buffer (DWORD-aligned),
         * NOT in the separate VB. We point the gatherer at the inline region
         * for the op, then restore it. FVF stride is always a multiple of 4, so
         * the inline data ends DWORD-aligned; the round-up documents/enforces
         * the alignment rule so a later stride change can't desync the stream.
         * numverts (WORD-derived) * stride (<= ~156) stays well within DWORD. */

        case FXDP2OP_TRIANGLEFAN_IMM: {      /* {DWORD dwEdgeFlags} + (count+2) inline verts */
            const BYTE *save_vb; DWORD save_nv, nv, t, vbytes;
            if(avail < 4) return dp2_fail(err_off, hdr, FXD_DP2E_MALFORMED);
            nv = count + 2;                          /* fan pivot + count tris */
            vbytes = nv * w.lay.stride;
            vbytes = (vbytes + 3u) & ~3u;            /* inline data DWORD-aligned */
            need = 4 + vbytes;
            if(need > avail) return dp2_fail(err_off, hdr, FXD_DP2E_MALFORMED);
            save_vb = w.vb; save_nv = w.nverts;
            w.vb = base + opnd + 4; w.nverts = nv;   /* draw from inline verts */
            for(t=0;t<count;t++) (void)tri_emit(&w, 0, t+1, t+2);
            tri_flush(&w);
            w.vb = save_vb; w.nverts = save_nv;
            off = opnd + need;
            break;
        }

        case FXDP2OP_LINELIST_IMM: {         /* (2*count) inline verts */
            const BYTE *save_vb; DWORD save_nv, nv, t, vbytes;
            nv = count * 2;
            vbytes = nv * w.lay.stride;
            vbytes = (vbytes + 3u) & ~3u;            /* inline data DWORD-aligned */
            need = vbytes;
            if(need > avail) return dp2_fail(err_off, hdr, FXD_DP2E_MALFORMED);
            save_vb = w.vb; save_nv = w.nverts;
            w.vb = base + opnd; w.nverts = nv;
            for(t=0;t<count;t++){
                if(!line_emit(&w, 2*t, 2*t+1)){
                    w.n=0; w.vb=save_vb; w.nverts=save_nv;
                    return dp2_fail(err_off, hdr, FXD_DP2E_MALFORMED);
                }
            }
            line_flush(&w);
            w.vb = save_vb; w.nverts = save_nv;
            off = opnd + need;
            break;
        }

        /* ---- recognized ops, consumed-and-skipped for now ----------------- */

        case FXDP2OP_WINFO:                  /* TODO(fxd3d M4c): w-buffer range      */
        case FXDP2OP_ZRANGE:                 /* TODO(fxd3d M4c): z min/max           */
        case FXDP2OP_SETRENDERTARGET:        /* TODO(fxd3d M4c): retarget + zbuffer  */
            need = count*8;
            if(need > avail) return dp2_fail(err_off, hdr, FXD_DP2E_MALFORMED);
            off = opnd + need;
            break;

        case FXDP2OP_VIEWPORTINFO:           /* TODO(fxd3d M4c): viewport/scissor    */
            need = count*16;
            if(need > avail) return dp2_fail(err_off, hdr, FXD_DP2E_MALFORMED);
            off = opnd + need;
            break;

        case FXDP2OP_SETPALETTE:             /* TODO(fxd3d M4c): palette attach      */
        case FXDP2OP_STATESET:               /* TODO(fxd3d M4c): state blocks        */
            need = count*12;
            if(need > avail) return dp2_fail(err_off, hdr, FXD_DP2E_MALFORMED);
            off = opnd + need;
            break;

        case FXDP2OP_TEXBLT:                 /* TODO(fxd3d M4c): texture blt         */
            need = count*36;
            if(need > avail) return dp2_fail(err_off, hdr, FXD_DP2E_MALFORMED);
            off = opnd + need;
            break;

        case FXDP2OP_CLEAR: {                /* TODO(fxd3d M4c): gb target/z clear   */
            /* one 16-byte {flags,color,depth,stencil} header + count RECTs   */
            need = 16 + count*16;
            if(need > avail) return dp2_fail(err_off, hdr, FXD_DP2E_MALFORMED);
            off = opnd + need;
            break;
        }

        case FXDP2OP_UPDATEPALETTE: {        /* TODO(fxd3d M4c): palette entries     */
            /* one {DWORD handle; WORD start,num} + num DWORD entries inline  */
            DWORD num;
            if(avail < 8) return dp2_fail(err_off, hdr, FXD_DP2E_MALFORMED);
            num = rd_u16(base + opnd + 6);
            need = 8 + num*4;
            if(need > avail) return dp2_fail(err_off, hdr, FXD_DP2E_MALFORMED);
            off = opnd + need;
            break;
        }

        default:
            /* Unknown/unsupported opcode. The DDK contract (d3dhal.h
             * "PFND3DPARSEUNKNOWNCOMMAND"): the DRIVER hands the command to the
             * runtime's parse-unknown callback, which parses/skips exactly that
             * one command and returns where to resume - so a state op we don't
             * implement (SETMATERIAL, SETLIGHT, SETTRANSFORM, ...) no longer
             * aborts the whole batch. Only if the callback itself can't parse
             * it (or none was supplied) do we STOP with COMMAND_UNPARSED at the
             * offending header. */
            if(parse){
                DWORD next = 0;
                if(parse(parse_ctx, base, hdr, cmd_len, &next) == 0
                   && next > hdr && next <= cmd_len){
                    off = next;          /* runtime consumed it; resume after  */
                    ncmd++;
                    continue;
                }
            }
            return dp2_fail(err_off, hdr, FXD_DP2E_UNKNOWN_OP);
        }

        ncmd++;
    }
    return ncmd;
}

/* Back-compat wrapper: no unknown-command hook (unknown opcode -> stop). */
int fxd_dp2_execute_real(fxd_device *dev, const void *cmds, DWORD cmd_len,
                         const void *verts, DWORD vert_count, DWORD fvf,
                         DWORD *rstates, DWORD *err_off)
{
    return fxd_dp2_execute_real_cb(dev, cmds, cmd_len, verts, vert_count,
                                   fvf, rstates, err_off, 0, 0);
}
