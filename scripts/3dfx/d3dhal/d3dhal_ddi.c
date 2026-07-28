/*
 * d3dhal_ddi.c - the DrawPrimitives2 command-buffer dispatcher + caps.
 *
 * The Direct3D runtime hands the driver a DP2 command buffer: a stream of
 * {opcode,count} headers each followed by operands (render-state pairs, TSS
 * pairs, or vertex runs). fxd_dp2_execute walks it and routes each command to
 * d3dhal_state / d3dhal_prim. This is the driver's single hot entry point.
 *
 * Without the DDK we parse the documented DP2 shape (see fxd3d.h mirrors) so
 * the dispatcher is unit-testable against synthetic buffers on the build host;
 * with -DHAVE_DDK the real D3DHAL_DP2COMMAND layout is used.
 *
 * Clean-room: implements the public DP2 contract from the Microsoft DDK.
 */
#include "fxd3d.h"
#include <string.h>

#ifdef HAVE_DDK

/* One D3DPRIMCAPS (line and triangle caps are identical - both run the same
 * PKT3 setup path). Only what the executor + gbkernel actually honor
 * (d3dhal_state.c cmp2gr/blend2gr maps, gb_set_* seam); the bit pattern
 * follows the hardware-verified vintage Voodoo3 driver's D3DDEVICEDESC fill
 * for the same hardware class, trimmed of what this HAL does not implement
 * (no w-buffer, no z-bias, no specular, no mipmap filters yet). */
static void fxd_fill_primcaps(D3DPRIMCAPS *p){
    memset(p, 0, sizeof(*p));
    p->dwSize     = sizeof(D3DPRIMCAPS);
    p->dwMiscCaps = D3DPMISCCAPS_MASKZ
                  | D3DPMISCCAPS_CULLNONE
                  | D3DPMISCCAPS_CULLCW
                  | D3DPMISCCAPS_CULLCCW;
    p->dwRasterCaps = D3DPRASTERCAPS_DITHER
                    | D3DPRASTERCAPS_SUBPIXEL
                    | D3DPRASTERCAPS_FOGVERTEX
                    | D3DPRASTERCAPS_FOGTABLE;
    p->dwZCmpCaps = D3DPCMPCAPS_NEVER        | D3DPCMPCAPS_LESS
                  | D3DPCMPCAPS_EQUAL        | D3DPCMPCAPS_LESSEQUAL
                  | D3DPCMPCAPS_GREATER      | D3DPCMPCAPS_NOTEQUAL
                  | D3DPCMPCAPS_GREATEREQUAL | D3DPCMPCAPS_ALWAYS;
    p->dwSrcBlendCaps = D3DPBLENDCAPS_ZERO
                      | D3DPBLENDCAPS_ONE
                      | D3DPBLENDCAPS_SRCALPHA
                      | D3DPBLENDCAPS_INVSRCALPHA;
    p->dwDestBlendCaps = D3DPBLENDCAPS_ZERO
                       | D3DPBLENDCAPS_ONE
                       | D3DPBLENDCAPS_SRCCOLOR
                       | D3DPBLENDCAPS_INVSRCCOLOR
                       | D3DPBLENDCAPS_SRCALPHA
                       | D3DPBLENDCAPS_INVSRCALPHA;
    p->dwAlphaCmpCaps = p->dwZCmpCaps;
    p->dwShadeCaps = D3DPSHADECAPS_COLORFLATRGB
                   | D3DPSHADECAPS_COLORGOURAUDRGB
                   | D3DPSHADECAPS_ALPHAFLATBLEND
                   | D3DPSHADECAPS_ALPHAGOURAUDBLEND
                   | D3DPSHADECAPS_FOGFLAT
                   | D3DPSHADECAPS_FOGGOURAUD;
    p->dwTextureCaps = D3DPTEXTURECAPS_PERSPECTIVE
                     | D3DPTEXTURECAPS_POW2
                     | D3DPTEXTURECAPS_ALPHA
                     | D3DPTEXTURECAPS_TRANSPARENCY;
    p->dwTextureFilterCaps = D3DPTFILTERCAPS_NEAREST
                           | D3DPTFILTERCAPS_LINEAR
                           | D3DPTFILTERCAPS_MINFPOINT
                           | D3DPTFILTERCAPS_MINFLINEAR
                           | D3DPTFILTERCAPS_MAGFPOINT
                           | D3DPTFILTERCAPS_MAGFLINEAR;
    p->dwTextureBlendCaps = D3DPTBLENDCAPS_DECAL
                          | D3DPTBLENDCAPS_MODULATE
                          | D3DPTBLENDCAPS_MODULATEALPHA
                          | D3DPTBLENDCAPS_COPY;
    p->dwTextureAddressCaps = D3DPTADDRESSCAPS_WRAP
                            | D3DPTADDRESSCAPS_CLAMP
                            | D3DPTADDRESSCAPS_INDEPENDENTUV;
}

int fxd_fill_caps(void *desc){
    /* `desc` is the D3DDEVICEDESC_V1 published as
     * D3DHAL_GLOBALDRIVERDATA.hwCaps - the ONLY device description DX5+
     * runtimes see ("stitches a D3DDEVICEDESC together using the
     * D3DDEVICEDESC_V1 embedded in the GLOBALDRIVERDATA", d3dhal.h). It was
     * a no-op stub (M4c-2 review #2): dwSize 0, no D3DDD_* validity bits, no
     * color model, dwDevCaps 0, empty tri caps - the runtime then enumerated
     * no usable HAL device at all. Fill the real thing: the Voodoo3-via-DP2
     * software-transform device. Depths are the core's native truth (16bpp
     * render, 16-bit Z ONLY - review #9: Dd_CanCreateSurface rejects any
     * other Z); the NT chassis ORs in the design-5a DDBD_32 render bit on
     * top, where the 32bpp RT-proxy present path actually lives. */
    D3DDEVICEDESC_V1 *d = (D3DDEVICEDESC_V1 *)desc;

    memset(d, 0, sizeof(*d));
    d->dwSize  = sizeof(D3DDEVICEDESC_V1);
    d->dwFlags = D3DDD_COLORMODEL
               | D3DDD_DEVCAPS
               | D3DDD_DEVICERENDERBITDEPTH
               | D3DDD_DEVICEZBUFFERBITDEPTH
               | D3DDD_LINECAPS
               | D3DDD_TRICAPS;
    d->dcmColorModel = D3DCOLOR_RGB;
    d->dwDevCaps = D3DDEVCAPS_FLOATTLVERTEX
                 | D3DDEVCAPS_EXECUTESYSTEMMEMORY
                 | D3DDEVCAPS_DRAWPRIMTLVERTEX
                 | D3DDEVCAPS_CANRENDERAFTERFLIP
                 | D3DDEVCAPS_TEXTUREVIDEOMEMORY
                 | D3DDEVCAPS_DRAWPRIMITIVES2
                 | D3DDEVCAPS_DRAWPRIMITIVES2EX;
    d->dtcTransformCaps.dwSize = sizeof(D3DTRANSFORMCAPS);
    d->dtcTransformCaps.dwCaps = 0;              /* runtime transforms       */
    d->bClipping = 0;                            /* runtime clips            */
    d->dlcLightingCaps.dwSize = sizeof(D3DLIGHTINGCAPS);
    d->dlcLightingCaps.dwCaps = 0;               /* runtime lights           */
    fxd_fill_primcaps(&d->dpcLineCaps);
    fxd_fill_primcaps(&d->dpcTriCaps);
    d->dwDeviceRenderBitDepth  = DDBD_16;
    d->dwDeviceZBufferBitDepth = DDBD_16;
    d->dwMaxBufferSize  = 0;
    d->dwMaxVertexCount = 0;
    return 1; /* one device (the hardware) */
}

#else  /* !HAVE_DDK */

int fxd_fill_caps(void *desc){
    /* Host build: no DDK caps structures exist. The capability decisions the
     * DDK fill above encodes: DX6/7 fixed-function DP2 sw-transform device,
     * RGB color model, gouraud, 16-bit Z (full compare set), alpha blend/
     * test, chroma key, vertex+table fog, bilinear, pow2 textures 565/1555/
     * 4444, 16bpp render (the chassis adds the 5a 32bpp proxy). No T&L
     * (runtime does it), no shaders. */
    (void)desc;
    return 1; /* one device (the hardware) */
}

#endif /* HAVE_DDK */

/* Read a little-endian DWORD (DP2 buffers are packed). */
static DWORD rd32(const unsigned char *p){
    return (DWORD)p[0]|((DWORD)p[1]<<8)|((DWORD)p[2]<<16)|((DWORD)p[3]<<24);
}

int fxd_dp2_execute(fxd_device *dev, const void *cmdbuf, int len){
    const unsigned char *p = (const unsigned char*)cmdbuf;
    const unsigned char *end = p + len;
    int ops = 0;

    while(p + (int)sizeof(fxd2_hdr) <= end){
        fxd2_hdr h;
        memcpy(&h, p, sizeof(h));
        p += sizeof(h);
        ops++;

        switch(h.bCommand){
            case FXD2_RENDERSTATE: {
                DWORD i;
                for(i=0;i<h.dwCount && p+8<=end;i++,p+=8)
                    fxd_set_renderstate(dev, rd32(p), rd32(p+4));
                break;
            }
            case FXD2_TEXTURESTAGESTATE: {
                DWORD i;
                /* operand triple: stage, state, value */
                for(i=0;i<h.dwCount && p+12<=end;i++,p+=12)
                    fxd_set_tss(dev, rd32(p), rd32(p+4), rd32(p+8));
                break;
            }
            case FXD2_POINTS: case FXD2_LINELIST:
            case FXD2_TRIANGLELIST: case FXD2_TRIANGLESTRIP: case FXD2_TRIANGLEFAN: {
                int n = (int)h.dwCount;
                const fxd_tlvertex *v = (const fxd_tlvertex*)p;
                if(p + n*(int)sizeof(fxd_tlvertex) <= end){
                    fxd_draw(dev, h.bCommand, v, n);
                    p += n*sizeof(fxd_tlvertex);
                }
                break;
            }
            case FXD2_SETRENDERTARGET: case FXD2_TEXBLT:
                /* operands consumed by count; handled in the host DDraw layer */
                p += h.dwCount * 4;
                break;
            default:
                /* unknown opcode: stop to avoid desync (runtime re-batches)   */
                return ops;
        }
    }
    return ops;
}
