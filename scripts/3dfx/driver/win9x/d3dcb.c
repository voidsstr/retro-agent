/*
 * d3dcb.c - Win98/ME Direct3D callback registration for fxD3D (skeleton).
 *
 * On 9x, Direct3D is layered on DirectDraw and runs in the GAME's process (user
 * mode) - which is why 9x is the easier first target: no kernel debugging. The
 * display driver publishes its D3D HAL by answering DirectDraw's GetDriverInfo
 * for GUID_D3DCallbacks3; the returned table's DrawPrimitives2 member is our
 * render path.
 *
 * The 2D + modeset + DirectDraw surfaces come from the host minivdd - we adopt
 * the vmdisp9x-shaped 2D/DDraw driver (Open Watcom, modern host) and set its
 * hardware layer to 3dfx via Glide's minihwc init. THIS file adds only the D3D
 * hookup on top.
 *
 * REQUIRES the 98 DDK for the real D3DHAL types (behind HAVE_DDK). The portable
 * reduction is ../ddi_glue.c. Clean-room: public DDHAL/D3D contract only.
 */
#include "../ddi_glue.h"

#ifdef HAVE_DDK
#include <windows.h>
#include <ddrawi.h>
#include <d3dhal.h>

/* DrawPrimitives2 - same DP2 model as NT; unpack the command buffer and run. */
static DWORD __stdcall d3d_DrawPrimitives2(LPD3DHAL_DRAWPRIMITIVES2DATA p)
{
    fxd_context *c = (fxd_context*)(ULONG_PTR)p->dwhContext;
    BYTE *base = (BYTE*)(ULONG_PTR)p->lpDDCommands;
    fxdglue_draw_primitives2(c, base + p->dwCommandOffset, (int)p->dwCommandLength);
    p->ddrval = DD_OK;
    return DDHAL_DRIVER_HANDLED;
}

static DWORD __stdcall d3d_ContextCreate(LPD3DHAL_CONTEXTCREATEDATA p)
{
    fxd_context *c = fxdglue_context_create(640,480,16);
    p->dwhContext = (ULONG_PTR)c;
    p->ddrval = c ? DD_OK : DDERR_OUTOFMEMORY;
    return DDHAL_DRIVER_HANDLED;
}
static DWORD __stdcall d3d_ContextDestroy(LPD3DHAL_CONTEXTDESTROYDATA p)
{
    fxdglue_context_destroy((fxd_context*)(ULONG_PTR)p->dwhContext);
    p->ddrval = DD_OK;
    return DDHAL_DRIVER_HANDLED;
}

/* GetDriverInfo hook: when the runtime asks for GUID_D3DCallbacks3, hand back
 * our table. The host DDHAL driver chains to this from its GetDriverInfo32. */
BOOL __stdcall fx9x_GetDriverInfo(LPDDHAL_GETDRIVERINFODATA p)
{
    static D3DHAL_CALLBACKS3   cb3;
    static D3DHAL_GLOBALDRIVERDATA gdd;

    if(IsEqualIID(&p->guidInfo, &GUID_D3DCallbacks3)){
        memset(&cb3,0,sizeof(cb3)); cb3.dwSize=sizeof(cb3);
        cb3.DrawPrimitives2 = d3d_DrawPrimitives2;
        memcpy(p->lpvData, &cb3, min(p->dwExpectedSize, sizeof(cb3)));
        p->ddRVal = DD_OK; return DDHAL_DRIVER_HANDLED;
    }
    if(IsEqualIID(&p->guidInfo, &GUID_D3DExtendedCaps) ||
       IsEqualIID(&p->guidInfo, &GUID_D3DCaps)){
        memset(&gdd,0,sizeof(gdd)); gdd.dwSize=sizeof(gdd);
        fxdglue_get_caps(&gdd.hwCaps);
        memcpy(p->lpvData, &gdd, min(p->dwExpectedSize, sizeof(gdd)));
        p->ddRVal = DD_OK; return DDHAL_DRIVER_HANDLED;
    }
    (void)d3d_ContextCreate; (void)d3d_ContextDestroy;
    return DDHAL_DRIVER_NOTHANDLED;  /* let the host driver handle the rest    */
}

#else
int fx9x_host_driver_is_ddk_only = 1;
#endif
