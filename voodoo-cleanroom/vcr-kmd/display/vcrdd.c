/*
 * vcrdd.c - vcr-kmd display driver: DDI entry points, PDEV and surface.
 *
 * The primary surface is an engine bitmap over the desktop's slice of the
 * linear frame buffer; GDI draws into it directly (no drawing hooks yet, so
 * nothing here can wedge the 2D engine). The miniport owns every register.
 */
#include "vcrdd.h"

static DRVFN g_drvfn[] = {
    { INDEX_DrvEnablePDEV,     (PFN)0 },
    { INDEX_DrvCompletePDEV,   (PFN)0 },
    { INDEX_DrvDisablePDEV,    (PFN)0 },
    { INDEX_DrvEnableSurface,  (PFN)0 },
    { INDEX_DrvDisableSurface, (PFN)0 },
    { INDEX_DrvAssertMode,     (PFN)0 },
    { INDEX_DrvGetModes,       (PFN)0 },
    { INDEX_DrvSetPalette,     (PFN)0 },
    { INDEX_DrvEscape,         (PFN)0 },
    { INDEX_DrvDisableDriver,  (PFN)0 },
};

/* the 20 colours Windows reserves in an 8 bpp palette (0-9 and 246-255) */
static const PALETTEENTRY k_sys20[20] = {
    {0,0,0,0}, {0x80,0,0,0}, {0,0x80,0,0}, {0x80,0x80,0,0}, {0,0,0x80,0},
    {0x80,0,0x80,0}, {0,0x80,0x80,0}, {0xc0,0xc0,0xc0,0}, {0xc0,0xdc,0xc0,0},
    {0xa6,0xca,0xf0,0},
    {0xff,0xfb,0xf0,0}, {0xa0,0xa0,0xa4,0}, {0x80,0x80,0x80,0}, {0xff,0,0,0},
    {0,0xff,0,0}, {0xff,0xff,0,0}, {0,0,0xff,0}, {0xff,0,0xff,0}, {0,0xff,0xff,0},
    {0xff,0xff,0xff,0},
};

/* ---- modes ------------------------------------------------------------------ */

static VIDEO_MODE_INFORMATION *query_modes(HANDLE h, ULONG *count)
{
    VIDEO_NUM_MODES n;
    VIDEO_MODE_INFORMATION *m;
    DWORD got;
    *count = 0;
    if (VcrIoctl(h, IOCTL_VIDEO_QUERY_NUM_AVAIL_MODES, NULL, 0, &n, sizeof n, &got) ||
        n.NumModes == 0 || n.ModeInformationLength != sizeof(VIDEO_MODE_INFORMATION))
        return NULL;
    m = (VIDEO_MODE_INFORMATION *)EngAllocMem(FL_ZERO_MEMORY,
                                              n.NumModes * sizeof *m, VCRDD_TAG);
    if (!m)
        return NULL;
    if (VcrIoctl(h, IOCTL_VIDEO_QUERY_AVAIL_MODES, NULL, 0, m, n.NumModes * sizeof *m,
                 &got)) {
        EngFreeMem(m);
        return NULL;
    }
    *count = n.NumModes;
    return m;
}

/* The mode a DEVMODE asks for. Frequency 0/1 means "default": the lowest
 * refresh offered at that size (the safe one for an unknown monitor). */
static LONG pick_mode(const VIDEO_MODE_INFORMATION *m, ULONG n, const DEVMODEW *dm)
{
    ULONG i, w = dm ? dm->dmPelsWidth : 0, h = dm ? dm->dmPelsHeight : 0;
    ULONG bpp = dm ? dm->dmBitsPerPel : 0, hz = dm ? dm->dmDisplayFrequency : 0;
    LONG best = -1;
    if (!w || !h) {
        w = 800;
        h = 600;
    }
    if (!bpp)
        bpp = 16;
    for (i = 0; i < n; i++) {
        if (m[i].VisScreenWidth != w || m[i].VisScreenHeight != h ||
            m[i].BitsPerPlane * m[i].NumberOfPlanes != bpp)
            continue;
        if (hz > 1) {
            if (m[i].Frequency == hz)
                return (LONG)i;
        } else if (best < 0 || m[i].Frequency < m[best].Frequency) {
            best = (LONG)i;
        }
    }
    return best;
}

ULONG APIENTRY DrvGetModes(HANDLE hDriver, ULONG cjSize, DEVMODEW *pdm)
{
    VIDEO_MODE_INFORMATION *m;
    ULONG n, i, need;
    g_hDriver = hDriver;
    m = query_modes(hDriver, &n);
    if (!m)
        return 0;
    need = n * sizeof(DEVMODEW);
    if (!pdm) {
        EngFreeMem(m);
        return need;
    }
    if (cjSize < need)
        n = cjSize / sizeof(DEVMODEW);
    for (i = 0; i < n; i++) {
        DEVMODEW *d = &pdm[i];
        static const WCHAR name[] = L"vcrdd";
        ULONG k;
        memset(d, 0, sizeof *d);
        for (k = 0; name[k]; k++)
            d->dmDeviceName[k] = name[k];
        d->dmSpecVersion = DM_SPECVERSION;
        d->dmDriverVersion = DM_SPECVERSION;
        d->dmSize = sizeof(DEVMODEW);
        d->dmBitsPerPel = m[i].NumberOfPlanes * m[i].BitsPerPlane;
        d->dmPelsWidth = m[i].VisScreenWidth;
        d->dmPelsHeight = m[i].VisScreenHeight;
        d->dmDisplayFrequency = m[i].Frequency;
        d->dmDisplayFlags = 0;
        d->dmFields = DM_BITSPERPEL | DM_PELSWIDTH | DM_PELSHEIGHT |
                      DM_DISPLAYFREQUENCY | DM_DISPLAYFLAGS;
    }
    VcrDd(VCR_LV_DEBUG, VCR_EV_DD_GET_MODES, n, n * sizeof(DEVMODEW), 0, 0, "DrvGetModes");
    EngFreeMem(m);
    return n * sizeof(DEVMODEW);
}

/* ---- PDEV ------------------------------------------------------------------- */

static HPALETTE make_palette(VCR_PDEV *pd)
{
    ULONG i;
    if (pd->bpp != 8)
        return EngCreatePalette(PAL_BITFIELDS, 0, NULL, pd->flRed, pd->flGreen, pd->flBlue);
    pd->pPal = (PALETTEENTRY *)EngAllocMem(FL_ZERO_MEMORY, 256 * sizeof(PALETTEENTRY),
                                           VCRDD_TAG);
    if (!pd->pPal)
        return NULL;
    for (i = 0; i < 256; i++) {         /* a 3-3-2 cube, then the reserved 20 */
        pd->pPal[i].peRed = (BYTE)(((i >> 5) & 7) * 255 / 7);
        pd->pPal[i].peGreen = (BYTE)(((i >> 2) & 7) * 255 / 7);
        pd->pPal[i].peBlue = (BYTE)((i & 3) * 255 / 3);
    }
    for (i = 0; i < 10; i++) {
        pd->pPal[i] = k_sys20[i];
        pd->pPal[246 + i] = k_sys20[10 + i];
    }
    return EngCreatePalette(PAL_INDEXED, 256, (ULONG *)pd->pPal, 0, 0, 0);
}

static void fill_gdiinfo(VCR_PDEV *pd, GDIINFO *gi)
{
    memset(gi, 0, sizeof *gi);
    gi->ulVersion = GDI_DRIVER_VERSION;
    gi->ulTechnology = DT_RASDISPLAY;
    gi->ulHorzSize = 320;
    gi->ulVertSize = 240;
    gi->ulHorzRes = pd->cx;
    gi->ulVertRes = pd->cy;
    gi->cBitsPixel = pd->bpp;
    gi->cPlanes = 1;
    gi->ulNumColors = pd->bpp == 8 ? 20 : (ULONG)-1;
    gi->flRaster = 0;
    gi->ulLogPixelsX = 96;
    gi->ulLogPixelsY = 96;
    gi->flTextCaps = TC_RA_ABLE;
    gi->ulDACRed = gi->ulDACGreen = gi->ulDACBlue = 8;
    gi->ulAspectX = 0x24;
    gi->ulAspectY = 0x24;
    gi->ulAspectXY = 0x33;
    gi->xStyleStep = 1;
    gi->yStyleStep = 1;
    gi->denStyleStep = 3;
    gi->ulNumPalReg = pd->bpp == 8 ? 256 : 0;
    gi->ciDevice.Red.x = 6700;
    gi->ciDevice.Red.y = 3300;
    gi->ciDevice.Green.x = 2100;
    gi->ciDevice.Green.y = 7100;
    gi->ciDevice.Blue.x = 1400;
    gi->ciDevice.Blue.y = 800;
    gi->ciDevice.AlignmentWhite.x = 3127;
    gi->ciDevice.AlignmentWhite.y = 3290;
    gi->ciDevice.RedGamma = gi->ciDevice.GreenGamma = gi->ciDevice.BlueGamma = 20000;
    gi->ulPrimaryOrder = PRIMARY_ORDER_CBA;
    gi->ulHTPatternSize = HT_PATSIZE_4x4_M;
    gi->ulHTOutputFormat = pd->bpp == 8 ? HT_FORMAT_8BPP
                         : pd->bpp == 16 ? HT_FORMAT_16BPP : HT_FORMAT_32BPP;
    gi->flHTFlags = HT_FLAG_ADDITIVE_PRIMS;
    gi->ulVRefresh = pd->freq;
    gi->ulBltAlignment = 0;
}

static void set_font(LOGFONTW *f, LONG h, LONG w, LONG weight, BYTE pitch, const WCHAR *face)
{
    ULONG i;
    memset(f, 0, sizeof *f);
    f->lfHeight = h;
    f->lfWidth = w;
    f->lfWeight = weight;
    f->lfCharSet = ANSI_CHARSET;
    f->lfOutPrecision = OUT_DEFAULT_PRECIS;
    f->lfClipPrecision = CLIP_DEFAULT_PRECIS;
    f->lfQuality = DEFAULT_QUALITY;
    f->lfPitchAndFamily = pitch;
    for (i = 0; face[i] && i < LF_FACESIZE - 1; i++)
        f->lfFaceName[i] = face[i];
}

static void fill_devinfo(VCR_PDEV *pd, DEVINFO *di)
{
    memset(di, 0, sizeof *di);
    di->flGraphicsCaps = GCAPS_OPAQUERECT | GCAPS_MONO_DITHER;
    if (pd->bpp == 8)
        di->flGraphicsCaps |= GCAPS_PALMANAGED | GCAPS_COLOR_DITHER;
    set_font(&di->lfDefaultFont, 16, 7, 700, VARIABLE_PITCH | FF_DONTCARE, L"System");
    set_font(&di->lfAnsiVarFont, 12, 9, 400, VARIABLE_PITCH | FF_DONTCARE, L"MS Sans Serif");
    set_font(&di->lfAnsiFixFont, 12, 9, 400, FIXED_PITCH | FF_DONTCARE, L"Courier");
    di->cFonts = 0;
    di->iDitherFormat = pd->iBitmapFormat;
    di->cxDither = di->cyDither = pd->bpp == 8 ? 8 : 0;
    di->hpalDefault = pd->hpalDefault;
}

DHPDEV APIENTRY DrvEnablePDEV(DEVMODEW *pdm, LPWSTR pwszLogAddress, ULONG cPat,
                              HSURF *phsurfPatterns, ULONG cjCaps, ULONG *pdevcaps,
                              ULONG cjDevInfo, DEVINFO *pdi, HDEV hdev,
                              LPWSTR pwszDeviceName, HANDLE hDriver)
{
    VCR_PDEV *pd;
    VIDEO_MODE_INFORMATION *m;
    ULONG n;
    LONG i;
    (void)pwszLogAddress;
    (void)cPat;
    (void)phsurfPatterns;
    (void)hdev;
    (void)pwszDeviceName;

    g_hDriver = hDriver;
    if (cjCaps < sizeof(GDIINFO) || cjDevInfo < sizeof(DEVINFO))
        return NULL;
    m = query_modes(hDriver, &n);
    if (!m) {
        VcrDd(VCR_LV_ERROR, VCR_EV_DD_FAIL, 1, 0, 0, 0, "no mode list from the miniport");
        return NULL;
    }
    i = pick_mode(m, n, pdm);
    if (i < 0) {
        VcrDd(VCR_LV_ERROR, VCR_EV_DD_FAIL, 2, pdm ? pdm->dmPelsWidth : 0,
              pdm ? pdm->dmPelsHeight : 0, pdm ? pdm->dmBitsPerPel : 0,
              "no mode matches the request");
        EngFreeMem(m);
        return NULL;
    }
    pd = (VCR_PDEV *)EngAllocMem(FL_ZERO_MEMORY, sizeof *pd, VCRDD_TAG);
    if (!pd) {
        EngFreeMem(m);
        return NULL;
    }
    pd->hDriver = hDriver;
    pd->ulMode = m[i].ModeIndex;
    pd->cx = m[i].VisScreenWidth;
    pd->cy = m[i].VisScreenHeight;
    pd->bpp = m[i].BitsPerPlane * m[i].NumberOfPlanes;
    pd->freq = m[i].Frequency;
    pd->lDelta = (LONG)m[i].ScreenStride;
    pd->flRed = m[i].RedMask;
    pd->flGreen = m[i].GreenMask;
    pd->flBlue = m[i].BlueMask;
    pd->iBitmapFormat = pd->bpp == 8 ? BMF_8BPP : pd->bpp == 16 ? BMF_16BPP : BMF_32BPP;
    EngFreeMem(m);

    pd->hpalDefault = make_palette(pd);
    if (!pd->hpalDefault) {
        VcrDd(VCR_LV_ERROR, VCR_EV_DD_FAIL, 3, pd->bpp, 0, 0, "palette creation failed");
        if (pd->pPal)
            EngFreeMem(pd->pPal);
        EngFreeMem(pd);
        return NULL;
    }
    fill_gdiinfo(pd, (GDIINFO *)pdevcaps);
    fill_devinfo(pd, pdi);
    VcrDd(VCR_LV_INFO, VCR_EV_DD_ENABLE_PDEV, pd->cx, pd->cy, pd->bpp, pd->freq,
          "DrvEnablePDEV mode %u", pd->ulMode);
    return (DHPDEV)pd;
}

VOID APIENTRY DrvCompletePDEV(DHPDEV dhpdev, HDEV hdev)
{
    ((VCR_PDEV *)dhpdev)->hdevEng = hdev;
    VcrDd(VCR_LV_DEBUG, VCR_EV_DD_COMPLETE_PDEV, 0, 0, 0, 0, "DrvCompletePDEV");
}

VOID APIENTRY DrvDisablePDEV(DHPDEV dhpdev)
{
    VCR_PDEV *pd = (VCR_PDEV *)dhpdev;
    VcrDd(VCR_LV_DEBUG, VCR_EV_DD_DISABLE_PDEV, 0, 0, 0, 0, "DrvDisablePDEV");
    if (pd->hpalDefault)
        EngDeletePalette(pd->hpalDefault);
    if (pd->pPal)
        EngFreeMem(pd->pPal);
    EngFreeMem(pd);
}

/* ---- surface ---------------------------------------------------------------- */

static void load_palette(VCR_PDEV *pd, ULONG first, ULONG count)
{
    UCHAR buf[sizeof(VIDEO_CLUT) + 256 * sizeof(ULONG)];
    VIDEO_CLUT *c = (VIDEO_CLUT *)buf;
    ULONG i;
    if (pd->bpp != 8 || !pd->pPal)
        return;
    c->NumEntries = (USHORT)count;
    c->FirstEntry = (USHORT)first;
    for (i = 0; i < count; i++) {
        c->LookupTable[i].RgbArray.Red = pd->pPal[first + i].peRed;
        c->LookupTable[i].RgbArray.Green = pd->pPal[first + i].peGreen;
        c->LookupTable[i].RgbArray.Blue = pd->pPal[first + i].peBlue;
        c->LookupTable[i].RgbArray.Unused = 0;
    }
    VcrIoctl(pd->hDriver, IOCTL_VIDEO_SET_COLOR_REGISTERS, c,
             sizeof(VIDEO_CLUT) - sizeof(ULONG) + count * sizeof(ULONG), NULL, 0, NULL);
}

BOOL VcrDdSetMode(VCR_PDEV *pd)
{
    VIDEO_MODE vm;
    DWORD rc;
    vm.RequestedMode = pd->ulMode;
    rc = VcrIoctl(pd->hDriver, IOCTL_VIDEO_SET_CURRENT_MODE, &vm, sizeof vm, NULL, 0, NULL);
    if (rc) {
        VcrDd(VCR_LV_ERROR, VCR_EV_DD_FAIL, 4, rc, pd->ulMode, 0, "SET_CURRENT_MODE failed");
        return FALSE;
    }
    load_palette(pd, 0, 256);
    return TRUE;
}

HSURF APIENTRY DrvEnableSurface(DHPDEV dhpdev)
{
    VCR_PDEV *pd = (VCR_PDEV *)dhpdev;
    VIDEO_MEMORY vmem;
    VIDEO_MEMORY_INFORMATION vmi;
    SIZEL sizl;
    HSURF hs;
    DWORD rc;

    if (!VcrDdSetMode(pd))
        return NULL;
    vmem.RequestedVirtualAddress = NULL;
    rc = VcrIoctl(pd->hDriver, IOCTL_VIDEO_MAP_VIDEO_MEMORY, &vmem, sizeof vmem, &vmi,
                  sizeof vmi, NULL);
    if (rc) {
        VcrDd(VCR_LV_ERROR, VCR_EV_DD_FAIL, 5, rc, 0, 0, "MAP_VIDEO_MEMORY failed");
        return NULL;
    }
    pd->pvRamBase = vmi.VideoRamBase;
    pd->pjScreen = (PUCHAR)vmi.FrameBufferBase;
    pd->cjFrameBuffer = vmi.FrameBufferLength;
    if ((ULONG)pd->lDelta * pd->cy > pd->cjFrameBuffer) {
        VcrDd(VCR_LV_ERROR, VCR_EV_DD_FAIL, 6, pd->lDelta * pd->cy, pd->cjFrameBuffer, 0,
              "mode larger than the mapped frame buffer");
        return NULL;
    }
    sizl.cx = pd->cx;
    sizl.cy = pd->cy;
    hs = (HSURF)EngCreateBitmap(sizl, pd->lDelta, pd->iBitmapFormat, BMF_TOPDOWN,
                                pd->pjScreen);
    if (!hs) {
        VcrDd(VCR_LV_ERROR, VCR_EV_DD_FAIL, 7, 0, 0, 0, "EngCreateBitmap failed");
        return NULL;
    }
    if (!EngAssociateSurface(hs, pd->hdevEng, 0)) {
        VcrDd(VCR_LV_ERROR, VCR_EV_DD_FAIL, 8, 0, 0, 0, "EngAssociateSurface failed");
        EngDeleteSurface(hs);
        return NULL;
    }
    pd->hsurfEng = hs;
    VcrDd(VCR_LV_INFO, VCR_EV_DD_ENABLE_SURF, (ULONG)(ULONG_PTR)pd->pjScreen, pd->lDelta,
          0, 1, "DrvEnableSurface %ux%ux%u", pd->cx, pd->cy, pd->bpp);
    return hs;
}

VOID APIENTRY DrvDisableSurface(DHPDEV dhpdev)
{
    VCR_PDEV *pd = (VCR_PDEV *)dhpdev;
    VIDEO_MEMORY vmem;
    VcrDd(VCR_LV_DEBUG, VCR_EV_DD_DISABLE_SURF, 0, 0, 0, 0, "DrvDisableSurface");
    if (pd->hsurfEng)
        EngDeleteSurface(pd->hsurfEng);
    pd->hsurfEng = NULL;
    if (pd->pvRamBase) {
        vmem.RequestedVirtualAddress = pd->pvRamBase;
        VcrIoctl(pd->hDriver, IOCTL_VIDEO_UNMAP_VIDEO_MEMORY, &vmem, sizeof vmem, NULL, 0,
                 NULL);
        pd->pvRamBase = NULL;
    }
}

BOOL APIENTRY DrvAssertMode(DHPDEV dhpdev, BOOL bEnable)
{
    VCR_PDEV *pd = (VCR_PDEV *)dhpdev;
    BOOL ok = TRUE;
    if (bEnable)
        ok = VcrDdSetMode(pd);
    else
        VcrIoctl(pd->hDriver, IOCTL_VIDEO_RESET_DEVICE, NULL, 0, NULL, 0, NULL);
    VcrDd(VCR_LV_INFO, VCR_EV_DD_ASSERT_MODE, bEnable, ok, 0, 0, "DrvAssertMode");
    return ok;
}

BOOL APIENTRY DrvSetPalette(DHPDEV dhpdev, PALOBJ *ppalo, FLONG fl, ULONG iStart,
                            ULONG cColors)
{
    VCR_PDEV *pd = (VCR_PDEV *)dhpdev;
    (void)fl;
    if (pd->bpp != 8 || !pd->pPal || iStart >= 256)
        return FALSE;
    if (iStart + cColors > 256)
        cColors = 256 - iStart;
    if (PALOBJ_cGetColors(ppalo, iStart, cColors, (ULONG *)&pd->pPal[iStart]) != cColors)
        return FALSE;
    load_palette(pd, iStart, cColors);
    VcrDd(VCR_LV_TRACE, VCR_EV_DD_SET_PALETTE, iStart, cColors, 0, 0, "DrvSetPalette");
    return TRUE;
}

VOID APIENTRY DrvDisableDriver(VOID)
{
}

BOOL APIENTRY DrvEnableDriver(ULONG iEngineVersion, ULONG cj, DRVENABLEDATA *pded)
{
    (void)iEngineVersion;
    if (cj < sizeof(DRVENABLEDATA))
        return FALSE;
    g_drvfn[0].pfn = (PFN)DrvEnablePDEV;
    g_drvfn[1].pfn = (PFN)DrvCompletePDEV;
    g_drvfn[2].pfn = (PFN)DrvDisablePDEV;
    g_drvfn[3].pfn = (PFN)DrvEnableSurface;
    g_drvfn[4].pfn = (PFN)DrvDisableSurface;
    g_drvfn[5].pfn = (PFN)DrvAssertMode;
    g_drvfn[6].pfn = (PFN)DrvGetModes;
    g_drvfn[7].pfn = (PFN)DrvSetPalette;
    g_drvfn[8].pfn = (PFN)DrvEscape;
    g_drvfn[9].pfn = (PFN)DrvDisableDriver;
    pded->pdrvfn = g_drvfn;
    pded->c = sizeof g_drvfn / sizeof g_drvfn[0];
    pded->iDriverVersion = DDI_DRIVER_VERSION_NT5;
    return TRUE;
}
