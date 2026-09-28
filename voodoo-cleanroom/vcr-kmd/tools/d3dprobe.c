/*
 * d3dprobe.c - a self-checking Direct3D 8 test program for the display
 * driver's Direct3D HAL (and for any other driver, as the reference).
 *
 *   d3dprobe caps                   adapter, driver, D3DCAPS8 and the formats
 *                                   the HAL accepts - no device is created,
 *                                   no mode is switched. "zmatch": for the
 *                                   X8R8G8B8 and R5G6B5 adapter/target formats,
 *                                   CheckDeviceFormat and CheckDepthStencilMatch
 *                                   of D24X8, D24S8 and D16 - how the runtime
 *                                   maps the HAL's Z list before any device
 *                                   exists - and "hal_fullscreen":
 *                                   CheckDeviceType of a fullscreen HAL
 *                                   device in each (0 for X8R8G8B8 on a
 *                                   VSA-100: Diag\D3D32 is off)
 *   d3dprobe render [--full] [--res WxH] [--bpp 16|32] [--noz] [--zfmt d16|d24x8|d24s8]
 *             [--tests a,b,...]
 *             one HAL device, then each test draws into the back buffer, the
 *             back buffer is LOCKED and read back, and the pixels are compared
 *             with values computed here (the scene is analytic) - no golden
 *             screenshot and no timing: an emulated or slow box gets the same
 *             verdict as a fast one. Windowed by default (the desktop's depth);
 *             --full is exclusive fullscreen, which is how games run.
 *             --noz: no depth buffer (EnableAutoDepthStencil FALSE) - the
 *             colour path alone (renderMode + colour fastfill, no aux buffer
 *             write); clears are TARGET only and ztest is skipped.
 *             --zfmt: ask for THIS depth format (EnableAutoDepthStencil on),
 *             not the one d3dprobe would pick - a device a HAL must REFUSE
 *             (a 16 bpp target with D24S8 on a Banshee/Voodoo3 or a VSA-100
 *             without Diag\D3D32) is then asked for as an application would.
 *             The RESULT says which format was asked ("zfmt") either way.
 *   d3dprobe perf [--full [--novsync]] [--res WxH] [--bpp N] [--frames N]
 *             textured triangles per second and frames per second (--novsync:
 *             fullscreen presents immediately, so the number is the chip's).
 *   --pace MS  raise the floor between display-mode switches (never below
 *             vcr_pace.h's 3 s; decimal, at most 30000 - anything else is
 *             refused before anything switches); only --full switches modes.
 *
 * With --full: a switch vcr_pace.h cannot pace (its lock held by a stuck tool,
 * a stamp that never stops moving) is not made - the run ends with a RESULT
 * "error" naming why, and a device still up is left for the exit hold. A
 * window that loses the foreground while the device is fullscreen (the
 * runtime gives the desktop back at that moment, unpaced) has that switch
 * recorded, ends the run and says "focus_lost":true, rather than let a
 * re-activation switch in again.
 *
 * Tests: clear flat gouraud tex modulate blend ztest bigtex (default), mip (explicit:
 * --tests mip. On the 86Box Voodoo3 XP's own 3dfx driver samples level 0 at
 * every size too - the emulator's LOD, not a driver fault - so it is not in
 * the default gate; it is the check to run on silicon)
 *
 * The VSA-100 texture path (vcr-kmd Diag\D3DBigTex; explicit, never in the
 * default list): tex512 tex1024 tex2048 (an 8x8-cell R5G6B5 texture of that
 * size, its LAST cell green - a chip that samples only part of it never shows
 * green), mip2048 (a full 2048 chain, one colour a level, levels 0-4 selected
 * by texel:pixel ratio - before, at and after the 256 level TBIG's base names),
 * tex8888 (A8R8G8B8), dxt1 dxt3 dxt5 (128x128, solid 4x4 blocks; the log
 * reports LockRect's pitch against a block row's), texhigh (~20 MB of textures
 * first, so the probe texture lands above 16 MB). Each is SKIPPED, not failed,
 * when the HAL does not offer its size or format; the RESULT counts
 * "skipped" and gives "max_texture".
 *
 * Every step is flushed to the log before the next, so a driver that hangs
 * the machine leaves the step it hung in. The final line is `RESULT {json}`;
 * the host reads the LAST one.
 */
#include <windows.h>
#include <d3d8.h>
#include <io.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vcr_pace.h"

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

/* a fullscreen device is up and the run is using it (between CreateDevice and
 * the start of the paced Release) */
static int g_held;
/* ... and the window lost the foreground meanwhile */
static int g_focus_lost;

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    /* Deactivated while fullscreen: the D3D runtime's hook on this window
     * minimizes it and gives the desktop back right now - a switch the gate
     * did not pace. Record it, and end the run: carrying on until something
     * re-activates the window would put the fullscreen mode back, unpaced
     * too. */
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
static int g_noz;               /* --noz: no depth buffer at all */
static D3DFORMAT g_zfmt = D3DFMT_UNKNOWN;   /* --zfmt: this depth format, not d3dprobe's pick */

/* --zfmt's names; D3DFMT_UNKNOWN for anything else (refused, nothing created) */
static D3DFORMAT zfmt_named(const char *v)
{
    if (!strcmp(v, "d16"))
        return D3DFMT_D16;
    if (!strcmp(v, "d24x8"))
        return D3DFMT_D24X8;
    if (!strcmp(v, "d24s8"))
        return D3DFMT_D24S8;
    return D3DFMT_UNKNOWN;
}
static int g_pass, g_fail;
static int g_skip;              /* checks not run: the HAL does not offer what they need */

static int g_nent;              /* entries written into the RESULT's checks */

/* the separator before a RESULT check entry - called once per entry */
static const char *sep(void)
{
    return g_nent++ ? "," : "";
}

/* Releasing a fullscreen device is the switch back to the desktop: hold the
 * mode for vcr_pace.h's floor first (2026-09-26, .124: every switch re-syncs
 * the CRT, and a short run switched out almost as soon as it had switched in).
 * A windowed device switches nothing, so it is not paced. 0 = the gate
 * refused the switch out (its lock busy, or the floor never passed): the
 * device is NOT released - that would give the mode back unpaced - and XP
 * reverts it as the process exits, after the exit hold. */
static int release_device(void)
{
    if (g_full && !vcr_pace_before_switch())
        return 0;
    g_held = 0;                         /* a deactivation from here on is ours */
    IDirect3DDevice8_Release(g_dev);
    if (g_full)
        vcr_pace_after_restore();
    g_dev = NULL;
    return 1;
}

/* the RESULT of a run whose device release was refused: its own line (the
 * host reads the LAST one), after the run's */
static void say_not_released(const char *mode)
{
    say("RESULT {\"mode\":\"%s\",\"error\":\"device not released: %s - the mode is left for"
        " the exit hold\"}", mode, g_vcr_pace_why);
}

/* ,"focus_lost":... for a RESULT, with an "error" when it was: the run was
 * cut short, and its numbers are not a pass */
static const char *focus_json(void)
{
    return g_focus_lost ? ",\"focus_lost\":true,\"error\":\"the window lost the foreground while"
                          " fullscreen - run ended\"" : ",\"focus_lost\":false";
}

static void quad_c(float x0, float y0, float x1, float y1, float z, DWORD c0, DWORD c1, DWORD c2,
                   DWORD c3)
{
    VC q[4] = { { x0, y0, z, 1, c0 }, { x1, y0, z, 1, c1 }, { x0, y1, z, 1, c2 },
                { x1, y1, z, 1, c3 } };
    IDirect3DDevice8_SetVertexShader(g_dev, FVF_C);
    IDirect3DDevice8_DrawPrimitiveUP(g_dev, D3DPT_TRIANGLESTRIP, 2, q, sizeof q[0]);
}

typedef struct { float x, y, z, rhw; DWORD c; float u0, v0, u1, v1; } VT2;
#define FVF_T2 (D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX2)

static void quad_t2(float x0, float y0, float x1, float y1, DWORD c)
{
    VT2 q[4] = { { x0, y0, 0.5f, 1, c, 0, 0, 0, 0 }, { x1, y0, 0.5f, 1, c, 1, 0, 1, 0 },
                 { x0, y1, 0.5f, 1, c, 0, 1, 0, 1 }, { x1, y1, 0.5f, 1, c, 1, 1, 1, 1 } };
    IDirect3DDevice8_SetVertexShader(g_dev, FVF_T2);
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
       "\"ok\":%d}", sep(), test, what, x, y, got, want, ok);
    if (ok) g_pass++; else g_fail++;
    return ok;
}

static int frame_begin(DWORD clear)
{
    HRESULT hr;
    pump();
    /* no depth buffer (--noz): D3DCLEAR_ZBUFFER would fail the whole Clear */
    hr = IDirect3DDevice8_Clear(g_dev, 0, NULL,
                                D3DCLEAR_TARGET | (g_noz ? 0 : D3DCLEAR_ZBUFFER), clear, 1.0f, 0);
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

/* ---- the VSA-100 texture path (vcr-kmd Diag\D3DBigTex) ------------------------------
 * tex512/tex1024/tex2048, mip2048, tex8888, dxt1/dxt3/dxt5, texhigh: each is
 * SKIPPED - not failed - when the HAL does not offer what it needs (a
 * MaxTextureWidth under the size, a format CheckDeviceFormat refuses), so the
 * same list runs on any driver and says what it could not check. */
static D3DCAPS8 g_caps;
static IDirect3D8 *g_d3d;
static D3DFORMAT g_adfmt;       /* the adapter format the device runs at */

static void skipped(const char *test, const char *why)
{
    say("  %s: skipped - %s", test, why);
    js("%s{\"test\":\"%s\",\"skipped\":\"%s\"}", sep(), test, why);
    g_skip++;
}

static void failed(const char *test, const char *what, HRESULT hr)
{
    say("  %s: %s %08lx", test, what, hr);
    js("%s{\"test\":\"%s\",\"error\":\"%s %08lx\",\"ok\":0}", sep(), test, what, hr);
    g_fail++;
}

static int tex_format_ok(D3DFORMAT f)
{
    return SUCCEEDED(IDirect3D8_CheckDeviceFormat(g_d3d, 0, D3DDEVTYPE_HAL, g_adfmt, 0,
                                                  D3DRTYPE_TEXTURE, f));
}

static WORD rgb565(DWORD c)
{
    return (WORD)(((c >> 19 & 31) << 11) | ((c >> 10 & 63) << 5) | (c >> 3 & 31));
}

/* cell (cx, cy) of an 8 x 8 cell pattern: a c0/c1 checker whose LAST cell is
 * green - a chip that samples only part of a big texture never shows it */
static DWORD cell_colour(int cx, int cy, DWORD c0, DWORD c1)
{
    if (cx == 7 && cy == 7)
        return 0xff00ff00;
    return ((cx + cy) & 1) ? c1 : c0;
}

/* one solid 4x4 block: colour0 = colour1 = c, every index 0 (DXT1's
 * 3-colour mode picks colour0 - opaque); DXT3 alpha all 15, DXT5 alpha0 =
 * alpha1 = 255 */
static void dxt_block(unsigned char *p, D3DFORMAT f, DWORD c)
{
    WORD v = rgb565(c);
    unsigned char *col = p;
    if (f == D3DFMT_DXT3) {
        memset(p, 0xff, 8);
        col = p + 8;
    } else if (f == D3DFMT_DXT5) {
        p[0] = p[1] = 0xff;
        memset(p + 2, 0, 6);
        col = p + 8;
    }
    col[0] = col[2] = (unsigned char)(v & 0xff);
    col[1] = col[3] = (unsigned char)(v >> 8);
    memset(col + 4, 0, 4);
}

static int g_lock_pitch;        /* what the last cells() LockRect answered */

/* a size x size one-level MANAGED texture of format f (R5G6B5, A8R8G8B8,
 * DXT1/3/5) holding the 8 x 8 cells. A DXT level is written as D3D8
 * documents it - rows of 4x4 blocks, (size / 4) x block bytes apart - and the
 * runtime's own Pitch is only reported (g_lock_pitch): writing at a pitch
 * that is not a block row's would run past the runtime's buffer */
static IDirect3DTexture8 *cells(const char *t, int size, D3DFORMAT f, DWORD c0, DWORD c1)
{
    IDirect3DTexture8 *tx = NULL;
    D3DLOCKED_RECT lr;
    HRESULT hr;
    int x, y, cell = size / 8;
    hr = IDirect3DDevice8_CreateTexture(g_dev, size, size, 1, 0, f, D3DPOOL_MANAGED, &tx);
    if (FAILED(hr)) {
        failed(t, "CreateTexture", hr);
        return NULL;
    }
    hr = IDirect3DTexture8_LockRect(tx, 0, &lr, NULL, 0);
    if (FAILED(hr)) {
        failed(t, "LockRect", hr);
        IDirect3DTexture8_Release(tx);
        return NULL;
    }
    g_lock_pitch = lr.Pitch;
    if (f == D3DFMT_DXT1 || f == D3DFMT_DXT3 || f == D3DFMT_DXT5) {
        int blk = f == D3DFMT_DXT1 ? 8 : 16, bw = size / 4;
        say("  %s: LockRect pitch %d (a block row is %d)", t, lr.Pitch, bw * blk);
        for (y = 0; y < size / 4; y++)
            for (x = 0; x < bw; x++)
                dxt_block((unsigned char *)lr.pBits + y * bw * blk + x * blk, f,
                          cell_colour(x * 4 / cell, y * 4 / cell, c0, c1));
    } else {
        for (y = 0; y < size; y++) {
            char *row = (char *)lr.pBits + y * lr.Pitch;
            for (x = 0; x < size; x++) {
                DWORD c = cell_colour(x / cell, y / cell, c0, c1);
                if (f == D3DFMT_A8R8G8B8)
                    ((DWORD *)row)[x] = c | 0xff000000;
                else
                    ((WORD *)row)[x] = rgb565(c);
            }
        }
    }
    IDirect3DTexture8_UnlockRect(tx, 0);
    return tx;
}

/* the cells texture drawn over [Q0,Q1) - 24 px a cell - and five cells read */
static void draw_cells(const char *t, IDirect3DTexture8 *tx)
{
    int cc[8], i;
    for (i = 0; i < 8; i++)
        cc[i] = Q0 + (int)((i + 0.5) * (Q1 - Q0) / 8);
    if (!frame_begin(0))
        return;
    IDirect3DDevice8_SetTexture(g_dev, 0, (IDirect3DBaseTexture8 *)tx);
    IDirect3DDevice8_SetTextureStageState(g_dev, 0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    IDirect3DDevice8_SetTextureStageState(g_dev, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    IDirect3DDevice8_SetTextureStageState(g_dev, 0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    IDirect3DDevice8_SetTextureStageState(g_dev, 0, D3DTSS_MINFILTER, D3DTEXF_POINT);
    IDirect3DDevice8_SetTextureStageState(g_dev, 0, D3DTSS_MAGFILTER, D3DTEXF_POINT);
    IDirect3DDevice8_SetTextureStageState(g_dev, 0, D3DTSS_MIPFILTER, D3DTEXF_NONE);
    quad_t(Q0, Q0, Q1, Q1, 0xffffffff, 1.0f);
    frame_end();
    IDirect3DDevice8_SetTexture(g_dev, 0, NULL);
    expect(t, "cell (0,0)", cc[0], cc[0], 0xff0000, 12);
    expect(t, "cell (1,0)", cc[1], cc[0], 0x0000ff, 12);
    expect(t, "cell (2,3)", cc[2], cc[3], 0x0000ff, 12);
    expect(t, "cell (6,7)", cc[6], cc[7], 0x0000ff, 12);
    expect(t, "last cell (7,7)", cc[7], cc[7], 0x00ff00, 12);
}

/* tex512 / tex1024 / tex2048 / tex8888 / dxt1 / dxt3 / dxt5 */
static void test_cells(const char *t, int size, D3DFORMAT f)
{
    IDirect3DTexture8 *tx;
    char why[96];
    if ((int)g_caps.MaxTextureWidth < size || (int)g_caps.MaxTextureHeight < size) {
        _snprintf(why, sizeof why, "MaxTexture %lux%lu", g_caps.MaxTextureWidth,
                  g_caps.MaxTextureHeight);
        why[sizeof why - 1] = 0;
        skipped(t, why);
        return;
    }
    if (!tex_format_ok(f)) {
        _snprintf(why, sizeof why, "format %lu not offered", (unsigned long)f);
        why[sizeof why - 1] = 0;
        skipped(t, why);
        return;
    }
    if ((tx = cells(t, size, f, 0xffff0000, 0xff0000ff)) == NULL)
        return;
    draw_cells(t, tx);
    IDirect3DTexture8_Release(tx);
}

/* mip2048: a full 2048 chain, each level one colour - 0 red, 1 green, 2 blue,
 * 3 yellow (the 256 level: where TBIG's texBaseAddr points), 4 magenta, the
 * rest cyan - drawn 192 px wide at 0.8, 2.4, 4.8, 9.6 and 19.2 texels a pixel
 * (LOD n + 0.26: floor and round agree) with point mip filtering: levels
 * before, AT and after the base must all come out */
static void test_mip2048(const char *t)
{
    static const DWORD lc[6] = { 0xffff0000, 0xff00ff00, 0xff0000ff, 0xffffff00, 0xffff00ff,
                                 0xff00ffff };
    static const struct { float ratio; DWORD want; const char *what; } q[5] = {
        { 0.8f, 0xff0000, "level 0 (2048)" }, { 2.4f, 0x00ff00, "level 1 (1024)" },
        { 4.8f, 0x0000ff, "level 2 (512)" }, { 9.6f, 0xffff00, "level 3 (256, the base)" },
        { 19.2f, 0xff00ff, "level 4 (128)" } };
    IDirect3DTexture8 *tx = NULL;
    HRESULT hr;
    DWORD lv, n;
    int i;
    if (g_caps.MaxTextureWidth < 2048 || g_caps.MaxTextureHeight < 2048) {
        skipped(t, "MaxTexture under 2048");
        return;
    }
    hr = IDirect3DDevice8_CreateTexture(g_dev, 2048, 2048, 0, 0, D3DFMT_R5G6B5, D3DPOOL_MANAGED, &tx);
    if (FAILED(hr)) {
        failed(t, "CreateTexture(2048, full chain)", hr);
        return;
    }
    n = IDirect3DTexture8_GetLevelCount(tx);
    say("  %lu levels", n);
    for (lv = 0; lv < n; lv++) {
        D3DLOCKED_RECT lr;
        D3DSURFACE_DESC ld;
        WORD v = rgb565(lc[lv < 5 ? lv : 5]);
        DWORD x, y;
        IDirect3DTexture8_GetLevelDesc(tx, lv, &ld);
        if (FAILED(IDirect3DTexture8_LockRect(tx, lv, &lr, NULL, 0)))
            continue;
        for (y = 0; y < ld.Height; y++)
            for (x = 0; x < ld.Width; x++)
                ((WORD *)((char *)lr.pBits + y * lr.Pitch))[x] = v;
        IDirect3DTexture8_UnlockRect(tx, lv);
    }
    for (i = 0; i < 5; i++) {
        if (!frame_begin(0))
            break;
        IDirect3DDevice8_SetTexture(g_dev, 0, (IDirect3DBaseTexture8 *)tx);
        IDirect3DDevice8_SetTextureStageState(g_dev, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
        IDirect3DDevice8_SetTextureStageState(g_dev, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        IDirect3DDevice8_SetTextureStageState(g_dev, 0, D3DTSS_MINFILTER, D3DTEXF_POINT);
        IDirect3DDevice8_SetTextureStageState(g_dev, 0, D3DTSS_MAGFILTER, D3DTEXF_POINT);
        IDirect3DDevice8_SetTextureStageState(g_dev, 0, D3DTSS_MIPFILTER, D3DTEXF_POINT);
        quad_t(Q0, Q0, Q1, Q1, 0xffffffff, (float)(Q1 - Q0) * q[i].ratio / 2048.0f);
        frame_end();
        expect(t, q[i].what, 128, 128, q[i].want, 12);
    }
    IDirect3DDevice8_SetTexture(g_dev, 0, NULL);
    IDirect3DDevice8_SetTextureStageState(g_dev, 0, D3DTSS_MIPFILTER, D3DTEXF_NONE);
    IDirect3DTexture8_Release(tx);
}

/* texhigh: ~20 MB of 256x256 grey textures drawn first (so each is placed in
 * video memory), then the 64x64 cells texture - which lands above 16 MB on
 * .124's 27 MB heap. A Voodoo3-width texture address (bits 23:4) samples it
 * 16 MB lower and the cells come out wrong; the VSA-100 path's 26 bits read
 * it right. The fill stops early on any failure and says how far it got */
#define HIGH_FILL_KB    (20 * 1024)
static void test_texhigh(const char *t)
{
    static IDirect3DTexture8 *fill[HIGH_FILL_KB / 128];
    IDirect3DTexture8 *tx;
    int nf = 0, i;
    char what[64];
    for (nf = 0; nf < HIGH_FILL_KB / 128; nf++) {
        D3DLOCKED_RECT lr;
        int y, x;
        if (FAILED(IDirect3DDevice8_CreateTexture(g_dev, 256, 256, 1, 0, D3DFMT_R5G6B5,
                                                  D3DPOOL_MANAGED, &fill[nf])))
            break;
        if (SUCCEEDED(IDirect3DTexture8_LockRect(fill[nf], 0, &lr, NULL, 0))) {
            for (y = 0; y < 256; y++)
                for (x = 0; x < 256; x++)
                    ((WORD *)((char *)lr.pBits + y * lr.Pitch))[x] = rgb565(0x808080);
            IDirect3DTexture8_UnlockRect(fill[nf], 0);
        }
        if (!frame_begin(0)) {                  /* drawn once: placed in video memory now */
            IDirect3DTexture8_Release(fill[nf]);
            break;
        }
        IDirect3DDevice8_SetTexture(g_dev, 0, (IDirect3DBaseTexture8 *)fill[nf]);
        IDirect3DDevice8_SetTextureStageState(g_dev, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
        IDirect3DDevice8_SetTextureStageState(g_dev, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        quad_t(0, 0, 8, 8, 0xffffffff, 1.0f);
        IDirect3DDevice8_EndScene(g_dev);
        IDirect3DDevice8_Present(g_dev, NULL, NULL, NULL, NULL);
    }
    IDirect3DDevice8_SetTexture(g_dev, 0, NULL);
    say("  texhigh: %d fill textures, %d KB drawn before the probe", nf, nf * 128);
    _snprintf(what, sizeof what, "after %d KB", nf * 128);
    what[sizeof what - 1] = 0;
    if ((tx = cells(t, 64, D3DFMT_R5G6B5, 0xffff0000, 0xff0000ff)) != NULL) {
        draw_cells(t, tx);
        IDirect3DTexture8_Release(tx);
    }
    js("%s{\"test\":\"%s\",\"note\":\"%s\"}", sep(), t, what);    /* neither pass nor fail */
    for (i = 0; i < nf; i++)
        IDirect3DTexture8_Release(fill[i]);
}

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
    } else if (!strcmp(t, "gouraudb")) {
        /* the gouraud quad with its corners rotated - first vertex BLUE: tells
         * "red never interpolates" (the corners read with red 0) from "the
         * first vertex's colour sticks" (every corner reads with blue ff) */
        if (!frame_begin(0)) return;
        untextured();
        quad_c(Q0, Q0, Q1, Q1, 0.5f, 0xff0000ff, 0xffff0000, 0xff00ff00, 0xffffffff);
        frame_end();
        expect(t, "near blue corner", Q0 + 2, Q0 + 2, 0x0000ff, 24);
        expect(t, "near red corner", Q1 - 3, Q0 + 2, 0xff0000, 24);
        expect(t, "near green corner", Q0 + 2, Q1 - 3, 0x00ff00, 24);
        expect(t, "near white corner", Q1 - 3, Q1 - 3, 0xffffff, 24);
    } else if (!strcmp(t, "tex") || !strcmp(t, "modulate") || !strcmp(t, "bigtex")) {
        int size = !strcmp(t, "bigtex") ? 256 : 64;
        IDirect3DTexture8 *tx = checker(size, 0xffff0000, 0xff0000ff);
        DWORD dif = !strcmp(t, "modulate") ? 0xff808080 : 0xffffffff;
        DWORD red = !strcmp(t, "modulate") ? 0x800000 : 0xff0000;
        DWORD blue = !strcmp(t, "modulate") ? 0x000080 : 0x0000ff;
        if (!tx) {
            js("%s{\"test\":\"%s\",\"error\":\"texture\",\"ok\":0}", sep(), t);
            g_fail++;
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
               sep(), hr);
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
    } else if (!strcmp(t, "tex2mod") || !strcmp(t, "tex2add")) {
        /* two stages, the second TMU: stage 0 the red/blue checker (x diffuse
         * white), stage 1 a solid texture that MODULATEs (grey 0x80) or ADDs
         * (green 0x40) the current colour */
        int add = !strcmp(t, "tex2add");
        IDirect3DTexture8 *a = checker(64, 0xffff0000, 0xff0000ff);
        IDirect3DTexture8 *b = checker(16, add ? 0xff004000 : 0xff808080, add ? 0xff004000 : 0xff808080);
        DWORD red = add ? 0xff4000 : 0x840000, blue = add ? 0x0041ff : 0x000084;
        if (!a || !b) {
            g_fail++;
            return;
        }
        if (!frame_begin(0)) return;
        IDirect3DDevice8_SetTexture(g_dev, 0, (IDirect3DBaseTexture8 *)a);
        IDirect3DDevice8_SetTexture(g_dev, 1, (IDirect3DBaseTexture8 *)b);
        IDirect3DDevice8_SetTextureStageState(g_dev, 0, D3DTSS_COLOROP, D3DTOP_MODULATE);
        IDirect3DDevice8_SetTextureStageState(g_dev, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        IDirect3DDevice8_SetTextureStageState(g_dev, 0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
        IDirect3DDevice8_SetTextureStageState(g_dev, 0, D3DTSS_MINFILTER, D3DTEXF_POINT);
        IDirect3DDevice8_SetTextureStageState(g_dev, 0, D3DTSS_MAGFILTER, D3DTEXF_POINT);
        IDirect3DDevice8_SetTextureStageState(g_dev, 0, D3DTSS_TEXCOORDINDEX, 0);
        IDirect3DDevice8_SetTextureStageState(g_dev, 1, D3DTSS_COLOROP, add ? D3DTOP_ADD : D3DTOP_MODULATE);
        IDirect3DDevice8_SetTextureStageState(g_dev, 1, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        IDirect3DDevice8_SetTextureStageState(g_dev, 1, D3DTSS_COLORARG2, D3DTA_CURRENT);
        IDirect3DDevice8_SetTextureStageState(g_dev, 1, D3DTSS_ALPHAOP, D3DTOP_SELECTARG2);
        IDirect3DDevice8_SetTextureStageState(g_dev, 1, D3DTSS_ALPHAARG2, D3DTA_CURRENT);
        IDirect3DDevice8_SetTextureStageState(g_dev, 1, D3DTSS_MINFILTER, D3DTEXF_POINT);
        IDirect3DDevice8_SetTextureStageState(g_dev, 1, D3DTSS_MAGFILTER, D3DTEXF_POINT);
        IDirect3DDevice8_SetTextureStageState(g_dev, 1, D3DTSS_TEXCOORDINDEX, 1);
        quad_t2(Q0, Q0, Q1, Q1, 0xffffffff);
        frame_end();
        IDirect3DDevice8_SetTextureStageState(g_dev, 1, D3DTSS_COLOROP, D3DTOP_DISABLE);
        IDirect3DDevice8_SetTextureStageState(g_dev, 1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
        IDirect3DDevice8_SetTexture(g_dev, 0, NULL);
        IDirect3DDevice8_SetTexture(g_dev, 1, NULL);
        IDirect3DTexture8_Release(a);
        IDirect3DTexture8_Release(b);
        expect(t, "red cell", tpx(4, 64), tpx(4, 64), red, 14);
        expect(t, "blue cell", tpx(12, 64), tpx(4, 64), blue, 14);
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
        if (g_noz) {
            say("  ztest: skipped, no depth buffer (--noz)");
            return;
        }
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
    } else if (!strcmp(t, "tex512")) {
        test_cells(t, 512, D3DFMT_R5G6B5);
    } else if (!strcmp(t, "tex1024")) {
        test_cells(t, 1024, D3DFMT_R5G6B5);
    } else if (!strcmp(t, "tex2048")) {
        test_cells(t, 2048, D3DFMT_R5G6B5);
    } else if (!strcmp(t, "tex8888")) {
        test_cells(t, 64, D3DFMT_A8R8G8B8);
    } else if (!strcmp(t, "dxt1")) {
        test_cells(t, 128, D3DFMT_DXT1);
    } else if (!strcmp(t, "dxt3")) {
        test_cells(t, 128, D3DFMT_DXT3);
    } else if (!strcmp(t, "dxt5")) {
        test_cells(t, 128, D3DFMT_DXT5);
    } else if (!strcmp(t, "mip2048")) {
        test_mip2048(t);
    } else if (!strcmp(t, "texhigh")) {
        test_texhigh(t);
    } else {
        say("  unknown test %s", t);
    }
}

int main(int argc, char **argv)
{
    const char *mode = "caps", *tests = "clear,flat,gouraud,gouraudb,tex,modulate,blend,ztest,bigtex,present,fogtable,fogvertex,tex2mod,tex2add";
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
    DWORD pace;
    const char *bad_pace = NULL, *bad_zfmt = NULL;

    /* A crash must die at once, not sit behind a Watson / "has encountered a
     * problem" box: that box keeps the process - and a fullscreen mode it
     * set - alive until someone at the box clicks it, and the fullscreen mode
     * hides it. The box is driven remotely; nobody is at .124's CRT. */
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    for (i = 1; i < argc; i++) {
        const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(a, "--res") && v) { sscanf(v, "%dx%d", &g_w, &g_h); i++; }
        else if (!strcmp(a, "--bpp") && v) { g_bpp = atoi(v); i++; }
        else if (!strcmp(a, "--frames") && v) { g_frames = atoi(v); i++; }
        else if (!strcmp(a, "--pace") && v) {
            /* atoi("-1") was a floor of 0xFFFFFFFF ms: refused, not wrapped */
            if (vcr_pace_parse_ms(v, &pace))
                vcr_pace_set_min(pace);
            else
                bad_pace = v;
            i++;
        }
        else if (!strcmp(a, "--tests") && v) { tests = v; i++; }
        else if (!strcmp(a, "--log") && v) { strncpy(g_logpath, v, sizeof g_logpath - 1); i++; }
        else if (!strcmp(a, "--full")) g_full = 1;
        else if (!strcmp(a, "--novsync")) g_novsync = 1;
        else if (!strcmp(a, "--noz")) g_noz = 1;
        else if (!strcmp(a, "--zfmt") && v) {
            if ((g_zfmt = zfmt_named(v)) == D3DFMT_UNKNOWN)
                bad_zfmt = v;
            i++;
        }
        else if (a[0] != '-') mode = a;
    }
    g_log = fopen(g_logpath, "w");
    say("d3dprobe %s: %s %dx%dx%d%s", mode, g_full ? "fullscreen" : "windowed", g_w, g_h, g_bpp,
        g_noz ? ", no depth buffer" : "");
    if (bad_pace) {
        say("RESULT {\"mode\":\"%s\",\"error\":\"--pace %s: decimal milliseconds, 0 to %u\"}",
            mode, bad_pace, VCR_PACE_MAX_MS);
        return 2;
    }
    /* before any device or switch: a format that is not named, or one asked
     * for with no depth buffer at all */
    if (bad_zfmt || (g_zfmt != D3DFMT_UNKNOWN && g_noz)) {
        say("RESULT {\"mode\":\"%s\",\"error\":\"--zfmt %s: d16, d24x8 or d24s8, and not with "
            "--noz\"}", mode, bad_zfmt ? bad_zfmt : "given");
        return 2;
    }

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
            { D3DFMT_DXT3, "texDXT3", 0, D3DRTYPE_TEXTURE },
            { D3DFMT_DXT5, "texDXT5", 0, D3DRTYPE_TEXTURE },
            { D3DFMT_D16, "z16", D3DUSAGE_DEPTHSTENCIL, D3DRTYPE_SURFACE },
            { D3DFMT_D24X8, "z24x8", D3DUSAGE_DEPTHSTENCIL, D3DRTYPE_SURFACE },
            { D3DFMT_D24S8, "z24s8", D3DUSAGE_DEPTHSTENCIL, D3DRTYPE_SURFACE },
        };
        /* the depth/stencil questions a D3D8 application asks before it
         * creates a device, per adapter (= target) format: is the Z format
         * there (CheckDeviceFormat) and does it pair with the target
         * (CheckDepthStencilMatch). The HAL lists the Z of every render
         * depth it offers, whatever the desktop is (vcr_rtfmt.h
         * vcr_rt_zlist): D16 always, D24X8/D24S8 beside it with Diag\\D3D32.
         * A DX7-DDI HAL's Z must match the target's size, so the expected
         * MATCH is D24X8/D24S8 for X8R8G8B8 and D16 for R5G6B5 - and D16 must
         * stay a format at a 32 bpp desktop with the switch armed, or the
         * proven 16 bpp fullscreen device made from that desktop is refused.
         * Asked for BOTH adapter formats: no mode is switched. */
        static const struct { D3DFORMAT f; const char *n; } zrt[] = {
            { D3DFMT_X8R8G8B8, "X8R8G8B8" }, { D3DFMT_R5G6B5, "R5G6B5" } };
        static const struct { D3DFORMAT f; const char *n; } zds[] = {
            { D3DFMT_D24X8, "D24X8" }, { D3DFMT_D24S8, "D24S8" }, { D3DFMT_D16, "D16" } };
        int j;
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
        js("},\"hal_fullscreen\":{");
        for (i = 0; i < (int)(sizeof zrt / sizeof zrt[0]); i++) {
            HRESULT t = IDirect3D8_CheckDeviceType(d3d, 0, D3DDEVTYPE_HAL, zrt[i].f, zrt[i].f,
                                                   FALSE);
            say("fullscreen HAL device at %s: %s (%08lx)", zrt[i].n, SUCCEEDED(t) ? "yes" : "no", t);
            js("%s\"%s\":%d", i ? "," : "", zrt[i].n, SUCCEEDED(t));
        }
        js("},\"zmatch\":{");
        for (i = 0; i < (int)(sizeof zrt / sizeof zrt[0]); i++) {
            js("%s\"%s\":{", i ? "," : "", zrt[i].n);
            for (j = 0; j < (int)(sizeof zds / sizeof zds[0]); j++) {
                HRESULT f = IDirect3D8_CheckDeviceFormat(d3d, 0, D3DDEVTYPE_HAL, zrt[i].f,
                                                         D3DUSAGE_DEPTHSTENCIL, D3DRTYPE_SURFACE,
                                                         zds[j].f);
                HRESULT m = IDirect3D8_CheckDepthStencilMatch(d3d, 0, D3DDEVTYPE_HAL, zrt[i].f,
                                                              zrt[i].f, zds[j].f);
                say("depth %s with %s: format %s (%08lx), match %s (%08lx)", zds[j].n, zrt[i].n,
                    SUCCEEDED(f) ? "yes" : "no", f, SUCCEEDED(m) ? "yes" : "no", m);
                js("%s\"%s\":{\"format\":%d,\"match\":%d}", j ? "," : "", zds[j].n,
                   SUCCEEDED(f), SUCCEEDED(m));
            }
            js("}");
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
    /* --noz: no depth buffer - the first 32 bpp step on silicon exercises
     * renderMode and the colour fastfill before anything writes the aux buffer */
    pp.EnableAutoDepthStencil = !g_noz;
    pp.AutoDepthStencilFormat = g_noz ? D3DFMT_UNKNOWN : D3DFMT_D16;
    /* a 32 bpp back buffer takes a Z of its own size where the HAL has one
     * (a DX7-DDI driver's Z must match the target's depth: the VSA-100 aux
     * buffer is 24+8 at 32 bpp) - D24X8, then D24S8; else D16 as before */
    if (!g_noz &&
        (pp.BackBufferFormat == D3DFMT_X8R8G8B8 || pp.BackBufferFormat == D3DFMT_A8R8G8B8)) {
        static const D3DFORMAT z32[] = { D3DFMT_D24X8, D3DFMT_D24S8 };
        D3DFORMAT afmt = g_full ? pp.BackBufferFormat : dm.Format;
        int k;
        for (k = 0; k < 2; k++)
            if (SUCCEEDED(IDirect3D8_CheckDeviceFormat(d3d, 0, D3DDEVTYPE_HAL, afmt,
                                                       D3DUSAGE_DEPTHSTENCIL, D3DRTYPE_SURFACE,
                                                       z32[k])) &&
                SUCCEEDED(IDirect3D8_CheckDepthStencilMatch(d3d, 0, D3DDEVTYPE_HAL, afmt,
                                                            pp.BackBufferFormat, z32[k]))) {
                pp.AutoDepthStencilFormat = z32[k];
                break;
            }
    }
    /* --zfmt: exactly what was asked, checked by nobody here - the runtime
     * and the HAL decide, and the RESULT says which one refused it */
    if (g_zfmt != D3DFMT_UNKNOWN)
        pp.AutoDepthStencilFormat = g_zfmt;
    pp.Flags = D3DPRESENTFLAG_LOCKABLE_BACKBUFFER;
    pp.hDeviceWindow = hwnd;
    if (g_full && g_novsync)        /* perf: measure the chip, not the refresh */
        pp.FullScreen_PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    g_fmt = pp.BackBufferFormat;
    say("CreateDevice(HAL, %s, back buffer %ux%u fmt %u, z fmt %u%s)",
        g_full ? "fullscreen" : "windowed", pp.BackBufferWidth, pp.BackBufferHeight,
        pp.BackBufferFormat, pp.AutoDepthStencilFormat, g_noz ? " - none, --noz" : "");
    /* fullscreen: CreateDevice is the switch in (through vcr_pace.h). The
     * failure return below is a return from main, so the header's atexit hold
     * covers the revert XP makes as the process ends. Refused by the gate,
     * no device is created and the run ends here. */
    if (g_full && !vcr_pace_before_switch()) {
        say("RESULT {\"mode\":\"%s\",\"error\":\"CreateDevice not made: %s\",\"adapter\":\"%s\"}",
            mode, g_vcr_pace_why, id.Description);
        return 2;
    }
    hr = IDirect3D8_CreateDevice(d3d, 0, D3DDEVTYPE_HAL, hwnd, D3DCREATE_SOFTWARE_VERTEXPROCESSING,
                                 &pp, &g_dev);
    /* even when it failed: the runtime sets the display mode before it
     * creates the swap chain's buffers and the depth buffer, so a failure
     * after that point was a re-sync - and may leave the mode set until exit.
     * Counting it only costs a failed run the hold. */
    if (g_full)
        vcr_pace_after_switch();
    if (FAILED(hr)) {
        say("RESULT {\"mode\":\"%s\",\"error\":\"CreateDevice %08lx\",\"adapter\":\"%s\","
            "\"fmt\":%u,\"zfmt\":%u}", mode, hr, id.Description, (unsigned)pp.BackBufferFormat,
            g_noz ? 0u : (unsigned)pp.AutoDepthStencilFormat);
        return 2;
    }
    g_held = g_full;
    say("device created");
    /* what the texture tests ask before they run (skipped, not failed, when
     * the HAL does not offer it): the caps, and formats at the device's mode */
    g_d3d = d3d;
    g_adfmt = g_full ? pp.BackBufferFormat : dm.Format;
    memset(&g_caps, 0, sizeof g_caps);
    IDirect3DDevice8_GetDeviceCaps(g_dev, &g_caps);
    say("MaxTexture %lux%lu", g_caps.MaxTextureWidth, g_caps.MaxTextureHeight);
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
        for (f = 0; f < g_frames && !g_focus_lost; f++) {
            pump();
            IDirect3DDevice8_Clear(g_dev, 0, NULL, D3DCLEAR_TARGET, 0x00202060, 1.0f, 0);
            IDirect3DDevice8_BeginScene(g_dev);
            IDirect3DDevice8_DrawPrimitiveUP(g_dev, D3DPT_TRIANGLELIST, N, tri, sizeof tri[0]);
            IDirect3DDevice8_EndScene(g_dev);
            IDirect3DDevice8_Present(g_dev, NULL, NULL, NULL, NULL);
        }
        dt = now_s() - t0;
        say("RESULT {\"mode\":\"perf\",\"adapter\":\"%s\",\"frames\":%d,\"frames_run\":%d,"
            "\"tris_per_frame\":%d,\"fps\":%.1f,\"tris_s\":%.0f%s}", id.Description, g_frames,
            f, N, dt > 0 ? f / dt : 0.0, dt > 0 ? f * N / dt : 0.0, focus_json());
        if (!release_device()) {
            say_not_released("perf");
            return 4;
        }
        return g_focus_lost ? 5 : 0;
    }

    js("{\"mode\":\"render\",\"adapter\":\"%s\",\"window\":\"%s\",\"fmt\":%u,\"zfmt\":%u,"
       "\"noz\":%d,\"checks\":[", id.Description, g_full ? "fullscreen" : "windowed", g_fmt,
       (unsigned)pp.AutoDepthStencilFormat, g_noz);
    {
        char list[512], *p, *save;
        strncpy(list, tests, sizeof list - 1);
        list[sizeof list - 1] = 0;
        /* a lost foreground ends the run: the tests left are not run */
        for (p = strtok_r(list, ",", &save); p && !g_focus_lost; p = strtok_r(NULL, ",", &save))
            run_test(p);
    }
    pump();                             /* a deactivation still queued is seen now */
    js("],\"readback\":\"%s\",\"pass\":%d,\"fail\":%d,\"skipped\":%d,\"max_texture\":%lu%s}",
       g_readvia, g_pass, g_fail, g_skip, g_caps.MaxTextureWidth, focus_json());
    if (!release_device()) {
        say("RESULT %s", g_json);       /* the checks as they stood ... */
        say_not_released("render");     /* ... and, last, why the run failed */
        return 4;
    }
    IDirect3D8_Release(d3d);
    say("RESULT %s", g_json);
    return g_fail || g_focus_lost ? 1 : 0;
}
