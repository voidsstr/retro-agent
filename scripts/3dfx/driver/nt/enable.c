/*
 * enable.c - NT/2000/XP display-driver host for fxD3D (skeleton).
 *
 * This is the "chassis" that Windows loads (the GDI display driver DLL, paired
 * with a videoprt miniport .sys). Its job here is narrow: provide the minimum
 * DirectDraw/Direct3D DDI plumbing so the runtime discovers fxD3D and calls its
 * DrawPrimitives2. 2D GDI + modeset are provided by the miniport + a stock
 * framebuffer path (or grafted from an existing 3dfx 2D driver); this file
 * focuses on the D3D hookup, which is the novel part.
 *
 * REQUIRES THE DDK to build (winddi.h, ddrawint.h, d3dhal.h, videoprt). Build
 * with the SOURCES file in this dir under the NT DDK `build` environment (or on
 * a provisioned XP box via the agent - see driver/README.md). The DDK-specific
 * code is bracketed by HAVE_DDK so this tree stays greppable/reviewable on the
 * Linux host; the portable render reduction lives in ../ddi_glue.c.
 *
 * Clean-room: implements the public NT display-DDI contract from the DDK.
 */
#include "../ddi_glue.h"
#include "fxd3d_dp2.h"

#ifdef HAVE_DDK
#include <windows.h>
#include <winddi.h>
#include <devioctl.h>      /* CTL_CODE etc. for the IOCTL macros (chassis order) */
#include <ntddvdeo.h>      /* IOCTL_VIDEO_SHARE_VIDEO_MEMORY, VIDEO_SHARE_MEMORY */
#include <ddrawint.h>
#include <d3dhal.h>

#include "gbkernel.h"      /* gbkernel_get_meminfo - the surface carve-out       */
#include "fxpdev.h"        /* FXPDEV (shared with chassis.c) + gbk pure helpers  */
#include "../../d3dhal/glidebackend.h"  /* gb_swap/gb_finish - present + drain   */

/* Forward decls for the DirectDraw enable path defined further down. */
DWORD APIENTRY DdGetDriverInfo(PDD_GETDRIVERINFODATA pData);
void fxnt_get_d3d_callbacks(D3DHAL_CALLBACKS *cb, D3DHAL_GLOBALDRIVERDATA *gd);
static void fxnt_fill_texformats(void);

/* Static D3D discovery blobs published through DD_HALINFO (the runtime reads
 * them during DrvGetDirectDrawInfo; they must outlive the call). */
static D3DHAL_GLOBALDRIVERDATA g_d3dGlobalData;
static D3DHAL_CALLBACKS        g_d3dCallbacksTbl;
#define FXNT_NUM_TEXFMT 3
static DDSURFACEDESC           g_d3dTexFormats[FXNT_NUM_TEXFMT];

/* The runtime's parse-unknown callback, stashed by DdGetDriverInfo (see the
 * GUID_D3DParseUnknownCommand handling near the bottom of this file). */
static PFND3DPARSEUNKNOWNCOMMAND g_pfnParseUnknown;

/* Adapter matching fxd_parse_unknown_fn: when the DP2 translator meets an
 * opcode it does not implement, route that ONE command through the runtime's
 * PFND3DPARSEUNKNOWNCOMMAND so it parses/skips it and tells us where to resume,
 * instead of aborting the whole batch. Returns 0 (with *next_off set) on
 * success, non-zero if the runtime can't parse it either. */
static int fxnt_parse_unknown(void *ctx, const void *cmds, DWORD off,
                              DWORD cmd_len, DWORD *next_off)
{
    LPVOID next = NULL;
    HRESULT hr;
    UNREFERENCED_PARAMETER(ctx);
    UNREFERENCED_PARAMETER(cmd_len);
    if (!g_pfnParseUnknown)
        return -1;
    hr = g_pfnParseUnknown((LPVOID)((const BYTE *)cmds + off), &next);
    if (hr != D3D_OK || next == NULL)
        return -1;
    *next_off = (DWORD)((const BYTE *)next - (const BYTE *)cmds);
    return 0;
}

/* ---- DirectDraw DDI: advertise D3D + surface support --------------------- */

/* Display pixel format from the PDEV's mode (masks come from the miniport). */
static void fxnt_fill_ddpf(DDPIXELFORMAT *pf, PFXPDEV ppdev)
{
    memset(pf, 0, sizeof(*pf));
    pf->dwSize        = sizeof(*pf);
    pf->dwFlags       = DDPF_RGB;
    pf->dwRGBBitCount = ppdev->ulBitsPerPel;
    pf->dwRBitMask    = ppdev->flRed;
    pf->dwGBitMask    = ppdev->flGreen;
    pf->dwBBitMask    = ppdev->flBlue;
}

/* The texture formats the TMU accepts, published via D3DHAL_GLOBALDRIVERDATA
 * (design sec 6 format map; P8 deferred with the palette DDI). Idempotent. */
static void fxnt_fill_texformats(void)
{
    DDPIXELFORMAT *pf;
    int i;

    memset(g_d3dTexFormats, 0, sizeof(g_d3dTexFormats));
    for (i = 0; i < FXNT_NUM_TEXFMT; i++) {
        g_d3dTexFormats[i].dwSize  = sizeof(DDSURFACEDESC);
        g_d3dTexFormats[i].dwFlags = DDSD_CAPS | DDSD_PIXELFORMAT;
        g_d3dTexFormats[i].ddsCaps.dwCaps = DDSCAPS_TEXTURE;
        g_d3dTexFormats[i].ddpfPixelFormat.dwSize = sizeof(DDPIXELFORMAT);
    }
    pf = &g_d3dTexFormats[0].ddpfPixelFormat;          /* RGB565            */
    pf->dwFlags = DDPF_RGB;
    pf->dwRGBBitCount = 16;
    pf->dwRBitMask = 0xF800; pf->dwGBitMask = 0x07E0; pf->dwBBitMask = 0x001F;
    pf = &g_d3dTexFormats[1].ddpfPixelFormat;          /* ARGB1555          */
    pf->dwFlags = DDPF_RGB | DDPF_ALPHAPIXELS;
    pf->dwRGBBitCount = 16;
    pf->dwRBitMask = 0x7C00; pf->dwGBitMask = 0x03E0; pf->dwBBitMask = 0x001F;
    pf->dwRGBAlphaBitMask = 0x8000;
    pf = &g_d3dTexFormats[2].ddpfPixelFormat;          /* ARGB4444          */
    pf->dwFlags = DDPF_RGB | DDPF_ALPHAPIXELS;
    pf->dwRGBBitCount = 16;
    pf->dwRBitMask = 0x0F00; pf->dwGBitMask = 0x00F0; pf->dwBBitMask = 0x000F;
    pf->dwRGBAlphaBitMask = 0xF000;
}

/* Called by the runtime to learn our DirectDraw/D3D capabilities and callback
 * table (M4c-2: real vmiData/caps so surfaces can actually be created). All
 * surface fpVidMem values are byte OFFSETS into the mapped video memory - the
 * GDI primary is offset 0 (vmiData.fpPrimary), kernel access is
 * pjScreen+fpVidMem, per-process user access is DdMapMemory's fpProcess+
 * fpVidMem. We declare NO DDraw-managed heaps: Dd_CreateSurface places every
 * vidmem surface itself on gbkernel's carve-out (driver-managed pattern), so
 * the layout stays under the host-tested gbk_surf allocator. */
BOOL APIENTRY DrvGetDirectDrawInfo(
        DHPDEV dhpdev, DD_HALINFO *pHalInfo,
        DWORD *pdwNumHeaps, VIDEOMEMORY *pvmList,
        DWORD *pdwNumFourCC, DWORD *pdwFourCC)
{
    PFXPDEV ppdev = (PFXPDEV)dhpdev;
    gbk_meminfo_t mi;
    int have3d;

    UNREFERENCED_PARAMETER(pvmList);
    UNREFERENCED_PARAMETER(pdwFourCC);

    if (ppdev == NULL) {
        return FALSE;
    }
    have3d = (gbkernel_get_meminfo(&mi) == 0);

    *pdwNumHeaps  = 0;   /* driver-managed surface placement - see above      */
    *pdwNumFourCC = 0;

    memset(pHalInfo, 0, sizeof(*pHalInfo));
    pHalInfo->dwSize = sizeof(*pHalInfo);

    /* vmiData: the primary's geometry/format so the runtime can build the
     * primary surface and compute vidmem addresses. */
    pHalInfo->vmiData.fpPrimary       = 0;
    pHalInfo->vmiData.dwDisplayWidth  = (DWORD)ppdev->cxScreen;
    pHalInfo->vmiData.dwDisplayHeight = (DWORD)ppdev->cyScreen;
    pHalInfo->vmiData.lDisplayPitch   = ppdev->lDeltaScreen;
    fxnt_fill_ddpf(&pHalInfo->vmiData.ddpfDisplay, ppdev);
    pHalInfo->vmiData.dwOffscreenAlign = 16;   /* gbk_surf_pitch granularity */
    pHalInfo->vmiData.dwOverlayAlign   = 16;
    pHalInfo->vmiData.dwTextureAlign   = 16;
    pHalInfo->vmiData.dwZBufferAlign   = 16;
    pHalInfo->vmiData.dwAlphaAlign     = 16;
    pHalInfo->vmiData.pvPrimary        = ppdev->pjScreen;

    /* Core caps: what Dd_Blt/Dd_Flip actually implement (SRCCOPY copies,
     * color/depth fill, the 3D present) - nothing speculative. */
    pHalInfo->ddCaps.dwSize  = sizeof(DDNTCORECAPS);
    pHalInfo->ddCaps.dwCaps  = DDCAPS_3D | DDCAPS_BLT | DDCAPS_BLTCOLORFILL
                             | DDCAPS_BLTDEPTHFILL | DDCAPS_ZBLTS | DDCAPS_GDI;
    pHalInfo->ddCaps.dwZBufferBitDepths = DDBD_16;   /* Voodoo3: 16-bit Z   */
    pHalInfo->ddCaps.dwVidMemTotal = have3d ? mi.surfSize : 0;
    pHalInfo->ddCaps.dwVidMemFree  = have3d ? mi.surfSize : 0;
    /* ROP support: SRCCOPY only (bit 0xCC of the 256-bit rop set) */
    pHalInfo->ddCaps.dwRops[0xCC >> 5] |= 1UL << (0xCC & 31);
    pHalInfo->ddCaps.ddsCaps.dwCaps =
        DDSCAPS_PRIMARYSURFACE | DDSCAPS_OFFSCREENPLAIN | DDSCAPS_FLIP |
        DDSCAPS_BACKBUFFER | DDSCAPS_3DDEVICE | DDSCAPS_TEXTURE |
        DDSCAPS_ZBUFFER | DDSCAPS_VIDEOMEMORY | DDSCAPS_LOCALVIDMEM;

    /* D3D discovery: the device caps + callback table the runtime binds at
     * this query (previously left NULL - the M3 "filled in D3D cb init" gap;
     * without these no D3D device is ever enumerated on the driver). */
    fxnt_fill_texformats();
    fxnt_get_d3d_callbacks(&g_d3dCallbacksTbl, &g_d3dGlobalData);
    g_d3dGlobalData.dwNumTextureFormats = FXNT_NUM_TEXFMT;
    g_d3dGlobalData.lpTextureFormats    = g_d3dTexFormats;
    pHalInfo->lpD3DGlobalDriverData = (LPVOID)&g_d3dGlobalData;
    pHalInfo->lpD3DHALCallbacks     = (LPVOID)&g_d3dCallbacksTbl;

    /* NT5+ D3D/extended caps are answered through GetDriverInfo. */
    pHalInfo->GetDriverInfo = DdGetDriverInfo;
    pHalInfo->dwFlags      |= DDHALINFO_GETDRIVERINFOSET;
    return TRUE;
}

/* ---- Direct3D DDI: the callback the runtime invokes to render ------------ */

/* One D3D context <-> one fxd_context. */
static DWORD APIENTRY d3d_ContextCreate(LPD3DHAL_CONTEXTCREATEDATA p)
{
    /* Render-target geometry from the runtime's RT surface. NT gotcha (same
     * aliasing as lpDDCommands in d3d_DrawPrimitives2): lpDDSLcl is typed
     * with the user-mode DDRAWI name but carries the kernel
     * PDD_SURFACE_LOCAL. depth stays 16 - the Voodoo pipeline renders 16bpp
     * internally even under the 5a 32bpp compat mode (the present converts). */
    PDD_SURFACE_LOCAL rt = (PDD_SURFACE_LOCAL)(void *)p->lpDDSLcl;
    fxd_context *c;
    int w = 640, h = 480;

    if (rt != NULL && rt->lpGbl != NULL &&
        rt->lpGbl->wWidth != 0 && rt->lpGbl->wHeight != 0) {
        w = (int)rt->lpGbl->wWidth;
        h = (int)rt->lpGbl->wHeight;
    }
    c = fxdglue_context_create(w, h, /*depth*/16);
    p->dwhContext = (ULONG_PTR)c;
    p->ddrval = c ? DD_OK : DDERR_OUTOFMEMORY;
    return DDHAL_DRIVER_HANDLED;
}

static DWORD APIENTRY d3d_ContextDestroy(LPD3DHAL_CONTEXTDESTROYDATA p)
{
    fxdglue_context_destroy((fxd_context*)(ULONG_PTR)p->dwhContext);
    p->ddrval = DD_OK;
    return DDHAL_DRIVER_HANDLED;
}

/* Byte capacity of a DP2 command/vertex-buffer surface (M4c-2 review #4): a
 * rectangular surface holds lPitch*wHeight bytes; a LINEAR D3D buffer (the
 * command/vertex-buffer case - wHeight 0) stores its byte size in the
 * dwLinearSize union member (DD_SURFACE_GLOBAL, ddrawint.h; the DDK 3dlabs
 * sample sizes D3D buffers by the same field). Returns 0 when the size is
 * unknown or the multiply would wrap - the caller must FAIL the batch then,
 * never walk an unproven window. */
static ULONG fxnt_surf_databytes(PDD_SURFACE_LOCAL psl)
{
    PDD_SURFACE_GLOBAL gbl = psl->lpGbl;

    if (gbl->wHeight != 0 && gbl->lPitch > 0) {
        if ((ULONG)gbl->lPitch > 0xFFFFFFFFUL / (ULONG)gbl->wHeight) {
            return 0;
        }
        return (ULONG)gbl->lPitch * (ULONG)gbl->wHeight;
    }
    return (ULONG)gbl->dwLinearSize;
}

/* THE hot path (M4a: the REAL DP2 stream). The runtime batches the frame into
 * a DP2 command buffer + a separate FVF vertex buffer; fxd_dp2_execute_real
 * translates both straight onto the fxD3D core. NT gotcha (see the header
 * decision notes): lpDDCommands/lpDDVertex are DD_SURFACE_LOCAL surface
 * OBJECTS - the bytes hang off lpGbl->fpVidMem - while lpVertices is a raw
 * user pointer only when D3DHALDP2_USERMEMVERTICES is set. */
static DWORD APIENTRY d3d_DrawPrimitives2(LPD3DHAL_DRAWPRIMITIVES2DATA p)
{
    fxd_context *c = (fxd_context*)(ULONG_PTR)p->dwhContext;
    PDD_SURFACE_LOCAL cmdsurf = (PDD_SURFACE_LOCAL)p->lpDDCommands;
    const BYTE *cmd;
    const BYTE *vtx = 0;
    DWORD errofs = 0;
    DWORD stride;
    int rc;

    if (!c || !cmdsurf || !cmdsurf->lpGbl ||
        cmdsurf->lpGbl->fpVidMem == 0) {   /* no command bytes at all       */
        p->ddrval = DDERR_GENERIC;
        return DDHAL_DRIVER_HANDLED;
    }
    /* Clamp the command window to the command surface's byte capacity BEFORE
     * the walk (M4c-2 review #4): the translator is provably safe WITHIN
     * [cmd, cmd+dwCommandLength) (19M-iter ASAN/UBSAN fuzz), so the caller
     * must prove that window lies inside the allocation - an oversized
     * offset/length would otherwise walk past it (OOB kernel read). The
     * clamp arithmetic is the host-tested gbk_dp2_window_ok. */
    if (!gbk_dp2_window_ok((h3u32)fxnt_surf_databytes(cmdsurf),
                           (h3u32)p->dwCommandOffset,
                           (h3u32)p->dwCommandLength, 1)) {
        p->dwErrorOffset = p->dwCommandOffset;
        p->ddrval = DDERR_INVALIDPARAMS;
        return DDHAL_DRIVER_HANDLED;
    }
    cmd = (const BYTE*)(ULONG_PTR)cmdsurf->lpGbl->fpVidMem + p->dwCommandOffset;

    stride = fxd_dp2_fvf_stride(p->dwVertexType);
    if (p->dwFlags & D3DHALDP2_USERMEMVERTICES) {
        /* lpVertices is a raw USER-mode pointer here (d3dnthal.h: "user
         * allocated memory") - the normal DX6/DX7 non-VB draw path, so it
         * cannot be rejected. Two-layer guard (review #6): the NT runtime
         * secures the [dwVertexOffset, +dwVertexLength*stride) window before
         * the call (the DDK 3dlabs sample relies on the same contract -
         * "memory is already secured"), and the WHOLE executor below runs
         * inside a structured-exception guard, so a torn/short/hostile
         * mapping faults into the handler and fails the batch instead of
         * bugchecking. No byte-size clamp is possible for user memory - the
         * SEH region IS the bound. */
        vtx = (const BYTE*)p->lpVertices;          /* raw user-mode pointer   */
    } else if (p->lpDDVertex) {                    /* vertex-buffer surface   */
        PDD_SURFACE_LOCAL vsurf = (PDD_SURFACE_LOCAL)p->lpDDVertex;
        if (vsurf->lpGbl) {
            /* review #4: bound dwVertexOffset + dwVertexLength*stride against
             * the VB surface's byte capacity - the executor validates vertex
             * INDICES against dwVertexLength but knows nothing of the
             * buffer's byte size. A bad FVF (stride 0) checks the offset
             * alone: the executor rejects the FVF before any vertex fetch. */
            if (!gbk_dp2_window_ok((h3u32)fxnt_surf_databytes(vsurf),
                                   (h3u32)p->dwVertexOffset,
                                   stride ? (h3u32)p->dwVertexLength : 0,
                                   stride ? (h3u32)stride : 1)) {
                p->dwErrorOffset = p->dwCommandOffset;
                p->ddrval = DDERR_INVALIDPARAMS;
                return DDHAL_DRIVER_HANDLED;
            }
            vtx = (const BYTE*)(ULONG_PTR)vsurf->lpGbl->fpVidMem;
        }
    }
    if (vtx)
        vtx += p->dwVertexOffset;

    /* DDI FP bracket (design risk #4 / M4b-2 minor #9): the kernel Glide
     * backend's textured vertex path (gbkernel gbk_vertex_words) does x87 work,
     * and the fxD3D reduction this DP2 executor runs may touch the FPU too.
     * Establish ONE EngSaveFloatingPointState region around the WHOLE
     * DrawPrimitives2 execution here at the DDI entry, so a preempted user
     * thread's x87/MMX/SSE state can never be corrupted no matter which gb_*
     * seam calls (gb_draw_*, gb_tex_bind) the stream reaches. gbkernel also
     * brackets each draw batch internally (it does not trust an external
     * caller); this outer region is the DDI-entry guarantee the design asks
     * for, and the two nest cleanly (each uses its own save buffer). The save
     * area is a fixed 512-byte (>= x86 FXSAVE), 16-byte-aligned buffer.
     *
     * If the engine CANNOT save FP state we SKIP the batch (M4b-2 review #6):
     * the reduction itself (vertex float copies in the fxD3D core) touches the
     * x87 FPU, so running it unbracketed could perturb the preempted thread's
     * FP state - drop the frame instead. EngSaveFloatingPointState at
     * PASSIVE_LEVEL with a valid buffer effectively never fails, so this is a
     * safety fallback, not a real path. */
    {
        BYTE      fpbuf[512 + 16];   /* >= x86 FXSAVE (512) + align slack       */
        void     *fpptr = (void *)(((unsigned long)fpbuf + 15UL) & ~15UL);
        ULONG     fpsaved = EngSaveFloatingPointState(fpptr, 512);

        if (!fpsaved) {
            p->ddrval = DDERR_GENERIC;
            return DDHAL_DRIVER_HANDLED;
        }

        /* Structured-exception guard (review #6): the vertex window can be
         * USER memory (D3DHALDP2_USERMEMVERTICES), which cannot be bounded
         * arithmetically here - if the secured mapping is short/gone, the
         * executor's reads fault. Catch the fault and fail JUST this batch
         * (MALFORMED -> DDERR_INVALIDPARAMS below) instead of bugchecking
         * win32k. The handler falls through to the FP-state restore, so the
         * interrupted thread's x87/SSE state survives the fault path too. */
        __try {
            rc = fxd_dp2_execute_real_cb(&c->dev, cmd, p->dwCommandLength,
                                         vtx, p->dwVertexLength /* count of VERTICES */,
                                         p->dwVertexType, (DWORD*)p->lpdwRStates, &errofs,
                                         fxnt_parse_unknown, NULL);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            errofs = 0;
            rc = FXD_DP2E_MALFORMED;
        }

        EngRestoreFloatingPointState(fpptr);
    }
    if (rc < 0) {
        /* Point the runtime at the offending DP2COMMAND (offset relative to the
         * same base as dwCommandOffset) either way. */
        p->dwErrorOffset = p->dwCommandOffset + errofs;
        if (rc == FXD_DP2E_UNKNOWN_OP) {
            /* COMMAND_UNPARSED protocol: an opcode we don't implement AND the
             * parse-unknown callback couldn't parse either (or none was
             * registered) - ask the runtime to parse from here and retry. */
            p->ddrval = D3DERR_COMMAND_UNPARSED;
        } else {
            /* FXD_DP2E_MALFORMED (truncated stream / out-of-range vertex ref)
             * or FXD_DP2E_BADFVF: the command IS understood and is invalid.
             * Fail the call - do NOT ask the runtime to reparse garbage (which
             * would let it silently skip a bad draw). */
            p->ddrval = DDERR_INVALIDPARAMS;
        }
    } else {
        p->dwErrorOffset = 0;   /* consumed the whole buffer                  */
        p->ddrval = DD_OK;
    }
    return DDHAL_DRIVER_HANDLED;
}

/* Publish the D3D HAL callback table (called from D3D device init). */
void fxnt_get_d3d_callbacks(D3DHAL_CALLBACKS *cb, D3DHAL_GLOBALDRIVERDATA *gd)
{
    memset(cb, 0, sizeof(*cb));
    cb->dwSize            = sizeof(*cb);
    cb->ContextCreate     = d3d_ContextCreate;
    cb->ContextDestroy    = d3d_ContextDestroy;
    /* DrawPrimitives2 lives in the D3DHAL_CALLBACKS3 table on modern runtimes;
     * wired the same way. */
    (void)d3d_DrawPrimitives2;

    memset(gd, 0, sizeof(*gd));
    gd->dwSize = sizeof(*gd);
    /* Full D3DDEVICEDESC_V1 <- fxD3D (M4c-2 review #2: this was a no-op stub,
     * publishing an all-zero hwCaps - no D3DDD_* validity bits, no color
     * model, no DP2 dev caps - so the runtime never enumerated a usable HAL
     * device). fxd_fill_caps now fills the real device description: validity
     * flags, D3DCOLOR_RGB, DRAWPRIMITIVES2/2EX dev caps, tri/line caps, and
     * the core's native DDBD_16 render / DDBD_16 Z depths. */
    fxdglue_get_caps(&gd->hwCaps);

    /* Per docs/3dfx-d3d-hal-design.md 5a "32-bit compatibility mode": the NT
     * surface path implements the 32bpp RT proxy + the 565->8888 present
     * convert, so OR the DDBD_32 render bit in HERE (the chassis layer that
     * owns that path). Z stays DDBD_16 ONLY (review #9): Dd_CanCreateSurface
     * rejects every non-16 Z depth, and the old DDBD_24 advertisement made
     * apps that pick the deepest advertised Z fail their whole D3D init. */
    gd->hwCaps.dwDeviceRenderBitDepth |= DDBD_32;
}

/* ---- DirectDraw DDI: enable pair + object callbacks ---------------------- *
 *
 * Clean-room: the DDraw/D3D driver interface from ddrawint.h. M4c-2: real
 * surface placement / lock / present bodies. The layout arithmetic (pitch,
 * sizes, the bump allocator, the 565->8888 expansion) is the host-tested
 * gbk_surf module; this file only unpacks DDK structures and moves bytes.
 * Everything here is INTEGER work - no new EngSaveFloatingPointState regions
 * are needed (the DDI FP bracket stays where the float math is:
 * d3d_DrawPrimitives2 above and the gbkernel draw paths).
 * ------------------------------------------------------------------------- */

/* Seed the DDraw offscreen allocator from gbkernel's carve-out (idempotent;
 * the region is reserved out of tram at attach - gbkernel_get_meminfo). */
static int fxnt_ensure_ddheap(PFXPDEV ppdev, const gbk_meminfo_t *mi)
{
    if (!ppdev->ddHeapLive) {
        if (mi->surfSize == 0) {
            return 0;
        }
        gbk_surfheap_init(&ppdev->ddHeap, mi->surfOffset, mi->surfSize);
        ppdev->ddHeapLive = 1;
    }
    return 1;
}

/* Per-surface ownership/role tags, stored in DD_SURFACE_GLOBAL.dwReserved1
 * ("reserved for use by display driver" - the W2K DDK samples tag surface
 * ownership through exactly this field). M4c-2 review #3/#5: DestroySurface
 * and the present paths used to identify surfaces by fpVidMem NUMERIC RANGE /
 * equality, which (a) let a system-memory pointer that numerically aliases
 * the heap range trigger a spurious heap free, and (b) broke after Flip,
 * because the runtime rotates the chain's fpVidMem values while the surface
 * OBJECTS (and their globals, tags included) keep their identity. */
#define FXNT_SURFTAG_HEAP  0x00000001UL  /* placed by gbk_surfheap_alloc; the
                                          * ONLY license to gbk_surfheap_free */
#define FXNT_SURFTAG_RT3D  0x00000002UL  /* the app's screen-sized 3D back/RT
                                          * - the present source object       */

/* 16bpp pixel-layout kind of a surface, resolved from its pixel-format MASKS
 * (or the display format when it carries none) - review #11: byte counts
 * alone mis-decode ARGB1555/4444 as RGB565. RAW = not a 16bpp-RGB layout
 * (Z, 8bpp, 32bpp): byte-identical copies only. BAD = 16bpp RGB with masks
 * we don't recognize - hand it back to the runtime's HEL. */
#define FXNT_PK_RAW   0
#define FXNT_PK_565   1
#define FXNT_PK_1555  2
#define FXNT_PK_4444  3
#define FXNT_PK_BAD   4

static int fxnt_surf_pixkind(PFXPDEV ppdev, PDD_SURFACE_LOCAL psl)
{
    DDPIXELFORMAT *pf;
    ULONG bits, r, g, b;

    if (psl->ddsCaps.dwCaps & DDSCAPS_ZBUFFER) {
        return FXNT_PK_RAW;
    }
    if (psl->dwFlags & DDRAWISURF_HASPIXELFORMAT) {
        pf = &psl->lpGbl->ddpfSurface;
        if (!(pf->dwFlags & DDPF_RGB)) {
            return FXNT_PK_RAW;
        }
        bits = pf->dwRGBBitCount;
        r = pf->dwRBitMask;
        g = pf->dwGBitMask;
        b = pf->dwBBitMask;
    } else {
        bits = ppdev->ulBitsPerPel;
        r = ppdev->flRed;
        g = ppdev->flGreen;
        b = ppdev->flBlue;
    }
    if (bits != 16) {
        return FXNT_PK_RAW;
    }
    if (r == 0xF800 && g == 0x07E0 && b == 0x001F) {
        return FXNT_PK_565;
    }
    if (r == 0x7C00 && g == 0x03E0 && b == 0x001F) {
        return FXNT_PK_1555;
    }
    if (r == 0x0F00 && g == 0x00F0 && b == 0x000F) {
        return FXNT_PK_4444;
    }
    return FXNT_PK_BAD;
}

/* Bytes per pixel of a surface: its own pixel format when it carries one
 * (DDRAWISURF_HASPIXELFORMAT), the Z depth for Z surfaces, else the display
 * depth. Returns 0 for a format we can't express (odd bit counts). */
static ULONG fxnt_surf_bytespp(PFXPDEV ppdev, PDD_SURFACE_LOCAL psl)
{
    DDPIXELFORMAT *pf;
    ULONG bits;

    if (psl->dwFlags & DDRAWISURF_HASPIXELFORMAT) {
        pf = &psl->lpGbl->ddpfSurface;
        if (pf->dwFlags & DDPF_ZBUFFER) {
            bits = pf->dwZBufferBitDepth;
        } else {
            bits = pf->dwRGBBitCount;
        }
    } else if (psl->ddsCaps.dwCaps & DDSCAPS_ZBUFFER) {
        bits = 16;                       /* Voodoo3: 16-bit Z only            */
    } else {
        bits = ppdev->ulBitsPerPel;
    }
    if (bits != 8 && bits != 16 && bits != 32) {
        return 0;
    }
    return bits >> 3;
}

/* Kernel VA of a vidmem surface's bits (pjScreen + fpVidMem). NULL when
 * fpVidMem is not an offset inside the mapped framebuffer - that is how a
 * system-memory surface (whose fpVidMem is a real user pointer) is told
 * apart, and it also bounds every CPU blt inside the BAR1 mapping. */
static BYTE *fxnt_surf_kva(PFXPDEV ppdev, PDD_SURFACE_LOCAL psl, ULONG *pcjMax)
{
    ULONG cjFb = (ULONG)ppdev->vmi.FrameBufferLength;
    ULONG off  = (ULONG)psl->lpGbl->fpVidMem;

    if (ppdev->pjScreen == NULL || off >= cjFb) {
        return NULL;
    }
    if (pcjMax != NULL) {
        *pcjMax = cjFb - off;
    }
    return ppdev->pjScreen + off;
}

/* DD_CALLBACKS (per-device DirectDraw object callbacks). */

/* Validate a create request against the Voodoo3: 16-bit Z; 16bpp RGB surfaces
 * everywhere; 32bpp RGB only for non-texture surfaces (the design-5a RT proxy
 * + plain offscreen on a 32bpp desktop); textures power-of-2 up to 256. */
static DWORD APIENTRY Dd_CanCreateSurface(PDD_CANCREATESURFACEDATA p)
{
    PFXPDEV ppdev = p->lpDD ? (PFXPDEV)p->lpDD->dhpdev : NULL;
    DDSURFACEDESC *pd = (DDSURFACEDESC *)p->lpDDSurfaceDesc;
    DDPIXELFORMAT *pf;
    DWORD caps, w, h;

    if (ppdev == NULL || pd == NULL) {
        return DDHAL_DRIVER_NOTHANDLED;
    }
    if (!ppdev->gbkAttached) {
        return DDHAL_DRIVER_NOTHANDLED;  /* 2D-only: runtime emulates in RAM  */
    }
    caps = pd->ddsCaps.dwCaps;

    if (caps & DDSCAPS_ZBUFFER) {
        if (p->bIsDifferentPixelFormat &&
            (pd->ddpfPixelFormat.dwFlags & DDPF_ZBUFFER) &&
            pd->ddpfPixelFormat.dwZBufferBitDepth != 16) {
            p->ddRVal = DDERR_INVALIDPIXELFORMAT;
            return DDHAL_DRIVER_HANDLED;
        }
        p->ddRVal = DD_OK;
        return DDHAL_DRIVER_HANDLED;
    }

    if (p->bIsDifferentPixelFormat) {
        pf = &pd->ddpfPixelFormat;
        if (!(pf->dwFlags & DDPF_RGB)) {
            p->ddRVal = DDERR_INVALIDPIXELFORMAT;
            return DDHAL_DRIVER_HANDLED;
        }
        if (pf->dwRGBBitCount != 16 &&
            !(pf->dwRGBBitCount == 32 && !(caps & DDSCAPS_TEXTURE))) {
            p->ddRVal = DDERR_INVALIDPIXELFORMAT;
            return DDHAL_DRIVER_HANDLED;
        }
    }

    if (caps & DDSCAPS_TEXTURE) {
        w = (pd->dwFlags & DDSD_WIDTH)  ? pd->dwWidth  : 0;
        h = (pd->dwFlags & DDSD_HEIGHT) ? pd->dwHeight : 0;
        if (w == 0 || h == 0 || w > 256 || h > 256 ||
            (w & (w - 1)) != 0 || (h & (h - 1)) != 0) {
            p->ddRVal = DDERR_INVALIDPARAMS;  /* TMU: pow2 up to 256x256      */
            return DDHAL_DRIVER_HANDLED;
        }
    }

    p->ddRVal = DD_OK;
    return DDHAL_DRIVER_HANDLED;
}

/* Place every vidmem surface on gbkernel's carve-out (driver-managed - no
 * DDraw heap). fpVidMem = board byte offset (primary is 0):
 *   - primary            -> offset 0, desktop pitch;
 *   - screen-sized 16-bit Z -> gbkernel's aux buffer;
 *   - the app's screen-sized 16bpp back/RT -> gbkernel's back color buffer,
 *     so the D3D core's output IS this surface (present blits it out);
 *   - everything else (textures, plain offscreen, the design-5a 32bpp RT
 *     proxy on a 32bpp desktop) -> the gbk_surfheap bump allocator over the
 *     reserved region. A 32bpp screen-sized back/RT is tagged as the present
 *     source: D3D still renders 16bpp into gbkernel's back buffer and the
 *     present converts 565->8888 (design 5a), so the proxy's own bits are
 *     only what the app writes there.
 * Failure of any surface unwinds this call's allocations and fails whole. */
static DWORD APIENTRY Dd_CreateSurface(PDD_CREATESURFACEDATA p)
{
    PFXPDEV ppdev = p->lpDD ? (PFXPDEV)p->lpDD->dhpdev : NULL;
    DDSURFACEDESC *pd = (DDSURFACEDESC *)p->lpDDSurfaceDesc;
    gbk_meminfo_t mi;
    DWORD i;
    ULONG nHeap = 0;
    int fail = 0;
    int rtWasValid;

    if (ppdev == NULL || p->lplpSList == NULL) {
        return DDHAL_DRIVER_NOTHANDLED;
    }
    if (!ppdev->gbkAttached || gbkernel_get_meminfo(&mi) != 0 ||
        !fxnt_ensure_ddheap(ppdev, &mi)) {
        return DDHAL_DRIVER_NOTHANDLED;  /* 2D-only: runtime RAM fallback     */
    }
    rtWasValid = ppdev->rt3dValid;

    for (i = 0; i < p->dwSCnt; i++) {
        PDD_SURFACE_LOCAL  psl = p->lplpSList[i];
        PDD_SURFACE_GLOBAL gbl;
        DWORD caps, w, h;
        ULONG bpp, pitch, bytes, off;

        if (psl == NULL || psl->lpGbl == NULL) {
            fail = 1;
            break;
        }
        gbl  = psl->lpGbl;
        caps = psl->ddsCaps.dwCaps;
        if (caps & DDSCAPS_SYSTEMMEMORY) {
            gbl->dwReserved1 = 0;        /* never ours (review #3)            */
            continue;                    /* the runtime's to allocate         */
        }
        gbl->dwReserved1 = 0;            /* tags are set below per placement  */

        w = gbl->wWidth;
        h = gbl->wHeight;
        if ((w == 0 || h == 0) && pd != NULL) {
            w = (pd->dwFlags & DDSD_WIDTH)  ? pd->dwWidth  : w;
            h = (pd->dwFlags & DDSD_HEIGHT) ? pd->dwHeight : h;
        }
        bpp = fxnt_surf_bytespp(ppdev, psl);
        if (bpp == 0 || w == 0 || h == 0) {
            fail = 1;
            break;
        }

        if (caps & DDSCAPS_PRIMARYSURFACE) {
            gbl->fpVidMem = 0;           /* the GDI primary at offset 0       */
            gbl->lPitch   = ppdev->lDeltaScreen;
            continue;
        }
        if ((caps & DDSCAPS_ZBUFFER) &&
            w == mi.width && h == mi.height && bpp == 2 && mi.auxOffset != 0) {
            gbl->fpVidMem = mi.auxOffset;    /* gbkernel's aux/z buffer       */
            gbl->lPitch   = (LONG)mi.stride3d;
            continue;
        }
        if ((caps & (DDSCAPS_BACKBUFFER | DDSCAPS_3DDEVICE)) &&
            !ppdev->rt3dValid && w == mi.width && h == mi.height && bpp == 2) {
            gbl->fpVidMem = mi.backOffset;   /* gbkernel's back color buffer  */
            gbl->lPitch   = (LONG)mi.stride3d;
            gbl->dwReserved1 = FXNT_SURFTAG_RT3D;   /* aliased, NOT heap      */
            ppdev->fpRT3d    = mi.backOffset;
            ppdev->rt3dValid = 1;
            continue;
        }

        pitch = gbk_surf_pitch((h3u32)w, (h3u32)bpp);
        bytes = gbk_surf_bytes(pitch, (h3u32)h);
        off   = (bytes != 0) ? gbk_surfheap_alloc(&ppdev->ddHeap, bytes)
                             : GBK_SURF_FAIL;
        if (off == GBK_SURF_FAIL) {
            fail = 1;
            break;
        }
        nHeap++;
        gbl->fpVidMem = off;
        gbl->lPitch   = (LONG)pitch;
        gbl->dwReserved1 = FXNT_SURFTAG_HEAP;   /* owned by the bump heap     */
        if ((caps & (DDSCAPS_BACKBUFFER | DDSCAPS_3DDEVICE)) &&
            !ppdev->rt3dValid && w == mi.width && h == mi.height && bpp == 4) {
            gbl->dwReserved1 |= FXNT_SURFTAG_RT3D;
            ppdev->fpRT3d    = off;      /* the design-5a 32bpp RT proxy      */
            ppdev->rt3dValid = 1;
        }
    }

    if (fail) {
        while (nHeap > 0) {
            (void)gbk_surfheap_free(&ppdev->ddHeap);
            nHeap--;
        }
        /* Also drop the tags stamped by this call: the runtime never sends a
         * FAILED create's surfaces to DdDestroySurface, but a stale HEAP tag
         * on a recycled global could otherwise license a bogus free later. */
        for (i = 0; i < p->dwSCnt; i++) {
            if (p->lplpSList[i] != NULL && p->lplpSList[i]->lpGbl != NULL) {
                p->lplpSList[i]->lpGbl->dwReserved1 = 0;
            }
        }
        ppdev->rt3dValid = rtWasValid;   /* undo a tag made by this call      */
        p->ddRVal = DDERR_OUTOFVIDEOMEMORY;
        return DDHAL_DRIVER_HANDLED;
    }
    p->ddRVal = DD_OK;
    return DDHAL_DRIVER_HANDLED;
}

/* Map/unmap the video memory into the requesting process. This is what makes
 * Dd_Lock's user pointers possible: the runtime stores fpProcess per process
 * and hands it back in DD_LOCKDATA. Standard NT contract via the paired
 * miniport (IOCTL_VIDEO_SHARE_VIDEO_MEMORY / UNSHARE). */
static DWORD APIENTRY Dd_MapMemory(PDD_MAPMEMORYDATA p)
{
    PFXPDEV ppdev = p->lpDD ? (PFXPDEV)p->lpDD->dhpdev : NULL;
    VIDEO_SHARE_MEMORY sh;
    VIDEO_SHARE_MEMORY_INFORMATION shi;
    DWORD cjRet;

    if (ppdev == NULL) {
        p->ddRVal = DDERR_GENERIC;
        return DDHAL_DRIVER_HANDLED;
    }

    memset(&sh, 0, sizeof(sh));
    sh.ProcessHandle = p->hProcess;
    if (p->bMap) {
        sh.ViewOffset = 0;
        sh.ViewSize   = (ULONG)ppdev->vmi.FrameBufferLength;
        sh.RequestedVirtualAddress = NULL;
        memset(&shi, 0, sizeof(shi));
        cjRet = 0;
        if (EngDeviceIoControl(ppdev->hDriver, IOCTL_VIDEO_SHARE_VIDEO_MEMORY,
                               &sh, sizeof(sh), &shi, sizeof(shi),
                               &cjRet) != NO_ERROR) {
            p->ddRVal = DDERR_GENERIC;
            return DDHAL_DRIVER_HANDLED;
        }
        p->fpProcess = (FLATPTR)shi.VirtualAddress;
    } else {
        sh.ViewOffset = 0;
        sh.ViewSize   = 0;
        sh.RequestedVirtualAddress = (PVOID)p->fpProcess;
        cjRet = 0;
        EngDeviceIoControl(ppdev->hDriver, IOCTL_VIDEO_UNSHARE_VIDEO_MEMORY,
                           &sh, sizeof(sh), NULL, 0, &cjRet);
    }
    p->ddRVal = DD_OK;
    return DDHAL_DRIVER_HANDLED;
}

static DWORD APIENTRY Dd_WaitForVerticalBlank(PDD_WAITFORVERTICALBLANKDATA p)
{
    /* TODO(fxd3d M4d): poll the vertical-retrace status bit once the exact
     * SSTIO status bit is on-card verified; NOTHANDLED = runtime emulation. */
    p->ddRVal = DDERR_UNSUPPORTED;
    return DDHAL_DRIVER_NOTHANDLED;
}

/* NT callbacks (queried via GUID_NTCallbacks in DdGetDriverInfo). */
static DWORD APIENTRY Dd_SetExclusiveMode(PDD_SETEXCLUSIVEMODEDATA p)
{
    PFXPDEV ppdev = p->lpDD ? (PFXPDEV)p->lpDD->dhpdev : NULL;
    if (ppdev != NULL) {
        ppdev->ddExclusive = (int)p->dwEnterExcl;
    }
    p->ddRVal = DD_OK;
    return DDHAL_DRIVER_HANDLED;
}

static DWORD APIENTRY Dd_FlipToGDISurface(PDD_FLIPTOGDISURFACEDATA p)
{
    /* Blit-present never moves the scanout base: the GDI primary (offset 0)
     * is always what the CRT scans, so both directions are a no-op. */
    UNREFERENCED_PARAMETER(p->dwToGDI);
    p->ddRVal = DD_OK;
    return DDHAL_DRIVER_HANDLED;
}

/* DD_SURFACECALLBACKS (per-surface object callbacks). */

/* Ownership is decided by the EXPLICIT dwReserved1 tag stamped at create time
 * (M4c-2 review #3) - NEVER by the fpVidMem value: a system-memory surface's
 * fpVidMem is a real user pointer that can numerically alias the heap's
 * board-offset range (the old range test then drove the live count to zero
 * and rewound the cursor under live surfaces -> two surfaces sharing VRAM),
 * and after a Flip the runtime rotates fpVidMem values between the chain's
 * surfaces while each object keeps its own global + tags. System-memory
 * surfaces bail first, exactly as the vintage Voodoo driver's
 * DdDestroySurface does; clearing the tag afterwards makes a double-destroy
 * of the same surface a no-op. */
static DWORD APIENTRY Dd_DestroySurface(PDD_DESTROYSURFACEDATA p)
{
    PFXPDEV ppdev = p->lpDD ? (PFXPDEV)p->lpDD->dhpdev : NULL;
    PDD_SURFACE_LOCAL psl = p->lpDDSurface;
    ULONG tags;

    if (ppdev == NULL || psl == NULL || psl->lpGbl == NULL) {
        p->ddRVal = DD_OK;
        return DDHAL_DRIVER_NOTHANDLED;
    }
    if (psl->ddsCaps.dwCaps & DDSCAPS_SYSTEMMEMORY) {
        p->ddRVal = DD_OK;               /* the runtime's allocation          */
        return DDHAL_DRIVER_NOTHANDLED;
    }
    tags = (ULONG)psl->lpGbl->dwReserved1;
    if ((tags & FXNT_SURFTAG_HEAP) && ppdev->ddHeapLive) {
        (void)gbk_surfheap_free(&ppdev->ddHeap);   /* refuses if unbalanced   */
    }
    if (tags & FXNT_SURFTAG_RT3D) {
        ppdev->rt3dValid = 0;            /* present source is gone            */
    }
    psl->lpGbl->dwReserved1 = 0;         /* double-destroy guard              */
    p->ddRVal = DD_OK;
    return DDHAL_DRIVER_HANDLED;
}

/* Return a MAPPED pointer to the surface bits. The pointer handed back must
 * be usable by the CALLING PROCESS, so it is built on the per-process base
 * from Dd_MapMemory (DD_LOCKDATA.fpProcess) + the surface's fpVidMem offset -
 * NOT on pjScreen, which is the kernel/session mapping (chassis.c
 * DrvEnableSurface) and would fault user mode. The 3D pipe is drained first
 * so a Lock of the render target reads settled pixels. */
static DWORD APIENTRY Dd_Lock(PDD_LOCKDATA p)
{
    PFXPDEV ppdev = p->lpDD ? (PFXPDEV)p->lpDD->dhpdev : NULL;
    PDD_SURFACE_LOCAL psl = p->lpDDSurface;
    ULONG off, bpp;

    if (ppdev == NULL || psl == NULL || psl->lpGbl == NULL) {
        return DDHAL_DRIVER_NOTHANDLED;
    }
    off = (ULONG)psl->lpGbl->fpVidMem;
    if (off >= (ULONG)ppdev->vmi.FrameBufferLength) {
        return DDHAL_DRIVER_NOTHANDLED;  /* not a vidmem offset (sysmem)      */
    }

    if (ppdev->gbkAttached) {
        gb_finish();                     /* drain the FIFO before CPU access  */
    }

    if (p->fpProcess == 0) {
        /* no per-process mapping yet: let the runtime compute the address
         * itself from its own DdMapMemory bookkeeping. */
        return DDHAL_DRIVER_NOTHANDLED;
    }
    if (p->bHasRect) {
        bpp = fxnt_surf_bytespp(ppdev, psl);
        if (bpp == 0) {
            p->ddRVal = DDERR_GENERIC;
            return DDHAL_DRIVER_HANDLED;
        }
        off += (ULONG)p->rArea.top * (ULONG)psl->lpGbl->lPitch
             + (ULONG)p->rArea.left * bpp;
    }
    p->lpSurfData = (LPVOID)((BYTE *)(ULONG_PTR)p->fpProcess + off);
    p->ddRVal = DD_OK;
    return DDHAL_DRIVER_HANDLED;
}

static DWORD APIENTRY Dd_Unlock(PDD_UNLOCKDATA p)
{
    /* Nothing to flush: CPU writes went straight to video memory. (M4d hook:
     * mark a locked TEXTURE surface dirty here for the handle->TMU download
     * path when D3dCreateSurfaceEx lands.) */
    p->ddRVal = DD_OK;
    return DDHAL_DRIVER_HANDLED;
}

/* Present for a flipping chain (design 5a + gbkernel design sec 5): the 3D
 * core always renders into gbkernel's fixed back color buffer, so a flip is
 * the 2D SRCCOPY blit of that buffer onto the primary at offset 0 - with the
 * 565->8888 expansion done by the 2D engine's dst format when the desktop is
 * 32bpp (gb_swap). gb_swap also throttles on the pending-swap count.
 *
 * M4c-2 review #5: the flip is keyed to DD_FLIPDATA.lpSurfTarg - the surface
 * the runtime says must become visible (ddrawint.h "target surface (to flip
 * to)"). In a standard chain that is the app's 3D back/RT OBJECT every flip:
 * object identity is stable while the runtime rotates the chain's fpVidMem
 * values after each success, which is exactly why the present source is
 * identified by the FXNT_SURFTAG_RT3D tag in the surface GLOBAL (it travels
 * with the object, not with the rotated offset - Dd_Blt and DestroySurface
 * key on the same tag, so the rotation can no longer misroute them). A flip
 * whose target is NOT the tagged 3D source (overlay chains etc.) is refused
 * -> the runtime emulates it. After our blt-present the visible primary
 * holds the same pixels as the back buffer, so a Lock of the target object
 * (whose fpVidMem now names the primary) reads the presented frame; real
 * scanout flips (vidDesktopStartAddr) remain M4d work. */
static DWORD APIENTRY Dd_Flip(PDD_FLIPDATA p)
{
    PFXPDEV ppdev = p->lpDD ? (PFXPDEV)p->lpDD->dhpdev : NULL;
    PDD_SURFACE_LOCAL targ = p->lpSurfTarg;

    if (ppdev == NULL || !ppdev->gbkAttached) {
        return DDHAL_DRIVER_NOTHANDLED;
    }
    if (targ == NULL || targ->lpGbl == NULL || !ppdev->rt3dValid ||
        ((ULONG)targ->lpGbl->dwReserved1 & FXNT_SURFTAG_RT3D) == 0) {
        return DDHAL_DRIVER_NOTHANDLED;  /* not the 3D chain: runtime HEL     */
    }
    gb_swap(0);
    p->ddRVal = DD_OK;
    return DDHAL_DRIVER_HANDLED;
}

/* One 1:1 destination sub-rect of a CPU blt: fill (16/32bpp) or copy with
 * optional 565->8888 expansion (the host-tested gbk_conv565_row_8888 - the
 * design-5a CPU convert used for windowed/partial presents and 2D blits on a
 * 32bpp desktop). Rects are clamped to both surfaces AND the framebuffer
 * mapping before any byte moves; pure integer throughout. */
static HRESULT fxnt_blt_rect(PFXPDEV ppdev,
                             PDD_SURFACE_LOCAL dst, PDD_SURFACE_LOCAL src,
                             LONG dl, LONG dt, LONG dr, LONG db,
                             LONG sl, LONG st,
                             int fill, ULONG fillVal)
{
    BYTE *pjDst, *pjSrc, *pjD, *pjS;
    ULONG cjDstMax, cjSrcMax;
    ULONG dbpp, sbpp;
    LONG  dPitch, sPitch, w, h, y, x, clip;
    ULONG need;
    int   skind, dkind;

    dbpp = fxnt_surf_bytespp(ppdev, dst);
    if (dbpp == 0) {
        return DDERR_UNSUPPORTED;
    }
    dPitch = dst->lpGbl->lPitch;

    /* clamp the dest rect to the dest surface */
    if (dl < 0) { sl -= dl; dl = 0; }
    if (dt < 0) { st -= dt; dt = 0; }
    if (dr > (LONG)dst->lpGbl->wWidth)  dr = (LONG)dst->lpGbl->wWidth;
    if (db > (LONG)dst->lpGbl->wHeight) db = (LONG)dst->lpGbl->wHeight;
    w = dr - dl;
    h = db - dt;
    if (w <= 0 || h <= 0) {
        return DD_OK;                    /* fully clipped away                */
    }

    pjDst = fxnt_surf_kva(ppdev, dst, &cjDstMax);
    if (pjDst == NULL || dPitch <= 0) {
        return DDERR_UNSUPPORTED;
    }
    /* whole dest walk must stay inside the mapping */
    need = (ULONG)(dt + h - 1) * (ULONG)dPitch + ((ULONG)(dl + w)) * dbpp;
    if (need > cjDstMax) {
        return DDERR_UNSUPPORTED;
    }

    if (fill) {
        for (y = 0; y < h; y++) {
            pjD = pjDst + (ULONG)(dt + y) * (ULONG)dPitch + (ULONG)dl * dbpp;
            if (dbpp == 2) {
                for (x = 0; x < w; x++)
                    ((USHORT *)pjD)[x] = (USHORT)fillVal;
            } else if (dbpp == 4) {
                for (x = 0; x < w; x++)
                    ((ULONG *)pjD)[x] = fillVal;
            } else {
                return DDERR_UNSUPPORTED;
            }
        }
        return DD_OK;
    }

    if (src == NULL || src->lpGbl == NULL) {
        return DDERR_UNSUPPORTED;
    }
    sbpp = fxnt_surf_bytespp(ppdev, src);
    if (sbpp == 0) {
        return DDERR_UNSUPPORTED;
    }
    sPitch = src->lpGbl->lPitch;

    /* Pixel-format dispatch (review #11): CanCreateSurface accepts THREE
     * 16bpp RGB layouts (565/1555/4444), so the copy/convert below must be
     * selected from the pixel-format MASKS, never from byte counts alone -
     * gbk_expand565 applied to a 1555/4444 pixel hue-shifts it (opaque 1555
     * red came out orange). Same-depth copies require the same layout on
     * both ends; 16->32 expands by the source's own layout; anything we
     * can't identify goes back to the runtime's HEL. */
    skind = (sbpp == 2) ? fxnt_surf_pixkind(ppdev, src) : FXNT_PK_RAW;
    dkind = (dbpp == 2) ? fxnt_surf_pixkind(ppdev, dst) : FXNT_PK_RAW;
    if (sbpp == dbpp) {
        if (sbpp == 2 && (skind != dkind || skind == FXNT_PK_BAD)) {
            return DDERR_UNSUPPORTED;
        }
    } else if (sbpp == 2 && dbpp == 4) {
        if (skind != FXNT_PK_565 && skind != FXNT_PK_1555 &&
            skind != FXNT_PK_4444) {
            return DDERR_UNSUPPORTED;
        }
    }

    /* clamp against the src surface too (shrinking the common w/h) */
    if (sl < 0) { dl -= sl; sl = 0; }
    if (st < 0) { dt -= st; st = 0; }
    w = dr - dl;
    h = db - dt;
    clip = (LONG)src->lpGbl->wWidth - sl;
    if (w > clip) w = clip;
    clip = (LONG)src->lpGbl->wHeight - st;
    if (h > clip) h = clip;
    if (w <= 0 || h <= 0) {
        return DD_OK;
    }

    pjSrc = fxnt_surf_kva(ppdev, src, &cjSrcMax);
    if (pjSrc == NULL || sPitch <= 0) {
        return DDERR_UNSUPPORTED;
    }
    need = (ULONG)(st + h - 1) * (ULONG)sPitch + ((ULONG)(sl + w)) * sbpp;
    if (need > cjSrcMax) {
        return DDERR_UNSUPPORTED;
    }

    for (y = 0; y < h; y++) {
        pjD = pjDst + (ULONG)(dt + y) * (ULONG)dPitch + (ULONG)dl * dbpp;
        pjS = pjSrc + (ULONG)(st + y) * (ULONG)sPitch + (ULONG)sl * sbpp;
        if (sbpp == dbpp) {
            memcpy(pjD, pjS, (ULONG)w * dbpp);
        } else if (sbpp == 2 && dbpp == 4) {
            if (skind == FXNT_PK_1555) {
                gbk_conv1555_row_8888((h3u32 *)pjD, (const h3u16 *)pjS, (h3u32)w);
            } else if (skind == FXNT_PK_4444) {
                gbk_conv4444_row_8888((h3u32 *)pjD, (const h3u16 *)pjS, (h3u32)w);
            } else {
                gbk_conv565_row_8888((h3u32 *)pjD, (const h3u16 *)pjS, (h3u32)w);
            }
        } else {
            return DDERR_UNSUPPORTED;    /* 32->16 shrink etc.: not needed    */
        }
    }
    return DD_OK;
}

/* Blt: (1) THE PRESENT - a full-screen SRCCOPY from the app's tagged 3D
 * back/RT surface to the primary runs as gb_swap's hardware 2D blit, which
 * IS the design-5a path (565 plain copy on a 16bpp desktop, the hardware
 * 565->8888 convert on a 32bpp one). (2) everything else that is a plain
 * 1:1 SRCCOPY / COLORFILL / DEPTHFILL between vidmem surfaces is done by
 * the CPU loops above (honoring the runtime's clip list). Anything fancier
 * (stretch, mirror, other ROPs, sysmem ends) -> NOTHANDLED, runtime HEL. */
static DWORD APIENTRY Dd_Blt(PDD_BLTDATA p)
{
    PFXPDEV ppdev = p->lpDD ? (PFXPDEV)p->lpDD->dhpdev : NULL;
    PDD_SURFACE_LOCAL dst = p->lpDDDestSurface;
    PDD_SURFACE_LOCAL src = p->lpDDSrcSurface;
    DD_SURFACE_GLOBAL rtGbl;
    DD_SURFACE_LOCAL  rtLcl;
    gbk_meminfo_t mi;
    HRESULT hr;
    LONG dw, dh;
    int fill = 0;
    ULONG fillVal = 0;

    if (ppdev == NULL || dst == NULL || dst->lpGbl == NULL) {
        return DDHAL_DRIVER_NOTHANDLED;
    }
    dw = p->rDest.right - p->rDest.left;
    dh = p->rDest.bottom - p->rDest.top;

    /* (1) the present. The 3D source is identified by the FXNT_SURFTAG_RT3D
     * OBJECT tag, not by fpVidMem equality (review #5): the runtime rotates
     * the chain's fpVidMem values after each Flip, so the offset test
     * matched the WRONG object once a flip had happened. */
    if (src != NULL && src->lpGbl != NULL &&
        ppdev->gbkAttached && ppdev->rt3dValid &&
        ((ULONG)src->lpGbl->dwReserved1 & FXNT_SURFTAG_RT3D) != 0 &&
        (dst->ddsCaps.dwCaps & DDSCAPS_PRIMARYSURFACE) != 0 &&
        !p->IsClipped &&
        p->rDest.left == 0 && p->rDest.top == 0 &&
        dw == ppdev->cxScreen && dh == ppdev->cyScreen) {
        gb_swap(0);
        p->ddRVal = DD_OK;
        return DDHAL_DRIVER_HANDLED;
    }

    /* (2) CPU paths: classify the request */
    if (p->dwFlags & DDBLT_COLORFILL) {
        fill = 1;
        fillVal = p->bltFX.dwFillColor;
    } else if (p->dwFlags & DDBLT_DEPTHFILL) {
        fill = 1;
        fillVal = p->bltFX.dwFillDepth;
    } else if (p->dwFlags & DDBLT_ROP) {
        if (((p->bltFX.dwROP >> 16) & 0xFF) != 0xCC) {
            return DDHAL_DRIVER_NOTHANDLED;   /* SRCCOPY only               */
        }
        if (src == NULL || src->lpGbl == NULL ||
            (p->rSrc.right - p->rSrc.left) != dw ||
            (p->rSrc.bottom - p->rSrc.top) != dh) {
            return DDHAL_DRIVER_NOTHANDLED;   /* stretch: runtime HEL       */
        }
    } else {
        return DDHAL_DRIVER_NOTHANDLED;
    }

    if (ppdev->gbkAttached) {
        gb_finish();                     /* drain 3D before the CPU touches   */
    }

    /* review #8: a windowed/clipped/partial present FROM the tagged 3D
     * back/RT must copy the RENDERED frame, which lives in gbkernel's 16bpp
     * back buffer - NOT the RT surface's own bits: the design-5a 32bpp proxy
     * is never rendered into (D3D draws 16bpp into gbkernel's buffer; the
     * proxy holds only app-written bytes), and after a Flip the object's
     * rotated fpVidMem no longer names the render output either. Re-point
     * the CPU source at the back buffer via a local 565 surface descriptor;
     * the sbpp==2/dbpp==4 row convert above then IS the 5a 565->8888
     * expansion for the windowed case (on a 16bpp desktop it is a plain 565
     * copy). Purely local structs - nothing runtime-owned is touched. */
    if (!fill && src != NULL && src->lpGbl != NULL &&
        ppdev->gbkAttached && ppdev->rt3dValid &&
        ((ULONG)src->lpGbl->dwReserved1 & FXNT_SURFTAG_RT3D) != 0 &&
        gbkernel_get_meminfo(&mi) == 0) {
        memset(&rtGbl, 0, sizeof(rtGbl));
        memset(&rtLcl, 0, sizeof(rtLcl));
        rtGbl.fpVidMem = (FLATPTR)mi.backOffset;
        rtGbl.lPitch   = (LONG)mi.stride3d;
        rtGbl.wWidth   = mi.width;
        rtGbl.wHeight  = mi.height;
        rtGbl.ddpfSurface.dwSize        = sizeof(DDPIXELFORMAT);
        rtGbl.ddpfSurface.dwFlags       = DDPF_RGB;
        rtGbl.ddpfSurface.dwRGBBitCount = 16;
        rtGbl.ddpfSurface.dwRBitMask    = 0xF800;   /* the back buffer is 565 */
        rtGbl.ddpfSurface.dwGBitMask    = 0x07E0;
        rtGbl.ddpfSurface.dwBBitMask    = 0x001F;
        rtLcl.lpGbl   = &rtGbl;
        rtLcl.dwFlags = DDRAWISURF_HASPIXELFORMAT;
        src = &rtLcl;
    }

    if (p->IsClipped) {
        DWORD i;
        hr = DD_OK;                      /* zero visible rects = draw nothing */
        if (p->prDestRects == NULL) {
            p->ddRVal = hr;
            return DDHAL_DRIVER_HANDLED;
        }
        for (i = 0; i < p->dwRectCnt && hr == DD_OK; i++) {
            LONG cl = p->prDestRects[i].left;
            LONG ct = p->prDestRects[i].top;
            hr = fxnt_blt_rect(ppdev, dst, fill ? NULL : src,
                               cl, ct,
                               p->prDestRects[i].right,
                               p->prDestRects[i].bottom,
                               p->rOrigSrc.left + (cl - p->rOrigDest.left),
                               p->rOrigSrc.top  + (ct - p->rOrigDest.top),
                               fill, fillVal);
        }
    } else {
        hr = fxnt_blt_rect(ppdev, dst, fill ? NULL : src,
                           p->rDest.left, p->rDest.top,
                           p->rDest.right, p->rDest.bottom,
                           p->rSrc.left, p->rSrc.top,
                           fill, fillVal);
    }

    if (hr == DDERR_UNSUPPORTED) {
        return DDHAL_DRIVER_NOTHANDLED;  /* let the runtime HEL take it       */
    }
    p->ddRVal = hr;
    return DDHAL_DRIVER_HANDLED;
}

/* Enable the DirectDraw DDI: fill the callback tables the runtime hands us. */
BOOL APIENTRY DrvEnableDirectDraw(
        DHPDEV dhpdev,
        DD_CALLBACKS        *pCallBacks,
        DD_SURFACECALLBACKS *pSurfaceCallBacks,
        DD_PALETTECALLBACKS *pPaletteCallBacks)
{
    PFXPDEV ppdev = (PFXPDEV)dhpdev;
    gbk_meminfo_t mi;

    /* Seed the offscreen-surface allocator from gbkernel's carve-out now
     * (Dd_CreateSurface also does this lazily; failure just means 2D-only). */
    if (ppdev != NULL && gbkernel_get_meminfo(&mi) == 0) {
        (void)fxnt_ensure_ddheap(ppdev, &mi);
    }

    memset(pCallBacks, 0, sizeof(*pCallBacks));
    pCallBacks->dwSize               = sizeof(*pCallBacks);
    pCallBacks->CreateSurface        = Dd_CreateSurface;
    pCallBacks->CanCreateSurface     = Dd_CanCreateSurface;
    pCallBacks->MapMemory            = Dd_MapMemory;
    pCallBacks->WaitForVerticalBlank = Dd_WaitForVerticalBlank;
    pCallBacks->dwFlags = DDHAL_CB32_CREATESURFACE | DDHAL_CB32_CANCREATESURFACE |
                          DDHAL_CB32_MAPMEMORY | DDHAL_CB32_WAITFORVERTICALBLANK;

    memset(pSurfaceCallBacks, 0, sizeof(*pSurfaceCallBacks));
    pSurfaceCallBacks->dwSize          = sizeof(*pSurfaceCallBacks);
    pSurfaceCallBacks->DestroySurface  = Dd_DestroySurface;
    pSurfaceCallBacks->Lock            = Dd_Lock;
    pSurfaceCallBacks->Unlock          = Dd_Unlock;
    pSurfaceCallBacks->Flip            = Dd_Flip;
    pSurfaceCallBacks->Blt             = Dd_Blt;
    pSurfaceCallBacks->dwFlags = DDHAL_SURFCB32_DESTROYSURFACE | DDHAL_SURFCB32_LOCK |
                                 DDHAL_SURFCB32_UNLOCK | DDHAL_SURFCB32_FLIP |
                                 DDHAL_SURFCB32_BLT;

    /* No hardware palette work yet (hi/true-colour only). */
    memset(pPaletteCallBacks, 0, sizeof(*pPaletteCallBacks));
    pPaletteCallBacks->dwSize = sizeof(*pPaletteCallBacks);

    return TRUE;
}

VOID APIENTRY DrvDisableDirectDraw(DHPDEV dhpdev)
{
    /* Every DDraw surface dies with the DirectDraw object: reset the bump
     * allocator + present tag so a re-enable starts from a clean region. */
    PFXPDEV ppdev = (PFXPDEV)dhpdev;
    if (ppdev != NULL) {
        ppdev->ddHeapLive  = 0;
        ppdev->rt3dValid   = 0;
        ppdev->ddExclusive = 0;
    }
}

/* The D3D callback table the runtime binds when it queries GUID_D3DCallbacks3.
 * DrawPrimitives2 is fxD3D's render entry point. dwFlags carries the
 * D3DHAL3_CB32_* VALIDITY bits (M4c-2 review #1): the runtime honors a member
 * ONLY when its bit is set - with dwFlags 0 the DrawPrimitives2 pointer was
 * treated as absent and no DP2 batch ever reached the driver. Set EXACTLY the
 * bits for members we provide (the vintage Voodoo3 driver does the same:
 * DRAWPRIMITIVES2 set, CLEAR2 omitted for its NULL Clear2); Clear2 and
 * ValidateTextureStageState are NULL here, so their bits stay clear. */
static const D3DHAL_CALLBACKS3 g_d3dCallbacks3 =
{
    sizeof(D3DHAL_CALLBACKS3),   /* dwSize                    */
    D3DHAL3_CB32_DRAWPRIMITIVES2,/* dwFlags: DP2 is valid     */
    (LPD3DHAL_CLEAR2CB)0,        /* Clear2 (TODO fxd3d M4)    */
    (LPVOID)0,                   /* lpvReserved               */
    (LPD3DHAL_VALIDATETEXTURESTAGESTATECB)0, /* ValidateTextureStageState */
    (LPD3DHAL_DRAWPRIMITIVES2CB)d3d_DrawPrimitives2          /* DrawPrimitives2 */
};

/* GUID_D3DCallbacks3 (public value, ddrawint.h). Held locally to avoid pulling
 * a dxguid import into this -nodefaultlib driver link. */
static const GUID k_GUID_D3DCallbacks3 =
{ 0xddf41230, 0xec0a, 0x11d0, { 0xa9, 0xb6, 0x00, 0xaa, 0x00, 0xc0, 0x99, 0x3e } };

/* GUID_D3DParseUnknownCommandCallback (public value, ddrawi.h). This query is
 * REVERSED: the runtime pushes its D3DParseUnknownCommand pointer INTO the
 * driver via lpvData around init time; we stash it and answer DD_OK. */
static const GUID k_GUID_D3DParseUnknownCommand =
{ 0x2e04ffa0, 0x98e4, 0x11d1, { 0x8c, 0xe1, 0x00, 0xa0, 0xc9, 0x06, 0x29, 0xa8 } };

/* GUID_NTCallbacks (public value, ddrawint.h): the NT-only driver callbacks -
 * SetExclusiveMode + FlipToGDISurface (FreeDriverMemory not needed: the bump
 * allocator cannot free mid-stream, so we never ask the runtime to retry). */
static const GUID k_GUID_NTCallbacks =
{ 0x6fe9ecde, 0xdf89, 0x11d1, { 0x9d, 0xb0, 0x00, 0x60, 0x08, 0x27, 0x71, 0xba } };

/* g_pfnParseUnknown (declared up top with its fxnt_parse_unknown adapter) is
 * populated here by the GUID_D3DParseUnknownCommand query, and consumed by
 * d3d_DrawPrimitives2 on an unhandled opcode - unknown commands are skipped
 * in-driver via the runtime callback instead of aborting the batch. */

/* GetDriverInfo - the NT5/DX7 extensibility query. The runtime asks for a GUID
 * and we hand back the matching callback table. We answer GUID_D3DCallbacks3
 * with our D3D callbacks (DrawPrimitives2). Unknown GUIDs -> DDERR_CURRENTLYNOTAVAIL. */
DWORD APIENTRY DdGetDriverInfo(PDD_GETDRIVERINFODATA pData)
{
    DWORD cjCopy;

    pData->dwActualSize = 0;
    pData->ddRVal       = DDERR_CURRENTLYNOTAVAIL;

    if (memcmp(&pData->guidInfo, &k_GUID_D3DCallbacks3, sizeof(GUID)) == 0) {
        pData->dwActualSize = sizeof(g_d3dCallbacks3);
        cjCopy = pData->dwExpectedSize;
        if (cjCopy > sizeof(g_d3dCallbacks3)) {
            cjCopy = sizeof(g_d3dCallbacks3);
        }
        if (pData->lpvData != NULL && cjCopy != 0) {
            memcpy(pData->lpvData, &g_d3dCallbacks3, cjCopy);
        }
        pData->ddRVal = DD_OK;
    }
    else if (memcmp(&pData->guidInfo, &k_GUID_D3DParseUnknownCommand, sizeof(GUID)) == 0) {
        /* reversed data direction: lpvData carries the callback pointer in */
        g_pfnParseUnknown = (PFND3DPARSEUNKNOWNCOMMAND)pData->lpvData;
        (void)g_pfnParseUnknown;
        pData->dwActualSize = pData->dwExpectedSize;
        pData->ddRVal = DD_OK;
    }
    else if (memcmp(&pData->guidInfo, &k_GUID_NTCallbacks, sizeof(GUID)) == 0) {
        DD_NTCALLBACKS nt;
        memset(&nt, 0, sizeof(nt));
        nt.dwSize  = sizeof(nt);
        nt.dwFlags = DDHAL_NTCB32_SETEXCLUSIVEMODE |
                     DDHAL_NTCB32_FLIPTOGDISURFACE;
        nt.FreeDriverMemory = NULL;
        nt.SetExclusiveMode = Dd_SetExclusiveMode;
        nt.FlipToGDISurface = Dd_FlipToGDISurface;
        pData->dwActualSize = sizeof(nt);
        cjCopy = pData->dwExpectedSize;
        if (cjCopy > sizeof(nt)) {
            cjCopy = sizeof(nt);
        }
        if (pData->lpvData != NULL && cjCopy != 0) {
            memcpy(pData->lpvData, &nt, cjCopy);
        }
        pData->ddRVal = DD_OK;
    }
    /* TODO(fxd3d M4): also answer GUID_D3DExtendedCaps / GUID_ZPixelFormats. */

    return DDHAL_DRIVER_HANDLED;
}

#else  /* !HAVE_DDK - documentation stub so the tree reviews on any host */

/* Built without the DDK: the real entry points above compile only in the NT
 * DDK environment. This stub records the contract for reviewers and lets the
 * portable glue (../ddi_glue.c) still link into the host unit test. */
int fxnt_host_driver_is_ddk_only = 1;

#endif /* HAVE_DDK */
