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

int fxd_fill_caps(void *desc){
    /* When HAVE_DDK, `desc` is a D3DDEVICEDESC to populate. Here we record the
     * capability decisions the VSA-100 supports; the DDK build copies these
     * into the real struct fields. Advertised: DX6/7 fixed-function, 1-2 TMU,
     * gouraud, z/w-buffer, per-pixel fog, alpha blend/test, chroma key,
     * bilinear + mipmap, texture formats 565/1555/4444/P8. No T&L (runtime
     * does it), no shaders. */
    (void)desc;
    return 1; /* one device (the hardware) */
}

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
