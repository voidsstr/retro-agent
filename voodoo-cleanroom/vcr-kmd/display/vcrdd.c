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
    { INDEX_DrvSetPointerShape, (PFN)0 },
    { INDEX_DrvMovePointer,    (PFN)0 },
    { INDEX_DrvBitBlt,         (PFN)0 },
    { INDEX_DrvCopyBits,       (PFN)0 },
    { INDEX_DrvTextOut,        (PFN)0 },
    { INDEX_DrvStrokePath,     (PFN)0 },
    { INDEX_DrvFillPath,       (PFN)0 },
    { INDEX_DrvLineTo,         (PFN)0 },
    { INDEX_DrvStretchBlt,     (PFN)0 },
    { INDEX_DrvStretchBltROP,  (PFN)0 },
    { INDEX_DrvAlphaBlend,     (PFN)0 },
    { INDEX_DrvGradientFill,   (PFN)0 },
    { INDEX_DrvTransparentBlt, (PFN)0 },
#ifdef VCR_HAVE_DDI
    { INDEX_DrvGetDirectDrawInfo, (PFN)0 },
    { INDEX_DrvEnableDirectDraw,  (PFN)0 },
    { INDEX_DrvDisableDirectDraw, (PFN)0 },
#endif
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

static ULONG mode_bpp(const VIDEO_MODE_INFORMATION *m)
{
    return m->BitsPerPlane * m->NumberOfPlanes;
}

/* w x h at bpp (0 = any depth): an exact refresh if we have it, else the
 * highest refresh BELOW it (never above - that is how a CRT gets driven out
 * of range), else the lowest. hz 0/1 means "default": the lowest. */
static LONG pick_refresh(const VIDEO_MODE_INFORMATION *m, ULONG n, ULONG w, ULONG h,
                         ULONG bpp, ULONG hz)
{
    ULONG i;
    LONG lowest = -1, below = -1;
    for (i = 0; i < n; i++) {
        if (m[i].VisScreenWidth != w || m[i].VisScreenHeight != h ||
            (bpp && mode_bpp(&m[i]) != bpp))
            continue;
        if (hz > 1 && m[i].Frequency == hz)
            return (LONG)i;
        if (lowest < 0 || m[i].Frequency < m[lowest].Frequency)
            lowest = (LONG)i;
        if (hz > 1 && m[i].Frequency < hz &&
            (below < 0 || m[i].Frequency > m[below].Frequency))
            below = (LONG)i;
    }
    return below >= 0 ? below : lowest;
}

/* How much of a w x h picture a mw x mh mode can show at w x h's shape: the
 * area of the largest w:h rectangle inside it. "Largest" by plain area would
 * hand a 1280x1024 CRT desktop the CVT 1280x720 over 1024x768. */
static ULONG shown_area(ULONG mw, ULONG mh, ULONG w, ULONG h)
{
    ULONG ew = mh * w / h, eh = mw * h / w;
    return (ew < mw ? ew : mw) * (eh < mh ? eh : mh);
}

/* The largest listed size that fits inside w x h (at bpp, 0 = any), into pw, ph;
 * 0 when none does. Largest by shown_area, then by area, then by width. */
static int largest_within(const VIDEO_MODE_INFORMATION *m, ULONG n, ULONG w, ULONG h,
                          ULONG bpp, ULONG *pw, ULONG *ph)
{
    ULONG i, bw = 0, bh = 0, bs = 0;
    for (i = 0; i < n; i++) {
        ULONG mw = m[i].VisScreenWidth, mh = m[i].VisScreenHeight, sa;
        if (!mw || !mh || mw > w || mh > h || (bpp && mode_bpp(&m[i]) != bpp))
            continue;
        sa = shown_area(mw, mh, w, h);
        if (!bw || sa > bs || (sa == bs && (mw * mh > bw * bh ||
                                            (mw * mh == bw * bh && mw > bw)))) {
            bw = mw;
            bh = mh;
            bs = sa;
        }
    }
    *pw = bw;
    *ph = bh;
    return bw != 0;
}

/* Of the depths listed at w x h, the one nearest bpp (the deeper on a tie) */
static ULONG nearest_bpp(const VIDEO_MODE_INFORMATION *m, ULONG n, ULONG w, ULONG h, ULONG bpp)
{
    ULONG i, best = 0, d, bd = 0;
    for (i = 0; i < n; i++) {
        if (m[i].VisScreenWidth != w || m[i].VisScreenHeight != h)
            continue;
        d = mode_bpp(&m[i]) > bpp ? mode_bpp(&m[i]) - bpp : bpp - mode_bpp(&m[i]);
        if (!best || d < bd || (d == bd && mode_bpp(&m[i]) > best)) {
            best = mode_bpp(&m[i]);
            bd = d;
        }
    }
    return best;
}

/* The mode a DEVMODE asks for (pick_refresh's rule for the refresh). A box can
 * ask for a refresh this driver does not list - the registry keeps the
 * previous driver's mode across a driver change - and failing the PDEV for it
 * would leave XP on its VGA driver.
 *
 * It can also ask for a SIZE or depth that is not listed: the list honours
 * the monitor's limits (vcrmp_ddc.c), so a 1280x1024 desktop persisted under
 * a good EDID is missing on the boot the monitor was off - the conservative
 * default stops at 1024x768@60. Failing the PDEV then hands the desktop to
 * the VGA driver, which is worse in every way, so fall back instead:
 *   1. the largest listed size within the request (the most of the requested
 *      picture it can show: 1024x768, not 1280x720, for 1280x1024), at the
 *      requested depth;
 *   2. the same at any depth (the nearest to the one asked for);
 *   3. 640x480, at its lowest refresh;
 *   4. the smallest listed mode, at its lowest refresh.
 * Every listed mode is inside the limits in force, so none of these can drive
 * an out-of-range signal; *fell_back says a fallback was taken (the caller
 * logs it at WARN). */
static LONG pick_mode(const VIDEO_MODE_INFORMATION *m, ULONG n, const DEVMODEW *dm,
                      ULONG *fell_back)
{
    ULONG i, w = dm ? dm->dmPelsWidth : 0, h = dm ? dm->dmPelsHeight : 0;
    ULONG bpp = dm ? dm->dmBitsPerPel : 0, hz = dm ? dm->dmDisplayFrequency : 0;
    ULONG fw, fh;
    LONG r;
    *fell_back = 0;
    if (!w || !h) {
        w = 800;
        h = 600;
    }
    if (!bpp)
        bpp = 16;
    if ((r = pick_refresh(m, n, w, h, bpp, hz)) >= 0)
        return r;
    *fell_back = 1;
    if (largest_within(m, n, w, h, bpp, &fw, &fh))
        return pick_refresh(m, n, fw, fh, bpp, hz);
    if (largest_within(m, n, w, h, 0, &fw, &fh))
        return pick_refresh(m, n, fw, fh, nearest_bpp(m, n, fw, fh, bpp), hz);
    if ((r = pick_refresh(m, n, 640, 480, nearest_bpp(m, n, 640, 480, bpp), 0)) >= 0)
        return r;
    for (i = 0, r = -1; i < n; i++)
        if (r < 0 || m[i].VisScreenWidth * m[i].VisScreenHeight <
                         m[r].VisScreenWidth * m[r].VisScreenHeight)
            r = (LONG)i;
    return r < 0 ? -1 : pick_refresh(m, n, m[r].VisScreenWidth, m[r].VisScreenHeight,
                                     mode_bpp(&m[r]), 0);
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
#ifdef VCR_HAVE_DDI
    /* Without this win32k probes our DirectDraw HAL at PDEV creation (info,
     * enable, ten GetDriverInfo queries) and disables it again, and no
     * application ever sees it (VM test bed, 2026-09-26). */
    di->flGraphicsCaps |= GCAPS_DIRECTDRAW;
#endif
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
    ULONG n, fell_back;
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
    i = pick_mode(m, n, pdm, &fell_back);
    if (i < 0) {
        VcrDd(VCR_LV_ERROR, VCR_EV_DD_FAIL, 2, pdm ? pdm->dmPelsWidth : 0,
              pdm ? pdm->dmPelsHeight : 0, pdm ? pdm->dmBitsPerPel : 0,
              "no mode matches the request");
        EngFreeMem(m);
        return NULL;
    }
    if (fell_back)
        VcrDd(VCR_LV_WARN, VCR_EV_DD_ENABLE_PDEV, m[i].VisScreenWidth, m[i].VisScreenHeight,
              mode_bpp(&m[i]), m[i].Frequency,
              "asked %ux%ux%u@%u, not listed (monitor limits?): using %ux%ux%u@%u",
              pdm ? pdm->dmPelsWidth : 0, pdm ? pdm->dmPelsHeight : 0,
              pdm ? pdm->dmBitsPerPel : 0, pdm ? pdm->dmDisplayFrequency : 0,
              m[i].VisScreenWidth, m[i].VisScreenHeight, mode_bpp(&m[i]), m[i].Frequency);
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
    pd->cjVram = vmi.VideoRamLength;
    pd->pjScreen = (PUCHAR)vmi.FrameBufferBase;
    pd->cjFrameBuffer = vmi.FrameBufferLength;
    if ((ULONG)pd->lDelta * pd->cy > pd->cjFrameBuffer) {
        VcrDd(VCR_LV_ERROR, VCR_EV_DD_FAIL, 6, pd->lDelta * pd->cy, pd->cjFrameBuffer, 0,
              "mode larger than the mapped frame buffer");
        return NULL;
    }
    sizl.cx = pd->cx;
    sizl.cy = pd->cy;
    /* The primary is an opaque DEVICE surface; GDI's drawing on it is hooked
     * and handed to the DIB engine on a bitmap over the same frame buffer
     * (vcrdd_punt.c). DirectDraw on XP will not run over a primary GDI draws
     * into by itself: with the frame buffer handed to GDI as the primary -
     * EngCreateBitmap, or a device surface EngModifySurface'd to expose its
     * bits - win32k probed our HAL on every PDEV and switched it off again
     * (the VM test bed, 2026-09-26). */
    pd->hsurfBits = (HSURF)EngCreateBitmap(sizl, pd->lDelta, pd->iBitmapFormat, BMF_TOPDOWN,
                                           pd->pjScreen);
    if (!pd->hsurfBits || !EngAssociateSurface(pd->hsurfBits, pd->hdevEng, 0) ||
        !(pd->psoBits = EngLockSurface(pd->hsurfBits))) {
        VcrDd(VCR_LV_ERROR, VCR_EV_DD_FAIL, 7, 0, 0, 0, "frame buffer bitmap failed");
        if (pd->hsurfBits)
            EngDeleteSurface(pd->hsurfBits);
        pd->hsurfBits = NULL;
        return NULL;
    }
    hs = EngCreateDeviceSurface((DHSURF)pd, sizl, pd->iBitmapFormat);
    if (!hs || !EngAssociateSurface(hs, pd->hdevEng, VCRDD_HOOKS)) {
        VcrDd(VCR_LV_ERROR, VCR_EV_DD_FAIL, 8, 0, 0, 0, "device surface failed");
        if (hs)
            EngDeleteSurface(hs);
        EngUnlockSurface(pd->psoBits);
        EngDeleteSurface(pd->hsurfBits);
        pd->psoBits = NULL;
        pd->hsurfBits = NULL;
        return NULL;
    }
    pd->hsurfEng = hs;
    VcrDd2dInit(pd);
    VcrDd(VCR_LV_INFO, VCR_EV_DD_ENABLE_SURF, (ULONG)(ULONG_PTR)pd->pjScreen, pd->lDelta,
          0, 1, "DrvEnableSurface %ux%ux%u", pd->cx, pd->cy, pd->bpp);
    VcrDdPointerProbe(pd);
    return hs;
}

VOID APIENTRY DrvDisableSurface(DHPDEV dhpdev)
{
    VCR_PDEV *pd = (VCR_PDEV *)dhpdev;
    VIDEO_MEMORY vmem;
    VcrDd(VCR_LV_DEBUG, VCR_EV_DD_DISABLE_SURF, 0, 0, 0, 0, "DrvDisableSurface");
    VcrDd2dTerm(pd);
    if (pd->hsurfEng)
        EngDeleteSurface(pd->hsurfEng);
    pd->hsurfEng = NULL;
    if (pd->psoBits)
        EngUnlockSurface(pd->psoBits);
    pd->psoBits = NULL;
    if (pd->hsurfBits)
        EngDeleteSurface(pd->hsurfBits);
    pd->hsurfBits = NULL;
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
    if (bEnable) {
        /* GDI takes the display back. A Glide client that released exclusive
         * mode cleared this already; one that was killed never will, and a
         * stale owner would keep the hardware pointer switched off. */
        if (pd->exclusive_pid)
            VcrDd(VCR_LV_WARN, VCR_EV_HWC_EXCLUSIVE, 0, pd->exclusive_pid, 2, 0,
                  "exclusive owner %u never released - cleared on re-assert", pd->exclusive_pid);
        pd->exclusive_pid = 0;
        ok = VcrDdSetMode(pd);
    } else {
        VcrDd2dSync(pd);        /* nothing of ours in flight across the reset */
        VcrIoctl(pd->hDriver, IOCTL_VIDEO_RESET_DEVICE, NULL, 0, NULL, 0, NULL);
    }
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
    g_drvfn[10].pfn = (PFN)DrvSetPointerShape;
    g_drvfn[11].pfn = (PFN)DrvMovePointer;
    g_drvfn[12].pfn = (PFN)DrvBitBlt;
    g_drvfn[13].pfn = (PFN)DrvCopyBits;
    g_drvfn[14].pfn = (PFN)DrvTextOut;
    g_drvfn[15].pfn = (PFN)DrvStrokePath;
    g_drvfn[16].pfn = (PFN)DrvFillPath;
    g_drvfn[17].pfn = (PFN)DrvLineTo;
    g_drvfn[18].pfn = (PFN)DrvStretchBlt;
    g_drvfn[19].pfn = (PFN)DrvStretchBltROP;
    g_drvfn[20].pfn = (PFN)DrvAlphaBlend;
    g_drvfn[21].pfn = (PFN)DrvGradientFill;
    g_drvfn[22].pfn = (PFN)DrvTransparentBlt;
#ifdef VCR_HAVE_DDI
    g_drvfn[23].pfn = (PFN)DrvGetDirectDrawInfo;
    g_drvfn[24].pfn = (PFN)DrvEnableDirectDraw;
    g_drvfn[25].pfn = (PFN)DrvDisableDirectDraw;
#endif
    pded->pdrvfn = g_drvfn;
    pded->c = sizeof g_drvfn / sizeof g_drvfn[0];
    pded->iDriverVersion = DDI_DRIVER_VERSION_NT5;
    return TRUE;
}
