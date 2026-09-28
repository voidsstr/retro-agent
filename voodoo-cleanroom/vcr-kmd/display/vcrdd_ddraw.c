/*
 * vcrdd_ddraw.c - the DirectDraw HAL (NT DDI), step one of Direct3D.
 *
 * Without a HAL, DirectDraw on this driver is the HEL: every surface in system
 * memory, every flip a copy - and Direct3D has no device at all, which is why
 * the D3D titles .124 runs on AmigaMerlin's driver cannot run on ours. This is
 * the chassis the D3D HAL (scripts/3dfx fxD3D) will live in:
 *
 *   heap     the video memory below the desktop (the desktop sits at the top,
 *            the hardware cursor one page under it); the DirectDraw runtime
 *            places surfaces in it itself, and computes Lock pointers from
 *            the process view MapMemory gives it
 *   flip     scan out from the target surface's offset (IOCTL_VCR_DDFLIP:
 *            vidDesktopStartAddr on the Voodoo, the VBE Y offset in the VM)
 *   vblank   WaitForVerticalBlank / GetScanLine from the miniport
 *   GDI      FlipToGDISurface / exclusive mode through GUID_NTCallbacks
 *
 * Built only when the public DDK's DirectDraw headers are present (Makefile
 * HAVE_DDI); otherwise DirectDraw stays the HEL, as before.
 */
#include "vcrdd.h"

#ifdef VCR_HAVE_DDI

static DWORD dd_ioctl(VCR_PDEV *pd, DWORD code, PVOID in, DWORD cin, PVOID out, DWORD cout)
{
    return VcrIoctl(pd->hDriver, code, in, cin, out, cout, NULL);
}

static void fill_pixfmt(VCR_PDEV *pd, DDPIXELFORMAT *pf)
{
    memset(pf, 0, sizeof *pf);
    pf->dwSize = sizeof *pf;
    pf->dwRGBBitCount = pd->bpp;
    if (pd->bpp == 8) {
        pf->dwFlags = DDPF_RGB | DDPF_PALETTEINDEXED8;
    } else {
        pf->dwFlags = DDPF_RGB;
        pf->dwRBitMask = pd->flRed;
        pf->dwGBitMask = pd->flGreen;
        pf->dwBBitMask = pd->flBlue;
    }
}

/* [heap_start, heap_end): below the desktop and the cursor page, or - where the
 * desktop is at offset 0 (the VM) - above it. The arithmetic is
 * include/vcr_ddheap.h's (host-tested): with Diag\DdHeapFloor = 1 the heap
 * starts one page up, because offset 0 is what the heap manager answers for
 * "no memory" - the first block of a heap at 0 is lost, and a single-pass
 * allocation of it fails. Absent/0: the layout as it has always been. */
static void heap_range(VCR_PDEV *pd, ULONG *start, ULONG *end)
{
    vcr_u32 s, e;
    vcr_dd_heap_range((vcr_u32)(pd->pjScreen - (PUCHAR)pd->pvRamBase),
                      (vcr_u32)pd->lDelta * pd->cy, pd->cjVram,
                      pd->dd_heap_floor ? VCR_DD_HEAP_FLOOR : 0, &s, &e);
    *start = s;
    *end = e;
}

BOOL APIENTRY DrvGetDirectDrawInfo(DHPDEV dhpdev, DD_HALINFO *hal, DWORD *nheaps,
                                   VIDEOMEMORY *vm, DWORD *nfourcc, DWORD *fourcc)
{
    VCR_PDEV *pd = (VCR_PDEV *)dhpdev;
    ULONG hs, he;
    if (!pd || !pd->pvRamBase || !pd->cjVram)
        return FALSE;
    heap_range(pd, &hs, &he);
    /* none, unless Diag\\D3DBigTex arms the VSA-100's DXT1/3/5 textures
     * (vcrdd_d3d.c): the count first, the codes on the second call */
    *nfourcc = VcrDdD3dFourCC(pd, fourcc);
    *nheaps = he > hs ? 1 : 0;
    if (vm && *nheaps) {             /* the second call hands us the array */
        memset(vm, 0, sizeof *vm);
        vm->dwFlags = VIDMEM_ISLINEAR;
        vm->fpStart = hs;
        vm->fpEnd = he - 1;          /* inclusive */
        /* kept: DirectDraw builds its heap in THIS array (lpHeap), and the
         * Direct3D half allocates mipmap chains from it (vcrdd_d3d.c) */
        pd->pvmList = vm;
    }
    memset(hal, 0, sizeof *hal);
    hal->dwSize = sizeof *hal;
    hal->vmiData.fpPrimary = (FLATPTR)(pd->pjScreen - (PUCHAR)pd->pvRamBase);
    hal->vmiData.dwDisplayWidth = pd->cx;
    hal->vmiData.dwDisplayHeight = pd->cy;
    hal->vmiData.lDisplayPitch = pd->lDelta;
    fill_pixfmt(pd, &hal->vmiData.ddpfDisplay);
    hal->vmiData.dwOffscreenAlign = 16;
    hal->vmiData.dwOverlayAlign = 16;
    hal->vmiData.dwTextureAlign = 16;
    hal->vmiData.dwZBufferAlign = 16;
    hal->vmiData.dwAlphaAlign = 16;
    hal->vmiData.pvPrimary = pd->pjScreen;
    hal->ddCaps.dwSize = sizeof hal->ddCaps;
    /* NOT DDCAPS_GDI: XP's DirectDraw probes a HAL that claims it, then
     * switches it off on every PDEV and no application ever gets it (the
     * runtime sets that bit itself; XP's own Cirrus driver reports
     * BLT | READSCANLINE | BLTCOLORFILL) - found by bisection in the VM,
     * 2026-09-26 */
    hal->ddCaps.dwCaps = DDCAPS_BLT | DDCAPS_BLTCOLORFILL | DDCAPS_READSCANLINE;
    /* Dd_Flip honours DDFLIP_NOVSYNC - and the runtime only ever sends it to a
     * HAL that says so: without this, D3D's PRESENT_INTERVAL_IMMEDIATE was
     * quietly a vsync'd flip (d3dprobe perf --novsync 85.0 fps at 85 Hz) */
    hal->ddCaps.dwCaps2 = DDCAPS2_FLIPNOVSYNC;
    hal->ddCaps.dwRops[SRCCOPY >> 21] |= 1u << ((SRCCOPY >> 16) & 31);  /* rop 0xCC */
    hal->ddCaps.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE | DDSCAPS_OFFSCREENPLAIN |
                                 DDSCAPS_FLIP;
    hal->ddCaps.dwVidMemTotal = he - hs;
    hal->ddCaps.dwVidMemFree = he - hs;
    hal->ddCaps.dwNumFourCCCodes = *nfourcc;
    hal->GetDriverInfo = DdGetDriverInfo;
    hal->dwFlags = DDHALINFO_GETDRIVERINFOSET;
    VcrDdD3dHalInfo(pd, hal);           /* Direct3D, where the 3D engine is */
    VcrDd(VCR_LV_INFO, VCR_EV_DD_DDRAW, 1, hs, he, (ULONG)hal->vmiData.fpPrimary,
          "DirectDraw HAL: heap %x-%x (%u KB), primary at %x%s", hs, he, (he - hs) >> 10,
          (ULONG)hal->vmiData.fpPrimary,
          pd->dd_heap_floor ? " (Diag\\DdHeapFloor: the heap starts off offset 0)" : "");
    return TRUE;
}

/* ---- DD_CALLBACKS -------------------------------------------------------------- */

static DWORD APIENTRY Dd_MapMemory(PDD_MAPMEMORYDATA p)
{
    VCR_PDEV *pd = (VCR_PDEV *)p->lpDD->dhpdev;
    VIDEO_SHARE_MEMORY sm;
    VIDEO_SHARE_MEMORY_INFORMATION si;
    DWORD rc;
    memset(&sm, 0, sizeof sm);
    sm.ProcessHandle = p->hProcess;
    if (p->bMap) {
        sm.ViewOffset = 0;
        sm.ViewSize = pd->cjVram;
        rc = dd_ioctl(pd, IOCTL_VIDEO_SHARE_VIDEO_MEMORY, &sm, sizeof sm, &si, sizeof si);
        p->fpProcess = rc ? 0 : (FLATPTR)si.VirtualAddress;
    } else {
        sm.RequestedVirtualAddress = (PVOID)p->fpProcess;
        rc = dd_ioctl(pd, IOCTL_VIDEO_UNSHARE_VIDEO_MEMORY, &sm, sizeof sm, NULL, 0);
    }
    VcrDd(rc ? VCR_LV_WARN : VCR_LV_DEBUG, VCR_EV_DD_DDRAW, 2, p->bMap, rc,
          (ULONG)p->fpProcess, "MapMemory %s -> %u", p->bMap ? "map" : "unmap", rc);
    p->ddRVal = rc ? DDERR_GENERIC : DD_OK;
    return DDHAL_DRIVER_HANDLED;
}

static int vblank(VCR_PDEV *pd, vcr_dd_vblank *v)
{
    memset(v, 0, sizeof *v);
    return !dd_ioctl(pd, IOCTL_VCR_VBLANK, NULL, 0, v, sizeof *v);
}

static DWORD APIENTRY Dd_WaitForVerticalBlank(PDD_WAITFORVERTICALBLANKDATA p)
{
    VCR_PDEV *pd = (VCR_PDEV *)p->lpDD->dhpdev;
    vcr_dd_vblank v;
    ULONG i;
    p->ddRVal = DD_OK;
    if (!vblank(pd, &v)) {
        p->ddRVal = DDERR_GENERIC;
        return DDHAL_DRIVER_HANDLED;
    }
    switch (p->dwFlags) {
    case DDWAITVB_I_TESTVB:
        p->bIsInVB = v.in_vblank;
        break;
    case DDWAITVB_BLOCKBEGIN:       /* out of vblank, then into it - bounded */
        for (i = 0; i < 200000 && v.in_vblank && vblank(pd, &v); i++)
            ;
        for (i = 0; i < 200000 && !v.in_vblank && vblank(pd, &v); i++)
            ;
        break;
    case DDWAITVB_BLOCKEND:
        for (i = 0; i < 200000 && !v.in_vblank && vblank(pd, &v); i++)
            ;
        for (i = 0; i < 200000 && v.in_vblank && vblank(pd, &v); i++)
            ;
        break;
    default:
        p->ddRVal = DDERR_INVALIDPARAMS;
    }
    return DDHAL_DRIVER_HANDLED;
}

/* The one place vidCurrentLine is reported. Never an unset answer: ddlab read
 * 2293576 here (stack garbage, the blank path left it unset) where XP's own
 * driver says 0. "In the blank" is status[6]'s answer ONLY, as before: the
 * register's own meaning is unmeasured. A line past the visible height is
 * NOT called the blank - 86Box reads 0x7ff on every read, so that would
 * answer DDERR_VERTICALBLANKINPROGRESS forever there (an app waiting for the
 * blank to end would never leave), and a doublescan mode (320x200 .. 512x384,
 * vdisp = 2 x height) whose counter runs in CRTC lines would call the lower
 * half of the picture the blank. Such a line goes out raw with DD_OK, as it
 * always has, until vidCurrentLine is measured on the VSA-100. Flip
 * completion never looks at it (include/vcr_flip.h). */
static DWORD APIENTRY Dd_GetScanLine(PDD_GETSCANLINEDATA p)
{
    VCR_PDEV *pd = (VCR_PDEV *)p->lpDD->dhpdev;
    vcr_dd_vblank v;
    p->dwScanLine = 0;
    if (!vblank(pd, &v)) {
        p->ddRVal = DDERR_GENERIC;
    } else if (v.in_vblank) {
        p->ddRVal = DDERR_VERTICALBLANKINPROGRESS;
    } else {
        p->dwScanLine = v.scanline;
        p->ddRVal = DD_OK;
    }
    return DDHAL_DRIVER_HANDLED;
}

/* ---- DD_SURFACECALLBACKS ------------------------------------------------------- */

static DWORD flip_to(VCR_PDEV *pd, ULONG offset)
{
    vcr_dd_flip f;
    f.offset = offset;
    return dd_ioctl(pd, IOCTL_VCR_DDFLIP, &f, sizeof f, NULL, 0);
}

static LONGLONG qpc(void)
{
    LONGLONG t;
    EngQueryPerformanceCounter(&t);
    return t;
}

/* One read of the retrace state for flip completion, timed: `*now` is the
 * clock AFTER the read (what the rule compares), and the read's own length
 * goes to the counters - a vblank IOCTL that blocks is one of the ways a
 * flip loop loses time outside the rule. */
static int flip_sample(VCR_PDEV *pd, vcr_dd_vblank *v, LONGLONG *now)
{
    LONGLONG a = qpc();
    int have = vblank(pd, v);
    *now = qpc();
    vcr_flip_note_poll(&pd->flip, *now - a);
    return have;
}

/* Has the chip taken the last flip? It latches the new start address at the
 * next vertical retrace, so a flip is done once a retrace has been seen that
 * began AFTER it (active display seen, then the blank), or once the deadline
 * has passed - which also covers a box whose retrace bit cannot be read. The
 * rule is include/vcr_flip.h; this is the IOCTL and the clock. */
static int flip_done(VCR_PDEV *pd)
{
    vcr_dd_vblank v;
    LONGLONG now;
    int have;
    if (!pd->flip.pending)
        return 1;
    have = flip_sample(pd, &v, &now);
    return vcr_flip_poll(&pd->flip, have, have && v.in_vblank, now);
}

static ULONG ticks_us(LONGLONG t, LONGLONG f)
{
    return f > 0 && t > 0 ? (ULONG)(t * 1000000 / f) : 0;
}

/* How the flips of one exclusive session completed - the question the .124
 * 16 bpp half rate left open (by retrace, by deadline, a blocked IOCTL, a
 * long wait). flips = retrace + deadline + superseded + still pending.
 * The counters live in the PDEV of the mode the flips ran in. A session that
 * set its own mode (every ddlab flip/blt run, d3dprobe --full) flips in a PDEV
 * that has left the screen by the time exclusive mode ends - ddlab restores
 * the mode BEFORE it releases DirectDraw, so the release reaches the desktop
 * PDEV, which counted nothing. So they are also logged when this PDEV's mode
 * leaves the screen (DrvAssertMode FALSE) and when its DirectDraw is disabled.
 * And a session that stays in the desktop's own mode ends with neither an
 * exclusive-mode release (XP does not send SetExclusiveMode(0) for it) nor a
 * mode change: it is logged when the flipping process's DirectDraw object goes
 * away (DestroyDDLocal, VcrDdFlipLocalGone). Each point logs only what is new
 * since the last (vcr_flip_stats_any), and the text names the point (`at`).
 * Log IOCTLs only - no hardware access. */
static void flip_stats_log(VCR_PDEV *pd, const char *at)
{
    vcr_flip_state *s = &pd->flip;
    LONGLONG f;
    if (!vcr_flip_stats_any(s))
        return;
    EngQueryPerformanceFrequency(&f);
    VcrDd(VCR_LV_INFO, VCR_EV_DD_DDRAW, 12, s->flips, s->by_retrace, s->by_deadline,
          "flips %u: retrace %u, deadline %u, superseded %u, pending %u (%s)", s->flips,
          s->by_retrace, s->by_deadline, s->superseded, s->pending, at);
    VcrDd(VCR_LV_INFO, VCR_EV_DD_DDRAW, 13, ticks_us(s->max_poll, f), ticks_us(s->max_wait, f),
          ticks_us(s->deadline, f), "flip reads %u: longest %u us; longest wait %u us; deadline %u us",
          s->polls, ticks_us(s->max_poll, f), ticks_us(s->max_wait, f), ticks_us(s->deadline, f));
    vcr_flip_stats_reset(s);
}

/* vcrdd.c: DrvAssertMode(FALSE) - this PDEV's mode is leaving the screen */
void VcrDdFlipStatsLog(VCR_PDEV *pd)
{
    flip_stats_log(pd, "mode off");
}

/* vcrdd_d3d.c: DestroyDDLocal - a process's DirectDraw object is going away,
 * expected in that process's context (unverified on XP: the DEBUG line below,
 * 511 a=14, records the pid it ran in, so the 86Box bed can settle it). The
 * end of a session that stayed in the desktop's own mode: logged if the flips
 * were this process's (vcr_flip_local_gone_logs). Log IOCTLs only. */
void VcrDdFlipLocalGone(VCR_PDEV *pd)
{
    ULONG pid = (ULONG)(ULONG_PTR)EngGetCurrentProcessId();
    int logs = vcr_flip_local_gone_logs(&pd->flip, pid);
    VcrDd(VCR_LV_DEBUG, VCR_EV_DD_DDRAW, 14, pid, pd->flip.owner, (ULONG)logs,
          "DestroyDDLocal: pid %u, flips of %u%s", pid, pd->flip.owner,
          logs ? " - logged" : "");
    if (logs)
        flip_stats_log(pd, "DD local gone");
}

static DWORD APIENTRY Dd_Flip(PDD_FLIPDATA p)
{
    VCR_PDEV *pd = (VCR_PDEV *)p->lpDD->dhpdev;
    ULONG off = (ULONG)p->lpSurfTarg->lpGbl->fpVidMem;
    /* DDFLIP_NOVSYNC (D3D's PRESENT_INTERVAL_IMMEDIATE): the app does not wait
     * for the retrace - the chip still latches whichever start address was
     * written last at the next one, so frames may not all reach the screen,
     * which is what the flag asks for. Holding such a flip pending capped
     * d3dprobe perf --novsync at exactly the refresh (85.0 fps, .124). */
    int novsync = (p->dwFlags & DDFLIP_NOVSYNC) != 0;
    vcr_dd_vblank v;
    LONGLONG now, f;
    int have;
    DWORD rc;
    /* the previous flip has not reached the screen: DDFLIP_WAIT retries */
    if (!novsync && !flip_done(pd)) {
        p->ddRVal = DDERR_WASSTILLDRAWING;
        return DDHAL_DRIVER_HANDLED;
    }
    VcrDd2dSync(pd);            /* a queued blit into the new front finishes first */
    rc = flip_to(pd, off);
    pd->dd_flips++;
    if (rc) {
        VcrDd(VCR_LV_WARN, VCR_EV_DD_DDRAW, 3, off, rc, pd->dd_flips, "Flip refused");
        p->ddRVal = DDERR_GENERIC;
        return DDHAL_DRIVER_HANDLED;
    }
    VcrDd(VCR_LV_TRACE, VCR_EV_DD_DDRAW, 3, off,
          p->lpSurfCurr ? (ULONG)p->lpSurfCurr->lpGbl->fpVidMem : 0, pd->dd_flips,
          "Flip %u: show %x (surface %p), current %x (surface %p) flags %x", pd->dd_flips, off,
          p->lpSurfTarg, p->lpSurfCurr ? (ULONG)p->lpSurfCurr->lpGbl->fpVidMem : 0,
          p->lpSurfCurr, p->dwFlags);
    /* The retrace state is read AFTER the start-address write, and the clock
     * after that: a write racing into a retrace is then reported a frame
     * late, never early (vcr_flip.h). The deadline is the nominal rate's
     * frame + 1/8 unless the miniport sends the achieved rate
     * (Diag\FlipDeadline = 1). */
    have = flip_sample(pd, &v, &now);
    EngQueryPerformanceFrequency(&f);
    vcr_flip_begin(&pd->flip, novsync, have, have && v.in_vblank, now,
                   vcr_flip_deadline(f, have ? v.refresh_mhz : 0, pd->freq));
    /* whose session this is: DestroyDDLocal logs it for that process only */
    vcr_flip_note_owner(&pd->flip, novsync, (ULONG)(ULONG_PTR)EngGetCurrentProcessId());
    pd->flip_from = p->lpSurfCurr ? (ULONG)p->lpSurfCurr->lpGbl->fpVidMem : 0xffffffffu;
    p->ddRVal = DD_OK;
    return DDHAL_DRIVER_HANDLED;
}

static DWORD APIENTRY Dd_GetFlipStatus(PDD_GETFLIPSTATUSDATA p)
{
    VCR_PDEV *pd = (VCR_PDEV *)p->lpDD->dhpdev;
    p->ddRVal = flip_done(pd) ? DD_OK : DDERR_WASSTILLDRAWING;
    return DDHAL_DRIVER_HANDLED;
}

/* ---- Blt: in software over video memory, for now ------------------------------------
 * Same-format copies (SRCCOPY, overlap-safe) and colour fills between video
 * memory surfaces. Everything else - stretch, colour keys, system memory,
 * clipped lists - is handed back to the HEL (DDHAL_DRIVER_NOTHANDLED), which
 * does it correctly. The 2D engine takes this over later; the interface does
 * not change. */

static PUCHAR surf_kva(VCR_PDEV *pd, PDD_SURFACE_LOCAL s, ULONG *max)
{
    ULONG off;
    if (!s || !s->lpGbl || (s->ddsCaps.dwCaps & DDSCAPS_SYSTEMMEMORY))
        return NULL;
    off = (ULONG)s->lpGbl->fpVidMem;
    if (off >= pd->cjVram)
        return NULL;
    *max = pd->cjVram - off;
    return (PUCHAR)pd->pvRamBase + off;
}

static ULONG surf_bytespp(VCR_PDEV *pd, PDD_SURFACE_LOCAL s)
{
    if (s->dwFlags & DDRAWISURF_HASPIXELFORMAT)
        return s->lpGbl->ddpfSurface.dwRGBBitCount / 8;
    return pd->bpp / 8;
}

static int rect_inside(const RECTL *r, PDD_SURFACE_LOCAL s)
{
    return r->left >= 0 && r->top >= 0 && r->right <= (LONG)s->lpGbl->wWidth &&
           r->bottom <= (LONG)s->lpGbl->wHeight && r->right > r->left && r->bottom > r->top;
}

/* A clipped blit - how a WINDOWED DirectDraw / Direct3D application presents:
 * back buffer -> primary through the window's clip list. Same-size copies
 * (keyed or not) and colour fills go to the 2D engine rectangle by rectangle;
 * anything else - a stretch, one surface onto itself, a surface in system
 * memory - goes back to the HEL, which would otherwise have done ALL of it by
 * reading video memory with the CPU. */
static DWORD clipped_blt(VCR_PDEV *pd, PDD_BLTDATA p)
{
    PDD_SURFACE_LOCAL d = p->lpDDDestSurface, s = p->lpDDSrcSurface;
    DWORD keyed = p->dwFlags & (DDBLT_KEYSRC | DDBLT_KEYSRCOVERRIDE);
    ULONG dmax, smax, bpp, i;
    LONG dx, dy, w, h;
    DDCOLORKEY ck = { 0, 0 };
    int fill = (p->dwFlags & DDBLT_COLORFILL) != 0;
    if (!pd->g2d_ok || pd->exclusive_pid || !p->prDestRects || !p->dwRectCnt ||
        !surf_kva(pd, d, &dmax))
        return DDHAL_DRIVER_NOTHANDLED;
    bpp = surf_bytespp(pd, d);
    if (fill) {
        if (keyed)
            return DDHAL_DRIVER_NOTHANDLED;
    } else {
        if (!s || s == d || !surf_kva(pd, s, &smax) || surf_bytespp(pd, s) != bpp ||
            ((p->dwFlags & DDBLT_ROP) && ((p->bltFX.dwROP >> 16) & 0xff) != 0xcc))
            return DDHAL_DRIVER_NOTHANDLED;
        w = p->rOrigDest.right - p->rOrigDest.left;
        h = p->rOrigDest.bottom - p->rOrigDest.top;
        if (p->rOrigSrc.right - p->rOrigSrc.left != w || p->rOrigSrc.bottom - p->rOrigSrc.top != h)
            return DDHAL_DRIVER_NOTHANDLED;         /* a stretch */
        if (p->dwFlags & DDBLT_KEYSRCOVERRIDE)
            ck = p->bltFX.ddckSrcColorkey;
        else if (keyed)
            ck = s->ddckCKSrcBlt;
    }
    dx = p->rOrigSrc.left - p->rOrigDest.left;
    dy = p->rOrigSrc.top - p->rOrigDest.top;
    for (i = 0; i < p->dwRectCnt; i++) {
        RECTL r;
        BOOL ok;
        r.left = p->prDestRects[i].left > p->rOrigDest.left ? p->prDestRects[i].left : p->rOrigDest.left;
        r.top = p->prDestRects[i].top > p->rOrigDest.top ? p->prDestRects[i].top : p->rOrigDest.top;
        r.right = p->prDestRects[i].right < p->rOrigDest.right ? p->prDestRects[i].right : p->rOrigDest.right;
        r.bottom = p->prDestRects[i].bottom < p->rOrigDest.bottom ? p->prDestRects[i].bottom : p->rOrigDest.bottom;
        if (r.right <= r.left || r.bottom <= r.top)
            continue;
        if (!rect_inside(&r, d))
            return DDHAL_DRIVER_NOTHANDLED;
        if (fill)
            ok = VcrDd2dFill(pd, (ULONG)d->lpGbl->fpVidMem, d->lpGbl->lPitch, bpp, r.left, r.top,
                             r.right - r.left, r.bottom - r.top, p->bltFX.dwFillColor);
        else
            ok = VcrDd2dCopy(pd, (ULONG)d->lpGbl->fpVidMem, d->lpGbl->lPitch,
                             (ULONG)s->lpGbl->fpVidMem, s->lpGbl->lPitch, bpp, r.left + dx,
                             r.top + dy, r.left, r.top, r.right - r.left, r.bottom - r.top,
                             keyed != 0, ck.dwColorSpaceLowValue, ck.dwColorSpaceHighValue);
        if (!ok)
            /* the engine refused before touching anything (i == 0) or gave up
             * part-way: the HEL redraws every rectangle from the untouched
             * source, which is correct for two different surfaces */
            return DDHAL_DRIVER_NOTHANDLED;
    }
    pd->dd_blts++;
    p->ddRVal = DD_OK;
    return DDHAL_DRIVER_HANDLED;
}

static DWORD APIENTRY Dd_Blt(PDD_BLTDATA p)
{
    VCR_PDEV *pd = (VCR_PDEV *)p->lpDD->dhpdev;
    PDD_SURFACE_LOCAL d = p->lpDDDestSurface, s = p->lpDDSrcSurface;
    const DWORD ok_flags = DDBLT_WAIT | DDBLT_ASYNC | DDBLT_COLORFILL | DDBLT_ROP |
                           DDBLT_KEYSRC | DDBLT_KEYSRCOVERRIDE;
    DWORD keyed = p->dwFlags & (DDBLT_KEYSRC | DDBLT_KEYSRCOVERRIDE);
    PUCHAR dp, sp;
    ULONG dmax, smax, bpp, doff;
    LONG w, h, y, x, dpitch, spitch;

    VcrDd(VCR_LV_TRACE, VCR_EV_DD_DDRAW, 10, d ? (ULONG)d->lpGbl->fpVidMem : 0,
          s ? s->ddsCaps.dwCaps : 0, p->dwFlags, "Blt");
    if (p->dwFlags & ~ok_flags)
        return DDHAL_DRIVER_NOTHANDLED;
    /* a compressed (FOURCC) surface is blocks, not pixels: never ours to blit
     * (they exist only with Diag\\D3DBigTex bit 1, vcrdd_d3d.c) */
    if (pd->tex_dxt && (VcrDdD3dIsFourCC(d) || (s && VcrDdD3dIsFourCC(s))))
        return DDHAL_DRIVER_NOTHANDLED;
    if (p->IsClipped)
        return clipped_blt(pd, p);
    if (!(dp = surf_kva(pd, d, &dmax)) || !rect_inside(&p->rDest, d))
        return DDHAL_DRIVER_NOTHANDLED;
    bpp = surf_bytespp(pd, d);
    if (bpp != 1 && bpp != 2 && bpp != 4)
        return DDHAL_DRIVER_NOTHANDLED;
    doff = (ULONG)d->lpGbl->fpVidMem;
    dpitch = d->lpGbl->lPitch;
    w = p->rDest.right - p->rDest.left;
    h = p->rDest.bottom - p->rDest.top;
    if ((ULONG)((p->rDest.bottom - 1) * dpitch + p->rDest.right * (LONG)bpp) > dmax)
        return DDHAL_DRIVER_NOTHANDLED;
    /* a blit into the buffer a pending flip is taking off the screen tears */
    if (doff == pd->flip_from && !flip_done(pd)) {
        p->ddRVal = DDERR_WASSTILLDRAWING;
        return DDHAL_DRIVER_HANDLED;
    }

    if (p->dwFlags & DDBLT_COLORFILL) {
        DWORD c = p->bltFX.dwFillColor;
        if (keyed)
            return DDHAL_DRIVER_NOTHANDLED;
        if (!VcrDd2dFill(pd, doff, dpitch, bpp, p->rDest.left, p->rDest.top, w, h, c)) {
            VcrDd2dSync(pd);
            dp += p->rDest.top * dpitch + p->rDest.left * (LONG)bpp;
            for (y = 0; y < h; y++, dp += dpitch) {
                if (bpp == 1)
                    memset(dp, (int)(c & 0xff), (size_t)w);
                else if (bpp == 2)
                    for (x = 0; x < w; x++)
                        ((USHORT *)dp)[x] = (USHORT)c;
                else
                    for (x = 0; x < w; x++)
                        ((ULONG *)dp)[x] = c;
            }
        }
    } else if (s && (!(p->dwFlags & DDBLT_ROP) || ((p->bltFX.dwROP >> 16) & 0xff) == 0xcc)) {
        /* a copy: an explicit SRCCOPY - the runtime passes the raster op as
         * 0x00CC0000, so GDI's SRCCOPY (0x00CC0020) never matches; only the
         * ROP byte counts - or a keyed blit, which carries no ROP at all */
        DDCOLORKEY ck = { 0, 0 };
        if (!(sp = surf_kva(pd, s, &smax)) || !rect_inside(&p->rSrc, s) ||
            surf_bytespp(pd, s) != bpp || p->rSrc.right - p->rSrc.left != w ||
            p->rSrc.bottom - p->rSrc.top != h)
            return DDHAL_DRIVER_NOTHANDLED;         /* stretch / format change: HEL */
        spitch = s->lpGbl->lPitch;
        if ((ULONG)((p->rSrc.bottom - 1) * spitch + p->rSrc.right * (LONG)bpp) > smax)
            return DDHAL_DRIVER_NOTHANDLED;
        if (p->dwFlags & DDBLT_KEYSRCOVERRIDE)
            ck = p->bltFX.ddckSrcColorkey;
        else if (keyed)
            ck = s->ddckCKSrcBlt;
        if (VcrDd2dCopy(pd, doff, dpitch, (ULONG)s->lpGbl->fpVidMem, spitch, bpp,
                        p->rSrc.left, p->rSrc.top, p->rDest.left, p->rDest.top, w, h,
                        keyed != 0, ck.dwColorSpaceLowValue, ck.dwColorSpaceHighValue)) {
            /* queued on the engine */
        } else if (keyed) {
            return DDHAL_DRIVER_NOTHANDLED;         /* the HEL keys in software */
        } else {
            VcrDd2dSync(pd);
            dp += p->rDest.top * dpitch + p->rDest.left * (LONG)bpp;
            sp += p->rSrc.top * spitch + p->rSrc.left * (LONG)bpp;
            if (sp < dp && sp + (h - 1) * spitch + w * (LONG)bpp > dp) {
                /* overlapping, destination below: bottom row first */
                for (y = h - 1; y >= 0; y--)
                    memmove(dp + y * dpitch, sp + y * spitch, (size_t)w * bpp);
            } else {
                for (y = 0; y < h; y++)
                    memmove(dp + y * dpitch, sp + y * spitch, (size_t)w * bpp);
            }
        }
    } else {
        return DDHAL_DRIVER_NOTHANDLED;
    }
    pd->dd_blts++;
    p->ddRVal = DD_OK;
    return DDHAL_DRIVER_HANDLED;
}

static DWORD APIENTRY Dd_GetBltStatus(PDD_GETBLTSTATUSDATA p)
{
    VCR_PDEV *pd = (VCR_PDEV *)p->lpDD->dhpdev;
    ULONG st = pd->pjRegs ? *(volatile ULONG *)pd->pjRegs : 0;
    /* CANBLT: the engine queues, always yes. ISBLTDONE: only once it drained */
    if ((p->dwFlags & DDGBS_ISBLTDONE) && pd->g2d_busy && pd->pjRegs &&
        ((st & (1u << 9)) || (st & 0x1fu) < pd->g2d_fifo_full))
        p->ddRVal = DDERR_WASSTILLDRAWING;
    else
        p->ddRVal = DD_OK;
    return DDHAL_DRIVER_HANDLED;
}

/* ---- GUID_NTCallbacks -------------------------------------------------------------- */

static DWORD APIENTRY Dd_SetExclusiveMode(PDD_SETEXCLUSIVEMODEDATA p)
{
    VCR_PDEV *pd = (VCR_PDEV *)p->lpDD->dhpdev;
    pd->dd_exclusive = p->dwEnterExcl;
    VcrDd(VCR_LV_INFO, VCR_EV_DD_DDRAW, 4, p->dwEnterExcl, 0, 0,
          "DirectDraw exclusive %u", p->dwEnterExcl);
    if (!p->dwEnterExcl)
        flip_stats_log(pd, "exclusive end");    /* the session's flips, then counters from zero */
    p->ddRVal = DD_OK;
    return DDHAL_DRIVER_HANDLED;
}

static DWORD APIENTRY Dd_FlipToGDISurface(PDD_FLIPTOGDISURFACEDATA p)
{
    VCR_PDEV *pd = (VCR_PDEV *)p->lpDD->dhpdev;
    DWORD rc = flip_to(pd, (ULONG)(pd->pjScreen - (PUCHAR)pd->pvRamBase));
    VcrDd(VCR_LV_INFO, VCR_EV_DD_DDRAW, 5, p->dwToGDI, rc, pd->dd_flips, "FlipToGDISurface");
    p->ddRVal = DD_OK;
    return DDHAL_DRIVER_HANDLED;
}

static int guid_eq(const GUID *a, const GUID *b)
{
    return memcmp(a, b, sizeof *a) == 0;
}

/* surfaces: the runtime places them in our heap and computes Lock pointers
 * itself; these exist so the HAL looks like a complete one to dxg, and log */
static DWORD APIENTRY Dd_CanCreateSurface(PDD_CANCREATESURFACEDATA p)
{
    VCR_PDEV *pd = (VCR_PDEV *)p->lpDD->dhpdev;
    DDSURFACEDESC *sd = p->lpDDSurfaceDesc;
    /* a 32-bit Z buffer (24+8, the 32 bpp 3D targets' aux buffer) exists on
     * the VSA-100 only, and is offered only with Diag\\D3D32 = 1: a
     * Banshee/Voodoo3 - or a VSA-100 without the switch - renders 16 bpp with
     * a 16-bit Z */
    if (pd && !pd->rt32 && sd && (sd->ddsCaps.dwCaps & DDSCAPS_ZBUFFER)) {
        DWORD zb = (sd->dwFlags & DDSD_PIXELFORMAT) ? sd->ddpfPixelFormat.dwZBufferBitDepth
                 : (sd->dwFlags & DDSD_ZBUFFERBITDEPTH) ? sd->dwZBufferBitDepth : 16;
        if (zb && zb != 16) {
            VcrDd(VCR_LV_INFO, VCR_EV_DD_DDRAW, 11, zb, pd->napalm, 0,
                  "CanCreateSurface: %u-bit Z refused (16-bit Z only: %s)", zb,
                  pd->napalm ? "32 bpp 3D not armed, Diag\\D3D32" : "Banshee/Voodoo3");
            p->ddRVal = DDERR_INVALIDPIXELFORMAT;
            return DDHAL_DRIVER_HANDLED;
        }
    }
    p->ddRVal = DD_OK;
    return DDHAL_DRIVER_HANDLED;
}

static DWORD APIENTRY Dd_CreateSurface(PDD_CREATESURFACEDATA p)
{
    VCR_PDEV *pd = (VCR_PDEV *)p->lpDD->dhpdev;
    VcrDd(VCR_LV_DEBUG, VCR_EV_DD_DDRAW, 7, p->dwSCnt,
          p->dwSCnt ? p->lplpSList[0]->ddsCaps.dwCaps : 0, 0, "CreateSurface");
    /* a mipmap chain must be ONE block, packed as the TMU walks it */
    if (VcrDdD3dCreateMipChain(pd, p))
        return DDHAL_DRIVER_HANDLED;
    /* a compressed texture (Diag\\D3DBigTex bit 1): sized here, placed by
     * DirectDraw - still NOTHANDLED */
    VcrDdD3dCreateTexSurface(pd, p);
    return DDHAL_DRIVER_NOTHANDLED;
}

static DWORD APIENTRY Dd_Lock(PDD_LOCKDATA p)
{
    VCR_PDEV *pd = (VCR_PDEV *)p->lpDD->dhpdev;
    VcrDd(VCR_LV_TRACE, VCR_EV_DD_DDRAW, 9, p->lpDDSurface ? (ULONG)p->lpDDSurface->lpGbl->fpVidMem : 0,
          p->lpDDSurface ? p->lpDDSurface->ddsCaps.dwCaps : 0, p->dwFlags, "Lock");
    /* the buffer a pending flip is taking off the screen is still visible */
    if (p->lpDDSurface && (ULONG)p->lpDDSurface->lpGbl->fpVidMem == pd->flip_from &&
        !flip_done(pd)) {
        p->ddRVal = DDERR_WASSTILLDRAWING;
        return DDHAL_DRIVER_HANDLED;
    }
    VcrDd2dSync(pd);            /* the CPU is about to read or write video memory */
    return DDHAL_DRIVER_NOTHANDLED;     /* the runtime computes the pointer */
}

/* a surface leaves: the Direct3D handle table and contexts must not keep it */
static DWORD APIENTRY Dd_DestroySurface(PDD_DESTROYSURFACEDATA p)
{
    VCR_PDEV *pd = (VCR_PDEV *)p->lpDD->dhpdev;
    VcrDdD3dSurfaceGone(p->lpDDSurface);
    /* memory we allocated (a mipmap chain) is ours to free; the rest the
     * runtime frees */
    if (VcrDdD3dFreeMipChain(pd, p->lpDDSurface)) {
        p->ddRVal = DD_OK;
        return DDHAL_DRIVER_HANDLED;
    }
    return DDHAL_DRIVER_NOTHANDLED;
}

static DWORD APIENTRY Dd_Unlock(PDD_UNLOCKDATA p)
{
    /* the CPU just wrote a texture the TMU may have cached (vcrdd_d3d.c) */
    VcrDdD3dTexWritten(p->lpDDSurface);
    return DDHAL_DRIVER_NOTHANDLED;
}

DWORD APIENTRY DdGetDriverInfo(PDD_GETDRIVERINFODATA p)
{
    /* GUID_NTCallbacks, from ddrawint.h (declared there with DEFINE_GUID) */
    static const GUID nt = { 0x6fe9ecde, 0xdf89, 0x11d1,
                             { 0x9d, 0xb0, 0x00, 0x60, 0x08, 0x27, 0x71, 0xba } };
    p->ddRVal = DDERR_CURRENTLYNOTAVAIL;
    if (VcrDdD3dDriverInfo((VCR_PDEV *)p->dhpdev, p)) {
        /* answered by the Direct3D half */
    } else if (guid_eq(&p->guidInfo, &nt)) {
        DD_NTCALLBACKS cb;
        DWORD n = p->dwExpectedSize < sizeof cb ? p->dwExpectedSize : sizeof cb;
        memset(&cb, 0, sizeof cb);
        cb.dwSize = sizeof cb;
        cb.dwFlags = DDHAL_NTCB32_SETEXCLUSIVEMODE | DDHAL_NTCB32_FLIPTOGDISURFACE;
        cb.SetExclusiveMode = Dd_SetExclusiveMode;
        cb.FlipToGDISurface = Dd_FlipToGDISurface;
        memcpy(p->lpvData, &cb, n);
        p->dwActualSize = sizeof cb;
        p->ddRVal = DD_OK;
    }
    VcrDd(VCR_LV_INFO, VCR_EV_DD_DDRAW, 8, p->guidInfo.Data1, p->dwExpectedSize,
          (ULONG)p->ddRVal, "GetDriverInfo %08x", p->guidInfo.Data1);
    return DDHAL_DRIVER_HANDLED;
}

BOOL APIENTRY DrvEnableDirectDraw(DHPDEV dhpdev, DD_CALLBACKS *cb, DD_SURFACECALLBACKS *scb,
                                  DD_PALETTECALLBACKS *pcb)
{
    VCR_PDEV *pd = (VCR_PDEV *)dhpdev;
    (void)pcb;
    cb->dwSize = sizeof *cb;
    cb->dwFlags = DDHAL_CB32_WAITFORVERTICALBLANK | DDHAL_CB32_GETSCANLINE |
                  DDHAL_CB32_MAPMEMORY | DDHAL_CB32_CANCREATESURFACE | DDHAL_CB32_CREATESURFACE;
    cb->CanCreateSurface = Dd_CanCreateSurface;
    cb->CreateSurface = Dd_CreateSurface;
    cb->WaitForVerticalBlank = Dd_WaitForVerticalBlank;
    cb->GetScanLine = Dd_GetScanLine;
    cb->MapMemory = Dd_MapMemory;
    scb->dwSize = sizeof *scb;
    scb->dwFlags = DDHAL_SURFCB32_FLIP | DDHAL_SURFCB32_GETFLIPSTATUS | DDHAL_SURFCB32_LOCK |
                   DDHAL_SURFCB32_UNLOCK | DDHAL_SURFCB32_BLT | DDHAL_SURFCB32_GETBLTSTATUS |
                   DDHAL_SURFCB32_DESTROYSURFACE;
    scb->DestroySurface = Dd_DestroySurface;
    scb->Blt = Dd_Blt;
    scb->GetBltStatus = Dd_GetBltStatus;
    scb->Lock = Dd_Lock;
    scb->Unlock = Dd_Unlock;
    scb->Flip = Dd_Flip;
    scb->GetFlipStatus = Dd_GetFlipStatus;
    pd->dd_enabled = 1;
    VcrDd(VCR_LV_INFO, VCR_EV_DD_DDRAW, 6, 1, 0, 0, "DrvEnableDirectDraw");
    return TRUE;
}

VOID APIENTRY DrvDisableDirectDraw(DHPDEV dhpdev)
{
    VCR_PDEV *pd = (VCR_PDEV *)dhpdev;
    flip_stats_log(pd, "DD disabled");  /* the backstop: this PDEV's flips, if not logged yet */
    pd->dd_enabled = 0;
    VcrDd(VCR_LV_INFO, VCR_EV_DD_DDRAW, 6, 0, pd->dd_flips, 0, "DrvDisableDirectDraw");
}

#endif /* VCR_HAVE_DDI */
