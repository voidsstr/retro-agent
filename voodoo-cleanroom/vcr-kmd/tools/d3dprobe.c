/*
 * d3dprobe.c - a self-checking Direct3D 8 test program for the display
 * driver's Direct3D HAL (and for any other driver, as the reference).
 *
 *   d3dprobe caps                   adapter, driver, D3DCAPS8 and the formats
 *                                   the HAL accepts - no device is created
 *   d3dprobe render [--full] [--res WxH] [--bpp 16|32] [--tests a,b,...]
 *             one HAL device, then each test draws into the back buffer, the
 *             back buffer is LOCKED and read back, and the pixels are compared
 *             with values computed here (the scene is analytic) - no golden
 *             screenshot and no timing: an emulated or slow box gets the same
 *             verdict as a fast one. Windowed by default (the desktop's depth);
 *             --full is exclusive fullscreen, which is how games run.
 *   d3dprobe perf [--full [--novsync]] [--res WxH] [--bpp N] [--frames N]
 *             textured triangles per second and frames per second (--novsync:
 *             fullscreen presents immediately, so the number is the chip's).
 *
 * Tests: clear flat gouraud tex modulate blend ztest bigtex (default), mip (explicit:
 * --tests mip. On the 86Box Voodoo3 XP's own 3dfx driver samples level 0 at
 * every size too - the emulator's LOD, not a driver fault - so it is not in
 * the default gate; it is the check to run on silicon)
 *
 * Every step is flushed to the log before the next, so a driver that hangs
 * the machine leaves the step it hung in. The final line is `RESULT {json}`.
 */
#include <windows.h>
#include <d3d8.h>
#include <io.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static FILE *g_log;
static char g_logpath[MAX_PATH] = "C:\\d3dprobe.log";

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

/* ---- json accumulation ------------------------------------------------------ */
static char g_json[16384];
static int g_jn;
static void js(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    g_jn += _vsnprintf(g_json + g_jn, sizeof g_json - g_jn - 1, fmt, ap);
    va_end(ap);
}

/* ---- the scene ------------------------------------------------------------ */
#define BB 256                  /* back buffer: BB x BB (windowed) */
#define Q0 32                   /* the quad covers [Q0, Q1) in x and y */
#define Q1 224

typedef struct { float x, y, z, rhw; DWORD c; } VC;
typedef struct { float x, y, z, rhw; DWORD c; float u, v; } VT;
#define FVF_C (D3DFVF_XYZRHW | D3DFVF_DIFFUSE)
#define FVF_T (D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1)

static IDirect3DDevice8 *g_dev;
static HWND g_hwnd;
static D3DFORMAT g_fmt;
static int g_w = 640, g_h = 480, g_bpp = 16, g_full, g_frames = 200, g_novsync;
static int g_pass, g_fail;

static void quad_c(float x0, float y0, float x1, float y1, float z, DWORD c0, DWORD c1, DWORD c2,
                   DWORD c3)
{
    VC q[4] = { { x0, y0, z, 1, c0 }, { x1, y0, z, 1, c1 }, { x0, y1, z, 1, c2 },
                { x1, y1, z, 1, c3 } };
    IDirect3DDevice8_SetVertexShader(g_dev, FVF_C);
    IDirect3DDevice8_DrawPrimitiveUP(g_dev, D3DPT_TRIANGLESTRIP, 2, q, sizeof q[0]);
}

typedef struct { float x, y, z, rhw; DWORD c, s; } VS;
#define FVF_S (D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_SPECULAR)

static void quad_fog(float x0, float y0, float x1, float y1, float rhw, DWORD c, DWORD spec)
{
    VS q[4] = { { x0, y0, 0.5f, rhw, c, spec }, { x1, y0, 0.5f, rhw, c, spec },
                { x0, y1, 0.5f, rhw, c, spec }, { x1, y1, 0.5f, rhw, c, spec } };
    IDirect3DDevice8_SetVertexShader(g_dev, FVF_S);
    IDirect3DDevice8_DrawPrimitiveUP(g_dev, D3DPT_TRIANGLESTRIP, 2, q, sizeof q[0]);
}

static DWORD fbits(float f)
{
    DWORD u;
    memcpy(&u, &f, 4);
    return u;
}

static void quad_t(float x0, float y0, float x1, float y1, DWORD c, float uv)
{
    VT q[4] = { { x0, y0, 0.5f, 1, c, 0, 0 }, { x1, y0, 0.5f, 1, c, uv, 0 },
                { x0, y1, 0.5f, 1, c, 0, uv }, { x1, y1, 0.5f, 1, c, uv, uv } };
    IDirect3DDevice8_SetVertexShader(g_dev, FVF_T);
    IDirect3DDevice8_DrawPrimitiveUP(g_dev, D3DPT_TRIANGLESTRIP, 2, q, sizeof q[0]);
}

/* ---- read back ---------------------------------------------------------------- */
static DWORD g_px[BB * BB];     /* the last frame, as 0x00RRGGBB */
static int g_rw, g_rh;
static const char *g_readvia = "none";

static DWORD expand(D3DFORMAT f, const unsigned char *p)
{
    DWORD v, r, g, b;
    switch (f) {
    case D3DFMT_R5G6B5:
        v = *(const WORD *)p;
        r = (v >> 11) & 31; g = (v >> 5) & 63; b = v & 31;
        return ((r << 3 | r >> 2) << 16) | ((g << 2 | g >> 4) << 8) | (b << 3 | b >> 2);
    case D3DFMT_X1R5G5B5:
    case D3DFMT_A1R5G5B5:
        v = *(const WORD *)p;
        r = (v >> 10) & 31; g = (v >> 5) & 31; b = v & 31;
        return ((r << 3 | r >> 2) << 16) | ((g << 3 | g >> 2) << 8) | (b << 3 | b >> 2);
    default:
        return *(const DWORD *)p & 0xffffff;
    }
}

/* lock the back buffer and copy the top-left BB x BB (or less) out */
static int readback(void)
{
    IDirect3DSurface8 *bb = NULL;
    D3DSURFACE_DESC d;
    D3DLOCKED_RECT lr;
    HRESULT hr;
    int x, y, bpp;

    hr = IDirect3DDevice8_GetBackBuffer(g_dev, 0, D3DBACKBUFFER_TYPE_MONO, &bb);
    if (FAILED(hr)) {
        say("  readback: GetBackBuffer %08lx", hr);
        return 0;
    }
    IDirect3DSurface8_GetDesc(bb, &d);
    hr = IDirect3DSurface8_LockRect(bb, &lr, NULL, D3DLOCK_READONLY);
    if (FAILED(hr)) {
        say("  readback: LockRect(back buffer) %08lx", hr);
        IDirect3DSurface8_Release(bb);
        return 0;
    }
    bpp = (d.Format == D3DFMT_R5G6B5 || d.Format == D3DFMT_X1R5G5B5 ||
           d.Format == D3DFMT_A1R5G5B5) ? 2 : 4;
    g_rw = d.Width < BB ? (int)d.Width : BB;
    g_rh = d.Height < BB ? (int)d.Height : BB;
    for (y = 0; y < g_rh; y++)
        for (x = 0; x < g_rw; x++)
            g_px[y * BB + x] = expand(d.Format, (unsigned char *)lr.pBits + y * lr.Pitch + x * bpp);
    IDirect3DSurface8_UnlockRect(bb);
    IDirect3DSurface8_Release(bb);
    g_readvia = "backbuffer";
    return 1;
}

static DWORD px(int x, int y) { return g_px[y * BB + x]; }

/* one pixel against an expected colour, per-channel tolerance */
static int expect(const char *test, const char *what, int x, int y, DWORD want, int tol)
{
    DWORD got = px(x, y);
    int i, ok = 1;
    for (i = 0; i < 3; i++) {
        int a = (int)(got >> (i * 8) & 0xff), b = (int)(want >> (i * 8) & 0xff);
        if (abs(a - b) > tol)
            ok = 0;
    }
    say("  %s %s (%d,%d): got %06lx want %06lx tol %d -> %s", test, what, x, y, got, want, tol,
        ok ? "ok" : "FAIL");
    js("%s{\"test\":\"%s\",\"at\":\"%s\",\"x\":%d,\"y\":%d,\"got\":\"%06lx\",\"want\":\"%06lx\","
       "\"ok\":%d}", g_pass + g_fail ? "," : "", test, what, x, y, got, want, ok);
    if (ok) g_pass++; else g_fail++;
    return ok;
}

static int frame_begin(DWORD clear)
{
    HRESULT hr;
    pump();
    hr = IDirect3DDevice8_Clear(g_dev, 0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, clear, 1.0f, 0);
    if (FAILED(hr))
        say("  Clear %08lx", hr);
    hr = IDirect3DDevice8_BeginScene(g_dev);
    if (FAILED(hr)) {
        say("  BeginScene %08lx", hr);
        return 0;
    }
    return 1;
}

static void frame_end(void)
{
    IDirect3DDevice8_EndScene(g_dev);
    readback();
    IDirect3DDevice8_Present(g_dev, NULL, NULL, NULL, NULL);
}

static void untextured(void)
{
    IDirect3DDevice8_SetTexture(g_dev, 0, NULL);
    IDirect3DDevice8_SetTextureStageState(g_dev, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    IDirect3DDevice8_SetTextureStageState(g_dev, 0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
    IDirect3DDevice8_SetTextureStageState(g_dev, 0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    IDirect3DDevice8_SetTextureStageState(g_dev, 0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
}

/* a size x size R5G6B5 checker of 8x8 cells, c0 at cell (0,0) */
static IDirect3DTexture8 *checker(int size, DWORD c0, DWORD c1)
{
    IDirect3DTexture8 *t = NULL;
    D3DLOCKED_RECT lr;
    HRESULT hr;
    int x, y;
    hr = IDirect3DDevice8_CreateTexture(g_dev, size, size, 1, 0, D3DFMT_R5G6B5, D3DPOOL_MANAGED, &t);
    if (FAILED(hr)) {
        say("  CreateTexture(%d, R5G6B5) %08lx", size, hr);
        return NULL;
    }
    hr = IDirect3DTexture8_LockRect(t, 0, &lr, NULL, 0);
    if (FAILED(hr)) {
        say("  texture LockRect %08lx", hr);
        IDirect3DTexture8_Release(t);
        return NULL;
    }
    for (y = 0; y < size; y++) {
        WORD *row = (WORD *)((char *)lr.pBits + y * lr.Pitch);
        for (x = 0; x < size; x++) {
            DWORD c = ((x >> 3) + (y >> 3)) & 1 ? c1 : c0;
            row[x] = (WORD)(((c >> 19 & 31) << 11) | ((c >> 10 & 63) << 5) | (c >> 3 & 31));
        }
    }
    IDirect3DTexture8_UnlockRect(t, 0);
    return t;
}

/* the screen pixel at the centre of texel (tx,ty) of a size-texel texture
 * stretched over [Q0,Q1) */
static int tpx(int t, int size) { return Q0 + (int)((t + 0.5) * (Q1 - Q0) / size); }

static void run_test(const char *t)
{
    int i;
    say("test %s", t);
    if (!strcmp(t, "clear")) {
        if (!frame_begin(0x00ff0000)) return;
        frame_end();
        expect(t, "corner", 0, 0, 0xff0000, 8);
        expect(t, "centre", 128, 128, 0xff0000, 8);
        if (!frame_begin(0x000000ff)) return;
        frame_end();
        expect(t, "second clear", 200, 60, 0x0000ff, 8);
    } else if (!strcmp(t, "flat")) {
        if (!frame_begin(0)) return;
        untextured();
        quad_c(Q0, Q0, Q1, Q1, 0.5f, 0xff00ff00, 0xff00ff00, 0xff00ff00, 0xff00ff00);
        frame_end();
        expect(t, "inside", 128, 128, 0x00ff00, 8);
        expect(t, "outside", 8, 8, 0x000000, 8);
        expect(t, "left edge in", Q0 + 1, 128, 0x00ff00, 8);
        expect(t, "right edge out", Q1 + 1, 128, 0x000000, 8);
    } else if (!strcmp(t, "gouraud")) {
        if (!frame_begin(0)) return;
        untextured();
        quad_c(Q0, Q0, Q1, Q1, 0.5f, 0xffff0000, 0xff00ff00, 0xff0000ff, 0xffffffff);
        frame_end();
        expect(t, "near red corner", Q0 + 2, Q0 + 2, 0xff0000, 24);
        expect(t, "near green corner", Q1 - 3, Q0 + 2, 0x00ff00, 24);
        expect(t, "near blue corner", Q0 + 2, Q1 - 3, 0x0000ff, 24);
    } else if (!strcmp(t, "tex") || !strcmp(t, "modulate") || !strcmp(t, "bigtex")) {
        int size = !strcmp(t, "bigtex") ? 256 : 64;
        IDirect3DTexture8 *tx = checker(size, 0xffff0000, 0xff0000ff);
        DWORD dif = !strcmp(t, "modulate") ? 0xff808080 : 0xffffffff;
        DWORD red = !strcmp(t, "modulate") ? 0x800000 : 0xff0000;
        DWORD blue = !strcmp(t, "modulate") ? 0x000080 : 0x0000ff;
        if (!tx) {
            g_fail++;
            js("%s{\"test\":\"%s\",\"error\":\"texture\",\"ok\":0}", g_pass + g_fail > 1 ? "," : "", t);
            return;
        }
        if (!frame_begin(0)) return;
        IDirect3DDevice8_SetTexture(g_dev, 0, (IDirect3DBaseTexture8 *)tx);
        IDirect3DDevice8_SetTextureStageState(g_dev, 0, D3DTSS_COLOROP, D3DTOP_MODULATE);
        IDirect3DDevice8_SetTextureStageState(g_dev, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        IDirect3DDevice8_SetTextureStageState(g_dev, 0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
        IDirect3DDevice8_SetTextureStageState(g_dev, 0, D3DTSS_MINFILTER, D3DTEXF_POINT);
        IDirect3DDevice8_SetTextureStageState(g_dev, 0, D3DTSS_MAGFILTER, D3DTEXF_POINT);
        IDirect3DDevice8_SetTextureStageState(g_dev, 0, D3DTSS_MIPFILTER, D3DTEXF_NONE);
        quad_t(Q0, Q0, Q1, Q1, dif, 1.0f);
        frame_end();
        IDirect3DDevice8_SetTexture(g_dev, 0, NULL);
        IDirect3DTexture8_Release(tx);
        expect(t, "cell (0,0)", tpx(4, size), tpx(4, size), red, 12);
        expect(t, "cell (1,0)", tpx(12, size), tpx(4, size), blue, 12);
        expect(t, "cell (1,1)", tpx(12, size), tpx(12, size), red, 12);
        expect(t, "last cell", tpx(size - 4, size), tpx(size - 4, size), red, 12);
    } else if (!strcmp(t, "mip")) {
        /* a 64x64 chain, every level one solid colour: L0 red, L1 green, L2
         * blue, L3 yellow, smaller magenta. Drawn 192, 32 and 16 pixels wide
         * with point mip filtering, the texel:pixel ratio (1/3, 2, 4) picks
         * L0, L1, L2. */
        static const DWORD lc[5] = { 0xffff0000, 0xff00ff00, 0xff0000ff, 0xffffff00, 0xffff00ff };
        static const struct { float x0, x1; DWORD want; const char *what; } q[3] = {
            { 32, 224, 0xff0000, "192 px: level 0" },
            { 112, 144, 0x00ff00, "32 px: level 1" },
            { 120, 136, 0x0000ff, "16 px: level 2" } };
        IDirect3DTexture8 *tx = NULL;
        HRESULT hr = IDirect3DDevice8_CreateTexture(g_dev, 64, 64, 0, 0, D3DFMT_R5G6B5,
                                                    D3DPOOL_MANAGED, &tx);
        DWORD lv, n;
        if (FAILED(hr)) {
            say("  CreateTexture(64, full chain) %08lx", hr);
            js("%s{\"test\":\"mip\",\"error\":\"CreateTexture %08lx\",\"ok\":0}",
               g_pass + g_fail ? "," : "", hr);
            g_fail++;
            return;
        }
        n = IDirect3DTexture8_GetLevelCount(tx);
        say("  %lu levels", n);
        for (lv = 0; lv < n; lv++) {
            D3DLOCKED_RECT lr;
            D3DSURFACE_DESC ld;
            DWORD c = lc[lv < 4 ? lv : 4], x, y;
            WORD px565 = (WORD)(((c >> 19 & 31) << 11) | ((c >> 10 & 63) << 5) | (c >> 3 & 31));
            IDirect3DTexture8_GetLevelDesc(tx, lv, &ld);
            if (FAILED(IDirect3DTexture8_LockRect(tx, lv, &lr, NULL, 0)))
                continue;
            for (y = 0; y < ld.Height; y++)
                for (x = 0; x < ld.Width; x++)
                    ((WORD *)((char *)lr.pBits + y * lr.Pitch))[x] = px565;
            IDirect3DTexture8_UnlockRect(tx, lv);
        }
        for (i = 0; i < 3; i++) {
            if (!frame_begin(0)) break;
            IDirect3DDevice8_SetTexture(g_dev, 0, (IDirect3DBaseTexture8 *)tx);
            IDirect3DDevice8_SetTextureStageState(g_dev, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
            IDirect3DDevice8_SetTextureStageState(g_dev, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
            IDirect3DDevice8_SetTextureStageState(g_dev, 0, D3DTSS_MINFILTER, D3DTEXF_POINT);
            IDirect3DDevice8_SetTextureStageState(g_dev, 0, D3DTSS_MAGFILTER, D3DTEXF_POINT);
            IDirect3DDevice8_SetTextureStageState(g_dev, 0, D3DTSS_MIPFILTER, D3DTEXF_POINT);
            quad_t(q[i].x0, q[i].x0, q[i].x1, q[i].x1, 0xffffffff, 1.0f);
            frame_end();
            expect(t, q[i].what, 128, 128, q[i].want, 12);
        }
        IDirect3DDevice8_SetTexture(g_dev, 0, NULL);
        IDirect3DDevice8_SetTextureStageState(g_dev, 0, D3DTSS_MIPFILTER, D3DTEXF_NONE);
        IDirect3DTexture8_Release(tx);
    } else if (!strcmp(t, "present")) {
        /* what Present puts ON THE SCREEN: windowed, a clipped blit from the
         * back buffer to the primary; read back through GDI from the window */
        HDC dc;
        COLORREF got;
        DWORD gotrgb;
        if (g_full) {
            say("  present: windowed only");
            return;
        }
        if (!frame_begin(0x00ff00ff)) return;
        untextured();
        quad_c(Q0, Q0, Q1, Q1, 0.5f, 0xff00ff00, 0xff00ff00, 0xff00ff00, 0xff00ff00);
        frame_end();                                /* includes Present */
        Sleep(200);
        dc = GetDC(g_hwnd);
        got = GetPixel(dc, 8, 8);
        gotrgb = ((got & 0xff) << 16) | (got & 0xff00) | ((got >> 16) & 0xff);
        g_px[8 * BB + 8] = gotrgb;
        expect(t, "screen: clear colour", 8, 8, 0xff00ff, 12);
        got = GetPixel(dc, 128, 128);
        gotrgb = ((got & 0xff) << 16) | (got & 0xff00) | ((got >> 16) & 0xff);
        g_px[128 * BB + 128] = gotrgb;
        expect(t, "screen: the quad", 128, 128, 0x00ff00, 12);
        ReleaseDC(g_hwnd, dc);
    } else if (!strcmp(t, "fogtable") || !strcmp(t, "fogvertex")) {
        /* red quads fogged toward blue. Table fog: linear from w=1 to w=5, the
         * quads at w = 1, 3, 8 (rhw 1, 1/3, 1/8). Vertex fog: the factor in the
         * specular alpha (255 none, 128 half, 0 all). */
        int table = !strcmp(t, "fogtable");
        static const struct { float rhw; DWORD sa; DWORD want; const char *what; } q[3] = {
            { 1.0f, 0xff, 0xff0000, "no fog" },
            { 1.0f / 3.0f, 0x80, 0x80007f, "half fog" },
            { 1.0f / 8.0f, 0x00, 0x0000ff, "all fog" } };
        if (!frame_begin(0)) return;
        untextured();
        IDirect3DDevice8_SetRenderState(g_dev, D3DRS_FOGENABLE, TRUE);
        IDirect3DDevice8_SetRenderState(g_dev, D3DRS_FOGCOLOR, 0xff0000ff);
        IDirect3DDevice8_SetRenderState(g_dev, D3DRS_FOGTABLEMODE, table ? D3DFOG_LINEAR : D3DFOG_NONE);
        IDirect3DDevice8_SetRenderState(g_dev, D3DRS_FOGVERTEXMODE, D3DFOG_NONE);
        IDirect3DDevice8_SetRenderState(g_dev, D3DRS_FOGSTART, fbits(1.0f));
        IDirect3DDevice8_SetRenderState(g_dev, D3DRS_FOGEND, fbits(5.0f));
        for (i = 0; i < 3; i++)
            quad_fog(16.0f + i * 80.0f, 64, 16.0f + i * 80.0f + 64, 192, table ? q[i].rhw : 1.0f,
                     0xffff0000, (q[i].sa << 24) | 0x000000);
        IDirect3DDevice8_SetRenderState(g_dev, D3DRS_FOGENABLE, FALSE);
        frame_end();
        for (i = 0; i < 3; i++)
            expect(t, q[i].what, 16 + i * 80 + 32, 128, q[i].want, i == 1 ? 40 : 12);
    } else if (!strcmp(t, "blend")) {
        if (!frame_begin(0x000000ff)) return;
        untextured();
        IDirect3DDevice8_SetRenderState(g_dev, D3DRS_ALPHABLENDENABLE, TRUE);
        IDirect3DDevice8_SetRenderState(g_dev, D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
        IDirect3DDevice8_SetRenderState(g_dev, D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
        quad_c(Q0, Q0, Q1, Q1, 0.5f, 0x80ff0000, 0x80ff0000, 0x80ff0000, 0x80ff0000);
        IDirect3DDevice8_SetRenderState(g_dev, D3DRS_ALPHABLENDENABLE, FALSE);
        frame_end();
        expect(t, "50% red over blue", 128, 128, 0x80007f, 20);
        expect(t, "outside", 8, 8, 0x0000ff, 8);
    } else if (!strcmp(t, "ztest")) {
        if (!frame_begin(0)) return;
        untextured();
        IDirect3DDevice8_SetRenderState(g_dev, D3DRS_ZENABLE, D3DZB_TRUE);
        IDirect3DDevice8_SetRenderState(g_dev, D3DRS_ZWRITEENABLE, TRUE);
        IDirect3DDevice8_SetRenderState(g_dev, D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
        /* near green, then far red over the whole quad, then nearer blue on the left half */
        quad_c(Q0, Q0, Q1, Q1, 0.25f, 0xff00ff00, 0xff00ff00, 0xff00ff00, 0xff00ff00);
        quad_c(Q0, Q0, Q1, Q1, 0.75f, 0xffff0000, 0xffff0000, 0xffff0000, 0xffff0000);
        quad_c(Q0, Q0, 128, Q1, 0.10f, 0xff0000ff, 0xff0000ff, 0xff0000ff, 0xff0000ff);
        IDirect3DDevice8_SetRenderState(g_dev, D3DRS_ZENABLE, D3DZB_FALSE);
        frame_end();
        expect(t, "far quad hidden", 180, 128, 0x00ff00, 8);
        expect(t, "near quad drawn", 64, 128, 0x0000ff, 8);
    } else {
        say("  unknown test %s", t);
    }
}

int main(int argc, char **argv)
{
    const char *mode = "caps", *tests = "clear,flat,gouraud,tex,modulate,blend,ztest,bigtex,present,fogtable,fogvertex";
    IDirect3D8 *d3d;
    D3DADAPTER_IDENTIFIER8 id;
    D3DDISPLAYMODE dm;
    D3DCAPS8 caps;
    D3DPRESENT_PARAMETERS pp;
    WNDCLASSA wc;
    HWND hwnd;
    RECT rc;
    HRESULT hr;
    int i;

    for (i = 1; i < argc; i++) {
        const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(a, "--res") && v) { sscanf(v, "%dx%d", &g_w, &g_h); i++; }
        else if (!strcmp(a, "--bpp") && v) { g_bpp = atoi(v); i++; }
        else if (!strcmp(a, "--frames") && v) { g_frames = atoi(v); i++; }
        else if (!strcmp(a, "--tests") && v) { tests = v; i++; }
        else if (!strcmp(a, "--log") && v) { strncpy(g_logpath, v, sizeof g_logpath - 1); i++; }
        else if (!strcmp(a, "--full")) g_full = 1;
        else if (!strcmp(a, "--novsync")) g_novsync = 1;
        else if (a[0] != '-') mode = a;
    }
    g_log = fopen(g_logpath, "w");
    say("d3dprobe %s: %s %dx%dx%d", mode, g_full ? "fullscreen" : "windowed", g_w, g_h, g_bpp);

    d3d = Direct3DCreate8(D3D_SDK_VERSION);
    if (!d3d) {
        say("RESULT {\"mode\":\"%s\",\"error\":\"Direct3DCreate8\"}", mode);
        return 3;
    }
    memset(&id, 0, sizeof id);
    IDirect3D8_GetAdapterIdentifier(d3d, 0, D3DENUM_NO_WHQL_LEVEL, &id);
    IDirect3D8_GetAdapterDisplayMode(d3d, 0, &dm);
    say("adapter \"%s\" driver %s %u.%u.%u.%u; desktop %ux%u fmt %u", id.Description, id.Driver,
        HIWORD(id.DriverVersion.HighPart), LOWORD(id.DriverVersion.HighPart),
        HIWORD(id.DriverVersion.LowPart), LOWORD(id.DriverVersion.LowPart), dm.Width, dm.Height,
        dm.Format);
    memset(&caps, 0, sizeof caps);
    hr = IDirect3D8_GetDeviceCaps(d3d, 0, D3DDEVTYPE_HAL, &caps);
    say("GetDeviceCaps(HAL) %08lx", hr);

    if (!strcmp(mode, "caps")) {
        static const struct { D3DFORMAT f; const char *n; DWORD usage; D3DRESOURCETYPE rt; } fm[] = {
            { D3DFMT_R5G6B5, "tex565", 0, D3DRTYPE_TEXTURE },
            { D3DFMT_A1R5G5B5, "tex1555", 0, D3DRTYPE_TEXTURE },
            { D3DFMT_A4R4G4B4, "tex4444", 0, D3DRTYPE_TEXTURE },
            { D3DFMT_A8R8G8B8, "tex8888", 0, D3DRTYPE_TEXTURE },
            { D3DFMT_P8, "texP8", 0, D3DRTYPE_TEXTURE },
            { D3DFMT_DXT1, "texDXT1", 0, D3DRTYPE_TEXTURE },
            { D3DFMT_D16, "z16", D3DUSAGE_DEPTHSTENCIL, D3DRTYPE_SURFACE },
            { D3DFMT_D24S8, "z24s8", D3DUSAGE_DEPTHSTENCIL, D3DRTYPE_SURFACE },
        };
        js("{\"mode\":\"caps\",\"adapter\":\"%s\",\"driver\":\"%s\",\"desktop_fmt\":%u,"
           "\"hal\":\"%08lx\"", id.Description, id.Driver, dm.Format, hr);
        if (SUCCEEDED(hr))
            js(",\"DevCaps\":\"%08lx\",\"PrimitiveMiscCaps\":\"%08lx\",\"RasterCaps\":\"%08lx\","
               "\"ZCmpCaps\":\"%08lx\",\"SrcBlendCaps\":\"%08lx\",\"DestBlendCaps\":\"%08lx\","
               "\"AlphaCmpCaps\":\"%08lx\",\"ShadeCaps\":\"%08lx\",\"TextureCaps\":\"%08lx\","
               "\"TextureFilterCaps\":\"%08lx\",\"TextureAddressCaps\":\"%08lx\","
               "\"TextureOpCaps\":\"%08lx\",\"FVFCaps\":\"%08lx\",\"MaxTexture\":\"%lux%lu\","
               "\"MaxTextureBlendStages\":%lu,\"MaxSimultaneousTextures\":%lu,"
               "\"MaxPrimitiveCount\":%lu,\"MaxVertexIndex\":%lu,\"VertexShaderVersion\":\"%08lx\"",
               caps.DevCaps, caps.PrimitiveMiscCaps, caps.RasterCaps, caps.ZCmpCaps,
               caps.SrcBlendCaps, caps.DestBlendCaps, caps.AlphaCmpCaps, caps.ShadeCaps,
               caps.TextureCaps, caps.TextureFilterCaps, caps.TextureAddressCaps,
               caps.TextureOpCaps, caps.FVFCaps, caps.MaxTextureWidth, caps.MaxTextureHeight,
               caps.MaxTextureBlendStages, caps.MaxSimultaneousTextures, caps.MaxPrimitiveCount,
               caps.MaxVertexIndex, caps.VertexShaderVersion);
        js(",\"formats\":{");
        for (i = 0; i < (int)(sizeof fm / sizeof fm[0]); i++) {
            HRESULT f = IDirect3D8_CheckDeviceFormat(d3d, 0, D3DDEVTYPE_HAL, dm.Format, fm[i].usage,
                                                     fm[i].rt, fm[i].f);
            say("format %s: %s", fm[i].n, SUCCEEDED(f) ? "yes" : "no");
            js("%s\"%s\":%d", i ? "," : "", fm[i].n, SUCCEEDED(f));
        }
        js("}}");
        say("RESULT %s", g_json);
        return 0;
    }

    /* a device: windowed in a BB x BB client area, or exclusive fullscreen */
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "d3dprobe";
    wc.hCursor = LoadCursorA(NULL, (LPCSTR)IDC_ARROW);
    RegisterClassA(&wc);
    SetRect(&rc, 0, 0, BB, BB);
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    hwnd = CreateWindowA("d3dprobe", "d3dprobe", g_full ? WS_POPUP | WS_VISIBLE
                         : WS_OVERLAPPEDWINDOW | WS_VISIBLE, 40, 40,
                         g_full ? g_w : rc.right - rc.left, g_full ? g_h : rc.bottom - rc.top,
                         NULL, NULL, wc.hInstance, NULL);
    pump();
    g_hwnd = hwnd;
    memset(&pp, 0, sizeof pp);
    pp.Windowed = !g_full;
    pp.SwapEffect = g_full ? D3DSWAPEFFECT_FLIP : D3DSWAPEFFECT_COPY;
    pp.BackBufferCount = 1;
    pp.BackBufferFormat = g_full ? (g_bpp == 32 ? D3DFMT_X8R8G8B8 : D3DFMT_R5G6B5) : dm.Format;
    pp.BackBufferWidth = g_full ? g_w : BB;
    pp.BackBufferHeight = g_full ? g_h : BB;
    pp.EnableAutoDepthStencil = TRUE;
    pp.AutoDepthStencilFormat = D3DFMT_D16;
    pp.Flags = D3DPRESENTFLAG_LOCKABLE_BACKBUFFER;
    pp.hDeviceWindow = hwnd;
    if (g_full && g_novsync)        /* perf: measure the chip, not the refresh */
        pp.FullScreen_PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    g_fmt = pp.BackBufferFormat;
    say("CreateDevice(HAL, %s, back buffer %ux%u fmt %u, D16)", g_full ? "fullscreen" : "windowed",
        pp.BackBufferWidth, pp.BackBufferHeight, pp.BackBufferFormat);
    hr = IDirect3D8_CreateDevice(d3d, 0, D3DDEVTYPE_HAL, hwnd, D3DCREATE_SOFTWARE_VERTEXPROCESSING,
                                 &pp, &g_dev);
    if (FAILED(hr)) {
        say("RESULT {\"mode\":\"%s\",\"error\":\"CreateDevice %08lx\",\"adapter\":\"%s\"}", mode,
            hr, id.Description);
        return 2;
    }
    say("device created");
    IDirect3DDevice8_SetRenderState(g_dev, D3DRS_LIGHTING, FALSE);
    IDirect3DDevice8_SetRenderState(g_dev, D3DRS_CULLMODE, D3DCULL_NONE);
    IDirect3DDevice8_SetRenderState(g_dev, D3DRS_ZENABLE, D3DZB_FALSE);
    IDirect3DDevice8_SetRenderState(g_dev, D3DRS_DITHERENABLE, FALSE);

    if (!strcmp(mode, "perf")) {
        IDirect3DTexture8 *tx = checker(64, 0xffff0000, 0xff0000ff);
        enum { N = 200 };
        static VT tri[N * 3];
        double t0, dt;
        int f;
        for (i = 0; i < N * 3; i++) {
            float x = (float)(20 + (i * 37) % (int)(pp.BackBufferWidth - 40));
            float y = (float)(20 + (i * 53) % (int)(pp.BackBufferHeight - 40));
            VT v = { x, y, 0.5f, 1, 0xffffffff, (float)(i % 3 == 1), (float)(i % 3 == 2) };
            tri[i] = v;
        }
        IDirect3DDevice8_SetTexture(g_dev, 0, (IDirect3DBaseTexture8 *)tx);
        IDirect3DDevice8_SetTextureStageState(g_dev, 0, D3DTSS_COLOROP, D3DTOP_MODULATE);
        IDirect3DDevice8_SetTextureStageState(g_dev, 0, D3DTSS_MINFILTER, D3DTEXF_LINEAR);
        IDirect3DDevice8_SetTextureStageState(g_dev, 0, D3DTSS_MAGFILTER, D3DTEXF_LINEAR);
        IDirect3DDevice8_SetVertexShader(g_dev, FVF_T);
        t0 = now_s();
        for (f = 0; f < g_frames; f++) {
            pump();
            IDirect3DDevice8_Clear(g_dev, 0, NULL, D3DCLEAR_TARGET, 0x00202060, 1.0f, 0);
            IDirect3DDevice8_BeginScene(g_dev);
            IDirect3DDevice8_DrawPrimitiveUP(g_dev, D3DPT_TRIANGLELIST, N, tri, sizeof tri[0]);
            IDirect3DDevice8_EndScene(g_dev);
            IDirect3DDevice8_Present(g_dev, NULL, NULL, NULL, NULL);
        }
        dt = now_s() - t0;
        say("RESULT {\"mode\":\"perf\",\"adapter\":\"%s\",\"frames\":%d,\"tris_per_frame\":%d,"
            "\"fps\":%.1f,\"tris_s\":%.0f}", id.Description, g_frames, N, g_frames / dt,
            g_frames * N / dt);
        IDirect3DDevice8_Release(g_dev);
        return 0;
    }

    js("{\"mode\":\"render\",\"adapter\":\"%s\",\"window\":\"%s\",\"fmt\":%u,\"checks\":[",
       id.Description, g_full ? "fullscreen" : "windowed", g_fmt);
    {
        char list[512], *p, *save;
        strncpy(list, tests, sizeof list - 1);
        list[sizeof list - 1] = 0;
        for (p = strtok_r(list, ",", &save); p; p = strtok_r(NULL, ",", &save))
            run_test(p);
    }
    js("],\"readback\":\"%s\",\"pass\":%d,\"fail\":%d}", g_readvia, g_pass, g_fail);
    IDirect3DDevice8_Release(g_dev);
    IDirect3D8_Release(d3d);
    say("RESULT %s", g_json);
    return g_fail ? 1 : 0;
}
