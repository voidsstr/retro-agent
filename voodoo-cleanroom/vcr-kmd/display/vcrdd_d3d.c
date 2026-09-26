/*
 * vcrdd_d3d.c - the Direct3D HAL (DX7-level NT DDI) on the Voodoo 3D engine.
 *
 * Shape: a DX7 "DrawPrimitives2" driver without transform and lighting. The
 * runtime transforms, lights and clips; everything reaching us is
 * pre-transformed vertices (D3DFVF_XYZRHW) and the command stream that draws
 * them. The pieces:
 *
 *   registration  DD_HALINFO's D3D global data (the DX5 caps) and callbacks;
 *                 GetDriverInfo answers GUID_D3DCallbacks3 (DrawPrimitives2,
 *                 Clear2), GUID_D3DExtendedCaps, GUID_ZPixelFormats and
 *                 GUID_Miscellaneous2Callbacks (CreateSurfaceEx - how DX7
 *                 names surfaces by handle - GetDriverState, DestroyDDLocal).
 *   contexts      ContextCreate/Destroy(All): a render target, a Z buffer and
 *                 the state the command stream sets.
 *   surfaces      a (DirectDraw local, handle) -> surface table, filled by
 *                 CreateSurfaceEx and emptied by DestroySurface/DestroyDDLocal.
 *   DP2           every DX7 opcode is parsed (so the walk never loses its
 *                 place); state, triangles, clears, TEXBLT and render-target
 *                 switches are executed, the rest is skipped by its size or
 *                 handed to the runtime's parse-unknown callback.
 *   state         D3D render and texture-stage state -> fbzColorPath, fbzMode,
 *                 alphaMode, textureMode, tLOD, texBaseAddr (integer only here;
 *                 the register writer vcrdd_3d.c owns the floats).
 *
 * Every walk is bounds-checked against the command length and the vertex
 * count: a malformed stream stops the walk, it never reads past a buffer.
 * The runtime's buffers are system-memory surfaces of the calling process,
 * locked by dxg for the duration of the call.
 */
/* the DX7 DDI: the D3D headers hide its records below this version */
#define DIRECT3D_VERSION 0x0700
#include "vcrdd.h"

#ifdef VCR_HAVE_DDI
#include <d3dnthal.h>
#include "vcrdd_3d.h"
#include "../include/vcr_texlod.h"
#include "../include/vcr_fog.h"

#define MAX_CTX         32
#define MAX_HANDLES     4096            /* power of two */
#define RS_MAX          256
#define TSS_MAX         32

typedef struct vcr_d3dctx {
    ULONG               in_use;
    VCR_PDEV           *pd;
    PDD_DIRECTDRAW_LOCAL ddlcl;
    DWORD               pid;
    PDD_SURFACE_LOCAL   rt, zb;
    DWORD               rs[RS_MAX];
    DWORD               tss[TSS_MAX];   /* stage 0 */
    DWORD               tex_handle;     /* stage 0 texture */
    DWORD               tss1[TSS_MAX];  /* stage 1 (the second TMU) */
    DWORD               tex_handle1;
    DWORD               tex_refused;    /* last handle logged as unusable */
    DWORD               mip_logged;
    PDD_SURFACE_LOCAL   tex_surf0, tex_surf1;   /* the bound textures, for the dirty check */
    ULONG               tex_writes_seen;
    ULONG               fog_vertex;     /* vertex fog: needs the specular colour */
    ULONG               dirty;
    vcr3d_regs          regs;
    vcr3d_target        target;
    ULONG               tex_w, tex_h, tex_ok;     /* TMU0's texture */
    ULONG               tex1_w, tex1_h;            /* TMU1's, with two stages */
    PVOID               fpu;            /* EngSaveFloatingPointState buffer */
    ULONG               tris, clears, dp2s, unparsed, tex_flushes;
} vcr_d3dctx;

typedef struct handle_ent {
    PDD_DIRECTDRAW_LOCAL ddlcl;
    DWORD               handle;
    PDD_SURFACE_LOCAL   surf;
} handle_ent;

static vcr_d3dctx g_ctx[MAX_CTX];
static ULONG g_tex_writes;              /* textures written since boot (VcrDdD3dTexWritten) */
static handle_ent g_h[MAX_HANDLES];
static PFND3DNTPARSEUNKNOWNCOMMAND g_parse_unknown;
static ULONG g_fpu_size;

/* ---- surfaces by handle ------------------------------------------------------------- */

static ULONG hslot(PDD_DIRECTDRAW_LOCAL l, DWORD h)
{
    return (((ULONG)(ULONG_PTR)l >> 4) * 2654435761u + h) & (MAX_HANDLES - 1);
}

static void handle_set(PDD_DIRECTDRAW_LOCAL l, DWORD h, PDD_SURFACE_LOCAL s)
{
    ULONG i, k = hslot(l, h), free_k = MAX_HANDLES;
    for (i = 0; i < MAX_HANDLES; i++, k = (k + 1) & (MAX_HANDLES - 1)) {
        if (g_h[k].ddlcl == l && g_h[k].handle == h) {
            g_h[k].surf = s;
            return;
        }
        if (!g_h[k].ddlcl) {
            if (free_k == MAX_HANDLES)
                free_k = k;
            break;
        }
    }
    if (free_k == MAX_HANDLES) {
        VcrDd(VCR_LV_WARN, VCR_EV_DD_D3D, 9, h, 0, 0, "surface handle table full");
        return;
    }
    g_h[free_k].ddlcl = l;
    g_h[free_k].handle = h;
    g_h[free_k].surf = s;
}

static PDD_SURFACE_LOCAL handle_get(PDD_DIRECTDRAW_LOCAL l, DWORD h)
{
    ULONG i, k = hslot(l, h);
    if (!h)
        return NULL;
    for (i = 0; i < MAX_HANDLES; i++, k = (k + 1) & (MAX_HANDLES - 1)) {
        if (!g_h[k].ddlcl)
            return NULL;
        if (g_h[k].ddlcl == l && g_h[k].handle == h)
            return g_h[k].surf;
    }
    return NULL;
}

/* forget a surface (or a whole DirectDraw local): open addressing, so the
 * table is rebuilt without the removed entries */
static void handle_forget(PDD_DIRECTDRAW_LOCAL l, PDD_SURFACE_LOCAL s)
{
    static handle_ent keep[MAX_HANDLES];
    ULONG i, n = 0;
    for (i = 0; i < MAX_HANDLES; i++) {
        if (!g_h[i].ddlcl || (l && g_h[i].ddlcl == l) || (s && g_h[i].surf == s))
            continue;
        keep[n++] = g_h[i];
    }
    memset(g_h, 0, sizeof g_h);
    for (i = 0; i < n; i++)
        handle_set(keep[i].ddlcl, keep[i].handle, keep[i].surf);
}

void VcrDdD3dSurfaceGone(PDD_SURFACE_LOCAL s)
{
    ULONG i;
    for (i = 0; i < MAX_CTX; i++) {
        if (!g_ctx[i].in_use)
            continue;
        if (g_ctx[i].rt == s)
            g_ctx[i].rt = NULL;
        if (g_ctx[i].zb == s)
            g_ctx[i].zb = NULL;
    }
    handle_forget(NULL, s);
}

/* does the surface carry its own pixel format? On NT DDRAWISURF_HASPIXELFORMAT
 * is NOT set on a Direct3D texture whose ddpfSurface is filled in (measured
 * on the 86Box Voodoo3: a 64x64 R5G6B5 managed texture, dwFlags 0) - the
 * format itself is the evidence */
static int has_pixfmt(PDD_SURFACE_LOCAL s)
{
    return (s->dwFlags & DDRAWISURF_HASPIXELFORMAT) || s->lpGbl->ddpfSurface.dwRGBBitCount;
}

static ULONG surf_bpp(VCR_PDEV *pd, PDD_SURFACE_LOCAL s)
{
    if (has_pixfmt(s))
        return s->lpGbl->ddpfSurface.dwRGBBitCount;
    return pd->bpp;
}

static int in_vidmem(PDD_SURFACE_LOCAL s)
{
    return s && s->lpGbl && !(s->ddsCaps.dwCaps & DDSCAPS_SYSTEMMEMORY);
}

/* ---- mipmap chains ------------------------------------------------------------------------
 * The TMU finds level n of a texture by adding the sizes of the levels above
 * it to the base, so a chain must be ONE block with its levels packed back to
 * back - the runtime would place every level separately. So the driver
 * allocates a chain itself, from DirectDraw's own heap (HeapVidMemAllocAligned
 * on the VIDEOMEMORY entry the heap was built in), and frees it in
 * DestroySurface. The surface-local dwReserved1 marks what is ours. */

/* DD_SURFACE_LOCAL.dwReserved1 (the display driver's): a magic in the top
 * half, flags below. */
#define SF_MAGIC    0x76630000u         /* 'vc' */
#define SF_MIP_TOP  0x1u                /* the level that owns the block */
#define SF_MIP_LVL  0x2u                /* a level inside someone's block */
#define SF_DIRTY    0x4u                /* the CPU wrote it since the TMU last looked */
#define SF_IS(s, f) (((s)->dwReserved1 & 0xffff0000u) == SF_MAGIC && ((s)->dwReserved1 & (f)))
#define MIP_TOP     (SF_MAGIC | SF_MIP_TOP)
#define MIP_LEVEL   (SF_MAGIC | SF_MIP_LVL)

typedef struct { DWORD dwStartAlignment, dwPitchAlignment, dwFlags, dwReserved2; } vcr_surfalign;
FLATPTR APIENTRY HeapVidMemAllocAligned(VIDEOMEMORY *vm, DWORD w, DWORD h, vcr_surfalign *a,
                                        LONG *pitch);
void APIENTRY VidMemFree(PVOID heap, FLATPTR ptr);

static ULONG big_side(ULONG w, ULONG h)
{
    return w > h ? w : h;
}

static ULONG ilog2(ULONG v)
{
    ULONG l = 0;
    while (v > 1) {
        v >>= 1;
        l++;
    }
    return l;
}

/* bytes before level k of a w x h 16 bpp chain (levels shrink to 1 x 1) */
static ULONG mip_offset(ULONG w, ULONG h, ULONG k)
{
    ULONG off = 0, i;
    for (i = 0; i < k; i++) {
        ULONG lw = w >> i, lh = h >> i;
        off += (lw ? lw : 1) * (lh ? lh : 1) * 2;
    }
    return off;
}

int VcrDdD3dCreateMipChain(VCR_PDEV *pd, PDD_CREATESURFACEDATA p)
{
    VIDEOMEMORY *vm = (VIDEOMEMORY *)pd->pvmList;
    PDD_SURFACE_LOCAL top;
    DDSURFACEDESC *sd = (DDSURFACEDESC *)p->lpDDSurfaceDesc;
    ULONG i, w0, h0, levels = 0, bpp;
    FLATPTR base;
    vcr_surfalign al;
    vcr_texlod t;
    LONG pitch = 0;
    if (!p->dwSCnt || !pd->pjRegs || !pd->g2d_ok || pd->d3d_disabled)
        return 0;
    top = p->lplpSList[0];
    if ((top->ddsCaps.dwCaps & (DDSCAPS_TEXTURE | DDSCAPS_MIPMAP)) !=
            (DDSCAPS_TEXTURE | DDSCAPS_MIPMAP) || (top->ddsCaps.dwCaps & DDSCAPS_SYSTEMMEMORY))
        return 0;
    bpp = sd && (sd->dwFlags & DDSD_PIXELFORMAT) ? sd->ddpfPixelFormat.dwRGBBitCount : pd->bpp;
    w0 = top->lpGbl->wWidth;
    h0 = top->lpGbl->wHeight;
    if (bpp != 16 || vcr_texlod_compute(w0, h0, 2, w0 * 2, 0, &t))
        return 0;                       /* the chip cannot sample it: the runtime places it */
    if (!vm || !vm->lpHeap) {
        VcrDd(VCR_LV_WARN, VCR_EV_DD_D3D, 10, (ULONG)(ULONG_PTR)vm, 0, 0,
              "mipmap chain: no DirectDraw heap to allocate from (list %p)", vm);
        return 0;
    }
    for (i = 0; i < p->dwSCnt; i++) {   /* every level must be a halving of the top */
        PDD_SURFACE_LOCAL s = p->lplpSList[i];
        ULONG k = ilog2(big_side(w0, h0)) - ilog2(big_side(s->lpGbl->wWidth, s->lpGbl->wHeight));
        ULONG ew = w0 >> k, eh = h0 >> k;
        if (s->lpGbl->wWidth != (ew ? ew : 1) || s->lpGbl->wHeight != (eh ? eh : 1))
            return 0;
        if (k + 1 > levels)
            levels = k + 1;
    }
    memset(&al, 0, sizeof al);
    al.dwStartAlignment = 16;
    al.dwPitchAlignment = 16;
    base = HeapVidMemAllocAligned(vm, mip_offset(w0, h0, levels), 1, &al, &pitch);
    if (!base) {
        VcrDd(VCR_LV_WARN, VCR_EV_DD_D3D, 10, w0, h0, levels,
              "mipmap chain %ux%u x%u: out of video memory", w0, h0, levels);
        p->ddRVal = DDERR_OUTOFVIDEOMEMORY;
        return 1;
    }
    for (i = 0; i < p->dwSCnt; i++) {
        PDD_SURFACE_LOCAL s = p->lplpSList[i];
        ULONG k = ilog2(big_side(w0, h0)) - ilog2(big_side(s->lpGbl->wWidth, s->lpGbl->wHeight));
        s->lpGbl->fpVidMem = base + mip_offset(w0, h0, k);
        s->lpGbl->lPitch = s->lpGbl->wWidth * 2;
        s->lpGbl->dwReserved1 = (ULONG_PTR)vm->lpHeap;
        s->dwReserved1 = s == top ? MIP_TOP : MIP_LEVEL;
        s->ddsCaps.dwCaps = (s->ddsCaps.dwCaps & ~DDSCAPS_SYSTEMMEMORY) | DDSCAPS_VIDEOMEMORY |
                            DDSCAPS_LOCALVIDMEM;
    }
    VcrDd(VCR_LV_DEBUG, VCR_EV_DD_D3D, 11, (ULONG)base, (w0 << 16) | h0, levels,
          "mipmap chain %ux%u, %u levels at %x (desc flags %x mipcount %u)", w0, h0, levels,
          (ULONG)base, sd ? sd->dwFlags : 0, sd && (sd->dwFlags & DDSD_MIPMAPCOUNT) ? sd->dwMipMapCount : 0);
    p->ddRVal = DD_OK;
    return 1;
}

int VcrDdD3dFreeMipChain(VCR_PDEV *pd, PDD_SURFACE_LOCAL s)
{
    (void)pd;
    if (!s || !s->lpGbl)
        return 0;
    if (SF_IS(s, SF_MIP_TOP)) {
        VidMemFree((PVOID)s->lpGbl->dwReserved1, s->lpGbl->fpVidMem);
        s->dwReserved1 = 0;
        return 1;
    }
    if (SF_IS(s, SF_MIP_LVL)) {
        s->dwReserved1 = 0;
        return 1;
    }
    return 0;
}

/* a texture was written by the CPU (Unlock, a HEL blit) or by TEXBLT: the
 * TMU's texture cache does not see that - flag it, and the next draw that
 * samples it flushes first. Every level of a chain is flagged on its top. */
void VcrDdD3dTexWritten(PDD_SURFACE_LOCAL s)
{
    if (!s || !(s->ddsCaps.dwCaps & DDSCAPS_TEXTURE) || !in_vidmem(s))
        return;
    if ((s->dwReserved1 & 0xffff0000u) != SF_MAGIC)
        s->dwReserved1 = SF_MAGIC;
    s->dwReserved1 |= SF_DIRTY;
    g_tex_writes++;
}

/* the next smaller level of a mipmap chain (attached to this one) */
static PDD_SURFACE_LOCAL next_mip(PDD_SURFACE_LOCAL s)
{
    PDD_ATTACHLIST a;
    for (a = s->lpAttachList; a; a = a->lpLink)
        if (a->lpAttached && (a->lpAttached->ddsCaps.dwCaps & DDSCAPS_MIPMAP) &&
            a->lpAttached->lpGbl && a->lpAttached->lpGbl->wWidth * a->lpAttached->lpGbl->wHeight <
                                        s->lpGbl->wWidth * s->lpGbl->wHeight)
            return a->lpAttached;
    return NULL;
}

/* ---- state -> registers ------------------------------------------------------------------ */

static ULONG blend_src(DWORD b)
{
    switch (b) {
    case D3DBLEND_ZERO:          return BF_ZERO;
    case D3DBLEND_SRCALPHA:      return BF_SRCALPHA;
    case D3DBLEND_INVSRCALPHA:   return BF_INVSRCALPHA;
    case D3DBLEND_DESTALPHA:     return BF_DSTALPHA;
    case D3DBLEND_INVDESTALPHA:  return BF_INVDSTALPHA;
    case D3DBLEND_DESTCOLOR:     return BF_COLOR;       /* a source factor's COLOR is the destination's */
    case D3DBLEND_INVDESTCOLOR:  return BF_INVCOLOR;
    case D3DBLEND_SRCALPHASAT:   return BF_SATURATE;
    case D3DBLEND_BOTHSRCALPHA:  return BF_SRCALPHA;
    case D3DBLEND_BOTHINVSRCALPHA: return BF_INVSRCALPHA;
    default:                     return BF_ONE;         /* ONE, and SRCCOLOR the chip cannot */
    }
}

static ULONG blend_dst(DWORD b, DWORD src)
{
    if (src == D3DBLEND_BOTHSRCALPHA)
        return BF_INVSRCALPHA;
    if (src == D3DBLEND_BOTHINVSRCALPHA)
        return BF_SRCALPHA;
    switch (b) {
    case D3DBLEND_ZERO:          return BF_ZERO;
    case D3DBLEND_SRCCOLOR:      return BF_COLOR;       /* a destination factor's COLOR is the source's */
    case D3DBLEND_INVSRCCOLOR:   return BF_INVCOLOR;
    case D3DBLEND_SRCALPHA:      return BF_SRCALPHA;
    case D3DBLEND_INVSRCALPHA:   return BF_INVSRCALPHA;
    case D3DBLEND_DESTALPHA:     return BF_DSTALPHA;
    case D3DBLEND_INVDESTALPHA:  return BF_INVDSTALPHA;
    default:                     return BF_ONE;         /* ONE, and DESTCOLOR the chip cannot */
    }
}

static ULONG cmp(DWORD f)
{
    return f >= D3DCMP_NEVER && f <= D3DCMP_ALWAYS ? f - 1 : 7;     /* D3DCMP_* - 1 = LT|EQ|GT bits */
}

/* is this argument the texture / the diffuse colour / the texture factor? */
#define ARG_KIND(a)     ((a) & D3DTA_SELECTMASK)

/* the colour combine for stage 0: other = texture (or c1 for TFACTOR),
 * local = the iterated (diffuse) colour. Unsupported ops degrade to the
 * nearest one the chip has, never to nothing. */
static ULONG color_path(vcr_d3dctx *c, int tex)
{
    DWORD op = c->tss[D3DTSS_COLOROP], a1 = ARG_KIND(c->tss[D3DTSS_COLORARG1]),
          a2 = ARG_KIND(c->tss[D3DTSS_COLORARG2]);
    DWORD aop = c->tss[D3DTSS_ALPHAOP], b1 = ARG_KIND(c->tss[D3DTSS_ALPHAARG1]),
          b2 = ARG_KIND(c->tss[D3DTSS_ALPHAARG2]);
    ULONG cp = 0, sel;
    int uses_tex = 0;

    /* ---- RGB */
    if (op == D3DTOP_SELECTARG2) {
        a1 = a2;
        op = D3DTOP_SELECTARG1;
    }
    if (op == D3DTOP_DISABLE || (!tex && (a1 == D3DTA_TEXTURE || a2 == D3DTA_TEXTURE))) {
        cp |= CP_RGBSEL_ITER | CP_CC_M(CC_M_ONE);                       /* diffuse */
    } else if (op == D3DTOP_SELECTARG1) {
        sel = a1 == D3DTA_TEXTURE ? CP_RGBSEL_TMU : a1 == D3DTA_TFACTOR ? CP_RGBSEL_C1
                                                                        : CP_RGBSEL_ITER;
        cp |= sel | CP_CC_M(CC_M_ONE);                                  /* pass other */
        uses_tex = a1 == D3DTA_TEXTURE;
    } else {
        /* two-argument ops: the texture (or TFACTOR) is "other", diffuse "local" */
        DWORD o = a1 == D3DTA_TEXTURE || a2 == D3DTA_TEXTURE ? D3DTA_TEXTURE
                : a1 == D3DTA_TFACTOR || a2 == D3DTA_TFACTOR ? D3DTA_TFACTOR : D3DTA_DIFFUSE;
        cp |= o == D3DTA_TEXTURE ? CP_RGBSEL_TMU : o == D3DTA_TFACTOR ? CP_RGBSEL_C1
                                                                      : CP_RGBSEL_ITER;
        uses_tex = o == D3DTA_TEXTURE;
        switch (op) {
        case D3DTOP_ADD:
            cp |= CP_CC_M(CC_M_ONE) | CP_CC_ADD_CLOCAL;
            break;
        case D3DTOP_BLENDTEXTUREALPHA:          /* (other - local) * a_tex + local */
            cp |= CP_CC_SUB_CLOCAL | CP_CC_M(CC_M_ATMU) | CP_CC_REVERSE | CP_CC_ADD_CLOCAL;
            break;
        case D3DTOP_BLENDDIFFUSEALPHA:          /* (other - local) * a_diffuse + local */
            cp |= CP_CC_SUB_CLOCAL | CP_CC_M(CC_M_ALOCAL) | CP_CC_REVERSE | CP_CC_ADD_CLOCAL;
            break;
        default:                                /* MODULATE, MODULATE2X/4X, and the rest */
            cp |= CP_CC_M(CC_M_CLOCAL) | CP_CC_REVERSE;
            break;
        }
    }

    /* ---- alpha: the same, on the alpha channel */
    if (aop == D3DTOP_SELECTARG2) {
        b1 = b2;
        aop = D3DTOP_SELECTARG1;
    }
    if (aop == D3DTOP_DISABLE || (!tex && (b1 == D3DTA_TEXTURE || b2 == D3DTA_TEXTURE))) {
        cp |= CP_ASEL_ITER | CP_CCA_M(CC_M_ONE);
    } else if (aop == D3DTOP_SELECTARG1) {
        cp |= (b1 == D3DTA_TEXTURE ? CP_ASEL_TMU : b1 == D3DTA_TFACTOR ? CP_ASEL_C1
                                                                      : CP_ASEL_ITER) |
              CP_CCA_M(CC_M_ONE);
        uses_tex |= b1 == D3DTA_TEXTURE;
    } else {
        DWORD o = b1 == D3DTA_TEXTURE || b2 == D3DTA_TEXTURE ? D3DTA_TEXTURE
                : b1 == D3DTA_TFACTOR || b2 == D3DTA_TFACTOR ? D3DTA_TFACTOR : D3DTA_DIFFUSE;
        cp |= o == D3DTA_TEXTURE ? CP_ASEL_TMU : o == D3DTA_TFACTOR ? CP_ASEL_C1 : CP_ASEL_ITER;
        uses_tex |= o == D3DTA_TEXTURE;
        if (aop == D3DTOP_ADD)
            cp |= CP_CCA_M(CC_M_ONE) | CP_CCA_ADD_CLOCAL;
        else
            cp |= CP_CCA_M(CC_M_CLOCAL) | CP_CCA_REVERSE;
    }
    if (uses_tex && tex)
        cp |= CP_TEXTURE;
    return cp;
}

/* the TMU view of a texture surface (common/vcr_texlod.c: LOD, aspect, and
 * the base the chip wants - where LOD 0 would be) under one stage's filter
 * and address state. FALSE: the chip cannot sample it. */
typedef struct tmu_view {
    ULONG textureMode, tLOD, texBaseAddr, w, h;
} tmu_view;

static BOOL tex_view(const DWORD *tss, PDD_SURFACE_LOCAL s, tmu_view *v)
{
    DDPIXELFORMAT *pf;
    vcr_texlod t;
    ULONG fmt;
    if (!in_vidmem(s) || !has_pixfmt(s))
        return FALSE;
    pf = &s->lpGbl->ddpfSurface;
    if (pf->dwRGBBitCount != 16)
        return FALSE;
    if (pf->dwRBitMask == 0xf800)
        fmt = TF_RGB565;
    else if (pf->dwRBitMask == 0x7c00)
        fmt = TF_ARGB1555;
    else if (pf->dwRBitMask == 0x0f00)
        fmt = TF_ARGB4444;
    else
        return FALSE;
    if (vcr_texlod_compute(s->lpGbl->wWidth, s->lpGbl->wHeight, 2, (ULONG)s->lpGbl->lPitch,
                           (ULONG)s->lpGbl->fpVidMem, &t))
        return FALSE;
    v->texBaseAddr = t.base;
    v->tLOD = t.tlod;
    /* a chain we packed: sample down to its smallest level, unless mipmapping
     * is off (MIPFILTER none: the top level only) */
    if (SF_IS(s, SF_MIP_TOP) && tss[D3DTSS_MIPFILTER] > D3DTFP_NONE) {
        PDD_SURFACE_LOCAL m = s;
        ULONG n = 1, lodmax;
        while ((m = next_mip(m)) != NULL && n < 9)
            n++;
        lodmax = t.lod + n - 1 > 8 ? 8 : t.lod + n - 1;
        v->tLOD = (v->tLOD & ~(0x3fu << 6)) | TL_LODMAX(lodmax);
    }
    v->textureMode = TM_PERSPECTIVE | TM_CLAMPW | TM_FORMAT(fmt);
    if (tss[D3DTSS_MAGFILTER] >= D3DTFG_LINEAR)
        v->textureMode |= TM_MAGFILTER;
    if (tss[D3DTSS_MINFILTER] >= D3DTFN_LINEAR)
        v->textureMode |= TM_MINFILTER;
    if (tss[D3DTSS_ADDRESSU] == D3DTADDRESS_CLAMP)
        v->textureMode |= TM_CLAMPS;
    if (tss[D3DTSS_ADDRESSV] == D3DTADDRESS_CLAMP)
        v->textureMode |= TM_CLAMPT;
    v->w = s->lpGbl->wWidth;
    v->h = s->lpGbl->wHeight;
    return TRUE;
}

/* stage 1 on TMU0, combining its texture (local) with TMU1's = stage 0's
 * (other). D3D's CURRENT also carries stage 0's diffuse term; here the
 * diffuse is applied after, in the colour combine - identical for MODULATE,
 * and for the usual lightmap stages. */
static ULONG stage1_combine(const DWORD *tss)
{
    DWORD op = tss[D3DTSS_COLOROP], aop = tss[D3DTSS_ALPHAOP];
    DWORD a1 = ARG_KIND(tss[D3DTSS_COLORARG1]), a2 = ARG_KIND(tss[D3DTSS_COLORARG2]);
    DWORD b1 = ARG_KIND(tss[D3DTSS_ALPHAARG1]), b2 = ARG_KIND(tss[D3DTSS_ALPHAARG2]);
    ULONG tc, tca;
    if (op == D3DTOP_SELECTARG1)
        tc = a1 == D3DTA_TEXTURE ? TM_TC_REPLACE : TM_TC_PASS;
    else if (op == D3DTOP_SELECTARG2)
        tc = a2 == D3DTA_TEXTURE ? TM_TC_REPLACE : TM_TC_PASS;
    else if (op == D3DTOP_ADD)
        tc = TM_TC_ADD;
    else
        tc = TM_TC_MULT;                            /* MODULATE(2X/4X) and the rest */
    if (aop == D3DTOP_DISABLE)
        tca = TM_TCA_PASS;
    else if (aop == D3DTOP_SELECTARG1)
        tca = b1 == D3DTA_TEXTURE ? TM_TCA_REPLACE : TM_TCA_PASS;
    else if (aop == D3DTOP_SELECTARG2)
        tca = b2 == D3DTA_TEXTURE ? TM_TCA_REPLACE : TM_TCA_PASS;
    else if (aop == D3DTOP_ADD)
        tca = TM_TCA_ADD;
    else
        tca = TM_TCA_MULT;
    return tc | tca;
}

static void compute_regs(vcr_d3dctx *c)
{
    vcr3d_regs *r = &c->regs;
    PDD_SURFACE_LOCAL t = handle_get(c->ddlcl, c->tex_handle),
                      t1 = handle_get(c->ddlcl, c->tex_handle1);
    tmu_view v0, v1;
    int tex = c->tex_handle && tex_view(c->tss, t, &v0);
    int two = tex && c->tss1[D3DTSS_COLOROP] != D3DTOP_DISABLE && c->tex_handle1 &&
              tex_view(c->tss1, t1, &v1);
    DWORD cull = c->rs[D3DRENDERSTATE_CULLMODE];

    if (c->tex_handle && !tex && c->tex_handle != c->tex_refused) {
        /* once per texture: why the chip cannot use it */
        c->tex_refused = c->tex_handle;
        VcrDd(VCR_LV_WARN, VCR_EV_DD_D3D, 7, c->tex_handle,
              t ? t->ddsCaps.dwCaps : 0xffffffffu,
              t && t->lpGbl ? ((ULONG)t->lpGbl->wWidth << 16) | t->lpGbl->wHeight : 0,
              "texture %u refused: surf %p caps %x %ux%u pitch %d flags %x bpp %u rmask %x",
              c->tex_handle, t, t ? t->ddsCaps.dwCaps : 0,
              t && t->lpGbl ? t->lpGbl->wWidth : 0, t && t->lpGbl ? t->lpGbl->wHeight : 0,
              t && t->lpGbl ? t->lpGbl->lPitch : 0, t ? t->dwFlags : 0,
              t && t->lpGbl ? t->lpGbl->ddpfSurface.dwRGBBitCount : 0,
              t && t->lpGbl ? t->lpGbl->ddpfSurface.dwRBitMask : 0);
    }
    c->tex_surf0 = tex ? t : NULL;
    c->tex_surf1 = two ? t1 : NULL;
    /* one texture: TMU0 samples stage 0 and passes it on. Two: TMU1 samples
     * stage 0, TMU0 samples stage 1 and combines the two */
    r->textured1 = two;
    if (two) {
        r->textureMode1 = v0.textureMode | TM_TC_REPLACE | TM_TCA_REPLACE;
        r->tLOD1 = v0.tLOD;
        r->texBaseAddr1 = v0.texBaseAddr;
        r->textureMode = v1.textureMode | stage1_combine(c->tss1);
        r->tLOD = v1.tLOD;
        r->texBaseAddr = v1.texBaseAddr;
        c->tex_w = v1.w;
        c->tex_h = v1.h;
        c->tex1_w = v0.w;
        c->tex1_h = v0.h;
    } else if (tex) {
        r->textureMode = v0.textureMode | TM_TC_REPLACE | TM_TCA_REPLACE;
        r->tLOD = v0.tLOD;
        r->texBaseAddr = v0.texBaseAddr;
        c->tex_w = v0.w;
        c->tex_h = v0.h;
    }
    r->textured = tex;
    c->tex_ok = tex;
    r->fbzColorPath = color_path(c, tex);
    r->fbzMode = FZ_RECTCLIP | FZ_RGBWRITE;
    if (c->rs[D3DRENDERSTATE_DITHERENABLE])
        r->fbzMode |= FZ_DITHER;
    if (c->zb && c->rs[D3DRENDERSTATE_ZENABLE] == D3DZB_TRUE) {
        r->fbzMode |= FZ_DEPTH | FZ_ZFUNC(cmp(c->rs[D3DRENDERSTATE_ZFUNC]));
        if (c->rs[D3DRENDERSTATE_ZWRITEENABLE])
            r->fbzMode |= FZ_ZAWRITE;
    }
    r->alphaMode = 0;
    if (c->rs[D3DRENDERSTATE_ALPHATESTENABLE])
        r->alphaMode |= AM_ATEST | AM_AFUNC(cmp(c->rs[D3DRENDERSTATE_ALPHAFUNC])) |
                        AM_AREF(c->rs[D3DRENDERSTATE_ALPHAREF] & 0xff);
    if (c->rs[D3DRENDERSTATE_ALPHABLENDENABLE]) {
        DWORD s = c->rs[D3DRENDERSTATE_SRCBLEND], d = c->rs[D3DRENDERSTATE_DESTBLEND];
        r->alphaMode |= AM_BLEND | AM_RGBSRC(blend_src(s)) | AM_RGBDST(blend_dst(d, s)) |
                        AM_ASRC(BF_ONE) | AM_ADST(BF_ZERO);
    }
    /* fog: table fog on W, or vertex fog carried in the specular alpha
     * (vcr_fog.h); FOGSTART/END/DENSITY are float bit patterns */
    r->fogMode = 0;
    c->fog_vertex = 0;
    if (c->rs[D3DRENDERSTATE_FOGENABLE]) {
        DWORD tm = c->rs[D3DRENDERSTATE_FOGTABLEMODE];
        r->fogMode = FM_ENABLE | FM_DITHER;
        if (tm == D3DFOG_LINEAR || tm == D3DFOG_EXP || tm == D3DFOG_EXP2) {
            r->fog_table[0] = tm == D3DFOG_LINEAR ? VCR_FOG_LINEAR
                            : tm == D3DFOG_EXP ? VCR_FOG_EXP : VCR_FOG_EXP2;
            r->fog_table[1] = c->rs[D3DRENDERSTATE_FOGSTART];
            r->fog_table[2] = c->rs[D3DRENDERSTATE_FOGEND];
            r->fog_table[3] = c->rs[D3DRENDERSTATE_FOGDENSITY];
        } else {
            memset(r->fog_table, 0, sizeof r->fog_table);   /* VCR_FOG_RAMP */
            c->fog_vertex = 1;
        }
    }
    r->fogColor = c->rs[D3DRENDERSTATE_FOGCOLOR] & 0xffffff;
    r->c0 = 0;
    r->c1 = c->rs[D3DRENDERSTATE_TEXTUREFACTOR];
    r->setupMode = SM_RGB | SM_A | SM_Z | SM_WFBI | (tex ? SM_W0 | SM_ST0 : 0) |
                   (two ? SM_W1 | SM_ST1 : 0);
    if (cull == D3DCULL_CCW)
        r->setupMode |= SM_CULL | SM_CULL_NEGATIVE;
    else if (cull == D3DCULL_CW)
        r->setupMode |= SM_CULL;
    c->dirty = 0;
}

static void target_of(vcr_d3dctx *c)
{
    vcr3d_target *t = &c->target;
    memset(t, 0, sizeof *t);
    if (!in_vidmem(c->rt))
        return;
    t->rt_off = (ULONG)c->rt->lpGbl->fpVidMem;
    t->rt_pitch = (ULONG)c->rt->lpGbl->lPitch;
    t->width = c->rt->lpGbl->wWidth;
    t->height = c->rt->lpGbl->wHeight;
    if (in_vidmem(c->zb)) {
        t->z_off = (ULONG)c->zb->lpGbl->fpVidMem;
        t->z_pitch = (ULONG)c->zb->lpGbl->lPitch;
    }
}

/* ---- contexts --------------------------------------------------------------------------- */

static vcr_d3dctx *ctx_of(ULONG_PTR h)
{
    if (h < 1 || h > MAX_CTX || !g_ctx[h - 1].in_use)
        return NULL;
    return &g_ctx[h - 1];
}

static void ctx_defaults(vcr_d3dctx *c)
{
    memset(c->rs, 0, sizeof c->rs);
    memset(c->tss, 0, sizeof c->tss);
    c->rs[D3DRENDERSTATE_ZENABLE] = c->zb ? D3DZB_TRUE : D3DZB_FALSE;
    c->rs[D3DRENDERSTATE_ZWRITEENABLE] = TRUE;
    c->rs[D3DRENDERSTATE_ZFUNC] = D3DCMP_LESSEQUAL;
    c->rs[D3DRENDERSTATE_SRCBLEND] = D3DBLEND_ONE;
    c->rs[D3DRENDERSTATE_DESTBLEND] = D3DBLEND_ZERO;
    c->rs[D3DRENDERSTATE_CULLMODE] = D3DCULL_CCW;
    c->rs[D3DRENDERSTATE_ALPHAFUNC] = D3DCMP_ALWAYS;
    c->rs[D3DRENDERSTATE_TEXTUREFACTOR] = 0xffffffff;
    c->tss[D3DTSS_COLOROP] = D3DTOP_MODULATE;
    c->tss[D3DTSS_COLORARG1] = D3DTA_TEXTURE;
    c->tss[D3DTSS_COLORARG2] = D3DTA_CURRENT;
    c->tss[D3DTSS_ALPHAOP] = D3DTOP_SELECTARG1;
    c->tss[D3DTSS_ALPHAARG1] = D3DTA_TEXTURE;
    c->tss[D3DTSS_ALPHAARG2] = D3DTA_CURRENT;
    c->tss[D3DTSS_ADDRESSU] = c->tss[D3DTSS_ADDRESSV] = D3DTADDRESS_WRAP;
    c->tss[D3DTSS_MAGFILTER] = D3DTFG_POINT;
    c->tss[D3DTSS_MINFILTER] = D3DTFN_POINT;
    c->tex_handle = 0;
    memset(c->tss1, 0, sizeof c->tss1);
    c->tss1[D3DTSS_COLOROP] = D3DTOP_DISABLE;
    c->tss1[D3DTSS_ALPHAOP] = D3DTOP_DISABLE;
    c->tss1[D3DTSS_COLORARG1] = D3DTA_TEXTURE;
    c->tss1[D3DTSS_COLORARG2] = D3DTA_CURRENT;
    c->tss1[D3DTSS_ALPHAARG1] = D3DTA_TEXTURE;
    c->tss1[D3DTSS_ALPHAARG2] = D3DTA_CURRENT;
    c->tss1[D3DTSS_TEXCOORDINDEX] = 1;
    c->tss1[D3DTSS_ADDRESSU] = c->tss1[D3DTSS_ADDRESSV] = D3DTADDRESS_WRAP;
    c->tss1[D3DTSS_MAGFILTER] = D3DTFG_POINT;
    c->tss1[D3DTSS_MINFILTER] = D3DTFN_POINT;
    c->tex_handle1 = 0;
    c->dirty = 1;
}

static DWORD APIENTRY D3d_ContextCreate(LPD3DNTHAL_CONTEXTCREATEDATA p)
{
    PDD_DIRECTDRAW_LOCAL l = p->lpDDLcl;
    VCR_PDEV *pd = (VCR_PDEV *)l->lpGbl->dhpdev;
    ULONG i;
    for (i = 0; i < MAX_CTX && g_ctx[i].in_use; i++)
        ;
    if (i == MAX_CTX) {
        p->ddrval = DDERR_OUTOFMEMORY;
        return DDHAL_DRIVER_HANDLED;
    }
    if (!g_fpu_size)
        g_fpu_size = EngSaveFloatingPointState(NULL, 0);
    memset(&g_ctx[i], 0, sizeof g_ctx[i]);
    g_ctx[i].fpu = EngAllocMem(FL_ZERO_MEMORY, g_fpu_size ? g_fpu_size : 512, VCRDD_TAG);
    if (!g_ctx[i].fpu) {
        p->ddrval = DDERR_OUTOFMEMORY;
        return DDHAL_DRIVER_HANDLED;
    }
    g_ctx[i].in_use = 1;
    g_ctx[i].pd = pd;
    g_ctx[i].ddlcl = l;
    g_ctx[i].pid = p->dwPID;
    g_ctx[i].rt = p->lpDDSLcl;
    g_ctx[i].zb = p->lpDDSZLcl;
    ctx_defaults(&g_ctx[i]);
    target_of(&g_ctx[i]);
    p->dwhContext = i + 1;
    p->ddrval = DD_OK;
    VcrDd(VCR_LV_INFO, VCR_EV_DD_D3D, 1, i + 1, g_ctx[i].target.rt_off, g_ctx[i].target.z_off,
          "ContextCreate %u: target %x (%ux%u pitch %u), z %x, pid %u", i + 1,
          g_ctx[i].target.rt_off, g_ctx[i].target.width, g_ctx[i].target.height,
          g_ctx[i].target.rt_pitch, g_ctx[i].target.z_off, p->dwPID);
    return DDHAL_DRIVER_HANDLED;
}

static void ctx_free(vcr_d3dctx *c)
{
    VcrDd(VCR_LV_INFO, VCR_EV_DD_D3D, 2, (ULONG)(c - g_ctx) + 1, c->dp2s, c->tris,
          "context %u gone: %u DrawPrimitives2, %u triangles, %u clears, %u unparsed, "
          "%u texture flushes", (ULONG)(c - g_ctx) + 1, c->dp2s, c->tris, c->clears,
          c->unparsed, c->tex_flushes);
    if (c->pd)
        VcrDd2dSync(c->pd);
    if (c->fpu)
        EngFreeMem(c->fpu);
    memset(c, 0, sizeof *c);
}

static DWORD APIENTRY D3d_ContextDestroy(LPD3DNTHAL_CONTEXTDESTROYDATA p)
{
    vcr_d3dctx *c = ctx_of(p->dwhContext);
    if (c)
        ctx_free(c);
    p->ddrval = c ? DD_OK : D3DNTHAL_CONTEXT_BAD;
    return DDHAL_DRIVER_HANDLED;
}

static DWORD APIENTRY D3d_ContextDestroyAll(LPD3DNTHAL_CONTEXTDESTROYALLDATA p)
{
    ULONG i;
    for (i = 0; i < MAX_CTX; i++)
        if (g_ctx[i].in_use && g_ctx[i].pid == p->dwPID)
            ctx_free(&g_ctx[i]);
    p->ddrval = DD_OK;
    return DDHAL_DRIVER_HANDLED;
}

/* ---- drawing ------------------------------------------------------------------------------ */

typedef struct dp2walk {
    vcr_d3dctx   *c;
    const UCHAR  *vb;           /* vertex 0 */
    ULONG         nverts, stride;
    vcr3d_draw    d;
    int           drawable;     /* a target, pre-transformed vertices, the engine */
    int           prepared;     /* target + state sent since the last change */
    ULONG         set_off[8], nsets;    /* texture coordinate sets in a vertex */
} dp2walk;

static BOOL fvf_layout(DWORD fvf, ULONG *stride, ULONG *diff, ULONG *tex, ULONG *spec,
                       ULONG *set_off, ULONG *nsets)
{
    ULONG off = 16, ntex = (fvf & D3DFVF_TEXCOUNT_MASK) >> D3DFVF_TEXCOUNT_SHIFT, i;
    if ((fvf & D3DFVF_POSITION_MASK) != D3DFVF_XYZRHW)
        return FALSE;
    if (fvf & D3DFVF_RESERVED1)
        off += 4;
    *diff = 0;
    if (fvf & D3DFVF_DIFFUSE) {
        *diff = off;
        off += 4;
    }
    *spec = 0;
    if (fvf & D3DFVF_SPECULAR) {
        *spec = off;
        off += 4;
    }
    *tex = off;
    for (i = 0; i < ntex; i++) {
        ULONG sz = (fvf >> (16 + 2 * i)) & 3;       /* 0: 2 floats, 1: 3, 2: 4, 3: 1 */
        if (i < 8)
            set_off[i] = off;
        off += sz == 0 ? 8 : sz == 1 ? 12 : sz == 2 ? 16 : 4;
    }
    *nsets = ntex;
    *stride = off;
    if (!ntex)
        *tex = 0;
    return TRUE;
}

/* a bound texture the CPU wrote (any level of it): flush the TMU's cache */
static void flush_written(vcr_d3dctx *c, PDD_SURFACE_LOCAL s)
{
    PDD_SURFACE_LOCAL m;
    vcr_texlod t;
    int dirty = 0, n = 0;
    if (!s || !s->lpGbl)
        return;
    for (m = s; m && n < 12; m = next_mip(m), n++)
        if (SF_IS(m, SF_DIRTY)) {
            dirty = 1;
            m->dwReserved1 &= ~SF_DIRTY;
        }
    if (!dirty || vcr_texlod_compute(s->lpGbl->wWidth, s->lpGbl->wHeight, 2,
                                     (ULONG)s->lpGbl->lPitch, (ULONG)s->lpGbl->fpVidMem, &t))
        return;
    VcrDd3dTexFlush(c->pd, t.base, (ULONG)s->lpGbl->fpVidMem);
    c->tex_flushes++;
}

static BOOL prepare(dp2walk *w)
{
    vcr_d3dctx *c = w->c;
    if (!w->drawable)
        return FALSE;
    if (g_tex_writes != c->tex_writes_seen) {       /* one compare per triangle */
        c->tex_writes_seen = g_tex_writes;
        if (c->dirty)
            compute_regs(c);                        /* the surfaces about to be bound */
        flush_written(c, c->tex_surf0);
        flush_written(c, c->tex_surf1);
        w->prepared = 0;                            /* the flush moved texBaseAddr */
    }
    if (w->prepared && !c->dirty)
        return TRUE;
    compute_regs(c);
    {
        ULONG i0 = c->tss[D3DTSS_TEXCOORDINDEX] & 7, i1 = c->tss1[D3DTSS_TEXCOORDINDEX] & 7;
        /* TMU0 reads stage 1's set when there are two stages, else stage 0's */
        ULONG s0 = c->regs.textured1 ? i1 : i0;
        if (c->tex_ok && s0 < w->nsets && (!c->regs.textured1 || i0 < w->nsets)) {
            w->d.textured = 1;
            w->d.tex_off = w->set_off[s0];
            VcrDd3dTexScale(&w->d, c->tex_w, c->tex_h);
            w->d.textured1 = c->regs.textured1;
            if (w->d.textured1) {
                w->d.tex1_off = w->set_off[i0];
                VcrDd3dTexScale1(&w->d, c->tex1_w, c->tex1_h);
            }
        } else {
            w->d.textured = w->d.textured1 = 0;
            c->regs.textured = c->regs.textured1 = 0;
            c->regs.fbzColorPath &= ~CP_TEXTURE;
            c->regs.setupMode &= ~(SM_W0 | SM_ST0 | SM_W1 | SM_ST1);
        }
    }
    w->d.fog_vertex = c->fog_vertex && w->d.spec_off;
    if (c->fog_vertex && !w->d.spec_off)
        c->regs.fogMode = 0;            /* vertex fog without a factor: none */
    if (!VcrDd3dTarget(c->pd, &c->target) || !VcrDd3dState(c->pd, &c->regs))
        return FALSE;
    w->prepared = 1;
    return TRUE;
}

static void tri(dp2walk *w, ULONG a, ULONG b, ULONG cc)
{
    if (a >= w->nverts || b >= w->nverts || cc >= w->nverts || !prepare(w))
        return;
    if (VcrDd3dTriangle(w->c->pd, &w->d, w->vb + a * w->stride, w->vb + b * w->stride,
                        w->vb + cc * w->stride))
        w->c->tris++;
}

static const UCHAR *imm_tri(dp2walk *w, const UCHAR *v0, const UCHAR *v1, const UCHAR *v2)
{
    if (prepare(w) && VcrDd3dTriangle(w->c->pd, &w->d, v0, v1, v2))
        w->c->tris++;
    return v2;
}

static WORD rw(const UCHAR *p)
{
    return (WORD)(p[0] | (p[1] << 8));
}

static DWORD rd(const UCHAR *p)
{
    return (DWORD)p[0] | ((DWORD)p[1] << 8) | ((DWORD)p[2] << 16) | ((DWORD)p[3] << 24);
}

static void set_rs(dp2walk *w, DWORD st, DWORD v, LPDWORD rstates)
{
    vcr_d3dctx *c = w->c;
    if (st >= RS_MAX)
        return;
    if (rstates)
        rstates[st] = v;
    if (st == D3DRENDERSTATE_TEXTUREHANDLE)     /* the legacy spelling of stage 0's texture */
        c->tex_handle = v;
    if (c->rs[st] != v || st == D3DRENDERSTATE_TEXTUREHANDLE) {
        c->rs[st] = v;
        c->dirty = 1;
    }
}

static void clear_rects(dp2walk *w, DWORD flags, DWORD color, DWORD zbits, const RECTL *r,
                        ULONG n)
{
    vcr_d3dctx *c = w->c;
    ULONG what = (flags & D3DCLEAR_TARGET ? VCR3D_CLEAR_COLOR : 0) |
                 (flags & D3DCLEAR_ZBUFFER ? VCR3D_CLEAR_Z : 0);
    if (!w->drawable || !what)
        return;
    if (VcrDd3dTarget(c->pd, &c->target) &&
        VcrDd3dClear(c->pd, &c->target, what, color, zbits, r, n))
        c->clears++;
    c->dirty = 1;               /* the clear rewrote fbzMode and c1 */
    w->prepared = 0;
}

/* copy one level's rectangle (TEXBLT): system or video memory to video memory */
static void texblt_level(VCR_PDEV *pd, PDD_SURFACE_LOCAL d, PDD_SURFACE_LOCAL s, LONG dx, LONG dy,
                         const RECTL *r)
{
    PUCHAR dp, sp;
    ULONG bpp, rows, bytes, y;
    LONG sx = r->left, sy = r->top;
    bpp = surf_bpp(pd, d) / 8;
    if (!bpp || bpp != surf_bpp(pd, s) / 8 || r->right <= sx || r->bottom <= sy ||
        r->right > (LONG)s->lpGbl->wWidth || r->bottom > (LONG)s->lpGbl->wHeight ||
        dx < 0 || dy < 0 || dx + (r->right - sx) > (LONG)d->lpGbl->wWidth ||
        dy + (r->bottom - sy) > (LONG)d->lpGbl->wHeight)
        return;
    rows = (ULONG)(r->bottom - sy);
    bytes = (ULONG)(r->right - sx) * bpp;
    if ((ULONG)d->lpGbl->fpVidMem + (dy + rows - 1) * (ULONG)d->lpGbl->lPitch + dx * bpp + bytes >
        pd->cjVram)
        return;
    dp = (PUCHAR)pd->pvRamBase + (ULONG)d->lpGbl->fpVidMem + dy * d->lpGbl->lPitch + dx * bpp;
    if (in_vidmem(s))
        sp = (PUCHAR)pd->pvRamBase + (ULONG)s->lpGbl->fpVidMem;
    else
        sp = (PUCHAR)s->lpGbl->fpVidMem;        /* system memory: a pointer in the caller */
    sp += sy * s->lpGbl->lPitch + sx * bpp;
    for (y = 0; y < rows; y++)
        memcpy(dp + y * d->lpGbl->lPitch, sp + y * s->lpGbl->lPitch, bytes);
}

/* TEXBLT copies the rectangle of the top level and the matching rectangle of
 * every smaller level the two chains share */
static void texblt(dp2walk *w, const D3DNTHAL_DP2TEXBLT *t)
{
    vcr_d3dctx *c = w->c;
    VCR_PDEV *pd = c->pd;
    PDD_SURFACE_LOCAL d = handle_get(c->ddlcl, t->dwDDDestSurface),
                      s = handle_get(c->ddlcl, t->dwDDSrcSurface);
    RECTL r = t->rSrc;
    LONG dx = t->pDest.x, dy = t->pDest.y;
    ULONG n = 0;
    if (!d || !s || !in_vidmem(d) || !d->lpGbl || !s->lpGbl)
        return;                                 /* dest 0 = a preload hint */
    VcrDd2dSync(pd);                            /* queued triangles may still sample it */
    VcrDdD3dTexWritten(d);
    while (d && s && n++ < 12) {
        texblt_level(pd, d, s, dx, dy, &r);
        d = next_mip(d);
        s = next_mip(s);
        r.left >>= 1;
        r.top >>= 1;
        r.right = r.right > 1 ? r.right >> 1 : 1;
        r.bottom = r.bottom > 1 ? r.bottom >> 1 : 1;
        if (r.right <= r.left)
            r.right = r.left + 1;
        if (r.bottom <= r.top)
            r.bottom = r.top + 1;
        dx >>= 1;
        dy >>= 1;
    }
}

/* the fixed record size of an opcode we skip, or 0 for "variable / unknown" */
static ULONG rec_size(BYTE op)
{
    switch (op) {
    case D3DNTDP2OP_VIEWPORTINFO:     return sizeof(D3DNTHAL_DP2VIEWPORTINFO);
    case D3DNTDP2OP_WINFO:            return sizeof(D3DNTHAL_DP2WINFO);
    case D3DNTDP2OP_SETPALETTE:       return sizeof(D3DNTHAL_DP2SETPALETTE);
    case D3DNTDP2OP_ZRANGE:           return sizeof(D3DNTHAL_DP2ZRANGE);
    case D3DNTDP2OP_SETMATERIAL:      return sizeof(D3DNTHAL_DP2SETMATERIAL);
    case D3DNTDP2OP_CREATELIGHT:      return sizeof(D3DNTHAL_DP2CREATELIGHT);
    case D3DNTDP2OP_SETTRANSFORM:     return sizeof(D3DNTHAL_DP2SETTRANSFORM);
    case D3DNTDP2OP_STATESET:         return sizeof(D3DNTHAL_DP2STATESET);
    case D3DNTDP2OP_SETPRIORITY:      return sizeof(D3DNTHAL_DP2SETPRIORITY);
    case D3DNTDP2OP_SETTEXLOD:        return sizeof(D3DNTHAL_DP2SETTEXLOD);
    case D3DNTDP2OP_SETCLIPPLANE:     return sizeof(D3DNTHAL_DP2SETCLIPPLANE);
    default:                        return 0;
    }
}

#define NEED(n)  do { if ((ULONG)(end - p) < (ULONG)(n)) goto bad; } while (0)

static HRESULT walk(dp2walk *w, const UCHAR *cmds, ULONG len, LPDWORD rstates, DWORD *erroff)
{
    const UCHAR *p = cmds, *end = cmds + len;
    vcr_d3dctx *c = w->c;
    while (p < end) {
        const UCHAR *hdr = p;
        BYTE op;
        ULONG n, i, sz;
        NEED(sizeof(D3DNTHAL_DP2COMMAND));
        op = p[0];
        n = rw(p + 2);
        p += sizeof(D3DNTHAL_DP2COMMAND);
        switch (op) {
        case D3DNTDP2OP_RENDERSTATE:
            NEED(n * 8);
            for (i = 0; i < n; i++)
                set_rs(w, rd(p + i * 8), rd(p + i * 8 + 4), rstates);
            p += n * 8;
            break;
        case D3DNTDP2OP_TEXTURESTAGESTATE:
            NEED(n * 8);
            for (i = 0; i < n; i++) {
                WORD stage = rw(p + i * 8), st = rw(p + i * 8 + 2);
                DWORD v = rd(p + i * 8 + 4);
                DWORD *ts = stage == 0 ? c->tss : stage == 1 ? c->tss1 : NULL;
                DWORD *th = stage == 0 ? &c->tex_handle : &c->tex_handle1;
                if (!ts || st >= TSS_MAX)
                    continue;
                if (st == D3DTSS_TEXTUREMAP) {
                    *th = v;
                    c->dirty = 1;
                    VcrDd(VCR_LV_DEBUG, VCR_EV_DD_D3D, 14, stage, v, 0, "stage %u texture %u",
                          stage, v);
                } else if (ts[st] != v) {
                    ts[st] = v;
                    c->dirty = 1;
                }
            }
            p += n * 8;
            break;
        case D3DNTDP2OP_TRIANGLELIST: {
            ULONG s;
            NEED(2);
            s = rw(p);
            p += 2;
            for (i = 0; i < n; i++)
                tri(w, s + 3 * i, s + 3 * i + 1, s + 3 * i + 2);
            break;
        }
        case D3DNTDP2OP_TRIANGLESTRIP: {
            ULONG s;
            NEED(2);
            s = rw(p);
            p += 2;
            for (i = 0; i < n; i++) {
                if (i & 1)
                    tri(w, s + i + 1, s + i, s + i + 2);    /* keep the winding */
                else
                    tri(w, s + i, s + i + 1, s + i + 2);
            }
            break;
        }
        case D3DNTDP2OP_TRIANGLEFAN: {
            ULONG s;
            NEED(2);
            s = rw(p);
            p += 2;
            for (i = 0; i < n; i++)
                tri(w, s, s + i + 1, s + i + 2);
            break;
        }
        case D3DNTDP2OP_INDEXEDTRIANGLELIST:          /* absolute v1 v2 v3 wFlags */
            NEED(n * 8);
            for (i = 0; i < n; i++)
                tri(w, rw(p + i * 8), rw(p + i * 8 + 2), rw(p + i * 8 + 4));
            p += n * 8;
            break;
        case D3DNTDP2OP_INDEXEDTRIANGLELIST2: {       /* start, then v1 v2 v3 relative */
            ULONG s;
            NEED(2 + n * 6);
            s = rw(p);
            p += 2;
            for (i = 0; i < n; i++)
                tri(w, s + rw(p + i * 6), s + rw(p + i * 6 + 2), s + rw(p + i * 6 + 4));
            p += n * 6;
            break;
        }
        case D3DNTDP2OP_INDEXEDTRIANGLESTRIP:
        case D3DNTDP2OP_INDEXEDTRIANGLEFAN: {         /* start, then n + 2 relative indices */
            ULONG s;
            NEED(2 + (n + 2) * 2);
            s = rw(p);
            p += 2;
            for (i = 0; i < n; i++) {
                ULONG a = s + rw(p + i * 2), b = s + rw(p + (i + 1) * 2),
                      cc = s + rw(p + (i + 2) * 2);
                if (op == D3DNTDP2OP_INDEXEDTRIANGLEFAN)
                    tri(w, s + rw(p), b, cc);
                else if (i & 1)
                    tri(w, b, a, cc);
                else
                    tri(w, a, b, cc);
            }
            p += (n + 2) * 2;
            break;
        }
        case D3DNTDP2OP_TRIANGLEFAN_IMM: {            /* edge flags, then n + 2 inline vertices */
            const UCHAR *v;
            NEED(4);
            p += 4;
            p = cmds + ((p - cmds + 3) & ~3);       /* vertex data is DWORD aligned */
            NEED((n + 2) * w->stride);
            v = p;
            for (i = 0; i < n && w->stride; i++)
                imm_tri(w, v, v + (i + 1) * w->stride, v + (i + 2) * w->stride);
            p += (n + 2) * w->stride;
            break;
        }
        case D3DNTDP2OP_LINELIST_IMM:                 /* 2n inline vertices: not drawn yet */
            p = cmds + ((p - cmds + 3) & ~3);
            NEED(2 * n * w->stride);
            p += 2 * n * w->stride;
            break;
        case D3DNTDP2OP_POINTS:                       /* n x {wCount, wVStart}: not drawn yet */
            NEED(n * 4);
            p += n * 4;
            break;
        case D3DNTDP2OP_LINELIST:
        case D3DNTDP2OP_LINESTRIP:
            NEED(2);
            p += 2;
            break;
        case D3DNTDP2OP_INDEXEDLINELIST:
            NEED(n * 4);
            p += n * 4;
            break;
        case D3DNTDP2OP_INDEXEDLINELIST2:
            NEED(2 + n * 4);
            p += 2 + n * 4;
            break;
        case D3DNTDP2OP_INDEXEDLINESTRIP:
            NEED(2 + (n + 1) * 2);
            p += 2 + (n + 1) * 2;
            break;
        case D3DNTDP2OP_UPDATEPALETTE:
            for (i = 0; i < n; i++) {
                NEED(8);
                sz = rw(p + 6);
                p += 8;
                NEED(sz * 4);
                p += sz * 4;
            }
            break;
        case D3DNTDP2OP_SETRENDERTARGET:
            NEED(n * 8);
            for (i = 0; i < n; i++) {
                PDD_SURFACE_LOCAL rt = handle_get(c->ddlcl, rd(p + i * 8)),
                                  zb = handle_get(c->ddlcl, rd(p + i * 8 + 4));
                if (!rt)
                    VcrDd(VCR_LV_WARN, VCR_EV_DD_D3D, 4, rd(p + i * 8), rd(p + i * 8 + 4), 0,
                          "SETRENDERTARGET: no surface for handle %u", rd(p + i * 8));
                if (rt) {
                    c->rt = rt;
                    c->zb = zb;
                    target_of(c);
                    c->dirty = 1;
                    w->prepared = 0;
                    w->drawable = w->drawable || (c->target.rt_off && w->stride);
                }
            }
            p += n * 8;
            break;
        case D3DNTDP2OP_CLEAR: {                      /* flags color depth stencil, n RECTs */
            NEED(16 + n * sizeof(RECT));
            clear_rects(w, rd(p), rd(p + 4), rd(p + 8), (const RECTL *)(p + 16), n);
            p += 16 + n * sizeof(RECT);
            break;
        }
        case D3DNTDP2OP_TEXBLT:
            NEED(n * sizeof(D3DNTHAL_DP2TEXBLT));
            for (i = 0; i < n; i++) {
                D3DNTHAL_DP2TEXBLT t;
                memcpy(&t, p + i * sizeof t, sizeof t);
                VcrDd(VCR_LV_DEBUG, VCR_EV_DD_D3D, 13, t.dwDDDestSurface, t.dwDDSrcSurface,
                      ((ULONG)t.rSrc.right << 16) | (ULONG)t.rSrc.bottom,
                      "TEXBLT %u <- %u (%dx%d)", t.dwDDDestSurface, t.dwDDSrcSurface,
                      t.rSrc.right - t.rSrc.left, t.rSrc.bottom - t.rSrc.top);
                texblt(w, &t);
            }
            p += n * sizeof(D3DNTHAL_DP2TEXBLT);
            break;
        case D3DNTDP2OP_EXT:
            NEED(8);
            sz = rd(p + 4);
            NEED(8 + sz);
            p += 8 + sz;
            break;
        default:
            sz = rec_size(op);
            if (sz) {
                NEED(n * sz);
                p += n * sz;
                break;
            }
            /* unknown: the runtime parses it, and tells us where to resume */
            if (g_parse_unknown) {
                PVOID next = NULL;
                HRESULT hr = g_parse_unknown((LPVOID)hdr, &next);
                c->unparsed++;
                if (SUCCEEDED(hr) && next && (const UCHAR *)next > hdr &&
                    (const UCHAR *)next <= end) {
                    p = (const UCHAR *)next;
                    break;
                }
            }
            *erroff = (DWORD)(hdr - cmds);
            VcrDd(VCR_LV_WARN, VCR_EV_DD_D3D, 5, op, *erroff, len, "DP2: opcode %u unparsed at %u",
                  op, *erroff);
            return D3DNTERR_COMMAND_UNPARSED;
        }
    }
    return DD_OK;
bad:
    VcrDd(VCR_LV_WARN, VCR_EV_DD_D3D, 6, p[0], (ULONG)(p - cmds), len,
          "DP2: stream ends inside a command at %u of %u", (ULONG)(p - cmds), len);
    *erroff = (DWORD)(p - cmds);
    return D3DNTERR_COMMAND_UNPARSED;
}

static DWORD APIENTRY D3d_DrawPrimitives2(LPD3DNTHAL_DRAWPRIMITIVES2DATA p)
{
    vcr_d3dctx *c = ctx_of(p->dwhContext);
    dp2walk w;
    const UCHAR *cmds;
    ULONG diff = 0, tex = 0, spec = 0, nsets = 0;
    HRESULT hr;

    p->dwErrorOffset = 0;
    if (!c || !p->lpDDCommands || !p->lpDDCommands->lpGbl) {
        p->ddrval = D3DNTHAL_CONTEXT_BAD;
        return DDHAL_DRIVER_HANDLED;
    }
    c->dp2s++;
    memset(&w, 0, sizeof w);
    w.c = c;
    if (fvf_layout(p->dwVertexType, &w.stride, &diff, &tex, &spec, w.set_off, &nsets)) {
        if (p->dwFlags & D3DNTHALDP2_USERMEMVERTICES)
            w.vb = (const UCHAR *)p->lpVertices;
        else if (p->lpDDVertex && p->lpDDVertex->lpGbl)
            w.vb = (const UCHAR *)p->lpDDVertex->lpGbl->fpVidMem;
        if (w.vb) {
            w.vb += p->dwVertexOffset;
            w.nverts = p->dwVertexLength;
        }
    }
    cmds = (const UCHAR *)p->lpDDCommands->lpGbl->fpVidMem + p->dwCommandOffset;
    /* a flip swaps the video memory of the flip chain's surfaces: the target
     * is read from the surface at every call, never kept from the last one */
    {
        ULONG was = c->target.rt_off;
        target_of(c);
        if (c->target.rt_off != was)
            VcrDd(VCR_LV_DEBUG, VCR_EV_DD_D3D, 8, was, c->target.rt_off, c->dp2s,
                  "DP2 %u: target %x -> %x (surface %p)", c->dp2s, was, c->target.rt_off, c->rt);
    }
    w.drawable = c->pd->g2d_ok && !c->pd->exclusive_pid && c->target.rt_off &&
                 (c->target.rt_pitch & 0xf) == 0;
    EngSaveFloatingPointState(c->fpu, g_fpu_size);
    VcrDd3dDrawInit(&w.d);
    w.d.diff_off = diff;
    w.d.tex_off = tex;
    w.d.spec_off = spec;
    w.nsets = nsets;
    hr = walk(&w, cmds, p->dwCommandLength, p->lpdwRStates, &p->dwErrorOffset);
    EngRestoreFloatingPointState(c->fpu);
    p->ddrval = hr;
    return DDHAL_DRIVER_HANDLED;
}

static DWORD APIENTRY D3d_Clear2(LPD3DNTHAL_CLEAR2DATA p)
{
    vcr_d3dctx *c = ctx_of(p->dwhContext);
    dp2walk w;
    ULONG i;
    if (!c) {
        p->ddrval = D3DNTHAL_CONTEXT_BAD;
        return DDHAL_DRIVER_HANDLED;
    }
    memset(&w, 0, sizeof w);
    w.c = c;
    target_of(c);
    w.drawable = c->pd->g2d_ok && !c->pd->exclusive_pid && c->target.rt_off;
    EngSaveFloatingPointState(c->fpu, g_fpu_size);
    for (i = 0; i < p->dwNumRects; i++) {
        RECTL r;
        DWORD z;
        r.left = p->lpRects[i].x1;
        r.top = p->lpRects[i].y1;
        r.right = p->lpRects[i].x2;
        r.bottom = p->lpRects[i].y2;
        memcpy(&z, &p->dvFillDepth, 4);
        clear_rects(&w, p->dwFlags, p->dwFillColor, z, &r, 1);
    }
    EngRestoreFloatingPointState(c->fpu);
    p->ddrval = DD_OK;
    return DDHAL_DRIVER_HANDLED;
}

static DWORD APIENTRY D3d_ValidateTextureStageState(LPD3DNTHAL_VALIDATETEXTURESTAGESTATEDATA p)
{
    p->dwNumPasses = 1;
    p->ddrval = DD_OK;
    return DDHAL_DRIVER_HANDLED;
}

/* ---- DX7 surface handles -------------------------------------------------------------- */

/* The runtime names a COMPLEX surface once, by its root: every surface
 * attached to it - the rest of a flip chain, a mipmap's levels - carries its
 * own handle and is found through the attach lists. A flip chain is a ring,
 * so the walk stops at a surface it has seen. (Without this a fullscreen
 * back buffer is never found: XP does not move video memory between the flip
 * chain's surfaces, it re-targets rendering with SETRENDERTARGET by handle -
 * measured on the 86Box Voodoo3, alternate frames drawn into the front.) */
static void name_surface(PDD_DIRECTDRAW_LOCAL l, PDD_SURFACE_LOCAL s)
{
    PDD_SURFACE_LOCAL seen[32];
    ULONG n = 0, i, k;
    seen[n++] = s;
    for (i = 0; i < n; i++) {
        PDD_SURFACE_LOCAL cur = seen[i];
        PDD_ATTACHLIST a;
        if (cur->lpSurfMore && cur->lpSurfMore->dwSurfaceHandle) {
            /* CreateSurfaceEx doubles as the DESTROY notice: fpVidMem 0 means
             * the handle no longer names this surface */
            if (cur->lpGbl && !cur->lpGbl->fpVidMem)
                handle_forget(NULL, cur);
            else
                handle_set(l, cur->lpSurfMore->dwSurfaceHandle, cur);
            VcrDd(VCR_LV_DEBUG, VCR_EV_DD_D3D, 3, cur->lpSurfMore->dwSurfaceHandle,
                  cur->ddsCaps.dwCaps, cur->lpGbl ? (ULONG)cur->lpGbl->fpVidMem : 0,
                  "CreateSurfaceEx %u", cur->lpSurfMore->dwSurfaceHandle);
        }
        for (a = cur->lpAttachList; a && n < 32; a = a->lpLink) {
            if (!a->lpAttached)
                continue;
            for (k = 0; k < n && seen[k] != a->lpAttached; k++)
                ;
            if (k == n)
                seen[n++] = a->lpAttached;
        }
    }
}

static DWORD APIENTRY Dd_CreateSurfaceEx(PDD_CREATESURFACEEXDATA p)
{
    if (p->lpDDSLcl)
        name_surface(p->lpDDLcl, p->lpDDSLcl);
    p->ddRVal = DD_OK;
    return DDHAL_DRIVER_HANDLED;
}

static DWORD APIENTRY Dd_GetDriverState(PDD_GETDRIVERSTATEDATA p)
{
    p->ddRVal = DDERR_UNSUPPORTED;
    return DDHAL_DRIVER_HANDLED;
}

static DWORD APIENTRY Dd_DestroyDDLocal(PDD_DESTROYDDLOCALDATA p)
{
    handle_forget(p->pDDLcl, NULL);
    p->ddRVal = DD_OK;
    return DDHAL_DRIVER_HANDLED;
}

/* ---- registration ------------------------------------------------------------------------- */

static D3DNTHAL_GLOBALDRIVERDATA g_gd;
static D3DNTHAL_CALLBACKS g_cb;
static DDSURFACEDESC g_texfmt[3];

static void texfmt(DDSURFACEDESC *d, DWORD flags, DWORD r, DWORD g, DWORD b, DWORD a)
{
    memset(d, 0, sizeof *d);
    d->dwSize = sizeof *d;
    d->dwFlags = DDSD_CAPS | DDSD_PIXELFORMAT;
    d->ddsCaps.dwCaps = DDSCAPS_TEXTURE;
    d->ddpfPixelFormat.dwSize = sizeof(DDPIXELFORMAT);
    d->ddpfPixelFormat.dwFlags = DDPF_RGB | flags;
    d->ddpfPixelFormat.dwRGBBitCount = 16;
    d->ddpfPixelFormat.dwRBitMask = r;
    d->ddpfPixelFormat.dwGBitMask = g;
    d->ddpfPixelFormat.dwBBitMask = b;
    d->ddpfPixelFormat.dwRGBAlphaBitMask = a;
}

static void prim_caps(D3DPRIMCAPS *c)
{
    memset(c, 0, sizeof *c);
    c->dwSize = sizeof *c;
    c->dwMiscCaps = D3DPMISCCAPS_CULLNONE | D3DPMISCCAPS_CULLCW | D3DPMISCCAPS_CULLCCW |
                    D3DPMISCCAPS_MASKZ;
    c->dwRasterCaps = D3DPRASTERCAPS_DITHER | D3DPRASTERCAPS_ZTEST | D3DPRASTERCAPS_SUBPIXEL |
                      D3DPRASTERCAPS_FOGVERTEX | D3DPRASTERCAPS_FOGTABLE | D3DPRASTERCAPS_WFOG;
    c->dwZCmpCaps = D3DPCMPCAPS_NEVER | D3DPCMPCAPS_LESS | D3DPCMPCAPS_EQUAL |
                    D3DPCMPCAPS_LESSEQUAL | D3DPCMPCAPS_GREATER | D3DPCMPCAPS_NOTEQUAL |
                    D3DPCMPCAPS_GREATEREQUAL | D3DPCMPCAPS_ALWAYS;
    c->dwAlphaCmpCaps = c->dwZCmpCaps;
    c->dwSrcBlendCaps = D3DPBLENDCAPS_ZERO | D3DPBLENDCAPS_ONE | D3DPBLENDCAPS_SRCALPHA |
                        D3DPBLENDCAPS_INVSRCALPHA | D3DPBLENDCAPS_DESTCOLOR |
                        D3DPBLENDCAPS_INVDESTCOLOR | D3DPBLENDCAPS_SRCALPHASAT |
                        D3DPBLENDCAPS_BOTHSRCALPHA | D3DPBLENDCAPS_BOTHINVSRCALPHA;
    c->dwDestBlendCaps = D3DPBLENDCAPS_ZERO | D3DPBLENDCAPS_ONE | D3DPBLENDCAPS_SRCCOLOR |
                         D3DPBLENDCAPS_INVSRCCOLOR | D3DPBLENDCAPS_SRCALPHA |
                         D3DPBLENDCAPS_INVSRCALPHA;
    c->dwShadeCaps = D3DPSHADECAPS_COLORFLATRGB | D3DPSHADECAPS_COLORGOURAUDRGB |
                     D3DPSHADECAPS_ALPHAFLATBLEND | D3DPSHADECAPS_ALPHAGOURAUDBLEND;
    c->dwTextureCaps = D3DPTEXTURECAPS_PERSPECTIVE | D3DPTEXTURECAPS_POW2 |
                       D3DPTEXTURECAPS_ALPHA | D3DPTEXTURECAPS_TRANSPARENCY;
    c->dwTextureFilterCaps = D3DPTFILTERCAPS_NEAREST | D3DPTFILTERCAPS_LINEAR |
                             D3DPTFILTERCAPS_MIPNEAREST | D3DPTFILTERCAPS_LINEARMIPNEAREST |
                             D3DPTFILTERCAPS_MINFPOINT | D3DPTFILTERCAPS_MINFLINEAR |
                             D3DPTFILTERCAPS_MAGFPOINT | D3DPTFILTERCAPS_MAGFLINEAR |
                             D3DPTFILTERCAPS_MIPFPOINT;
    c->dwTextureBlendCaps = D3DPTBLENDCAPS_DECAL | D3DPTBLENDCAPS_MODULATE |
                            D3DPTBLENDCAPS_MODULATEALPHA | D3DPTBLENDCAPS_COPY |
                            D3DPTBLENDCAPS_ADD;
    c->dwTextureAddressCaps = D3DPTADDRESSCAPS_WRAP | D3DPTADDRESSCAPS_CLAMP |
                              D3DPTADDRESSCAPS_INDEPENDENTUV;
}

/* DrvGetDirectDrawInfo: the D3D half of the HAL - only with the 3D engine */
void VcrDdD3dHalInfo(VCR_PDEV *pd, DD_HALINFO *hal)
{
    D3DNTHALDEVICEDESC_V1 *d = &g_gd.hwCaps;
    if (!pd->pjRegs || !pd->g2d_ok || pd->d3d_disabled)
        return;
    memset(&g_gd, 0, sizeof g_gd);
    g_gd.dwSize = sizeof g_gd;
    d->dwSize = sizeof *d;
    d->dwFlags = D3DDD_COLORMODEL | D3DDD_DEVCAPS | D3DDD_LINECAPS | D3DDD_TRICAPS |
                 D3DDD_DEVICERENDERBITDEPTH | D3DDD_DEVICEZBUFFERBITDEPTH;
    d->dcmColorModel = D3DCOLOR_RGB;
    d->dwDevCaps = D3DDEVCAPS_FLOATTLVERTEX | D3DDEVCAPS_EXECUTESYSTEMMEMORY |
                   D3DDEVCAPS_TLVERTEXSYSTEMMEMORY | D3DDEVCAPS_TEXTUREVIDEOMEMORY |
                   D3DDEVCAPS_DRAWPRIMTLVERTEX | D3DDEVCAPS_CANRENDERAFTERFLIP |
                   D3DDEVCAPS_DRAWPRIMITIVES2 | D3DDEVCAPS_DRAWPRIMITIVES2EX |
                   D3DDEVCAPS_HWRASTERIZATION;
    prim_caps(&d->dpcTriCaps);
    prim_caps(&d->dpcLineCaps);
    d->dwDeviceRenderBitDepth = DDBD_16;
    d->dwDeviceZBufferBitDepth = DDBD_16;
    texfmt(&g_texfmt[0], 0, 0xf800, 0x07e0, 0x001f, 0);
    texfmt(&g_texfmt[1], DDPF_ALPHAPIXELS, 0x7c00, 0x03e0, 0x001f, 0x8000);
    texfmt(&g_texfmt[2], DDPF_ALPHAPIXELS, 0x0f00, 0x00f0, 0x000f, 0xf000);
    g_gd.dwNumTextureFormats = 3;
    g_gd.lpTextureFormats = g_texfmt;

    memset(&g_cb, 0, sizeof g_cb);
    g_cb.dwSize = sizeof g_cb;
    g_cb.ContextCreate = D3d_ContextCreate;
    g_cb.ContextDestroy = D3d_ContextDestroy;
    g_cb.ContextDestroyAll = D3d_ContextDestroyAll;

    hal->lpD3DGlobalDriverData = &g_gd;
    hal->lpD3DHALCallbacks = &g_cb;
    hal->ddCaps.dwCaps |= DDCAPS_3D;
    hal->ddCaps.ddsCaps.dwCaps |= DDSCAPS_3DDEVICE | DDSCAPS_TEXTURE | DDSCAPS_ZBUFFER |
                                  DDSCAPS_MIPMAP;
    hal->ddCaps.dwZBufferBitDepths = DDBD_16;
}

static int geq(const GUID *a, const GUID *b)
{
    return memcmp(a, b, sizeof *a) == 0;
}

static void answer(PDD_GETDRIVERINFODATA p, const void *data, DWORD size)
{
    DWORD n = p->dwExpectedSize < size ? p->dwExpectedSize : size;
    memcpy(p->lpvData, data, n);
    p->dwActualSize = size;
    p->ddRVal = DD_OK;
}

/* GetDriverInfo's Direct3D queries: 1 = answered */
int VcrDdD3dDriverInfo(VCR_PDEV *pd, PDD_GETDRIVERINFODATA p)
{
    static const GUID cb3 = { 0xddf41230, 0xec0a, 0x11d0, { 0xa9, 0xb6, 0x00, 0xaa, 0x00, 0xc0, 0x99, 0x3e } };
    static const GUID ext = { 0x7de41f80, 0x9d93, 0x11d0, { 0x89, 0xab, 0x00, 0xa0, 0xc9, 0x05, 0x41, 0x29 } };
    static const GUID zpf = { 0x93869880, 0x36cf, 0x11d1, { 0x9b, 0x1b, 0x00, 0xaa, 0x00, 0xbb, 0xb8, 0xae } };
    static const GUID misc2 = { 0x406b2f00, 0x3e5a, 0x11d1, { 0xb6, 0x40, 0x00, 0xaa, 0x00, 0xa1, 0xf9, 0x6a } };
    static const GUID unk = { 0x2e04ffa0, 0x98e4, 0x11d1, { 0x8c, 0xe1, 0x00, 0xa0, 0xc9, 0x06, 0x29, 0xa8 } };
    if (!pd || !pd->pjRegs || !pd->g2d_ok || pd->d3d_disabled)
        return 0;
    if (geq(&p->guidInfo, &cb3)) {
        D3DNTHAL_CALLBACKS3 c;
        memset(&c, 0, sizeof c);
        c.dwSize = sizeof c;
        c.dwFlags = D3DNTHAL3_CB32_CLEAR2 | D3DNTHAL3_CB32_DRAWPRIMITIVES2 |
                    D3DNTHAL3_CB32_VALIDATETEXTURESTAGESTATE;
        c.Clear2 = D3d_Clear2;
        c.ValidateTextureStageState = D3d_ValidateTextureStageState;
        c.DrawPrimitives2 = D3d_DrawPrimitives2;
        answer(p, &c, sizeof c);
        return 1;
    }
    if (geq(&p->guidInfo, &ext)) {
        D3DNTHAL_D3DEXTENDEDCAPS x;
        DWORD guard = 0xc5000000u, guard_pos = 0x45000000u, maxw = 0x47800000u; /* -2048, 2048, 65536 */
        memset(&x, 0, sizeof x);
        x.dwSize = sizeof x;
        x.dwMinTextureWidth = x.dwMinTextureHeight = 1;
        x.dwMaxTextureWidth = x.dwMaxTextureHeight = 256;
        x.dwMaxTextureAspectRatio = 8;
        x.dwMaxAnisotropy = 1;
        (void)guard;
        (void)guard_pos;
        memcpy(&x.dvMaxVertexW, &maxw, 4);
        x.dwFVFCaps = 2;
        x.dwTextureOpCaps = D3DTEXOPCAPS_DISABLE | D3DTEXOPCAPS_SELECTARG1 |
                            D3DTEXOPCAPS_SELECTARG2 | D3DTEXOPCAPS_MODULATE |
                            D3DTEXOPCAPS_ADD | D3DTEXOPCAPS_BLENDDIFFUSEALPHA |
                            D3DTEXOPCAPS_BLENDTEXTUREALPHA;
        x.wMaxTextureBlendStages = 2;
        x.wMaxSimultaneousTextures = 2;
        answer(p, &x, sizeof x);
        return 1;
    }
    if (geq(&p->guidInfo, &zpf)) {
        struct { DWORD n; DDPIXELFORMAT f; } z;
        memset(&z, 0, sizeof z);
        z.n = 1;
        z.f.dwSize = sizeof z.f;
        z.f.dwFlags = DDPF_ZBUFFER;
        z.f.dwZBufferBitDepth = 16;
        z.f.dwZBitMask = 0xffff;
        answer(p, &z, sizeof z);
        return 1;
    }
    if (geq(&p->guidInfo, &misc2)) {
        DD_MISCELLANEOUS2CALLBACKS m;
        memset(&m, 0, sizeof m);
        m.dwSize = sizeof m;
        m.dwFlags = DDHAL_MISC2CB32_CREATESURFACEEX | DDHAL_MISC2CB32_GETDRIVERSTATE |
                    DDHAL_MISC2CB32_DESTROYDDLOCAL;
        m.CreateSurfaceEx = Dd_CreateSurfaceEx;
        m.GetDriverState = Dd_GetDriverState;
        m.DestroyDDLocal = Dd_DestroyDDLocal;
        answer(p, &m, sizeof m);
        return 1;
    }
    if (geq(&p->guidInfo, &unk)) {
        g_parse_unknown = (PFND3DNTPARSEUNKNOWNCOMMAND)p->lpvData;
        p->ddRVal = DD_OK;
        return 1;
    }
    return 0;
}

#endif /* VCR_HAVE_DDI */
