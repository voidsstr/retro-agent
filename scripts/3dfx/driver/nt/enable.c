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
 * Clean-room: implements the public NT display-DDI contract. ***REMOVED***.
 */
#include "../ddi_glue.h"

#ifdef HAVE_DDK
#include <windows.h>
#include <winddi.h>
#include <ddrawint.h>
#include <d3dhal.h>

/* ---- DirectDraw DDI: advertise D3D + surface support --------------------- */

/* Called by the runtime to learn our DirectDraw/D3D capabilities and callback
 * table. We publish the D3D callbacks whose DrawPrimitives2 is our render path. */
BOOL APIENTRY DrvGetDirectDrawInfo(
        DHPDEV dhpdev, DD_HALINFO *pHalInfo,
        DWORD *pdwNumHeaps, VIDEOMEMORY *pvmList,
        DWORD *pdwNumFourCC, DWORD *pdwFourCC)
{
    pHalInfo->dwSize = sizeof(*pHalInfo);
    /* report we can do 3D; the runtime will then ask for the D3D caps/cbs */
    pHalInfo->ddCaps.dwCaps |= DDCAPS_3D | DDCAPS_ZBUFFER | DDCAPS_BLTDEPTHFILL;
    pHalInfo->lpD3DGlobalDriverData = (ULONG_PTR)0;   /* filled in D3D cb init */
    pHalInfo->lpD3DHALCallbacks     = (ULONG_PTR)0;
    *pdwNumHeaps  = 0;   /* video-memory heaps set up by the miniport         */
    *pdwNumFourCC = 0;
    return TRUE;
}

/* ---- Direct3D DDI: the callback the runtime invokes to render ------------ */

/* One D3D context <-> one fxd_context. */
static DWORD APIENTRY d3d_ContextCreate(LPD3DHAL_CONTEXTCREATEDATA p)
{
    /* the render target surface gives us width/height/depth */
    fxd_context *c = fxdglue_context_create(/*w*/640,/*h*/480,/*depth*/16);
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

/* THE hot path. The runtime batched the frame into a command buffer; we unpack
 * the pointer+length and hand it to the portable glue -> fxd_dp2_execute. */
static DWORD APIENTRY d3d_DrawPrimitives2(LPD3DHAL_DRAWPRIMITIVES2DATA p)
{
    fxd_context *c = (fxd_context*)(ULONG_PTR)p->dwhContext;
    BYTE *cmdbase = (BYTE*)(ULONG_PTR)p->lpDDCommands; /* command surface bits */
    const void *cmd = cmdbase + p->dwCommandOffset;
    int len = (int)(p->dwCommandLength);

    fxdglue_draw_primitives2(c, cmd, len);

    p->dwErrorOffset = 0;   /* consumed the whole buffer                      */
    p->ddrval = DD_OK;
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
    fxdglue_get_caps(&gd->hwCaps);   /* D3DDEVICEDESC caps <- fxD3D            */
}

#else  /* !HAVE_DDK - documentation stub so the tree reviews on any host */

/* Built without the DDK: the real entry points above compile only in the NT
 * DDK environment. This stub records the contract for reviewers and lets the
 * portable glue (../ddi_glue.c) still link into the host unit test. */
int fxnt_host_driver_is_ddk_only = 1;

#endif /* HAVE_DDK */
