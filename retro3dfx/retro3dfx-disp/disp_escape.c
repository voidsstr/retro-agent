/*
 * disp_escape.c - retro3dfx-disp: the HWCEXT escape SERVER.
 *
 * This is the core of "even more code we own": a Windows display driver that
 * answers the exact ExtEscape protocol retro3dfx-glide sends, so our (unmodified)
 * Glide detects and maps the Voodoo cooperatively - no direct PCI grab, no fight
 * with the Windows display driver (this IS the display driver).
 *
 * When the runtime forwards ExtEscape(hdc, 0x3df3, ...) to us, GDI calls
 * DrvEscape below. We dispatch on the HWCEXT_* opcode and fill the response.
 *
 * The hard hardware bits (BAR mapping, mode-set) live in the driver's device
 * bring-up (disp_hw.c, modeled on the open Device3Dfx logic + RISCyVoodoo/
 * vmdisp9x chassis). This file is the protocol layer and is deliberately
 * DDK-independent in structure so the dispatch logic is reviewable/testable;
 * the DrvEscape entry + PDEV access are behind HAVE_DDK.
 */
#include "retro3dfx_hwcext.h"
#include <string.h>

/* Per-adapter state the chassis fills at StartDevice (disp_hw.c). */
typedef struct {
    R3DFX_U32 vendorID, deviceID, chipRev, fbRam;
    R3DFX_U32 pciStride, hwStride, tileMark;
    /* user-visible linear addresses of the card's regions, set when we map the
     * BARs into the calling process (GETLINEARADDR): [0]=registers, [1]=fb */
    R3DFX_U32 linear[R3DFX_HWCEXT_MAX_BASEADDR];
    R3DFX_U32 numLinear;
    R3DFX_U32 nextContext;
    int       exclusive;
} r3dfx_adapter;

/* Provided by disp_hw.c: map this adapter's PCI BARs into the process `pHandle`
 * and fill adapter->linear[]/numLinear. Returns 1 on success. (Real body needs
 * the DDK: EngMapView/MmMapLockedPagesSpecifyCache of the BAR ranges.) */
int  r3dfx_hw_map_linear(r3dfx_adapter *a, R3DFX_U32 pHandle);
void r3dfx_hw_set_exclusive(r3dfx_adapter *a, int on);

/* ---- the escape dispatcher (pure logic; unit-testable) ------------------- */
/* in  = the request buffer (r3dfx_req_hdr + opcode-specific payload)
 * out = the response buffer; *out_status set to R3DFX_STATUS_OK/FAIL.
 * returns bytes written to out (0 if unhandled). */
int r3dfx_escape_dispatch(r3dfx_adapter *a,
                          const void *in, int in_len,
                          void *out, int out_len,
                          R3DFX_U32 *out_status)
{
    const r3dfx_req_hdr *req = (const r3dfx_req_hdr *)in;
    *out_status = R3DFX_STATUS_FAIL;
    if (!a || !req || in_len < (int)sizeof(R3DFX_U32)) return 0;

    switch (req->which) {
    case R3DFX_HWCEXT_GETDRIVERVERSION: {
        r3dfx_res_driverversion *r = (r3dfx_res_driverversion *)out;
        if (out_len < (int)sizeof(*r)) return 0;
        r->major = 1; r->minor = 0;
        *out_status = R3DFX_STATUS_OK;
        return sizeof(*r);
    }
    case R3DFX_HWCEXT_GETDEVICECONFIG: {
        /* "are you a 3dfx Glide device?" - the probe that the 2001 driver
         * ignores. We answer YES with the real card identity. */
        r3dfx_res_deviceconfig *r = (r3dfx_res_deviceconfig *)out;
        if (out_len < (int)sizeof(*r)) return 0;
        if (a->vendorID != R3DFX_PCI_VENDOR) return 0;  /* not our card */
        memset(r, 0, sizeof(*r));
        r->devNum   = req->devNo;
        r->vendorID = a->vendorID;
        r->deviceID = a->deviceID;
        r->fbRam    = a->fbRam;
        r->chipRev  = a->chipRev;
        r->pciStride= a->pciStride;
        r->hwStride = a->hwStride;
        r->tileMark = a->tileMark;
        *out_status = R3DFX_STATUS_OK;
        return sizeof(*r);
    }
    case R3DFX_HWCEXT_GETLINEARADDR: {
        /* map the card's BARs into the caller and hand back the addresses -
         * this is what lets Glide reach the registers + framebuffer. */
        const r3dfx_req_linearaddr *q = (const r3dfx_req_linearaddr *)in;
        r3dfx_res_linearaddr *r = (r3dfx_res_linearaddr *)out;
        unsigned i;
        if (in_len < (int)sizeof(*q) || out_len < (int)sizeof(*r)) return 0;
        if (!r3dfx_hw_map_linear(a, q->pHandle)) return 0;
        r->numBaseAddrs = a->numLinear;
        for (i = 0; i < R3DFX_HWCEXT_MAX_BASEADDR; i++)
            r->baseAddresses[i] = (i < a->numLinear) ? a->linear[i] : 0;
        *out_status = R3DFX_STATUS_OK;
        return sizeof(*r);
    }
    case R3DFX_HWCEXT_ALLOCCONTEXT: {
        r3dfx_res_alloccontext *r = (r3dfx_res_alloccontext *)out;
        if (out_len < (int)sizeof(*r)) return 0;
        r->contextID = ++a->nextContext;
        *out_status = R3DFX_STATUS_OK;
        return sizeof(*r);
    }
    case R3DFX_HWCEXT_HWCSETEXCLUSIVE:
        r3dfx_hw_set_exclusive(a, 1); a->exclusive = 1;
        *out_status = R3DFX_STATUS_OK; return 0;
    case R3DFX_HWCEXT_HWCRLSEXCLUSIVE:
    case R3DFX_HWCEXT_RESTORE_DESKTOP:
        r3dfx_hw_set_exclusive(a, 0); a->exclusive = 0;
        *out_status = R3DFX_STATUS_OK; return 0;
    case R3DFX_HWCEXT_RELEASECONTEXT:
    case R3DFX_HWCEXT_QUERYCONTEXT:
        *out_status = R3DFX_STATUS_OK; return 0;
    default:
        /* FIFO alloc/execute, AGP info, gamma, overlay: add as Glide exercises
         * them. Unhandled -> status FAIL, Glide falls back / errors clearly. */
        return 0;
    }
}

#ifdef HAVE_DDK
#include <windows.h>
#include <winddi.h>

/* GDI entry. `pso` -> our PDEV -> the r3dfx_adapter. iEsc is the escape code
 * the runtime forwarded (0x3df3 etc.). */
ULONG APIENTRY DrvEscape(SURFOBJ *pso, ULONG iEsc, ULONG cjIn, PVOID pvIn,
                         ULONG cjOut, PVOID pvOut)
{
    r3dfx_adapter *a;
    R3DFX_U32 status = R3DFX_STATUS_FAIL;
    if (iEsc != R3DFX_EXT_HWC && iEsc != R3DFX_EXT_HWC_OLD &&
        iEsc != R3DFX_EXT_HWC_WXP)
        return 0;                     /* not ours; GDI tries the next handler */
    a = (r3dfx_adapter *)(((PDEV_context)pso->dhpdev)->adapter);
    r3dfx_escape_dispatch(a, pvIn, (int)cjIn, pvOut, (int)cjOut, &status);
    return status;                    /* Glide reads this as resStatus        */
}
#endif /* HAVE_DDK */
