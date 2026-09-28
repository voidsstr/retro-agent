/*
 * ddlab.c - a DirectDraw test program for the kernel driver's DirectDraw HAL.
 *
 *   ddlab caps                      HAL vs HEL caps, video memory total / free
 *   ddlab flip [--res WxH] [--bpp N] [--frames N] [--work-us N]
 *             exclusive fullscreen, a primary + 1 back buffer. Per frame: lock
 *             the back buffer, write a frame-numbered pattern, flip (DDFLIP_WAIT),
 *             lock the FRONT buffer and read the pattern back - so a flip that
 *             does not really change what is scanned out (or a Lock pointer that
 *             points at the wrong surface) is a counted mismatch, not a guess.
 *             Also: where the surfaces live (video memory = the HAL is in use),
 *             flips per second, and the vertical-blank period.
 *             Per frame too (the .124 16 bpp half rate - was it one long first
 *             frame, scattered long frames, or every frame two refreshes?):
 *             first_frame_ms (loop start to the first Flip's return: the first
 *             Lock maps video memory), max_frame_ms and slow_frames (flip to
 *             flip, over 1.5 refreshes), and flips_s_first_last - the rate
 *             between the first and the last flip, which, unlike flips_s,
 *             does not count the first frame's setup as a flip period.
 *             --work-us N: N microseconds of busy work right after each Flip,
 *             with no DirectDraw call - the D3D pattern (render, then wait for
 *             the flip), which a tighter flip deadline is for (include/
 *             vcr_flip.h). 0 (the default) is the loop as before.
 *             fast_frames / min_frame_ms: flip to flip in UNDER half a refresh
 *             - a flip the driver called done before the chip could have
 *             latched it (the retrace rule is never early, include/
 *             vcr_flip.h); flips_s above vblank_hz is otherwise unexplained.
 *   ddlab zsurf [--zbits 16|24|32]
 *             one Z-buffer surface (256x256, video memory) with an explicit
 *             DDPF_ZBUFFER pixel format - 32 is 24 of depth + 8 of stencil
 *             (D24S8) - asked for the way a DirectDraw 7 / Direct3D 7
 *             application creates one: the request that reaches the HAL's
 *             CanCreateSurface, which the D3D8 runtime never lets through for
 *             a format the HAL does not list. Normal cooperative level, no
 *             window, NO mode switch. RESULT: the HRESULT and whether it was
 *             created (the recorder names the layer: event 511 a=11 is the
 *             driver's CanCreateSurface refusal).
 *   ddlab sdlddraw [--res WxH] [--bpp 16|32] [--src WxH] [--frames N] [--direct]
 *             DOSBox 0.74's [sdl] output=ddraw through SDL 1.2.13's DirectX 5
 *             backend, call for call (IDirectDraw2 / IDirectDrawSurface3):
 *             exclusive fullscreen at --res with SDL's own refresh pick, the
 *             primary alone, a --src blit surface in video memory with an
 *             explicit pitch and pixel format, then per frame Lock/write/
 *             Unlock it and Blt it onto the primary (a stretch when --src !=
 *             --res: DOSBox with aspect=true, 640x400 -> 640x480) and read
 *             the primary back. Every step's HRESULT is in the RESULT, and
 *             "dosbox" says what DOSBox would have done ("ddraw", or
 *             "surface-fallback:<step>" - the 640x400 Descent took on .124).
 *             --direct: that fallback instead (the primary locked and
 *             written every frame, at --res). See do_sdlddraw().
 *   ddlab blt [--res WxH] [--bpp N]
 *             two off-screen surfaces: a pattern blitted A -> B (SRCCOPY), a
 *             colour fill, and an OVERLAPPING blit inside one surface (a scroll,
 *             where a naive copy smears) - each read back and compared; then
 *             copies per second. Also where the surfaces live.
 *
 *   --pace MS  raise the floor between display-mode switches (never below
 *             vcr_pace.h's 3 s; decimal, at most 30000 - anything else is
 *             refused before anything switches): flip and blt each switch into
 *             their mode and back out, and every switch is a monitor re-sync.
 *
 * A switch vcr_pace.h cannot pace (its lock held by a stuck tool, a stamp that
 * never stops moving) is not made: the run ends with a RESULT "error" naming
 * why, and a mode already held is left for the exit hold. A run whose window
 * loses the foreground while it holds the mode (DirectDraw gives the desktop
 * back at that moment, unpaced) records that switch, ends the run and says
 * "focus_lost":true, rather than let a re-activation switch in again.
 *
 * Every step is flushed to the log before the next (the glideprobe rule).
 * The final line is `RESULT {json}`; the host reads the LAST one.
 */
#define INITGUID
#include <windows.h>
#include <ddraw.h>
#include <io.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vcr_pace.h"

static FILE *g_log;
static char  g_logpath[MAX_PATH] = "C:\\ddlab.log";

static void say(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    if (g_log) {
        vfprintf(g_log, fmt, ap);
        fputc('\n', g_log);
        fflush(g_log);
        FlushFileBuffers((HANDLE)_get_osfhandle(_fileno(g_log)));
    }
    va_end(ap);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    printf("\n");
    fflush(stdout);
    va_end(ap);
}

/* the exclusive mode is up and the run is using it (flip / blt, between the
 * switch in and the start of the paced switch out) */
static int g_held;
/* ... and the window lost the foreground meanwhile */
static int g_focus_lost;

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    /* Deactivated while exclusive: DirectDraw's hook on this window gives the
     * desktop back right now - a switch the gate did not pace. Record it, and
     * end the run: carrying on until something re-activates the window would
     * be a switch back in, unpaced too. */
    if (m == WM_ACTIVATEAPP && !w && g_held) {
        vcr_pace_mark();
        g_focus_lost = 1;
    }
    return DefWindowProcA(h, m, w, l);
}

/* Once the foreground is lost, nothing more is dispatched: a re-activation
 * still queued would have the runtime switch the fullscreen mode back in,
 * unpaced, and the run is ending anyway. */
static void pump(void)
{
    MSG msg;
    while (!g_focus_lost && PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
}

static double now_s(void)
{
    LARGE_INTEGER f, t;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart / (double)f.QuadPart;
}

static int g_w = 640, g_h = 480, g_bpp = 16, g_frames = 120;
static long g_work_us;          /* --work-us: busy work after each Flip */
static int g_zbits = 32;        /* zsurf --zbits: 16, 24 or 32 (24+8 stencil) */
#define WORK_US_MAX 1000000

/* spin, touching nothing - the application "rendering" */
static void busy_us(long us)
{
    double end;
    if (us <= 0)
        return;
    end = now_s() + us / 1e6;
    while (now_s() < end)
        ;
}

/* the pattern for frame f: every byte of row 0 and of the middle row */
static unsigned char pat(int f, int x)
{
    return (unsigned char)((f * 37 + x * 11 + 5) & 0xff);
}

static void write_pattern(DDSURFACEDESC2 *d, int f)
{
    unsigned char *p = (unsigned char *)d->lpSurface;
    int rows[2] = { 0, (int)d->dwHeight / 2 }, r, x, bytes = (int)(d->dwWidth * (g_bpp / 8));
    for (r = 0; r < 2; r++)
        for (x = 0; x < bytes; x++)
            p[rows[r] * d->lPitch + x] = pat(f, x);
}

static int check_pattern(DDSURFACEDESC2 *d, int f)
{
    unsigned char *p = (unsigned char *)d->lpSurface;
    int rows[2] = { 0, (int)d->dwHeight / 2 }, r, x, bytes = (int)(d->dwWidth * (g_bpp / 8));
    for (r = 0; r < 2; r++)
        for (x = 0; x < bytes; x += 3)
            if (p[rows[r] * d->lPitch + x] != pat(f, x))
                return 0;
    return 1;
}

static const char *where(DWORD caps)
{
    return (caps & DDSCAPS_VIDEOMEMORY) ? "video" : (caps & DDSCAPS_SYSTEMMEMORY) ? "system" : "?";
}

/* ,"focus_lost":... for a RESULT, with an "error" when it was: the run was
 * cut short, and its numbers are not a pass */
static const char *focus_json(void)
{
    return g_focus_lost ? ",\"focus_lost\":true,\"error\":\"the window lost the foreground while"
                          " exclusive - run ended\"" : ",\"focus_lost\":false";
}

/* The paced switch out. Refused by vcr_pace.h (its lock busy, or the floor
 * never passed): neither RestoreDisplayMode nor a Release - either would give
 * the mode back unpaced - and a RESULT of its own says so (the host reads the
 * LAST one); XP reverts the mode as the process exits, after the exit hold. */
static int restore_mode(LPDIRECTDRAW7 dd, const char *mode)
{
    if (!vcr_pace_before_switch()) {
        say("RESULT {\"mode\":\"%s\",\"error\":\"RestoreDisplayMode not made: %s - the mode is"
            " left for the exit hold\"}", mode, g_vcr_pace_why);
        return 0;
    }
    g_held = 0;                         /* a deactivation from here on is ours */
    IDirectDraw7_RestoreDisplayMode(dd);
    vcr_pace_after_restore();
    return 1;
}

/* ---- sdlddraw: DOSBox 0.74's [sdl] output=ddraw, call for call ---------------------
 *
 * Descent (Games-Library/Descent1/dosboxD1.conf: output=ddraw, aspect=true,
 * fullresolution=original, fulldouble=false) scanned out a blank DOS screen
 * at 640x400x32 on .124 in 2 of 3 runs (2026-09-28 02:47, 06:57). 640x400 is
 * a mode that path never asks for: with aspect=true the ddraw output sets
 * 640x480 and STRETCHES DOSBox's 640x400 picture into it (the working 06:53
 * run's frame: 320x200 content, columns doubled, rows 2 or 3 times). Only its
 * fallback - "Failed to create ddraw surface, back to normal surface." in
 * GFX_SetSize (src/gui/sdlmain.cpp) - sets width x height = 640x400. So one of
 * the DirectDraw calls below failed there, and DOSBox then made no progress.
 *
 * This replays that path through the same interfaces SDL 1.2.13's DirectX 5
 * backend uses (src/video/windx5/SDL_dx5video.c) - IDirectDraw2 and
 * IDirectDrawSurface3, DDSURFACEDESC v1 - so the HAL can be judged without
 * DOSBox's own timing, and every step's HRESULT is in the RESULT:
 *   DX5_VideoInit     DirectDrawCreate + IDirectDraw2; EnumDisplayModes with
 *                     refresh rates, SDL's EnumModes2 rule for each mode's rate
 *   DX5_SetVideoMode  SetCooperativeLevel(FULLSCREEN|EXCLUSIVE|ALLOWREBOOT);
 *                     the window over the screen and SDL's foreground wait
 *                     (SDL loops FOREVER; bounded here, reported); SetDisplayMode
 *                     at that rate, then at 0; the primary (PRIMARYSURFACE |
 *                     VIDEOMEMORY, no back buffer: fulldouble=false); its Lock
 *                     (NOSYSLOCK|WAIT) must report the size and format asked
 *                     (DX5_AllocDDSurface); SDL_ClearSurface's colour fill
 *   GFX_SetSize       the blit surface: OFFSCREENPLAIN|VIDEOMEMORY at --src,
 *                     created WITH a pitch and a pixel format, checked in video
 *                     memory, locked once (DX5_AllocDDSurface again)
 *   every frame       Lock it (NOSYSLOCK|WAIT), write, Unlock, and
 *                     Blt(primary, (0,0,--res), blit, NULL, DDBLT_WAIT) - a
 *                     stretch when --src != --res - then the primary is read
 *                     back: each row must be a source row at most one off
 *                     the ideal mapping, pixel for sampled pixel.
 * --direct: the output=surface path DOSBox falls back to (SDL's shadow surface
 * copied into the locked primary each frame, SDL_UpdateRects) at --res - no
 * blit surface. `dosbox` in the RESULT names what DOSBox would have done:
 * "ddraw", or "surface-fallback:<the step that failed>". */

typedef struct { int w, h, bpp, rate; } sdl_mode;
#define SDL_MODES_MAX 512
static sdl_mode g_sdl_modes[SDL_MODES_MAX];
static int g_sdl_nmodes;
static DEVMODEA g_sdl_desk;
static int g_sw = 640, g_sh = 400, g_direct;

/* SDL_dx5video.c EnumModes2, rule for rule: a mode's refresh is the first one
 * enumerated for it, raised to a later one that is higher but not above the
 * desktop's refresh (85 for a mode larger than the desktop) - and only while
 * it is still the newest entry of its depth. */
static HRESULT WINAPI sdl_enum_modes(LPDDSURFACEDESC d, LPVOID ctx)
{
    int bpp = (int)d->ddpfPixelFormat.dwRGBBitCount, rate = (int)d->dwRefreshRate, maxr, i;
    (void)ctx;
    if (bpp != 8 && bpp != 16 && bpp != 24 && bpp != 32)
        return DDENUMRET_OK;
    maxr = d->dwWidth <= g_sdl_desk.dmPelsWidth && d->dwHeight <= g_sdl_desk.dmPelsHeight ?
           (int)g_sdl_desk.dmDisplayFrequency : 85;
    for (i = g_sdl_nmodes - 1; i >= 0 && g_sdl_modes[i].bpp != bpp; i--)
        ;
    if (i >= 0 && g_sdl_modes[i].w == (int)d->dwWidth && g_sdl_modes[i].h == (int)d->dwHeight) {
        if (rate > g_sdl_modes[i].rate && rate <= maxr)
            g_sdl_modes[i].rate = rate;
        return DDENUMRET_OK;
    }
    if (g_sdl_nmodes < SDL_MODES_MAX) {
        g_sdl_modes[g_sdl_nmodes].w = (int)d->dwWidth;
        g_sdl_modes[g_sdl_nmodes].h = (int)d->dwHeight;
        g_sdl_modes[g_sdl_nmodes].bpp = bpp;
        g_sdl_modes[g_sdl_nmodes].rate = rate;
        g_sdl_nmodes++;
    }
    return DDENUMRET_OK;
}

/* DX5_SetVideoMode's rate: the depth's list from its newest entry down, the
 * first w x h. Not listed: SDL_GetVideoMode would not even try the mode. */
static int sdl_mode_rate(int w, int h, int bpp, int *listed)
{
    int i;
    for (i = g_sdl_nmodes - 1; i >= 0; i--)
        if (g_sdl_modes[i].bpp == bpp && g_sdl_modes[i].w == w && g_sdl_modes[i].h == h) {
            *listed = 1;
            return g_sdl_modes[i].rate;
        }
    *listed = 0;
    return 0;
}

/* frame f's pixel at (x, row) of the picture DOSBox draws: every row and
 * column different, so a row taken from the wrong place is seen */
static DWORD sdl_px(int f, int row, int x, int bpp)
{
    DWORD v = (DWORD)row * 2654435761u + (DWORD)x * 40503u + (DWORD)f * 97u;
    return bpp == 16 ? (v & 0xffffu) : (v & 0x00ffffffu);
}

static DWORD sdl_get(const unsigned char *row, int x, int bpp)
{
    return bpp == 16 ? ((const USHORT *)row)[x] : (((const DWORD *)row)[x] & 0x00ffffffu);
}

static void sdl_draw(unsigned char *p, long pitch, int w, int h, int f, int bpp)
{
    int x, y;
    for (y = 0; y < h; y++, p += pitch)
        for (x = 0; x < w; x++) {
            if (bpp == 16)
                ((USHORT *)p)[x] = (USHORT)sdl_px(f, y, x, bpp);
            else
                ((DWORD *)p)[x] = sdl_px(f, y, x, bpp);
        }
}

/* Is destination row `row` (of dw x dh) some source row within one of the
 * ideal row, at the sampled columns, each within one of its ideal column?
 * Same size: the row itself, exactly. */
static int sdl_row_ok(const unsigned char *row, int dw, int dh, int sw, int sh, int dy, int f,
                      int bpp)
{
    const int cols[6] = { 0, 1, dw / 3, dw / 2, dw - 2, dw - 1 };
    int k, cand, ys, y0 = (int)((long)dy * sh / dh);
    for (cand = -1; cand <= 1; cand++) {
        int good = 1;
        ys = y0 + cand;
        if (ys < 0 || ys >= sh || ((sw == dw && sh == dh) && cand))
            continue;
        for (k = 0; k < 6 && good; k++) {
            int dx = cols[k];
            int xs0 = (int)((long)dx * sw / dw), c, hit = 0;
            DWORD v = sdl_get(row, dx, bpp);
            for (c = -1; c <= 1 && !hit; c++) {
                int xs = xs0 + c;
                if (xs < 0 || xs >= sw || ((sw == dw && sh == dh) && c))
                    continue;
                hit = v == sdl_px(f, ys, xs, bpp);
            }
            good = hit;
        }
        if (good)
            return 1;
    }
    return 0;
}

/* the paced give-back for the DirectDraw 2 object (restore_mode's twin) */
static int restore_mode2(LPDIRECTDRAW2 dd2)
{
    if (!vcr_pace_before_switch()) {
        say("RESULT {\"mode\":\"sdlddraw\",\"error\":\"RestoreDisplayMode not made: %s - the "
            "mode is left for the exit hold\"}", g_vcr_pace_why);
        return 0;
    }
    g_held = 0;
    IDirectDraw2_RestoreDisplayMode(dd2);
    vcr_pace_after_restore();
    return 1;
}

static int do_sdlddraw(DWORD hal_caps)
{
    WNDCLASSA wc;
    HWND hwnd;
    LPDIRECTDRAW dd1 = NULL;
    LPDIRECTDRAW2 dd2 = NULL;
    LPDIRECTDRAWSURFACE s1 = NULL;
    LPDIRECTDRAWSURFACE3 prim = NULL, blit = NULL;
    DDSURFACEDESC sd;
    DDPIXELFORMAT pf;
    DDSCAPS caps;
    DDBLTFX fx;
    RECT r;
    HRESULT hr, hr_coop = 0, hr_mode = 0, hr_mode0 = 0, hr_prim = 0, hr_plock = 0, hr_fill = 0;
    HRESULT hr_blit = 0, hr_block = 0, hr_first_blt = 0, hr_first_lock = 0;
    const char *failed = NULL, *blit_in = "none", *verdict;
    int rate, listed, fg_ok = 0, fg_ms = 0, f, bad_rows = 0, lock_fail = 0, blt_fail = 0;
    int lost = 0, fill_bad = 0, prim_w = 0, prim_h = 0, prim_bpp = 0, bytes = g_bpp / 8, y;
    long ppitch = 0, bpitch = 0, from_primary = 0;
    unsigned char *pp = NULL, *pb = NULL;
    double t0, t = 0;

    if (g_bpp != 16 && g_bpp != 32) {
        say("RESULT {\"mode\":\"sdlddraw\",\"error\":\"--bpp %d: DOSBox's ddraw output draws "
            "15/16 or 32 bpp - use 16 or 32\"}", g_bpp);
        return 2;
    }
    if (g_sw <= 0 || g_sh <= 0 || g_sw > 2048 || g_sh > 2048) {
        say("RESULT {\"mode\":\"sdlddraw\",\"error\":\"--src %dx%d\"}", g_sw, g_sh);
        return 2;
    }
    /* DX5_VideoInit: DirectDrawCreate, then the DirectDraw 2 interface */
    hr = DirectDrawCreate(NULL, &dd1, NULL);
    if (SUCCEEDED(hr)) {
        hr = IDirectDraw_QueryInterface(dd1, &IID_IDirectDraw2, (void **)&dd2);
        IDirectDraw_Release(dd1);
    }
    if (FAILED(hr) || !dd2) {
        say("RESULT {\"mode\":\"sdlddraw\",\"error\":\"DirectDrawCreate/IDirectDraw2 %08lx\"}", hr);
        return 3;
    }
    memset(&g_sdl_desk, 0, sizeof g_sdl_desk);
    g_sdl_desk.dmSize = sizeof g_sdl_desk;
    EnumDisplaySettingsA(NULL, ENUM_CURRENT_SETTINGS, &g_sdl_desk);
    IDirectDraw2_EnumDisplayModes(dd2, DDEDM_REFRESHRATES, NULL, NULL, sdl_enum_modes);
    rate = sdl_mode_rate(g_w, g_h, g_bpp, &listed);
    say("desktop %lux%lux%lu@%lu; %d modes; %dx%dx%d %s, SDL's rate %d", g_sdl_desk.dmPelsWidth,
        g_sdl_desk.dmPelsHeight, g_sdl_desk.dmBitsPerPel, g_sdl_desk.dmDisplayFrequency,
        g_sdl_nmodes, g_w, g_h, g_bpp, listed ? "listed" : "NOT LISTED", rate);
    if (!listed) {
        say("RESULT {\"mode\":\"sdlddraw\",\"res\":\"%dx%dx%d\",\"listed\":false,"
            "\"dosbox\":\"surface-fallback:mode not listed\",\"error\":\"%dx%dx%d is not in "
            "the driver's mode list\"}", g_w, g_h, g_bpp, g_w, g_h, g_bpp);
        IDirectDraw2_Release(dd2);
        return 4;
    }

    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "ddlab_sdl";
    RegisterClassA(&wc);
    hwnd = CreateWindowExA(0, "ddlab_sdl", "ddlab sdlddraw", WS_POPUP, 0, 0, g_w, g_h, NULL,
                           NULL, wc.hInstance, NULL);
    /* DX5_SetVideoMode: the cooperative level first, then the window over the
     * screen and the wait for the foreground, then the mode */
    hr_coop = IDirectDraw2_SetCooperativeLevel(dd2, hwnd, DDSCL_FULLSCREEN | DDSCL_EXCLUSIVE |
                                               DDSCL_ALLOWREBOOT);
    say("SetCooperativeLevel -> %08lx", hr_coop);
    if (FAILED(hr_coop))
        failed = "SetCooperativeLevel";
    if (!failed) {
        SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, GetSystemMetrics(SM_CXSCREEN),
                     GetSystemMetrics(SM_CYSCREEN), SWP_NOCOPYBITS);
        ShowWindow(hwnd, SW_SHOW);
        /* SDL: while (GetForegroundWindow() != SDL_Window) { SetForegroundWindow;
         * SDL_Delay(100); } - with no bound. 10 s here, and said. */
        for (fg_ms = 0; fg_ms < 10000; fg_ms += 100) {
            pump();
            if (GetForegroundWindow() == hwnd) {
                fg_ok = 1;
                break;
            }
            SetForegroundWindow(hwnd);
            Sleep(100);
        }
        say("foreground: %s after %d ms", fg_ok ? "yes" : "NO (SDL would wait here forever)",
            fg_ms);
        /* DOSBox would stop HERE, for good; the lab goes on, so the HAL's
         * own steps are still judged - the verdict says "hang" */
    }
    if (!failed) {
        if (!vcr_pace_before_switch()) {
            say("RESULT {\"mode\":\"sdlddraw\",\"error\":\"SetDisplayMode not made: %s\"}",
                g_vcr_pace_why);
            return 4;
        }
        hr_mode = IDirectDraw2_SetDisplayMode(dd2, g_w, g_h, g_bpp, rate, 0);
        vcr_pace_after_switch();
        g_held = 1;
        say("SetDisplayMode(%dx%dx%d @%d) -> %08lx", g_w, g_h, g_bpp, rate, hr_mode);
        if (FAILED(hr_mode) && rate) {
            if (!vcr_pace_before_switch()) {
                say("RESULT {\"mode\":\"sdlddraw\",\"error\":\"SetDisplayMode (rate 0) not made:"
                    " %s\"}", g_vcr_pace_why);
                return 4;
            }
            hr_mode0 = IDirectDraw2_SetDisplayMode(dd2, g_w, g_h, g_bpp, 0, 0);
            vcr_pace_after_switch();
            say("SetDisplayMode(%dx%dx%d @0) -> %08lx", g_w, g_h, g_bpp, hr_mode0);
            if (FAILED(hr_mode0))
                failed = "SetDisplayMode (SDL tries a window)";
        } else if (FAILED(hr_mode)) {
            failed = "SetDisplayMode (SDL tries a window)";
        }
    }
    if (!failed) {
        memset(&sd, 0, sizeof sd);
        sd.dwSize = sizeof sd;
        sd.dwFlags = DDSD_CAPS;
        sd.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE | DDSCAPS_VIDEOMEMORY;
        hr_prim = IDirectDraw2_CreateSurface(dd2, &sd, &s1, NULL);
        if (SUCCEEDED(hr_prim)) {
            hr_prim = IDirectDrawSurface_QueryInterface(s1, &IID_IDirectDrawSurface3,
                                                        (void **)&prim);
            IDirectDrawSurface_Release(s1);
        }
        say("CreateSurface(primary) -> %08lx", hr_prim);
        if (FAILED(hr_prim) || !prim)
            failed = "CreateSurface(primary)";
    }
    memset(&pf, 0, sizeof pf);
    if (!failed) {
        memset(&sd, 0, sizeof sd);
        sd.dwSize = sizeof sd;
        sd.dwFlags = DDSD_PIXELFORMAT | DDSD_CAPS;
        hr = IDirectDrawSurface3_GetSurfaceDesc(prim, &sd);
        pf = sd.ddpfPixelFormat;
        if (FAILED(hr) || !(pf.dwFlags & DDPF_RGB))
            failed = "primary not RGB";
        else if (!(sd.ddsCaps.dwCaps & DDSCAPS_VIDEOMEMORY))
            failed = "primary not in video memory";
    }
    if (!failed) {
        /* DX5_AllocDDSurface on the primary: one Lock, and it must say what
         * was asked */
        memset(&sd, 0, sizeof sd);
        sd.dwSize = sizeof sd;
        hr_plock = IDirectDrawSurface3_Lock(prim, NULL, &sd, DDLOCK_NOSYSLOCK | DDLOCK_WAIT, NULL);
        if (SUCCEEDED(hr_plock)) {
            pp = (unsigned char *)sd.lpSurface;
            ppitch = sd.lPitch;
            prim_w = (int)sd.dwWidth;
            prim_h = (int)sd.dwHeight;
            prim_bpp = (int)sd.ddpfPixelFormat.dwRGBBitCount;
            IDirectDrawSurface3_Unlock(prim, NULL);
        }
        say("primary Lock -> %08lx: %dx%dx%d pitch %ld at %p", hr_plock, prim_w, prim_h, prim_bpp,
            ppitch, pp);
        if (FAILED(hr_plock))
            failed = "primary Lock";
        else if (prim_w != g_w || prim_h != g_h)
            failed = "primary size (DDraw created surface with wrong size)";
        else if (prim_bpp != g_bpp)
            failed = "primary depth";
    }
    if (!failed) {
        /* SDL_ClearSurface: DX5_FillHWRect, a DDBLTFX with only its size and
         * colour set - the rest is whatever was on SDL's stack */
        memset(&fx, 0xcc, sizeof fx);
        fx.dwSize = sizeof fx;
        fx.dwFillColor = 0;
        SetRect(&r, 0, 0, g_w, g_h);
        hr_fill = IDirectDrawSurface3_Blt(prim, &r, NULL, NULL, DDBLT_COLORFILL | DDBLT_WAIT, &fx);
        say("Blt COLORFILL (SDL_ClearSurface) -> %08lx", hr_fill);
        memset(&sd, 0, sizeof sd);
        sd.dwSize = sizeof sd;
        if (SUCCEEDED(IDirectDrawSurface3_Lock(prim, NULL, &sd, DDLOCK_NOSYSLOCK | DDLOCK_WAIT,
                                               NULL))) {
            for (y = 0; y < g_h; y += 7) {
                const unsigned char *row = (const unsigned char *)sd.lpSurface + y * sd.lPitch;
                if (sdl_get(row, 0, g_bpp) || sdl_get(row, g_w / 2, g_bpp) ||
                    sdl_get(row, g_w - 1, g_bpp))
                    fill_bad++;
            }
            IDirectDrawSurface3_Unlock(prim, NULL);
        }
    }
    if (!failed && !g_direct) {
        /* GFX_SetSize, SCREEN_SURFACE_DDRAW: SDL_CreateRGBSurface(SDL_HWSURFACE)
         * -> DX5_AllocDDSurface: width, height, caps, a PITCH and a pixel format */
        memset(&sd, 0, sizeof sd);
        sd.dwSize = sizeof sd;
        sd.dwFlags = DDSD_WIDTH | DDSD_HEIGHT | DDSD_CAPS | DDSD_PITCH | DDSD_PIXELFORMAT;
        sd.dwWidth = g_sw;
        sd.dwHeight = g_sh;
        sd.lPitch = (g_sw * bytes + 3) & ~3;            /* SDL_CalculatePitch */
        sd.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_VIDEOMEMORY;
        sd.ddpfPixelFormat.dwSize = sizeof sd.ddpfPixelFormat;
        sd.ddpfPixelFormat.dwFlags = DDPF_RGB;
        sd.ddpfPixelFormat.dwRGBBitCount = g_bpp;
        sd.ddpfPixelFormat.dwRBitMask = pf.dwRBitMask;
        sd.ddpfPixelFormat.dwGBitMask = pf.dwGBitMask;
        sd.ddpfPixelFormat.dwBBitMask = pf.dwBBitMask;
        hr_blit = IDirectDraw2_CreateSurface(dd2, &sd, &s1, NULL);
        if (SUCCEEDED(hr_blit)) {
            hr_blit = IDirectDrawSurface_QueryInterface(s1, &IID_IDirectDrawSurface3,
                                                        (void **)&blit);
            IDirectDrawSurface_Release(s1);
        }
        say("CreateSurface(blit %dx%d, video) -> %08lx", g_sw, g_sh, hr_blit);
        if (FAILED(hr_blit) || !blit) {
            failed = "CreateSurface(blit)";
        } else {
            memset(&caps, 0, sizeof caps);
            IDirectDrawSurface3_GetCaps(blit, &caps);
            blit_in = where(caps.dwCaps);
            if (!(caps.dwCaps & DDSCAPS_VIDEOMEMORY))
                failed = "blit not in video memory (No room in video memory)";
        }
    }
    if (!failed && !g_direct) {
        memset(&sd, 0, sizeof sd);
        sd.dwSize = sizeof sd;
        hr_block = IDirectDrawSurface3_Lock(blit, NULL, &sd, DDLOCK_NOSYSLOCK | DDLOCK_WAIT, NULL);
        if (SUCCEEDED(hr_block)) {
            pb = (unsigned char *)sd.lpSurface;
            bpitch = sd.lPitch;
            if ((int)sd.dwWidth != g_sw || (int)sd.dwHeight != g_sh)
                failed = "blit size";
            else if ((int)sd.ddpfPixelFormat.dwRGBBitCount != g_bpp ||
                     sd.ddpfPixelFormat.dwRBitMask != pf.dwRBitMask ||
                     sd.ddpfPixelFormat.dwGBitMask != pf.dwGBitMask ||
                     sd.ddpfPixelFormat.dwBBitMask != pf.dwBBitMask)
                failed = "blit format (DDraw didn't use SDL surface description)";
            IDirectDrawSurface3_Unlock(blit, NULL);
        } else {
            failed = "blit Lock";
        }
        /* where the runtime put it: relative to the primary (at the top) */
        from_primary = pp && pb ? (long)(pb - pp) : 0;
        say("blit Lock -> %08lx: pitch %ld at %p (%+ld from the primary)", hr_block, bpitch, pb,
            from_primary);
    }

    t0 = now_s();
    for (f = 0; !failed && f < g_frames && !g_focus_lost; f++) {
        memset(&sd, 0, sizeof sd);
        sd.dwSize = sizeof sd;
        if (g_direct) {
            /* the fallback: SDL_UpdateRects copies the shadow into the
             * locked primary */
            hr = IDirectDrawSurface3_Lock(prim, NULL, &sd, DDLOCK_NOSYSLOCK | DDLOCK_WAIT, NULL);
            if (FAILED(hr)) {
                if (!lock_fail++)
                    hr_first_lock = hr;
                if (hr == DDERR_SURFACELOST) {
                    lost++;
                    IDirectDrawSurface3_Restore(prim);
                }
                continue;
            }
            sdl_draw((unsigned char *)sd.lpSurface, sd.lPitch, g_w, g_h, f, g_bpp);
            IDirectDrawSurface3_Unlock(prim, NULL);
        } else {
            /* GFX_StartUpdate: SDL_LockSurface(blit) */
            hr = IDirectDrawSurface3_Lock(blit, NULL, &sd, DDLOCK_NOSYSLOCK | DDLOCK_WAIT, NULL);
            if (FAILED(hr)) {
                if (!lock_fail++)
                    hr_first_lock = hr;
                if (hr == DDERR_SURFACELOST) {
                    lost++;
                    IDirectDrawSurface3_Restore(blit);
                }
                continue;
            }
            sdl_draw((unsigned char *)sd.lpSurface, sd.lPitch, g_sw, g_sh, f, g_bpp);
            IDirectDrawSurface3_Unlock(blit, NULL);
            /* GFX_EndUpdate: the blit to the primary, DDBLT_WAIT, no source rect */
            SetRect(&r, 0, 0, g_w, g_h);
            hr = IDirectDrawSurface3_Blt(prim, &r, blit, NULL, DDBLT_WAIT, NULL);
            if (FAILED(hr)) {
                if (!blt_fail++)
                    hr_first_blt = hr;
                if (hr == DDERR_SURFACELOST) {      /* what DOSBox does */
                    lost++;
                    IDirectDrawSurface3_Restore(blit);
                    IDirectDrawSurface3_Restore(prim);
                }
                continue;
            }
        }
        /* the check: the primary is what is scanned out */
        memset(&sd, 0, sizeof sd);
        sd.dwSize = sizeof sd;
        if (FAILED(hr = IDirectDrawSurface3_Lock(prim, NULL, &sd, DDLOCK_NOSYSLOCK | DDLOCK_WAIT,
                                                 NULL))) {
            if (!lock_fail++)
                hr_first_lock = hr;
            continue;
        }
        for (y = 0; y < g_h; y++) {
            const unsigned char *row = (const unsigned char *)sd.lpSurface + y * sd.lPitch;
            if (!(g_direct ? sdl_row_ok(row, g_w, g_h, g_w, g_h, y, f, g_bpp)
                           : sdl_row_ok(row, g_w, g_h, g_sw, g_sh, y, f, g_bpp))) {
                if (!bad_rows)
                    say("  frame %d row %d: not the picture (%08lx at x 0)", f, y,
                        sdl_get(row, 0, g_bpp));
                bad_rows++;
            }
        }
        IDirectDrawSurface3_Unlock(prim, NULL);
        if ((f & 15) == 0)
            pump();
    }
    t = now_s() - t0;
    pump();
    /* what DOSBox would have done: SDL's foreground wait comes before
     * everything after SetCooperativeLevel, and never ends */
    if (SUCCEEDED(hr_coop) && !fg_ok)
        verdict = "hang:foreground - SDL waits for it forever";
    else if (failed)
        verdict = g_direct ? "setup failed" : "surface-fallback";
    else
        verdict = g_direct ? "surface" : "ddraw";
    if (SUCCEEDED(hr_coop) && !fg_ok && !failed)
        failed = "foreground never came (SDL hangs)";
    say("RESULT {\"mode\":\"sdlddraw\",\"path\":\"%s\",\"res\":\"%dx%dx%d\",\"src\":\"%dx%d\","
        "\"desktop\":\"%lux%lux%lu@%lu\",\"rate\":%d,\"coop\":\"%08lx\",\"foreground\":%s,"
        "\"foreground_ms\":%d,\"setmode\":\"%08lx\",\"setmode0\":\"%08lx\",\"primary\":\"%08lx\","
        "\"primary_lock\":\"%08lx\",\"primary_size\":\"%dx%dx%d\",\"primary_pitch\":%ld,"
        "\"fill\":\"%08lx\",\"fill_bad\":%d,\"blit\":\"%08lx\",\"blit_in\":\"%s\","
        "\"blit_lock\":\"%08lx\",\"blit_pitch\":%ld,\"blit_from_primary\":%ld,"
        "\"frames\":%d,\"frames_run\":%d,\"bad_rows\":%d,\"lock_fail\":%d,\"first_lock\":\"%08lx\","
        "\"blt_fail\":%d,\"first_blt\":\"%08lx\",\"lost\":%d,\"fps\":%.1f,\"hal_caps\":\"%08lx\","
        "\"dosbox\":\"%s%s%s\"%s%s%s%s}",
        g_direct ? "surface" : "ddraw", g_w, g_h, g_bpp, g_sw, g_sh, g_sdl_desk.dmPelsWidth,
        g_sdl_desk.dmPelsHeight, g_sdl_desk.dmBitsPerPel, g_sdl_desk.dmDisplayFrequency, rate,
        hr_coop, fg_ok ? "true" : "false", fg_ms, hr_mode, hr_mode0, hr_prim, hr_plock, prim_w,
        prim_h, prim_bpp, ppitch, hr_fill, fill_bad, hr_blit, blit_in, hr_block, bpitch,
        from_primary, g_frames, f, bad_rows, lock_fail, hr_first_lock, blt_fail, hr_first_blt,
        lost, t > 0 ? f / t : 0.0, hal_caps,
        verdict, failed && !strchr(verdict, ':') ? ":" : "",
        failed && !strchr(verdict, ':') ? failed : "", failed ? ",\"error\":\"" : "",
        failed ? failed : "", failed ? "\"" : "", focus_json());
    if (blit)
        IDirectDrawSurface3_Release(blit);
    if (prim)
        IDirectDrawSurface3_Release(prim);
    if (g_held && !restore_mode2(dd2))
        return 8;
    IDirectDraw2_SetCooperativeLevel(dd2, hwnd, DDSCL_NORMAL);
    IDirectDraw2_Release(dd2);
    DestroyWindow(hwnd);
    return g_focus_lost ? 9 : failed ? 5 : bad_rows || lock_fail || blt_fail || fill_bad ? 7 : 0;
}

int main(int argc, char **argv)
{
    const char *mode = "caps";
    LPDIRECTDRAW7 dd = NULL;
    DDCAPS hal, hel;
    DWORD total = 0, freem = 0;
    DDSCAPS2 vm;
    HRESULT hr;
    int i;
    DWORD pace;
    const char *bad_pace = NULL, *bad_work = NULL, *bad_zbits = NULL;

    /* A crash must die at once, not sit behind a Watson / "has encountered a
     * problem" box: that box keeps the process - and the exclusive mode it
     * set - alive until someone at the box clicks it, and a fullscreen mode
     * hides it. The box is driven remotely; nobody is at .124's CRT. */
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    for (i = 1; i < argc; i++) {
        const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(a, "--res") && v) { sscanf(v, "%dx%d", &g_w, &g_h); i++; }
        else if (!strcmp(a, "--bpp") && v) { g_bpp = atoi(v); i++; }
        else if (!strcmp(a, "--frames") && v) { g_frames = atoi(v); i++; }
        else if (!strcmp(a, "--src") && v) { sscanf(v, "%dx%d", &g_sw, &g_sh); i++; }
        else if (!strcmp(a, "--direct")) g_direct = 1;
        else if (!strcmp(a, "--work-us") && v) {
            char *end = NULL;
            long n = strtol(v, &end, 10);
            if (end == v || *end || n < 0 || n > WORK_US_MAX)
                bad_work = v;
            else
                g_work_us = n;
            i++;
        }
        else if (!strcmp(a, "--zbits") && v) {
            if (!strcmp(v, "16") || !strcmp(v, "24") || !strcmp(v, "32"))
                g_zbits = atoi(v);
            else
                bad_zbits = v;
            i++;
        }
        else if (!strcmp(a, "--pace") && v) {
            /* atoi("-1") was a floor of 0xFFFFFFFF ms: refused, not wrapped */
            if (vcr_pace_parse_ms(v, &pace))
                vcr_pace_set_min(pace);
            else
                bad_pace = v;
            i++;
        }
        else if (!strcmp(a, "--log") && v) { strncpy(g_logpath, v, sizeof g_logpath - 1); i++; }
        else if (a[0] != '-') mode = a;
    }
    g_log = fopen(g_logpath, "w");
    say("ddlab %s: %dx%dx%d frames %d work %ld us", mode, g_w, g_h, g_bpp, g_frames, g_work_us);
    if (bad_pace) {
        say("RESULT {\"mode\":\"%s\",\"error\":\"--pace %s: decimal milliseconds, 0 to %u\"}",
            mode, bad_pace, VCR_PACE_MAX_MS);
        return 2;
    }
    if (bad_work) {
        say("RESULT {\"mode\":\"%s\",\"error\":\"--work-us %s: decimal microseconds, 0 to %d\"}",
            mode, bad_work, WORK_US_MAX);
        return 2;
    }
    if (bad_zbits) {
        say("RESULT {\"mode\":\"%s\",\"error\":\"--zbits %s: 16, 24 or 32\"}", mode, bad_zbits);
        return 2;
    }

    hr = DirectDrawCreateEx(NULL, (void **)&dd, &IID_IDirectDraw7, NULL);
    if (FAILED(hr)) {
        say("RESULT {\"mode\":\"%s\",\"error\":\"DirectDrawCreateEx %08lx\"}", mode, hr);
        return 3;
    }
    memset(&hal, 0, sizeof hal);
    memset(&hel, 0, sizeof hel);
    hal.dwSize = hel.dwSize = sizeof hal;
    IDirectDraw7_GetCaps(dd, &hal, &hel);
    memset(&vm, 0, sizeof vm);
    vm.dwCaps = DDSCAPS_VIDEOMEMORY;
    IDirectDraw7_GetAvailableVidMem(dd, &vm, &total, &freem);
    say("HAL caps %08lx ddsCaps %08lx vidmem %lu/%lu; HEL caps %08lx", hal.dwCaps,
        hal.ddsCaps.dwCaps, freem, total, hel.dwCaps);

    if (!strcmp(mode, "caps")) {
        say("RESULT {\"mode\":\"caps\",\"hal_caps\":\"%08lx\",\"hal_ddscaps\":\"%08lx\","
            "\"vidmem_total\":%lu,\"vidmem_free\":%lu,\"hel_caps\":\"%08lx\"}",
            hal.dwCaps, hal.ddsCaps.dwCaps, total, freem, hel.dwCaps);
        IDirectDraw7_Release(dd);
        return 0;
    }

    if (!strcmp(mode, "sdlddraw")) {
        DWORD hc = hal.dwCaps;
        /* SDL makes ONE DirectDraw object, through DirectDrawCreate and the
         * DirectDraw 2 interface: this DirectDraw 7 one goes first */
        IDirectDraw7_Release(dd);
        return do_sdlddraw(hc);
    }

    if (!strcmp(mode, "vidmem")) {
        /* No window, no exclusive mode, no SetDisplayMode: nothing switches.
         * Fills video memory with offscreen surfaces until it runs out, then
         * checks every one through the CPU (Lock) and through the 2D engine
         * (a colour fill, then a copy from its neighbour): the addresses
         * above 32 MB a chip that the V5 6000's 256 MB mode puts in reach -
         * a 25-bit address anywhere on that path shows up as bad surfaces
         * at the top of the heap only (2026-09-27). */
        enum { VM_MAX = 128, VW = 512, VH = 256 };
        static LPDIRECTDRAWSURFACE7 s[VM_MAX];
        static unsigned char *ptr[VM_MAX];
        static long pitch[VM_MAX];
        LPDIRECTDRAWSURFACE7 prim = NULL;
        DDSURFACEDESC2 sd;
        DDBLTFX fx;
        RECT r;
        unsigned char *pp = NULL;
        long lo = 0, hi = 0, off;
        int n = 0, i, x, y, bpp_b = 0, bad_cpu = 0, bad_fill = 0, bad_copy = 0, blt_fail = 0;
        int bad_surf_cpu = 0, bad_surf_eng = 0;
        HRESULT coop = IDirectDraw7_SetCooperativeLevel(dd, NULL, DDSCL_NORMAL);
#define VM_PAT(i, x, y) ((unsigned char)((i) * 37 + (x) * 7 + (y) * 13))
        memset(&sd, 0, sizeof sd);
        sd.dwSize = sizeof sd;
        sd.dwFlags = DDSD_CAPS;
        sd.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE;
        if (SUCCEEDED(IDirectDraw7_CreateSurface(dd, &sd, &prim, NULL))) {
            memset(&sd, 0, sizeof sd);
            sd.dwSize = sizeof sd;
            if (SUCCEEDED(IDirectDrawSurface7_Lock(prim, NULL, &sd, DDLOCK_WAIT, NULL))) {
                pp = sd.lpSurface;
                bpp_b = (int)sd.ddpfPixelFormat.dwRGBBitCount / 8;
                IDirectDrawSurface7_Unlock(prim, NULL);
            }
        }
        for (n = 0; n < VM_MAX; n++) {
            memset(&sd, 0, sizeof sd);
            sd.dwSize = sizeof sd;
            sd.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT;
            sd.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_VIDEOMEMORY;
            sd.dwWidth = VW;
            sd.dwHeight = VH;
            if (FAILED(IDirectDraw7_CreateSurface(dd, &sd, &s[n], NULL)))
                break;
            memset(&sd, 0, sizeof sd);
            sd.dwSize = sizeof sd;
            if (FAILED(IDirectDrawSurface7_Lock(s[n], NULL, &sd, DDLOCK_WAIT, NULL))) {
                IDirectDrawSurface7_Release(s[n]);
                break;
            }
            ptr[n] = sd.lpSurface;
            pitch[n] = sd.lPitch;
            if (!bpp_b)
                bpp_b = (int)sd.ddpfPixelFormat.dwRGBBitCount / 8;
            /* the CPU writes surface n's own pattern */
            for (y = 0; y < VH; y++)
                for (x = 0; x < VW * bpp_b; x++)
                    ptr[n][y * pitch[n] + x] = VM_PAT(n, x, y);
            IDirectDrawSurface7_Unlock(s[n], NULL);
            if (pp) {
                off = (long)(ptr[n] - pp);
                if (!n || off < lo) lo = off;
                if (!n || off > hi) hi = off;
            }
        }
        say("vidmem: %d surfaces of %dx%d x %d bytes (coop %08lx), offsets from the primary "
            "%ld .. %ld", n, VW, VH, bpp_b, coop, lo, hi);
        /* 1. every surface read back through the CPU */
        for (i = 0; i < n; i++) {
            int b0 = bad_cpu;
            memset(&sd, 0, sizeof sd);
            sd.dwSize = sizeof sd;
            IDirectDrawSurface7_Lock(s[i], NULL, &sd, DDLOCK_WAIT, NULL);
            for (y = 0; y < VH; y++)
                for (x = 0; x < VW * bpp_b; x += 3)
                    if (((unsigned char *)sd.lpSurface)[y * sd.lPitch + x] != VM_PAT(i, x, y))
                        bad_cpu++;
            IDirectDrawSurface7_Unlock(s[i], NULL);
            if (bad_cpu != b0) {
                bad_surf_cpu++;
                say("  surface %d (+%ld from the primary): %d bad CPU bytes", i,
                    pp ? (long)(ptr[i] - pp) : 0, bad_cpu - b0);
            }
        }
        /* 2. the engine fills a rectangle of every surface */
        memset(&fx, 0, sizeof fx);
        fx.dwSize = sizeof fx;
        fx.dwFillColor = 0x5a5a5a5au;
        SetRect(&r, 32, 32, 96, 64);
        for (i = 0; i < n; i++)
            if (FAILED(IDirectDrawSurface7_Blt(s[i], &r, NULL, NULL, DDBLT_COLORFILL | DDBLT_WAIT, &fx)))
                blt_fail++;
        for (i = 0; i < n; i++) {
            int b0 = bad_fill;
            memset(&sd, 0, sizeof sd);
            sd.dwSize = sizeof sd;
            IDirectDrawSurface7_Lock(s[i], NULL, &sd, DDLOCK_WAIT, NULL);
            for (y = 0; y < VH; y += 2)
                for (x = 0; x < VW * bpp_b; x += 3) {
                    int in = y >= 32 && y < 64 && x >= 32 * bpp_b && x < 96 * bpp_b;
                    unsigned char v = ((unsigned char *)sd.lpSurface)[y * sd.lPitch + x];
                    if (in ? v != 0x5a : v != VM_PAT(i, x, y))
                        bad_fill++;
                }
            IDirectDrawSurface7_Unlock(s[i], NULL);
            if (bad_fill != b0) {
                bad_surf_eng++;
                say("  surface %d (+%ld): %d bad bytes after the engine's fill", i,
                    pp ? (long)(ptr[i] - pp) : 0, bad_fill - b0);
            }
        }
        /* 3. the engine copies surface i into surface i-1, in rising order,
         * so each source is still its own when it is read */
        for (i = 1; i < n; i++)
            if (FAILED(IDirectDrawSurface7_Blt(s[i - 1], NULL, s[i], NULL, DDBLT_WAIT, NULL)))
                blt_fail++;
        for (i = 1; i < n; i++) {
            int b0 = bad_copy;
            memset(&sd, 0, sizeof sd);
            sd.dwSize = sizeof sd;
            IDirectDrawSurface7_Lock(s[i - 1], NULL, &sd, DDLOCK_WAIT, NULL);
            for (y = 0; y < VH; y += 2)
                for (x = 0; x < VW * bpp_b; x += 3) {
                    int in = y >= 32 && y < 64 && x >= 32 * bpp_b && x < 96 * bpp_b;
                    unsigned char v = ((unsigned char *)sd.lpSurface)[y * sd.lPitch + x];
                    if (in ? v != 0x5a : v != VM_PAT(i, x, y))
                        bad_copy++;
                }
            IDirectDrawSurface7_Unlock(s[i - 1], NULL);
            if (bad_copy != b0) {
                bad_surf_eng++;
                say("  surface %d <- %d (+%ld <- +%ld): %d bad bytes after the engine's copy",
                    i - 1, i, pp ? (long)(ptr[i - 1] - pp) : 0, pp ? (long)(ptr[i] - pp) : 0,
                    bad_copy - b0);
            }
        }
        say("RESULT {\"mode\":\"vidmem\",\"surfaces\":%d,\"surface_bytes\":%d,\"bpp\":%d,"
            "\"from_primary_lo\":%ld,\"from_primary_hi\":%ld,\"vidmem_total\":%lu,"
            "\"bad_cpu\":%d,\"bad_fill\":%d,\"bad_copy\":%d,\"blt_fail\":%d,"
            "\"bad_surfaces_cpu\":%d,\"bad_surfaces_engine\":%d}",
            n, VW * VH * bpp_b, bpp_b * 8, lo, hi, total, bad_cpu, bad_fill, bad_copy, blt_fail,
            bad_surf_cpu, bad_surf_eng);
        for (i = 0; i < n; i++)
            IDirectDrawSurface7_Release(s[i]);
        if (prim)
            IDirectDrawSurface7_Release(prim);
        IDirectDraw7_Release(dd);
        return (n && !bad_cpu && !bad_fill && !bad_copy && !blt_fail) ? 0 : 1;
    }

    if (!strcmp(mode, "zsurf")) {
        /* no window, no exclusive mode, no SetDisplayMode: nothing switches */
        LPDIRECTDRAWSURFACE7 z = NULL;
        DDSURFACEDESC2 sd;
        HRESULT coop = IDirectDraw7_SetCooperativeLevel(dd, NULL, DDSCL_NORMAL);
        memset(&sd, 0, sizeof sd);
        sd.dwSize = sizeof sd;
        sd.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
        sd.ddsCaps.dwCaps = DDSCAPS_ZBUFFER | DDSCAPS_VIDEOMEMORY;
        sd.dwWidth = sd.dwHeight = 256;
        sd.ddpfPixelFormat.dwSize = sizeof sd.ddpfPixelFormat;
        sd.ddpfPixelFormat.dwFlags = DDPF_ZBUFFER;
        sd.ddpfPixelFormat.dwZBufferBitDepth = (DWORD)g_zbits;
        if (g_zbits == 32) {            /* D24S8: depth low, stencil in the top byte */
            sd.ddpfPixelFormat.dwFlags |= DDPF_STENCILBUFFER;
            sd.ddpfPixelFormat.dwStencilBitDepth = 8;
            sd.ddpfPixelFormat.dwZBitMask = 0x00ffffff;
            sd.ddpfPixelFormat.dwStencilBitMask = 0xff000000;
        } else {
            sd.ddpfPixelFormat.dwZBitMask = g_zbits == 24 ? 0x00ffffff : 0x0000ffff;
        }
        say("SetCooperativeLevel(NORMAL) -> %08lx", coop);
        hr = IDirectDraw7_CreateSurface(dd, &sd, &z, NULL);
        say("CreateSurface(Z %d bits, video memory) -> %08lx", g_zbits, hr);
        if (SUCCEEDED(hr) && z) {
            memset(&sd, 0, sizeof sd);
            sd.dwSize = sizeof sd;
            IDirectDrawSurface7_GetSurfaceDesc(z, &sd);
        }
        say("RESULT {\"mode\":\"zsurf\",\"zbits\":%d,\"hr\":\"%08lx\",\"created\":%d,"
            "\"in\":\"%s\",\"hal_caps\":\"%08lx\"}", g_zbits, hr, SUCCEEDED(hr) && z ? 1 : 0,
            SUCCEEDED(hr) && z ? where(sd.ddsCaps.dwCaps) : "none", hal.dwCaps);
        if (z)
            IDirectDrawSurface7_Release(z);
        IDirectDraw7_Release(dd);
        return 0;
    }

    if (!strcmp(mode, "flip")) {
        WNDCLASSA wc;
        HWND hwnd;
        LPDIRECTDRAWSURFACE7 prim = NULL, back = NULL;
        DDSURFACEDESC2 sd;
        DDSCAPS2 bc;
        int f, bad = 0, lockfail = 0;
        const char *prim_in;
        HRESULT lhr = 0;
        double t0, t, vb0, vbt = 0;
        int vbn = 0;
        DWORD scan = 0;
        /* when each Flip returned: the per-frame summary */
        double *ft = (double *)malloc(sizeof(double) * (size_t)(g_frames > 0 ? g_frames : 1));
        int nft = 0, slow = -1, fast = -1;
        double first_ms = 0, max_ms = 0, min_ms = 0, ff_rate = 0, period;

        memset(&wc, 0, sizeof wc);
        wc.lpfnWndProc = wndproc;
        wc.hInstance = GetModuleHandleA(NULL);
        wc.lpszClassName = "ddlab";
        RegisterClassA(&wc);
        hwnd = CreateWindowExA(WS_EX_TOPMOST, "ddlab", "ddlab", WS_POPUP | WS_VISIBLE, 0, 0,
                               g_w, g_h, NULL, NULL, wc.hInstance, NULL);
        ShowWindow(hwnd, SW_SHOW);
        pump();
        hr = IDirectDraw7_SetCooperativeLevel(dd, hwnd, DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN);
        say("SetCooperativeLevel -> %08lx", hr);
        /* every switch goes through vcr_pace.h (2026-09-26, .124: a sweep
         * re-synced the CRT ~250 times at two a second). Each error return
         * below is a return from main, so its atexit hold covers the revert
         * XP makes when this process ends still holding the mode. Refused by
         * the gate, nothing is switched and the run ends here. */
        if (!vcr_pace_before_switch()) {
            say("RESULT {\"mode\":\"flip\",\"error\":\"SetDisplayMode not made: %s\"}",
                g_vcr_pace_why);
            return 4;
        }
        hr = IDirectDraw7_SetDisplayMode(dd, g_w, g_h, g_bpp, 0, 0);
        /* even when refused: win32k may have set the new mode on the chip and
         * fallen back, i.e. re-synced the monitor, and the HRESULT does not say
         * which - count it, which only costs a failed run the hold */
        vcr_pace_after_switch();
        g_held = 1;
        say("SetDisplayMode -> %08lx", hr);
        if (FAILED(hr)) {
            say("RESULT {\"mode\":\"flip\",\"error\":\"SetDisplayMode %08lx\"}", hr);
            return 4;
        }
        memset(&sd, 0, sizeof sd);
        sd.dwSize = sizeof sd;
        sd.dwFlags = DDSD_CAPS | DDSD_BACKBUFFERCOUNT;
        sd.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE | DDSCAPS_FLIP | DDSCAPS_COMPLEX;
        sd.dwBackBufferCount = 1;
        hr = IDirectDraw7_CreateSurface(dd, &sd, &prim, NULL);
        say("CreateSurface(primary + 1 back) -> %08lx", hr);
        if (FAILED(hr)) {
            say("RESULT {\"mode\":\"flip\",\"error\":\"CreateSurface %08lx\"}", hr);
            return 5;
        }
        memset(&bc, 0, sizeof bc);
        bc.dwCaps = DDSCAPS_BACKBUFFER;
        hr = IDirectDrawSurface7_GetAttachedSurface(prim, &bc, &back);
        say("GetAttachedSurface(back buffer) -> %08lx", hr);
        if (FAILED(hr) || !back) {
            say("RESULT {\"mode\":\"flip\",\"error\":\"no back buffer %08lx\"}", hr);
            return 6;
        }
        memset(&sd, 0, sizeof sd);
        sd.dwSize = sizeof sd;
        IDirectDrawSurface7_GetSurfaceDesc(prim, &sd);
        prim_in = where(sd.ddsCaps.dwCaps);
        say("primary in %s memory, pitch %ld, fpVidMem-visible caps %08lx", prim_in, sd.lPitch,
            sd.ddsCaps.dwCaps);

        t0 = now_s();
        for (f = 0; f < g_frames && !g_focus_lost; f++) {
            memset(&sd, 0, sizeof sd);
            sd.dwSize = sizeof sd;
            if (FAILED(lhr = IDirectDrawSurface7_Lock(back, NULL, &sd, DDLOCK_WAIT, NULL))) {
                if (!lockfail++)
                    say("  back buffer Lock -> %08lx", lhr);
                continue;
            }
            write_pattern(&sd, f);
            IDirectDrawSurface7_Unlock(back, NULL);
            IDirectDrawSurface7_Flip(prim, NULL, DDFLIP_WAIT);
            if (ft)
                ft[nft++] = now_s();
            busy_us(g_work_us);             /* --work-us: "render", no DirectDraw call */
            memset(&sd, 0, sizeof sd);
            sd.dwSize = sizeof sd;
            if (FAILED(lhr = IDirectDrawSurface7_Lock(prim, NULL, &sd, DDLOCK_WAIT, NULL))) {
                if (!lockfail++)
                    say("  primary Lock -> %08lx", lhr);
                continue;
            }
            if (!check_pattern(&sd, f)) {
                if (!bad)
                    say("  frame %d: the front buffer does not hold what was flipped to it", f);
                bad++;
            }
            IDirectDrawSurface7_Unlock(prim, NULL);
            if ((f & 15) == 0)
                pump();
        }
        t = now_s() - t0;
        pump();                         /* a deactivation still queued is seen now */
        /* vertical blank period: 30 block-begins (not after a lost focus:
         * the desktop mode's, not the run's) */
        if (!g_focus_lost) {
            IDirectDraw7_WaitForVerticalBlank(dd, DDWAITVB_BLOCKBEGIN, NULL);
            vb0 = now_s();
            for (i = 0; i < 30; i++)
                if (SUCCEEDED(IDirectDraw7_WaitForVerticalBlank(dd, DDWAITVB_BLOCKBEGIN, NULL)))
                    vbn++;
            vbt = now_s() - vb0;
            IDirectDraw7_GetScanLine(dd, &scan);
        }
        /* flip to flip: the longest, and how many took over 1.5 refreshes
         * (the refresh measured just above; -1 when it could not be) */
        period = vbn && vbt > 0 ? vbt / vbn : 0;
        if (nft)
            first_ms = (ft[0] - t0) * 1000;
        if (nft > 1) {
            int k;
            slow = fast = period > 0 ? 0 : -1;
            min_ms = (ft[1] - ft[0]) * 1000;
            for (k = 1; k < nft; k++) {
                double d = ft[k] - ft[k - 1];
                if (d * 1000 > max_ms)
                    max_ms = d * 1000;
                if (d * 1000 < min_ms)
                    min_ms = d * 1000;
                if (period > 0 && d > 1.5 * period)
                    slow++;
                if (period > 0 && d < 0.5 * period)
                    fast++;
            }
            if (ft[nft - 1] > ft[0])
                ff_rate = (nft - 1) / (ft[nft - 1] - ft[0]);
        }
        say("RESULT {\"mode\":\"flip\",\"res\":\"%dx%dx%d\",\"primary_in\":\"%s\",\"frames\":%d,"
            "\"frames_run\":%d,\"mismatch\":%d,\"lock_fail\":%d,\"flips_s\":%.1f,"
            "\"vblank_hz\":%.1f,\"scanline\":%lu,\"hal_caps\":\"%08lx\",\"work_us\":%ld,"
            "\"first_frame_ms\":%.2f,\"max_frame_ms\":%.2f,\"slow_frames\":%d,"
            "\"min_frame_ms\":%.2f,\"fast_frames\":%d,"
            "\"flips_s_first_last\":%.1f%s}",
            g_w, g_h, g_bpp, prim_in, g_frames, f, bad, lockfail, t > 0 ? f / t : 0.0,
            vbn ? vbn / vbt : 0.0, scan, hal.dwCaps, g_work_us, first_ms, max_ms, slow, min_ms,
            fast, ff_rate, focus_json());
        free(ft);
        /* the hold: a short run still sits the floor */
        if (!restore_mode(dd, "flip"))
            return 8;
        IDirectDraw7_Release(dd);
        return g_focus_lost ? 9 : bad || lockfail ? 7 : 0;
    }
    if (!strcmp(mode, "blt")) {
        WNDCLASSA wc;
        HWND hwnd;
        LPDIRECTDRAWSURFACE7 a = NULL, b = NULL;
        DDSURFACEDESC2 sd;
        DDBLTFX fx;
        RECT r;
        int bpp_b, x, y, bad_copy = 0, bad_fill = 0, bad_scroll = 0, bad_key = 0, n;
        const int W = 256, H = 256;
        double t0, t;
        const char *a_in;

        memset(&wc, 0, sizeof wc);
        wc.lpfnWndProc = wndproc;
        wc.hInstance = GetModuleHandleA(NULL);
        wc.lpszClassName = "ddlab";
        RegisterClassA(&wc);
        hwnd = CreateWindowExA(WS_EX_TOPMOST, "ddlab", "ddlab", WS_POPUP | WS_VISIBLE, 0, 0,
                               g_w, g_h, NULL, NULL, wc.hInstance, NULL);
        pump();
        IDirectDraw7_SetCooperativeLevel(dd, hwnd, DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN);
        /* paced as in flip; blt is over in well under a second, so without
         * the hold its switch out followed the switch in by 0.05-1.5 s */
        if (!vcr_pace_before_switch()) {
            say("RESULT {\"mode\":\"blt\",\"error\":\"SetDisplayMode not made: %s\"}",
                g_vcr_pace_why);
            return 4;
        }
        hr = IDirectDraw7_SetDisplayMode(dd, g_w, g_h, g_bpp, 0, 0);
        vcr_pace_after_switch();        /* refused or not - see flip */
        g_held = 1;
        if (FAILED(hr)) {
            say("RESULT {\"mode\":\"blt\",\"error\":\"SetDisplayMode %08lx\"}", hr);
            return 4;
        }
        for (n = 0; n < 2; n++) {
            memset(&sd, 0, sizeof sd);
            sd.dwSize = sizeof sd;
            sd.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT;
            sd.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_VIDEOMEMORY;
            sd.dwWidth = W;
            sd.dwHeight = H;
            hr = IDirectDraw7_CreateSurface(dd, &sd, n ? &b : &a, NULL);
            if (FAILED(hr)) {
                say("RESULT {\"mode\":\"blt\",\"error\":\"CreateSurface(video) %08lx\"}", hr);
                return 5;
            }
        }
        bpp_b = g_bpp / 8;
        memset(&sd, 0, sizeof sd);
        sd.dwSize = sizeof sd;
        IDirectDrawSurface7_GetSurfaceDesc(a, &sd);
        a_in = where(sd.ddsCaps.dwCaps);
        /* where each surface's Lock points, relative to the primary's */
        {
            LPDIRECTDRAWSURFACE7 prim = NULL;
            DDSURFACEDESC2 ps;
            unsigned char *pp = NULL, *pa = NULL, *pb = NULL;
            memset(&ps, 0, sizeof ps);
            ps.dwSize = sizeof ps;
            ps.dwFlags = DDSD_CAPS;
            ps.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE;
            if (SUCCEEDED(IDirectDraw7_CreateSurface(dd, &ps, &prim, NULL))) {
                memset(&ps, 0, sizeof ps); ps.dwSize = sizeof ps;
                if (SUCCEEDED(IDirectDrawSurface7_Lock(prim, NULL, &ps, DDLOCK_WAIT, NULL))) {
                    pp = ps.lpSurface; IDirectDrawSurface7_Unlock(prim, NULL);
                }
            }
            memset(&ps, 0, sizeof ps); ps.dwSize = sizeof ps;
            if (SUCCEEDED(IDirectDrawSurface7_Lock(a, NULL, &ps, DDLOCK_WAIT, NULL))) {
                pa = ps.lpSurface; IDirectDrawSurface7_Unlock(a, NULL);
            }
            memset(&ps, 0, sizeof ps); ps.dwSize = sizeof ps;
            if (SUCCEEDED(IDirectDrawSurface7_Lock(b, NULL, &ps, DDLOCK_WAIT, NULL))) {
                pb = ps.lpSurface; IDirectDrawSurface7_Unlock(b, NULL);
            }
            say("Lock pointers: primary %p, A %p (+%lx), B %p (+%lx)", pp, pa,
                (unsigned long)(pa - pp), pb, (unsigned long)(pb - pp));
            if (prim)
                IDirectDrawSurface7_Release(prim);
        }
        /* A = pattern */
        memset(&sd, 0, sizeof sd);
        sd.dwSize = sizeof sd;
        IDirectDrawSurface7_Lock(a, NULL, &sd, DDLOCK_WAIT, NULL);
        for (y = 0; y < H; y++)
            for (x = 0; x < W * bpp_b; x++)
                ((unsigned char *)sd.lpSurface)[y * sd.lPitch + x] = (unsigned char)(x * 7 + y * 13);
        IDirectDrawSurface7_Unlock(a, NULL);
        /* A -> B, whole surface */
        hr = IDirectDrawSurface7_Blt(b, NULL, a, NULL, DDBLT_WAIT, NULL);
        say("Blt A->B -> %08lx", hr);
        memset(&sd, 0, sizeof sd);
        sd.dwSize = sizeof sd;
        IDirectDrawSurface7_Lock(b, NULL, &sd, DDLOCK_WAIT, NULL);
        for (y = 0; y < H; y++)
            for (x = 0; x < W * bpp_b; x += 5)
                if (((unsigned char *)sd.lpSurface)[y * sd.lPitch + x] != (unsigned char)(x * 7 + y * 13))
                    bad_copy++;
        IDirectDrawSurface7_Unlock(b, NULL);
        /* colour fill a rectangle of B */
        memset(&fx, 0, sizeof fx);
        fx.dwSize = sizeof fx;
        fx.dwFillColor = 0x5a5a5a5au;
        SetRect(&r, 16, 16, 80, 48);
        hr = IDirectDrawSurface7_Blt(b, &r, NULL, NULL, DDBLT_COLORFILL | DDBLT_WAIT, &fx);
        say("Blt COLORFILL -> %08lx", hr);
        memset(&sd, 0, sizeof sd);
        sd.dwSize = sizeof sd;
        IDirectDrawSurface7_Lock(b, NULL, &sd, DDLOCK_WAIT, NULL);
        for (y = 0; y < H; y++)
            for (x = 0; x < W * bpp_b; x++) {
                int in = y >= 16 && y < 48 && x >= 16 * bpp_b && x < 80 * bpp_b;
                unsigned char v = ((unsigned char *)sd.lpSurface)[y * sd.lPitch + x];
                if (in ? v != 0x5a : v != (unsigned char)(x * 7 + y * 13))
                    bad_fill++;
            }
        IDirectDrawSurface7_Unlock(b, NULL);
        /* overlapping: scroll A down by 8 rows inside itself */
        {
            RECT src, dst;
            SetRect(&src, 0, 0, W, H - 8);
            SetRect(&dst, 0, 8, W, H);
            hr = IDirectDrawSurface7_Blt(a, &dst, a, &src, DDBLT_WAIT, NULL);
            say("Blt A->A (scroll 8) -> %08lx", hr);
        }
        memset(&sd, 0, sizeof sd);
        sd.dwSize = sizeof sd;
        IDirectDrawSurface7_Lock(a, NULL, &sd, DDLOCK_WAIT, NULL);
        for (y = 8; y < H; y++)
            for (x = 0; x < W * bpp_b; x += 3)
                if (((unsigned char *)sd.lpSurface)[y * sd.lPitch + x] != (unsigned char)(x * 7 + (y - 8) * 13))
                    bad_scroll++;
        IDirectDrawSurface7_Unlock(a, NULL);
        /* a source-keyed blit (sprites): A's left half is the key colour and
         * must leave B untouched, its right half must land */
        {
            DWORD key = g_bpp == 8 ? 0xfd : g_bpp == 16 ? 0xf81f : 0x00ff00ff;
            DWORD val = g_bpp == 8 ? 0x11 : g_bpp == 16 ? 0x07e0 : 0x0000ff00;
            DDCOLORKEY ck;
            memset(&sd, 0, sizeof sd);
            sd.dwSize = sizeof sd;
            IDirectDrawSurface7_Lock(b, NULL, &sd, DDLOCK_WAIT, NULL);
            for (y = 0; y < H; y++)
                memset((unsigned char *)sd.lpSurface + y * sd.lPitch, 0x33, (size_t)W * bpp_b);
            IDirectDrawSurface7_Unlock(b, NULL);
            memset(&sd, 0, sizeof sd);
            sd.dwSize = sizeof sd;
            IDirectDrawSurface7_Lock(a, NULL, &sd, DDLOCK_WAIT, NULL);
            for (y = 0; y < H; y++)
                for (x = 0; x < W; x++) {
                    unsigned char *px = (unsigned char *)sd.lpSurface + y * sd.lPitch + x * bpp_b;
                    DWORD v = x < W / 2 ? key : val;
                    memcpy(px, &v, (size_t)bpp_b);
                }
            IDirectDrawSurface7_Unlock(a, NULL);
            ck.dwColorSpaceLowValue = ck.dwColorSpaceHighValue = key;
            IDirectDrawSurface7_SetColorKey(a, DDCKEY_SRCBLT, &ck);
            hr = IDirectDrawSurface7_Blt(b, NULL, a, NULL, DDBLT_KEYSRC | DDBLT_WAIT, NULL);
            say("Blt A->B (source colour key) -> %08lx", hr);
            memset(&sd, 0, sizeof sd);
            sd.dwSize = sizeof sd;
            IDirectDrawSurface7_Lock(b, NULL, &sd, DDLOCK_WAIT, NULL);
            for (y = 0; y < H; y++)
                for (x = 0; x < W; x++) {
                    unsigned char *px = (unsigned char *)sd.lpSurface + y * sd.lPitch + x * bpp_b;
                    DWORD v = 0, want = x < W / 2 ? 0x33333333u : val;
                    memcpy(&v, px, (size_t)bpp_b);
                    if (bpp_b < 4)
                        want &= (1u << (bpp_b * 8)) - 1;
                    if (v != want)
                        bad_key++;
                }
            IDirectDrawSurface7_Unlock(b, NULL);
            IDirectDrawSurface7_SetColorKey(a, DDCKEY_SRCBLT, NULL);
        }
        /* rate - unless the window lost the foreground meanwhile: then the
         * surfaces are lost and the run ends (a deactivation still queued
         * is seen by this pump) */
        pump();
        t = 0;
        if (!g_focus_lost) {
            t0 = now_s();
            for (n = 0; n < 200; n++)
                IDirectDrawSurface7_Blt(b, NULL, a, NULL, DDBLT_WAIT, NULL);
            t = now_s() - t0;
        }
        say("RESULT {\"mode\":\"blt\",\"res\":\"%dx%dx%d\",\"surfaces_in\":\"%s\","
            "\"bad_copy\":%d,\"bad_fill\":%d,\"bad_scroll\":%d,\"bad_key\":%d,\"blts_s\":%.0f,"
            "\"mpix_s\":%.1f,\"hal_caps\":\"%08lx\"%s}",
            g_w, g_h, g_bpp, a_in, bad_copy, bad_fill, bad_scroll, bad_key, t > 0 ? 200 / t : 0.0,
            t > 0 ? 200.0 * W * H / t / 1e6 : 0.0, hal.dwCaps, focus_json());
        if (!restore_mode(dd, "blt"))   /* the hold */
            return 8;
        IDirectDraw7_Release(dd);
        return g_focus_lost ? 9 : bad_copy || bad_fill || bad_scroll || bad_key ? 7 : 0;
    }
    say("RESULT {\"error\":\"unknown mode %s\"}", mode);
    return 2;
}
