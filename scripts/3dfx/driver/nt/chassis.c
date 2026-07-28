/*
 * chassis.c - clean-room NT/2000/XP GDI_DRIVER "chassis" for fxD3D.
 *
 * This is the 2D / PDEV / modeset half of the display driver DLL (fxd3ddd.dll).
 * It is an UNACCELERATED framebuffer display driver: it hooks NOTHING for
 * drawing, so GDI renders directly into the linear framebuffer we map from the
 * paired video miniport. Correctness of the DDK contract is the goal here;
 * hardware 2D acceleration is a later milestone. The DirectDraw / Direct3D DDI
 * (where fxD3D actually renders) lives in enable.c and is published through the
 * DRVFN table below.
 *
 * Clean-room: this implements ONLY the public Microsoft NT display-driver DDI
 * contract (winddi.h) following the standard DDK "framebuf" unaccelerated
 * display-driver pattern, plus the public video-miniport IOCTL contract
 * (ntddvdeo.h). No proprietary 3dfx driver source was consulted.
 *
 * REQUIRES THE DDK to build (winddi.h, ntddvdeo.h, ddrawint.h, d3dhal.h).
 * DDK code is bracketed by HAVE_DDK so the tree stays reviewable on Linux.
 */
#include "../ddi_glue.h"

#ifdef HAVE_DDK

#include <windows.h>
#include <winddi.h>
#include <devioctl.h>      /* CTL_CODE, FILE_DEVICE_VIDEO, METHOD_BUFFERED (IOCTL macros) */
#include <ntddvdeo.h>      /* IOCTL_VIDEO_*, VIDEO_MODE_INFORMATION, VIDEO_MEMORY_INFORMATION */
#include <ddrawint.h>
#include <d3dhal.h>

#include "gbkernel.h"      /* gbkernel_attach/detach - the kernel Glide backend */
#include "fxpdev.h"        /* FXPDEV - the per-PDEV state shared with enable.c  */

/* DrvEscape lives in gbkdebug.c (the on-card bring-up ladder); referenced by
 * the DRVFN table below. */
ULONG APIENTRY DrvEscape(SURFOBJ *pso, ULONG iEsc, ULONG cjIn, PVOID pvIn,
                         ULONG cjOut, PVOID pvOut);

/* Allocation tag for our PDEV blocks ('DxF3' little-endian -> "3FxD"). */
#define FXD3D_ALLOC_TAG   'DxF3'

/* Depth of the bring-up aux/z buffer we request from gbkernel: 16-bit (the
 * Voodoo pipeline is natively 16bpp; DDBD_16 Z is advertised in enable.c). */
#define FXD3D_ZDEPTH_BYTES   2

/* ------------------------------------------------------------------------- *
 *  Small helpers.
 * ------------------------------------------------------------------------- */

/* Fill in the per-bpp colour masks / GDI formats from a bit depth. */
static void fxchassis_format_from_bpp(PFXPDEV ppdev, ULONG bpp)
{
    ppdev->ulBitsPerPel = bpp;
    if (bpp == 32) {
        ppdev->ulBitmapType  = BMF_32BPP;
        ppdev->iDitherFormat = BMF_32BPP;
        ppdev->flRed   = 0x00FF0000;   /* 8-8-8 XRGB */
        ppdev->flGreen = 0x0000FF00;
        ppdev->flBlue  = 0x000000FF;
    } else {
        /* default / 16bpp: 5-6-5 RGB */
        ppdev->ulBitsPerPel  = 16;
        ppdev->ulBitmapType  = BMF_16BPP;
        ppdev->iDitherFormat = BMF_16BPP;
        ppdev->flRed   = 0x0000F800;
        ppdev->flGreen = 0x000007E0;
        ppdev->flBlue  = 0x0000001F;
    }
}

/* Standard DDK default font descriptors (values from the DDK framebuf sample,
 * screen.c). The NT display-driver contract REQUIRES the driver fill DEVINFO's
 * lfDefaultFont / lfAnsiVarFont / lfAnsiFixFont; a zeroed LOGFONTW (empty face,
 * height 0) is a contract violation that leaves the console text mapper without
 * a device default. */
#define FXD3D_SYSTM_LOGFONT \
    {16,7,0,0,700,0,0,0,ANSI_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS, \
     DEFAULT_QUALITY,VARIABLE_PITCH | FF_DONTCARE,L"System"}
#define FXD3D_HELVE_LOGFONT \
    {12,9,0,0,400,0,0,0,ANSI_CHARSET,OUT_DEFAULT_PRECIS,CLIP_STROKE_PRECIS, \
     PROOF_QUALITY,VARIABLE_PITCH | FF_DONTCARE,L"MS Sans Serif"}
#define FXD3D_COURI_LOGFONT \
    {12,9,0,0,400,0,0,0,ANSI_CHARSET,OUT_DEFAULT_PRECIS,CLIP_STROKE_PRECIS, \
     PROOF_QUALITY,FIXED_PITCH | FF_DONTCARE,L"Courier"}

/* DEVINFO template: the DDK-mandated default GDI objects (the three fonts) plus
 * our unaccelerated caps. DrvEnablePDEV copies this then overrides the runtime
 * fields (iDitherFormat, hpalDefault). Field order matches winddi.h DEVINFO. */
static const DEVINFO gDevInfoFxd3d =
{
    GCAPS_OPAQUERECT,       /* flGraphicsCaps                         */
    FXD3D_SYSTM_LOGFONT,    /* lfDefaultFont                          */
    FXD3D_HELVE_LOGFONT,    /* lfAnsiVarFont                          */
    FXD3D_COURI_LOGFONT,    /* lfAnsiFixFont                          */
    0,                      /* cFonts (no device fonts)               */
    BMF_16BPP,              /* iDitherFormat (overridden per-bpp)     */
    0,                      /* cxDither                               */
    0,                      /* cyDither                               */
    (HPALETTE)0,            /* hpalDefault (created at runtime)        */
    0                       /* flGraphicsCaps2                        */
};

/* ------------------------------------------------------------------------- *
 *  fxchassis_select_mode - re-query the paired miniport's available modes and
 *  find the VIDEO_MODE_INFORMATION whose geometry matches the DEVMODEW GDI
 *  handed us. This is the DDK "framebuf" bInitPDEV pattern: DrvGetModes gives
 *  GDI a DEVMODEW list, GDI hands ONE back at DrvEnablePDEV, and we must recover
 *  the miniport's ModeIndex (plus the authoritative ScreenStride / physical mm
 *  size / colour masks) for that mode so DrvEnableSurface can program it with
 *  IOCTL_VIDEO_SET_CURRENT_MODE. We deliberately do NOT trust dmDriverExtra to
 *  carry the index across GDI's DEVMODE round-trips - we re-match by
 *  width/height/bpp[/freq], exactly as the framebuf/s3virge samples do.
 *
 *  Returns TRUE + *pModeOut filled on a match; FALSE if the miniport query
 *  fails or no advertised mode matches (caller then fails the PDEV cleanly).
 * ------------------------------------------------------------------------- */
static BOOL
fxchassis_select_mode(HANDLE hDriver, const DEVMODEW *pdm,
                      VIDEO_MODE_INFORMATION *pModeOut)
{
    VIDEO_NUM_MODES         numModes;
    PVIDEO_MODE_INFORMATION pModes, pVm, pExact, pFallback, pSel;
    DWORD  bytesReturned;
    ULONG  cModes, cjModeInfo, i;
    BYTE  *pjRaw;
    BOOL   bDefault;

    bytesReturned = 0;
    if (EngDeviceIoControl(hDriver, IOCTL_VIDEO_QUERY_NUM_AVAIL_MODES,
                           NULL, 0, &numModes, sizeof(numModes),
                           &bytesReturned) != NO_ERROR ||
        bytesReturned < sizeof(numModes)) {
        return FALSE;
    }
    cModes     = numModes.NumModes;
    cjModeInfo = numModes.ModeInformationLength;
    if (cModes == 0 || cjModeInfo == 0 ||
        cjModeInfo < sizeof(VIDEO_MODE_INFORMATION)) {
        return FALSE;
    }

    pModes = (PVIDEO_MODE_INFORMATION)
             EngAllocMem(FL_ZERO_MEMORY, cModes * cjModeInfo, FXD3D_ALLOC_TAG);
    if (pModes == NULL) {
        return FALSE;
    }

    bytesReturned = 0;
    if (EngDeviceIoControl(hDriver, IOCTL_VIDEO_QUERY_AVAIL_MODES,
                           NULL, 0, pModes, cModes * cjModeInfo,
                           &bytesReturned) != NO_ERROR) {
        EngFreeMem(pModes);
        return FALSE;
    }

    /* An all-zero DEVMODE means GDI wants the miniport's default (first valid)
     * mode. */
    bDefault = (pdm->dmPelsWidth == 0 && pdm->dmPelsHeight == 0 &&
                pdm->dmBitsPerPel == 0 && pdm->dmDisplayFrequency == 0);

    /* Two-pass: prefer an exact width/height/bpp/frequency match; fall back to
     * geometry-only (ignore frequency) so a GDI-normalized/zeroed refresh field
     * can't force a hard PDEV failure. */
    pjRaw     = (BYTE *)pModes;
    pExact    = NULL;
    pFallback = NULL;
    for (i = 0; i < cModes; i++) {
        pVm = (PVIDEO_MODE_INFORMATION)(pjRaw + (i * cjModeInfo));
        if (pVm->Length == 0) {
            continue;   /* miniport marks an unusable slot with Length 0 */
        }
        if ((pVm->NumberOfPlanes * pVm->BitsPerPlane) != 16 &&
            (pVm->NumberOfPlanes * pVm->BitsPerPlane) != 32) {
            continue;   /* only the depths DrvGetModes publishes (review #7) */
        }
        if (bDefault) {
            pExact = pVm;
            break;
        }
        if ((LONG)pVm->VisScreenWidth  == (LONG)pdm->dmPelsWidth  &&
            (LONG)pVm->VisScreenHeight == (LONG)pdm->dmPelsHeight &&
            (LONG)(pVm->NumberOfPlanes * pVm->BitsPerPlane)
                                       == (LONG)pdm->dmBitsPerPel) {
            if (pFallback == NULL) {
                pFallback = pVm;
            }
            if (pVm->Frequency == pdm->dmDisplayFrequency) {
                pExact = pVm;
                break;
            }
        }
    }

    pSel = pExact ? pExact : pFallback;
    if (pSel != NULL) {
        *pModeOut = *pSel;
    }
    EngFreeMem(pModes);
    return (pSel != NULL);
}

/* ------------------------------------------------------------------------- *
 *  DrvGetModes - enumerate the miniport's available modes as DEVMODEW[].
 *
 *  Public contract: query IOCTL_VIDEO_QUERY_NUM_AVAIL_MODES to size the list,
 *  then IOCTL_VIDEO_QUERY_AVAIL_MODES to fetch VIDEO_MODE_INFORMATION[], and
 *  translate each into a DEVMODEW the GDI mode list understands. When pdm is
 *  NULL, GDI is asking only for the required byte count.
 * ------------------------------------------------------------------------- */
ULONG APIENTRY DrvGetModes(HANDLE hDriver, ULONG cjSize, DEVMODEW *pdm)
{
    VIDEO_NUM_MODES         numModes;
    PVIDEO_MODE_INFORMATION pVideoModes;
    PVIDEO_MODE_INFORMATION pVm;
    DWORD  bytesReturned;
    ULONG  cModes;
    ULONG  cjModeInfo;
    ULONG  cjNeeded;
    ULONG  i;
    ULONG  cWritten;
    BYTE  *pjRaw;

    bytesReturned = 0;
    if (EngDeviceIoControl(hDriver, IOCTL_VIDEO_QUERY_NUM_AVAIL_MODES,
                           NULL, 0,
                           &numModes, sizeof(numModes),
                           &bytesReturned) != NO_ERROR) {
        return 0;
    }

    cModes     = numModes.NumModes;
    cjModeInfo = numModes.ModeInformationLength;
    if (cModes == 0 || cjModeInfo == 0) {
        return 0;
    }

    /* Byte count GDI must provide: one DEVMODEW per mode. */
    cjNeeded = cModes * sizeof(DEVMODEW);
    if (pdm == NULL) {
        return cjNeeded;
    }
    if (cjSize < sizeof(DEVMODEW)) {
        return 0;
    }

    pVideoModes = (PVIDEO_MODE_INFORMATION)
                  EngAllocMem(FL_ZERO_MEMORY, cModes * cjModeInfo, FXD3D_ALLOC_TAG);
    if (pVideoModes == NULL) {
        return 0;
    }

    bytesReturned = 0;
    if (EngDeviceIoControl(hDriver, IOCTL_VIDEO_QUERY_AVAIL_MODES,
                           NULL, 0,
                           pVideoModes, cModes * cjModeInfo,
                           &bytesReturned) != NO_ERROR) {
        EngFreeMem(pVideoModes);
        return 0;
    }

    pjRaw    = (BYTE *)pVideoModes;
    cWritten = 0;
    for (i = 0; i < cModes; i++) {
        if ((ULONG)((cWritten + 1) * sizeof(DEVMODEW)) > cjSize) {
            break;   /* GDI's buffer is full */
        }
        pVm = (PVIDEO_MODE_INFORMATION)(pjRaw + (i * cjModeInfo));
        if (pVm->Length == 0) {
            continue;   /* miniport marks an unusable slot with Length 0 */
        }
        /* Publish ONLY 16/32bpp modes (M4c-2 review #7): the driver's whole
         * pipeline - GDI format masks, the DDraw surface path, the blit-
         * present dst format - supports exactly these two depths, but the
         * stock 3dfx miniport also advertises 8bpp modes ("Run in 256
         * colors"). Passing one through used to reach gbkernel coerced to
         * 16bpp and scribble past the primary; now it is never offered
         * (and gbkernel's real-depth guard skips the present if one ever
         * arrives another way). */
        if ((pVm->NumberOfPlanes * pVm->BitsPerPlane) != 16 &&
            (pVm->NumberOfPlanes * pVm->BitsPerPlane) != 32) {
            continue;
        }

        memset(pdm, 0, sizeof(DEVMODEW));
        /* Driver/device identity. dmDeviceName is left as the GDI default. */
        pdm->dmSpecVersion   = DM_SPECVERSION;
        pdm->dmDriverVersion = DM_SPECVERSION;
        pdm->dmSize          = sizeof(DEVMODEW);
        pdm->dmDriverExtra   = 0;

        pdm->dmBitsPerPel    = pVm->NumberOfPlanes * pVm->BitsPerPlane;
        pdm->dmPelsWidth     = pVm->VisScreenWidth;
        pdm->dmPelsHeight    = pVm->VisScreenHeight;
        pdm->dmDisplayFrequency = pVm->Frequency;
        pdm->dmDisplayFlags  = 0;
        pdm->dmFields        = DM_BITSPERPEL | DM_PELSWIDTH | DM_PELSHEIGHT |
                               DM_DISPLAYFLAGS | DM_DISPLAYFREQUENCY;
        cWritten++;
        pdm = (DEVMODEW *)(((BYTE *)pdm) + sizeof(DEVMODEW));
    }

    EngFreeMem(pVideoModes);
    return cWritten * sizeof(DEVMODEW);
}

/* ------------------------------------------------------------------------- *
 *  DrvEnablePDEV - build a PDEV + fill GDIINFO / DEVINFO from a DEVMODEW.
 * ------------------------------------------------------------------------- */
DHPDEV APIENTRY DrvEnablePDEV(
    DEVMODEW *pdm,
    LPWSTR    pwszLogAddress,
    ULONG     cPat,
    HSURF    *phsurfPatterns,
    ULONG     cjCaps,
    ULONG    *pdevcaps,
    ULONG     cjDevInfo,
    DEVINFO  *pdi,
    HDEV      hdev,
    LPWSTR    pwszDeviceName,
    HANDLE    hDriver)
{
    PFXPDEV  ppdev;
    GDIINFO *pgdi;
    ULONG    bpp;
    VIDEO_MODE_INFORMATION vmode;

    UNREFERENCED_PARAMETER(pwszLogAddress);
    UNREFERENCED_PARAMETER(cPat);
    UNREFERENCED_PARAMETER(phsurfPatterns);
    UNREFERENCED_PARAMETER(hdev);
    UNREFERENCED_PARAMETER(pwszDeviceName);

    if (cjCaps < sizeof(GDIINFO) || cjDevInfo < sizeof(DEVINFO) || pdm == NULL) {
        return (DHPDEV)0;
    }

    ppdev = (PFXPDEV)EngAllocMem(FL_ZERO_MEMORY, sizeof(FXPDEV), FXD3D_ALLOC_TAG);
    if (ppdev == NULL) {
        return (DHPDEV)0;
    }

    /* Recover the miniport mode (ModeIndex + authoritative ScreenStride /
     * physical-mm size / colour masks) for the DEVMODEW GDI chose. Without this
     * ppdev->ulMode has no meaningful value to hand IOCTL_VIDEO_SET_CURRENT_MODE
     * in DrvEnableSurface, and the stride would be a guess. Fail the PDEV
     * cleanly if the mode can't be resolved - GDI then tries another mode or
     * falls back, which is correct (a PDEV built on a mode we cannot program is
     * exactly the old fallback bug). */
    memset(&vmode, 0, sizeof(vmode));
    if (!fxchassis_select_mode(hDriver, pdm, &vmode)) {
        EngFreeMem(ppdev);
        return (DHPDEV)0;
    }

    bpp = vmode.NumberOfPlanes * vmode.BitsPerPlane;
    if (bpp == 0) {
        bpp = pdm->dmBitsPerPel ? pdm->dmBitsPerPel : 16;
    }
    fxchassis_format_from_bpp(ppdev, bpp);
    /* Prefer the miniport's real colour masks when it reports them (keep the
     * bpp-derived 5-6-5 / 8-8-8 defaults if it reports zeroes, so the default
     * palette is never built from a degenerate mask). */
    if (vmode.RedMask && vmode.GreenMask && vmode.BlueMask) {
        ppdev->flRed   = vmode.RedMask;
        ppdev->flGreen = vmode.GreenMask;
        ppdev->flBlue  = vmode.BlueMask;
    }

    ppdev->hDriver     = hDriver;
    ppdev->cxScreen    = (LONG)vmode.VisScreenWidth;
    ppdev->cyScreen    = (LONG)vmode.VisScreenHeight;
    ppdev->ulFrequency = vmode.Frequency;
    ppdev->ulMode      = vmode.ModeIndex;   /* the number SET_CURRENT_MODE needs */
    /* Real scanline pitch from the miniport (may be padded above width*bpp);
     * guessing width*bpp shears the desktop on a padded board. */
    ppdev->lDeltaScreen = (LONG)vmode.ScreenStride;
    if (ppdev->lDeltaScreen == 0) {
        ppdev->lDeltaScreen = (LONG)(ppdev->cxScreen * (ppdev->ulBitsPerPel >> 3));
    }

    /* ---- GDIINFO: describe the device to GDI ------------------------------ */
    pgdi = (GDIINFO *)pdevcaps;
    memset(pgdi, 0, sizeof(GDIINFO));
    pgdi->ulVersion    = GDI_DRIVER_VERSION;
    pgdi->ulTechnology = DT_RASDISPLAY;
    /* Physical screen size in millimetres. GDI does NOT synthesize these from
     * resolution - a 0 here reports a 0mm screen to apps and risks a
     * divide-by-zero in DPI-from-size paths. Take the miniport's value; if it
     * reports 0, derive from resolution at 96 DPI. */
    pgdi->ulHorzSize   = vmode.XMillimeter;
    pgdi->ulVertSize   = vmode.YMillimeter;
    if (pgdi->ulHorzSize == 0) {
        pgdi->ulHorzSize = (ULONG)ppdev->cxScreen * 254 / (10 * 96);
    }
    if (pgdi->ulVertSize == 0) {
        pgdi->ulVertSize = (ULONG)ppdev->cyScreen * 254 / (10 * 96);
    }
    pgdi->ulHorzRes    = ppdev->cxScreen;
    pgdi->ulVertRes    = ppdev->cyScreen;
    pgdi->cBitsPixel   = ppdev->ulBitsPerPel;
    pgdi->cPlanes      = 1;
    pgdi->ulNumColors  = (ULONG)-1;   /* > 256 colours */
    pgdi->flRaster     = 0;
    pgdi->ulLogPixelsX = 96;
    pgdi->ulLogPixelsY = 96;
    pgdi->flTextCaps   = TC_RA_ABLE;
    pgdi->ulDACRed     = 8;
    pgdi->ulDACGreen   = 8;
    pgdi->ulDACBlue    = 8;
    pgdi->ulAspectX    = 0x24;
    pgdi->ulAspectY    = 0x24;
    pgdi->ulAspectXY   = 0x33;
    pgdi->xStyleStep   = 1;
    pgdi->yStyleStep   = 1;
    pgdi->denStyleStep = 3;
    pgdi->ptlPhysOffset.x = 0;
    pgdi->ptlPhysOffset.y = 0;
    pgdi->szlPhysSize.cx  = 0;
    pgdi->szlPhysSize.cy  = 0;
    pgdi->ulNumPalReg  = 0;
    /* Halftone init (standard hi/true-colour framebuffer values). */
    pgdi->ulDevicePelsDPI  = 0;
    pgdi->ulPrimaryOrder   = PRIMARY_ORDER_CBA;
    pgdi->ulHTPatternSize  = HT_PATSIZE_4x4_M;
    pgdi->ulHTOutputFormat = HT_FORMAT_8BPP;
    pgdi->flHTFlags        = HT_FLAG_ADDITIVE_PRIMS;
    pgdi->ulVRefresh       = ppdev->ulFrequency;
    pgdi->ulBltAlignment   = 1;

    /* Device colour characterization (COLORINFO) consumed by the halftone / ICM
     * path. Standard DDK additive-primary CIE values (framebuf default); an
     * all-zero ciDevice gives degenerate halftone colour. Cyan/Magenta/Yellow +
     * dye terms stay 0 (already memset) - correct for an additive display. */
    pgdi->ciDevice.Red.x   = 6700;  pgdi->ciDevice.Red.y   = 3300;  pgdi->ciDevice.Red.Y   = 0;
    pgdi->ciDevice.Green.x = 2100;  pgdi->ciDevice.Green.y = 7100;  pgdi->ciDevice.Green.Y = 0;
    pgdi->ciDevice.Blue.x  = 1400;  pgdi->ciDevice.Blue.y  = 800;   pgdi->ciDevice.Blue.Y  = 0;
    pgdi->ciDevice.AlignmentWhite.x = 3127;
    pgdi->ciDevice.AlignmentWhite.y = 3290;
    pgdi->ciDevice.AlignmentWhite.Y = 0;
    pgdi->ciDevice.RedGamma   = 20000;
    pgdi->ciDevice.GreenGamma = 20000;
    pgdi->ciDevice.BlueGamma  = 20000;

    /* ---- DEVINFO: capabilities + default GDI objects ---------------------- *
     * Copy the DDK-standard template (it carries the three mandatory default
     * LOGFONTW font descriptors - a zeroed DEVINFO omits them, a contract
     * violation), then override the runtime-computed fields. */
    *pdi = gDevInfoFxd3d;
    pdi->iDitherFormat   = ppdev->iDitherFormat;

    /* Default bitfields palette for hi/true colour. */
    ppdev->hpalDefault = EngCreatePalette(PAL_BITFIELDS, 0, (ULONG *)NULL,
                                          ppdev->flRed, ppdev->flGreen, ppdev->flBlue);
    pdi->hpalDefault   = ppdev->hpalDefault;

    return (DHPDEV)ppdev;
}

/* ------------------------------------------------------------------------- *
 *  DrvCompletePDEV - GDI hands us the finished HDEV.
 * ------------------------------------------------------------------------- */
VOID APIENTRY DrvCompletePDEV(DHPDEV dhpdev, HDEV hdev)
{
    PFXPDEV ppdev = (PFXPDEV)dhpdev;
    ppdev->hdevEng = hdev;
}

/* ------------------------------------------------------------------------- *
 *  DrvDisablePDEV - tear down a PDEV (surface already disabled).
 * ------------------------------------------------------------------------- */
VOID APIENTRY DrvDisablePDEV(DHPDEV dhpdev)
{
    PFXPDEV ppdev = (PFXPDEV)dhpdev;
    if (ppdev == NULL) {
        return;
    }
    if (ppdev->hpalDefault != (HPALETTE)0) {
        EngDeletePalette(ppdev->hpalDefault);
        ppdev->hpalDefault = (HPALETTE)0;
    }
    EngFreeMem(ppdev);
}

/* ------------------------------------------------------------------------- *
 *  fxchassis_attach_backend - bring the kernel Glide backend (gbkernel) up on
 *  the paired miniport's BARs once the framebuffer (BAR1) has been mapped.
 *
 *  Public contract (design "Miniport pairing" + gbkernel "MMIO plumbing gap"):
 *    - BAR0 register aperture  <- IOCTL_VIDEO_QUERY_PUBLIC_ACCESS_RANGES (a
 *      conformant miniport declares its BARs as public access ranges in
 *      HwFindAdapter; register space is inherently non-cached);
 *    - BAR1 linear framebuffer <- IOCTL_VIDEO_MAP_VIDEO_MEMORY (already mapped
 *      into ppdev->pjScreen by the caller).
 *
 *  *** HARD REQUIREMENT: the BAR1 mapping MUST be NON-CACHED *** - gbk_mmio.h's
 *  contract. The CMDFIFO ring lives in BAR1, and GBK_WMB is a documented
 *  compiler-barrier no-op that is correct ONLY on a non-cached mapping; a cached
 *  or write-combining BAR1 (without upgrading GBK_WMB to a real store fence)
 *  lets FIFO word stores coalesce past gbk_fifo_advance -> the hole-counter
 *  consumes a partial packet -> CMDFIFO desync -> kernel hang. IOCTL_VIDEO_MAP_
 *  VIDEO_MEMORY exposes no cache attribute (the miniport chooses it when it
 *  registers the frame-buffer range), so at bring-up this is a *contract on the
 *  paired 3dfxvsm.sys miniport*, verified on-card at M4d via FXDBG_PROBE (BAR0
 *  reads back sane cmdFifo0 state) + FXDBG_CLEAR (a FASTFILL actually lands). If
 *  M4d shows BAR1 is mapped write-combining, GBK_WMB must grow an sfence before
 *  enabling the FIFO on that pairing.
 *
 *  VRAM size + desktop geometry come from the mode already set. Failure is
 *  NON-FATAL: log (EngDebugPrint - a no-op on free XP; the real diagnostic is
 *  the FXDBG_PROBE escape) and leave the surface as a 2D-only framebuffer;
 *  gbkAttached stays 0 so DrvDisableSurface skips the detach.
 * ------------------------------------------------------------------------- */
static void
fxchassis_attach_backend(PFXPDEV ppdev)
{
    VIDEO_PUBLIC_ACCESS_RANGES ranges[FXD3D_MAX_PUBRANGES];
    DWORD    bytesReturned;
    ULONG    nRanges, i;
    VOID    *bar0va;
    unsigned vramBytes, desktopEnd, desktopStride;

    ppdev->pvBar0          = NULL;
    ppdev->pubRangesMapped = 0;
    ppdev->gbkAttached     = 0;
    ppdev->cPubRangeVA     = 0;

    /* 1. Query + map the miniport's public access ranges. Input is NULL/0; the
     *    miniport maps each declared range and returns its VA. */
    memset(ranges, 0, sizeof(ranges));
    bytesReturned = 0;
    if (EngDeviceIoControl(ppdev->hDriver,
                           IOCTL_VIDEO_QUERY_PUBLIC_ACCESS_RANGES,
                           NULL, 0,
                           ranges, sizeof(ranges),
                           &bytesReturned) != NO_ERROR) {
        EngDebugPrint("fxd3ddd: ",
                      "gbkernel attach: QUERY_PUBLIC_ACCESS_RANGES failed; 2D-only\n",
                      (va_list)0);
        return;
    }
    ppdev->pubRangesMapped = 1;   /* mapped now: DrvDisableSurface must free    */

    /* BAR0 = the register aperture: the first MEMORY-space range with a valid
     * VA (M4d: confirm the index on the stock 3dfxvsm.sys miniport via
     * FXDBG_PROBE). */
    nRanges = bytesReturned / sizeof(VIDEO_PUBLIC_ACCESS_RANGES);
    if (nRanges > FXD3D_MAX_PUBRANGES) {
        nRanges = FXD3D_MAX_PUBRANGES;
    }
    /* Retain EVERY non-NULL VA the miniport mapped (not just BAR0): the FREE on
     * teardown must list them all, or the ones we drop leak. BAR0 is separately
     * the first MEMORY-space range with a valid VA. */
    bar0va = NULL;
    for (i = 0; i < nRanges; i++) {
        if (ranges[i].VirtualAddress == NULL) {
            continue;
        }
        ppdev->apvPubRange[ppdev->cPubRangeVA] = ranges[i].VirtualAddress;
        ppdev->cPubRangeVA++;
        if (bar0va == NULL && ranges[i].MappedInIoSpace == 0) {
            bar0va = ranges[i].VirtualAddress;
        }
    }
    if (bar0va == NULL) {
        EngDebugPrint("fxd3ddd: ",
                      "gbkernel attach: no memory-space public range (BAR0); 2D-only\n",
                      (va_list)0);
        return;   /* ranges stay mapped; freed on disable */
    }
    ppdev->pvBar0 = bar0va;

    /* 2. VRAM size (from the FB map's VIDEO_MEMORY_INFORMATION) + desktop
     *    geometry. desktopEnd = first board byte above the GDI primary;
     *    desktopStride = the primary's byte pitch (blit-present dst pitch).
     *
     *    ONE length rules the whole offset space (M4c-2 review #10):
     *    FrameBufferLength - the length of the mapping that actually backs
     *    pjScreen / the DdMapMemory per-process view (on the paired 3dfx
     *    miniport it is AdapterMemorySize, the POPULATED VRAM). It is what
     *    every enable.c bound uses (fxnt_surf_kva, the Dd_Lock sysmem
     *    discriminator, Dd_MapMemory ViewSize), so the carve-out MUST be
     *    computed over the same length. VideoRamLength is the WRONG field
     *    here: the miniport reports the BAR1 decode length there (32 MB even
     *    on a 16 MB board), and carving over it used to place the back/aux
     *    buffers and much of the surface heap above populated VRAM - beyond
     *    every bound and beyond the mapping. Against a miniport reporting
     *    only the visible screen in FrameBufferLength the carve simply fails
     *    -> clean 2D-only degrade, never a stray offset. */
    vramBytes     = (unsigned)ppdev->vmi.FrameBufferLength;
    desktopStride = (unsigned)ppdev->lDeltaScreen;
    desktopEnd    = (unsigned)ppdev->cyScreen * (unsigned)ppdev->lDeltaScreen;

    /* 3. Bring the board up. BAR1 = ppdev->pjScreen (non-cached, per contract).
     *    desktopBpp steers the blit-present dst format (design 5a: a 32bpp
     *    desktop gets the hw 565->8888 convert-blit); depthBytes>0 => request
     *    a 16-bit aux/z buffer above the color set. */
    if (gbkernel_attach(bar0va, ppdev->pjScreen, vramBytes,
                        desktopEnd, desktopStride,
                        (unsigned)ppdev->ulBitsPerPel,
                        (int)ppdev->cxScreen, (int)ppdev->cyScreen,
                        FXD3D_ZDEPTH_BYTES) != 0) {
        EngDebugPrint("fxd3ddd: ",
                      "gbkernel attach: backend attach failed; 2D-only\n",
                      (va_list)0);
        return;   /* graceful: the 2D framebuffer surface is still fully live */
    }
    ppdev->gbkAttached = 1;
}

/* ------------------------------------------------------------------------- *
 *  DrvEnableSurface - map the framebuffer + wrap it as a GDI-drawable surface.
 *
 *  Public contract: ask the miniport to map video memory
 *  (IOCTL_VIDEO_MAP_VIDEO_MEMORY), then create a device surface and point its
 *  bits at the mapped linear framebuffer with EngModifySurface. flHooks=0 =
 *  hook nothing -> GDI draws straight into the framebuffer (unaccelerated).
 * ------------------------------------------------------------------------- */
HSURF APIENTRY DrvEnableSurface(DHPDEV dhpdev)
{
    PFXPDEV ppdev = (PFXPDEV)dhpdev;
    VIDEO_MEMORY vm;
    VIDEO_MODE   vmSet;
    DWORD  bytesReturned;
    HSURF  hsurf;
    SIZEL  sizl;

    /* 0. Program the miniport INTO the mode this PDEV was built for, BEFORE
     *    mapping video memory. This is the load-blocker fix: the standard NT
     *    display-driver contract (DDK framebuf bInitSURF) is
     *    SET_CURRENT_MODE -> MAP_VIDEO_MEMORY, because the framebuffer
     *    location/size is modal (it can move between modes). Skipping the modeset
     *    leaves the stock 3dfxvsm miniport unprogrammed, so MAP_VIDEO_MEMORY
     *    hands back a NULL / last-mode base -> DrvEnableSurface returns 0 ->
     *    win32k falls back to VGA (DrvEscape never reached). ppdev->ulMode is the
     *    real miniport ModeIndex recovered in DrvEnablePDEV (fxchassis_select_mode).
     *    IOCTL_VIDEO_SET_CURRENT_MODE takes a VIDEO_MODE { ULONG RequestedMode }. */
    memset(&vmSet, 0, sizeof(vmSet));
    vmSet.RequestedMode = ppdev->ulMode;
    bytesReturned = 0;
    if (EngDeviceIoControl(ppdev->hDriver, IOCTL_VIDEO_SET_CURRENT_MODE,
                           &vmSet, sizeof(vmSet),
                           NULL, 0,
                           &bytesReturned) != NO_ERROR) {
        return (HSURF)0;   /* can't program the mode -> fail cleanly, don't map */
    }

    /* 1. Map the linear framebuffer from the paired miniport. */
    memset(&vm, 0, sizeof(vm));
    vm.RequestedVirtualAddress = NULL;
    memset(&ppdev->vmi, 0, sizeof(ppdev->vmi));
    bytesReturned = 0;
    if (EngDeviceIoControl(ppdev->hDriver, IOCTL_VIDEO_MAP_VIDEO_MEMORY,
                           &vm, sizeof(vm),
                           &ppdev->vmi, sizeof(ppdev->vmi),
                           &bytesReturned) != NO_ERROR) {
        return (HSURF)0;
    }
    ppdev->pjScreen = (BYTE *)ppdev->vmi.FrameBufferBase;
    if (ppdev->pjScreen == NULL) {
        return (HSURF)0;
    }

    /* 2. Create a device surface of the right size + compatible format. */
    sizl.cx = ppdev->cxScreen;
    sizl.cy = ppdev->cyScreen;
    hsurf = EngCreateDeviceSurface((DHSURF)ppdev, sizl, ppdev->ulBitmapType);
    if (hsurf == (HSURF)0) {
        return (HSURF)0;
    }

    /* 3. Bind it to our HDEV, hooking nothing (unaccelerated framebuffer). */
    if (!EngAssociateSurface(hsurf, ppdev->hdevEng, 0)) {
        EngDeleteSurface(hsurf);
        return (HSURF)0;
    }

    /* 4. Point the surface's bits at the mapped framebuffer (MS_NOTSYSTEMMEMORY
     *    = these bits are device video memory, not swappable RAM). */
    if (!EngModifySurface(hsurf, ppdev->hdevEng, 0 /*flHooks*/,
                          MS_NOTSYSTEMMEMORY,
                          (DHSURF)ppdev, ppdev->pjScreen,
                          ppdev->lDeltaScreen, (VOID *)NULL)) {
        EngDeleteSurface(hsurf);
        return (HSURF)0;
    }

    ppdev->hsurfScreen = hsurf;

    /* 5. Bring the kernel Glide backend up on the paired miniport's BARs so the
     *    D3D DDI (and the FXDBG_* bring-up ladder) can render. Non-fatal: a
     *    failed attach leaves this as a 2D-only framebuffer surface. */
    fxchassis_attach_backend(ppdev);

    return hsurf;
}

/* ------------------------------------------------------------------------- *
 *  DrvDisableSurface - destroy the framebuffer surface.
 * ------------------------------------------------------------------------- */
VOID APIENTRY DrvDisableSurface(DHPDEV dhpdev)
{
    PFXPDEV ppdev = (PFXPDEV)dhpdev;
    DWORD   bytesReturned;
    if (ppdev == NULL) {
        return;
    }

    /* Tear the 3D backend down first (disables the CMDFIFO), then release the
     * BAR0 public-access-range mapping - inverse order of attach. */
    if (ppdev->gbkAttached) {
        gbkernel_detach();
        ppdev->gbkAttached = 0;
    }
    if (ppdev->pubRangesMapped) {
        /* Free EXACTLY the ranges QUERY mapped: the miniport's FREE handler
         * requires an INPUT buffer of VIDEO_MEMORY[] whose RequestedVirtualAddress
         * fields are the VAs it returned (it rejects InputBufferLength <
         * sizeof(VIDEO_MEMORY) with ERROR_INSUFFICIENT_BUFFER). The old NULL/0
         * input therefore freed NOTHING -> the register aperture leaked on every
         * teardown/mode change. Mirror s3virge/3dlabs vDisableHardware: hand it
         * a VIDEO_MEMORY[] listing every retained VA. */
        if (ppdev->cPubRangeVA > 0) {
            VIDEO_MEMORY vmFree[FXD3D_MAX_PUBRANGES];
            ULONG        j;
            for (j = 0; j < ppdev->cPubRangeVA; j++) {
                vmFree[j].RequestedVirtualAddress = ppdev->apvPubRange[j];
            }
            bytesReturned = 0;
            EngDeviceIoControl(ppdev->hDriver,
                               IOCTL_VIDEO_FREE_PUBLIC_ACCESS_RANGES,
                               vmFree,
                               ppdev->cPubRangeVA * sizeof(VIDEO_MEMORY),
                               NULL, 0, &bytesReturned);
        }
        ppdev->pubRangesMapped = 0;
        ppdev->pvBar0          = NULL;
        ppdev->cPubRangeVA     = 0;
    }

    if (ppdev->hsurfScreen != (HSURF)0) {
        EngDeleteSurface(ppdev->hsurfScreen);
        ppdev->hsurfScreen = (HSURF)0;
    }
    /* The miniport reclaims the mapped framebuffer on mode change / disable;
     * TODO(fxd3d M4): issue IOCTL_VIDEO_UNMAP_VIDEO_MEMORY here for symmetry. */
    ppdev->pjScreen = NULL;
}

/* ------------------------------------------------------------------------- *
 *  DrvAssertMode - GDI is switching this PDEV in/out of the foreground.
 *
 *  bEnable==TRUE  -> reassert our mode (return TRUE = we own the hardware).
 *  bEnable==FALSE -> relinquish to the VGA/console (reset to a known mode).
 *  The primary modeset now happens in DrvEnableSurface (SET_CURRENT_MODE before
 *  MAP). GDI drives a full DisableSurface/EnableSurface rebuild on a foreground
 *  switch for this unaccelerated driver (no DrvResetPDEV in the table), so that
 *  rebuild re-issues SET_CURRENT_MODE - a no-op reassert here is correct.
 *  TODO(fxd3d M4): reassert IOCTL_VIDEO_SET_CURRENT_MODE / RESET_DEVICE on
 *  bEnable transitions once we hold the mode across foreground switches.
 * ------------------------------------------------------------------------- */
BOOL APIENTRY DrvAssertMode(DHPDEV dhpdev, BOOL bEnable)
{
    PFXPDEV ppdev = (PFXPDEV)dhpdev;
    UNREFERENCED_PARAMETER(bEnable);
    if (ppdev == NULL) {
        return FALSE;
    }
    return TRUE;
}

/* ========================================================================= *
 *  DEPLOYMENT CONTRACT - how win32k must be told to load THIS DLL (read this
 *  before blaming the code for a "silent VGA fallback"). The load contract in
 *  DrvEnableDriver below is sound; win32k will only ever CALL it if the active
 *  adapter's Display-CLASS instance key names fxd3ddd as its GDI DLL. On an
 *  XP box where the vintage 3dfxv3d was SetupAPI/PnP-installed, the AUTHORITATIVE
 *  selector is NOT Services\3dfxvs\Device0 and NOT the volatile boot-regenerated
 *  Control\Video\{GUID}\NNNN alias - it is the PnP CLASS instance key:
 *
 *      HKLM\SYSTEM\CurrentControlSet\Control\Class\
 *          {4D36E968-E325-11CE-BFC1-08002BE10318}\NNNN
 *
 *  where NNNN is the instance whose Service=3dfxvs (the one the PCI device's
 *  Enum\...\Driver value points at). A bare reg-swap of the other two keys is
 *  INSUFFICIENT - win32k keeps loading whatever InstalledDisplayDrivers in the
 *  CLASS key names (the vintage 3dfxv3d), which is exactly why the edited values
 *  "never revert": nothing reads them. Correct deploy (do this on the box, NOT
 *  in this .c):
 *    1. Copy fxd3ddd.dll -> %SystemRoot%\system32 (rename-named DLLs are
 *       WFP-untracked, so no WHQL catalog / driver-store signing is required).
 *    2. Set, in the ACTIVE Class instance key above (keep Service=3dfxvs to keep
 *       the stock 3dfxvsm miniport pairing):
 *          InstalledDisplayDrivers = fxd3ddd   (REG_MULTI_SZ, base name, no .dll)
 *    3. Reboot (or restart the display stack).
 *  PROVEN-DURABLE path: install a COMPLETE Display INF that names fxd3ddd via
 *  updrv.exe (UpdateDriverForPlugAndPlayDevices) against "PCI\VEN_121A&DEV_0005"
 *  - the same SetupAPI/PnP method that made voodoo3-wfp.inf load the vintage
 *  driver durably (it rebuilds the Class-key config authoritatively). The INF
 *  needs, per install section, CopyFiles=fxd3ddd.dll + AddReg writing
 *  HKR,,InstalledDisplayDrivers,0x00010000,fxd3ddd (+ DefaultSettings.*), and -
 *  for stock-miniport pairing - NO AddService section (keep Service=3dfxvs);
 *  note a services section must be decorated off the install-section name
 *  ([<InstallSec>.Services]), not a bare [FXD3D.NT.Services]. (See the fxd3d.inf
 *  findings - the current INF only CopyFiles and mis-decorates its services
 *  section, so it cannot yet be that "proper INF".)
 *
 *  Also verify the built image is loadable by win32k's restricted loader: PE
 *  subsystem = NATIVE, imports resolve ONLY against WIN32K.SYS (no MSVCRT/
 *  KERNEL32), __stdcall (/Gz) entry points - i.e. built by the W2K DDK `build`
 *  with this dir's SOURCES (TARGETTYPE=GDI_DRIVER), never cross-built with
 *  mingw/msvcrt. A mis-subsystem image also silently fails to load and falls
 *  back with this identical symptom.
 * ========================================================================= */

/* ------------------------------------------------------------------------- *
 *  DRVFN dispatch table + DrvEnableDriver / DrvDisableDriver.
 *
 *  DrvEnableDriver is the single export GDI resolves (see dispdrv.def). It
 *  hands GDI the table of Drv* callbacks we implement. Everything not listed
 *  here is handled/simulated by GDI itself (this is an unaccelerated driver).
 * ------------------------------------------------------------------------- */
static const DRVFN gadrvfn[] =
{
    /* --- required 2D / PDEV / surface / modeset (this file) --- */
    { INDEX_DrvEnablePDEV,         (PFN)DrvEnablePDEV        },
    { INDEX_DrvCompletePDEV,       (PFN)DrvCompletePDEV      },
    { INDEX_DrvDisablePDEV,        (PFN)DrvDisablePDEV       },
    { INDEX_DrvEnableSurface,      (PFN)DrvEnableSurface     },
    { INDEX_DrvDisableSurface,     (PFN)DrvDisableSurface    },
    { INDEX_DrvAssertMode,         (PFN)DrvAssertMode        },
    { INDEX_DrvGetModes,           (PFN)DrvGetModes          },

    /* --- global driver teardown (optional, but GDI resolves it BY INDEX from
     * this table, not by DLL export - without the entry the slot stays NULL and
     * DrvDisableDriver is never called on unload/session change). --- */
    { INDEX_DrvDisableDriver,      (PFN)DrvDisableDriver     },

    /* --- on-card bring-up ladder escape handler (gbkdebug.c) --- */
    { INDEX_DrvEscape,             (PFN)DrvEscape            },

    /* --- DirectDraw / Direct3D DDI (enable.c) --- */
    { INDEX_DrvGetDirectDrawInfo,  (PFN)DrvGetDirectDrawInfo },
    { INDEX_DrvEnableDirectDraw,   (PFN)DrvEnableDirectDraw  },
    { INDEX_DrvDisableDirectDraw,  (PFN)DrvDisableDirectDraw }
};

BOOL APIENTRY DrvEnableDriver(ULONG iEngineVersion, ULONG cj, DRVENABLEDATA *pded)
{
    UNREFERENCED_PARAMETER(iEngineVersion);

    if (cj < sizeof(DRVENABLEDATA)) {
        return FALSE;
    }

    pded->iDriverVersion = DDI_DRIVER_VERSION_NT4;   /* 0x00020000 */
    pded->c              = sizeof(gadrvfn) / sizeof(gadrvfn[0]);
    pded->pdrvfn         = (DRVFN *)gadrvfn;
    return TRUE;
}

VOID APIENTRY DrvDisableDriver(VOID)
{
    /* Nothing global to release; per-PDEV teardown happens in DrvDisablePDEV. */
}

#endif /* HAVE_DDK */
