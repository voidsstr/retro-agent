/*
 * ddlab.c - a DirectDraw test program for the kernel driver's DirectDraw HAL.
 *
 *   ddlab caps                      HAL vs HEL caps, video memory total / free
 *   ddlab flip [--res WxH] [--bpp N] [--frames N]
 *             exclusive fullscreen, a primary + 1 back buffer. Per frame: lock
 *             the back buffer, write a frame-numbered pattern, flip (DDFLIP_WAIT),
 *             lock the FRONT buffer and read the pattern back - so a flip that
 *             does not really change what is scanned out (or a Lock pointer that
 *             points at the wrong surface) is a counted mismatch, not a guess.
 *             Also: where the surfaces live (video memory = the HAL is in use),
 *             flips per second, and the vertical-blank period.
 *   ddlab blt [--res WxH] [--bpp N]
 *             two off-screen surfaces: a pattern blitted A -> B (SRCCOPY), a
 *             colour fill, and an OVERLAPPING blit inside one surface (a scroll,
 *             where a naive copy smears) - each read back and compared; then
 *             copies per second. Also where the surfaces live.
 *
 * Every step is flushed to the log before the next (the glideprobe rule).
 * The final line is `RESULT {json}`.
 */
#define INITGUID
#include <windows.h>
#include <ddraw.h>
#include <io.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    return DefWindowProcA(h, m, w, l);
}

static void pump(void)
{
    MSG msg;
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
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

int main(int argc, char **argv)
{
    const char *mode = "caps";
    LPDIRECTDRAW7 dd = NULL;
    DDCAPS hal, hel;
    DWORD total = 0, freem = 0;
    DDSCAPS2 vm;
    HRESULT hr;
    int i;

    for (i = 1; i < argc; i++) {
        const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(a, "--res") && v) { sscanf(v, "%dx%d", &g_w, &g_h); i++; }
        else if (!strcmp(a, "--bpp") && v) { g_bpp = atoi(v); i++; }
        else if (!strcmp(a, "--frames") && v) { g_frames = atoi(v); i++; }
        else if (!strcmp(a, "--log") && v) { strncpy(g_logpath, v, sizeof g_logpath - 1); i++; }
        else if (a[0] != '-') mode = a;
    }
    g_log = fopen(g_logpath, "w");
    say("ddlab %s: %dx%dx%d frames %d", mode, g_w, g_h, g_bpp, g_frames);

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
        hr = IDirectDraw7_SetDisplayMode(dd, g_w, g_h, g_bpp, 0, 0);
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
        for (f = 0; f < g_frames; f++) {
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
        /* vertical blank period: 30 block-begins */
        IDirectDraw7_WaitForVerticalBlank(dd, DDWAITVB_BLOCKBEGIN, NULL);
        vb0 = now_s();
        for (i = 0; i < 30; i++)
            if (SUCCEEDED(IDirectDraw7_WaitForVerticalBlank(dd, DDWAITVB_BLOCKBEGIN, NULL)))
                vbn++;
        vbt = now_s() - vb0;
        IDirectDraw7_GetScanLine(dd, &scan);
        say("RESULT {\"mode\":\"flip\",\"res\":\"%dx%dx%d\",\"primary_in\":\"%s\",\"frames\":%d,"
            "\"mismatch\":%d,\"lock_fail\":%d,\"flips_s\":%.1f,\"vblank_hz\":%.1f,"
            "\"scanline\":%lu,\"hal_caps\":\"%08lx\"}",
            g_w, g_h, g_bpp, prim_in, g_frames, bad, lockfail, g_frames / t,
            vbn ? vbn / vbt : 0.0, scan, hal.dwCaps);
        IDirectDraw7_RestoreDisplayMode(dd);
        IDirectDraw7_Release(dd);
        return bad || lockfail ? 7 : 0;
    }
    if (!strcmp(mode, "blt")) {
        WNDCLASSA wc;
        HWND hwnd;
        LPDIRECTDRAWSURFACE7 a = NULL, b = NULL;
        DDSURFACEDESC2 sd;
        DDBLTFX fx;
        RECT r;
        int bpp_b, x, y, bad_copy = 0, bad_fill = 0, bad_scroll = 0, n;
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
        hr = IDirectDraw7_SetDisplayMode(dd, g_w, g_h, g_bpp, 0, 0);
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
        /* rate */
        t0 = now_s();
        for (n = 0; n < 200; n++)
            IDirectDrawSurface7_Blt(b, NULL, a, NULL, DDBLT_WAIT, NULL);
        t = now_s() - t0;
        say("RESULT {\"mode\":\"blt\",\"res\":\"%dx%dx%d\",\"surfaces_in\":\"%s\","
            "\"bad_copy\":%d,\"bad_fill\":%d,\"bad_scroll\":%d,\"blts_s\":%.0f,"
            "\"mpix_s\":%.1f,\"hal_caps\":\"%08lx\"}",
            g_w, g_h, g_bpp, a_in, bad_copy, bad_fill, bad_scroll, 200 / t,
            200.0 * W * H / t / 1e6, hal.dwCaps);
        IDirectDraw7_RestoreDisplayMode(dd);
        IDirectDraw7_Release(dd);
        return bad_copy || bad_fill || bad_scroll ? 7 : 0;
    }
    say("RESULT {\"error\":\"unknown mode %s\"}", mode);
    return 2;
}
