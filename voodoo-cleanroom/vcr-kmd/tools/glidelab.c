/*
 * glidelab.c - a Glide 3 test program for the kernel driver: specific things,
 * measured, on any SLI/AA configuration, with no game in the way.
 *
 * Modes (one per run; the SLI/AA topology is SSTH3_SLI_AA_CONFIGURATION, set
 * in the registry and/or with --cfg, which sets it in-process):
 *
 *   fill     fill rate: `--layers` full-screen quads per frame (flat colour,
 *            no depth, no texture) for `--frames` frames, grFinish, timed with
 *            the performance counter -> Mpixel/s and fps. --blend adds alpha
 *            blending, which READS the colour buffer: the memory-bandwidth
 *            variant (what scanout at a higher refresh takes away).
 *   bands    every scanline its own exact RGB565 value (dither off), read back
 *            through grLfbReadRegion - under SLI that read gathers each band
 *            from the chip that owns it - and compared line by line. Reports
 *            the bad lines grouped by owning chip, so "chip 2 drew nothing"
 *            or "the slaves are one band off" is a number. --origin lower
 *            repeats it with the lower-left origin (Y flipped per chip).
 *   cycle    open / draw / close `--cycles` times in one process: SLI set up
 *            and torn down again and again (the kernel's enable/disable path).
 *   abandon  open, draw, and exit WITHOUT grSstWinClose/grGlideShutdown -
 *            what a crashed or force-killed game leaves; the kernel driver
 *            must put the desktop back and turn SLI off by itself. Run it,
 *            then anything else, and check the second run and the desktop.
 *
 * Every step is flushed to the log before the next (glideprobe's rule: when
 * the board wedges the box, the last line names the call). The final line is
 * `RESULT {json}`.
 *
 * Glide is late-bound (any glide3x.dll: --dll), as in glideprobe.c.
 *
 * Build: make glidelab (Makefile; needs the Glide SDK headers, GLIDE_SDK=).
 */
#include <windows.h>
#include <io.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "glide.h"

static FILE *g_log;
static char  g_logpath[MAX_PATH] = "C:\\glidelab.log";

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

/* ---- Glide, late-bound ------------------------------------------------------- */
static HMODULE g_dll;

static FARPROC sym(const char *name)
{
    char buf[128];
    FARPROC p = GetProcAddress(g_dll, name);
    int n;
    if (p)
        return p;
    _snprintf(buf, sizeof buf, "_%s", name);
    if ((p = GetProcAddress(g_dll, buf)))
        return p;
    for (n = 0; n <= 32; n += 4) {
        _snprintf(buf, sizeof buf, "%s@%d", name, n);
        if ((p = GetProcAddress(g_dll, buf)))
            return p;
        _snprintf(buf, sizeof buf, "_%s@%d", name, n);
        if ((p = GetProcAddress(g_dll, buf)))
            return p;
    }
    return NULL;
}

#define GFN(ret, name, args) static ret (__stdcall *p_##name) args;
GFN(void, grGlideInit, (void))
GFN(void, grGlideShutdown, (void))
GFN(void, grSstSelect, (int))
GFN(FxU32, grGet, (FxU32, FxU32, FxI32 *))
GFN(const char *, grGetString, (FxU32))
GFN(FxU32, grSstWinOpen, (FxU32, GrScreenResolution_t, GrScreenRefresh_t, GrColorFormat_t,
                          GrOriginLocation_t, int, int))
GFN(FxBool, grSstWinClose, (FxU32))
GFN(void, grBufferClear, (GrColor_t, GrAlpha_t, FxU32))
GFN(void, grBufferSwap, (FxU32))
GFN(void, grFinish, (void))
GFN(void, grVertexLayout, (FxU32, FxI32, FxU32))
GFN(void, grDrawTriangle, (const void *, const void *, const void *))
GFN(void, grColorCombine, (GrCombineFunction_t, GrCombineFactor_t, GrCombineLocal_t,
                           GrCombineOther_t, FxBool))
GFN(void, grAlphaCombine, (GrCombineFunction_t, GrCombineFactor_t, GrCombineLocal_t,
                           GrCombineOther_t, FxBool))
GFN(void, grConstantColorValue, (GrColor_t))
GFN(void, grDitherMode, (GrDitherMode_t))
GFN(void, grAlphaBlendFunction, (GrAlphaBlendFnc_t, GrAlphaBlendFnc_t, GrAlphaBlendFnc_t,
                                 GrAlphaBlendFnc_t))
GFN(void, grDepthBufferMode, (GrDepthBufferMode_t))
GFN(void, grDepthMask, (FxBool))
GFN(void, grCullMode, (GrCullMode_t))
GFN(void, grSstOrigin, (GrOriginLocation_t))
GFN(FxBool, grLfbReadRegion, (GrBuffer_t, FxU32, FxU32, FxU32, FxU32, FxU32, void *))

static int bind_glide(void)
{
#define B(name) p_##name = (void *)sym(#name)
    B(grGlideInit); B(grGlideShutdown); B(grSstSelect); B(grGet); B(grGetString);
    B(grSstWinOpen); B(grSstWinClose); B(grBufferClear); B(grBufferSwap); B(grFinish);
    B(grVertexLayout); B(grDrawTriangle); B(grColorCombine); B(grAlphaCombine);
    B(grConstantColorValue); B(grDitherMode); B(grAlphaBlendFunction);
    B(grDepthBufferMode); B(grDepthMask); B(grCullMode); B(grSstOrigin); B(grLfbReadRegion);
#undef B
    if (!p_grGlideInit || !p_grSstWinOpen || !p_grSstWinClose || !p_grDrawTriangle ||
        !p_grVertexLayout || !p_grColorCombine || !p_grConstantColorValue ||
        !p_grBufferSwap || !p_grBufferClear || !p_grFinish || !p_grLfbReadRegion) {
        say("FAIL: a required Glide entry point is missing");
        return 0;
    }
    return 1;
}

/* ---- window (grSstWinOpen with hWnd 0 hangs - see glideprobe.c) --------------- */
static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_CLOSE)
        return 0;
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

static HWND make_window(int w, int h)
{
    WNDCLASSA wc;
    HWND hwnd;
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = "glidelab";
    RegisterClassA(&wc);
    hwnd = CreateWindowExA(WS_EX_TOPMOST, "glidelab", "glidelab", WS_POPUP | WS_VISIBLE,
                           0, 0, w, h, NULL, NULL, wc.hInstance, NULL);
    if (hwnd) {
        ShowWindow(hwnd, SW_SHOW);
        SetForegroundWindow(hwnd);
        pump();
    }
    return hwnd;
}

/* ---- options ----------------------------------------------------------------------- */
static struct {
    const char *mode, *dll, *res;
    int w, h, hz, cfg, frames, layers, blend, cycles, lower;
} O = { "fill", "glide3x.dll", "640x480", 640, 480, 60, -1, 200, 4, 0, 10, 0 };

static const struct { const char *name; int code; } RES[] = {
    { "640x480", GR_RESOLUTION_640x480 },   { "800x600", GR_RESOLUTION_800x600 },
    { "1024x768", GR_RESOLUTION_1024x768 }, { "1280x960", GR_RESOLUTION_1280x960 },
    { "1280x1024", GR_RESOLUTION_1280x1024 }, { "1600x1200", GR_RESOLUTION_1600x1200 },
    { NULL, 0 }
};
static const struct { int hz, code; } HZ[] = {
    { 60, GR_REFRESH_60Hz }, { 70, GR_REFRESH_70Hz }, { 72, GR_REFRESH_72Hz },
    { 75, GR_REFRESH_75Hz }, { 85, GR_REFRESH_85Hz }, { 100, GR_REFRESH_100Hz },
    { 120, GR_REFRESH_120Hz }, { 0, 0 }
};

typedef struct { float x, y; } vtx;

static void quad(float x0, float y0, float x1, float y1)
{
    vtx a = { x0, y0 }, b = { x1, y0 }, c = { x1, y1 }, d = { x0, y1 };
    p_grDrawTriangle(&a, &b, &c);
    p_grDrawTriangle(&a, &c, &d);
}

static void flat_state(void)
{
    p_grVertexLayout(GR_PARAM_XY, 0, GR_PARAM_ENABLE);
    p_grColorCombine(GR_COMBINE_FUNCTION_LOCAL, GR_COMBINE_FACTOR_NONE,
                     GR_COMBINE_LOCAL_CONSTANT, GR_COMBINE_OTHER_NONE, FXFALSE);
    if (p_grAlphaCombine)
        p_grAlphaCombine(GR_COMBINE_FUNCTION_LOCAL, GR_COMBINE_FACTOR_NONE,
                         GR_COMBINE_LOCAL_CONSTANT, GR_COMBINE_OTHER_NONE, FXFALSE);
    if (p_grDepthBufferMode)
        p_grDepthBufferMode(GR_DEPTHBUFFER_DISABLE);
    if (p_grDepthMask)
        p_grDepthMask(FXFALSE);
    if (p_grCullMode)
        p_grCullMode(GR_CULL_DISABLE);
    if (p_grDitherMode)
        p_grDitherMode(GR_DITHER_DISABLE);
    if (p_grAlphaBlendFunction)
        p_grAlphaBlendFunction(GR_BLEND_ONE, GR_BLEND_ZERO, GR_BLEND_ONE, GR_BLEND_ZERO);
}

static FxU32 open_board(HWND hwnd, int rescode, int hzcode)
{
    FxU32 ctx;
    say("step: grSstWinOpen %s %dHz origin %s", O.res, O.hz, O.lower ? "lower" : "upper");
    pump();
    ctx = p_grSstWinOpen((FxU32)(uintptr_t)hwnd, (GrScreenResolution_t)rescode,
                         (GrScreenRefresh_t)hzcode, GR_COLORFORMAT_ARGB,
                         O.lower ? GR_ORIGIN_LOWER_LEFT : GR_ORIGIN_UPPER_LEFT, 2, 1);
    pump();
    say("  -> context 0x%lx", (unsigned long)ctx);
    return ctx;
}

static int chips_in_use(void)
{
    FxI32 v = 1;
    if (p_grGet && p_grGet(GR_NUM_FB, sizeof v, &v))
        return (int)v;
    return 1;
}

static double now_s(void)
{
    LARGE_INTEGER f, t;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart / (double)f.QuadPart;
}

/* ---- the modes ---------------------------------------------------------------------- */
static int do_fill(void)
{
    int f, l;
    double t0, t;
    flat_state();
    if (O.blend && p_grAlphaBlendFunction)
        p_grAlphaBlendFunction(GR_BLEND_SRC_ALPHA, GR_BLEND_ONE_MINUS_SRC_ALPHA,
                               GR_BLEND_ONE, GR_BLEND_ZERO);
    /* warm up */
    for (f = 0; f < 10; f++) {
        p_grBufferClear(0, 0, 0xffff);
        p_grConstantColorValue(0x80406080u);
        quad(0, 0, (float)O.w, (float)O.h);
        p_grBufferSwap(0);
        pump();
    }
    p_grFinish();
    say("step: %d frames x %d layers%s", O.frames, O.layers, O.blend ? " (blended)" : "");
    t0 = now_s();
    for (f = 0; f < O.frames; f++) {
        for (l = 0; l < O.layers; l++) {
            p_grConstantColorValue(0x80000000u | ((f * 7 + l * 53) & 0xff) << 16 |
                                   ((l * 91) & 0xff) << 8 | (f & 0xff));
            quad(0, 0, (float)O.w, (float)O.h);
        }
        p_grBufferSwap(0);
        if ((f & 15) == 0)
            pump();
    }
    p_grFinish();
    t = now_s() - t0;
    say("RESULT {\"mode\":\"fill\",\"res\":\"%s\",\"cfg\":%d,\"chips\":%d,\"frames\":%d,"
        "\"layers\":%d,\"blend\":%d,\"seconds\":%.4f,\"fps\":%.2f,\"mpix_s\":%.1f}",
        O.res, O.cfg, chips_in_use(), O.frames, O.layers, O.blend, t, O.frames / t,
        (double)O.w * O.h * O.layers * O.frames / t / 1e6);
    return 0;
}

static unsigned line_code(int y)
{
    unsigned v = (unsigned)y * 40503u + 0x2d1u;
    return (v ^ (v >> 7)) & 0xffff;
}

static GrColor_t code_to_argb(unsigned v)
{
    unsigned r = (v >> 11) & 31, g = (v >> 5) & 63, b = v & 31;
    return 0xff000000u | (r << 3) << 16 | (g << 2) << 8 | (b << 3);
}

static int do_bands(void)
{
    int y, chips = chips_in_use(), nl = 8, bad = 0, badc[8] = { 0 }, first_bad = -1;
    unsigned short *buf = (unsigned short *)malloc((size_t)O.w * O.h * 2);
    const char *e = getenv("SSTH3_SLI_BAND_HEIGHT");
    if (e)
        nl = atoi(e);
    if (!buf)
        return 9;
    flat_state();
    p_grBufferClear(0, 0, 0xffff);
    for (y = 0; y < O.h; y++) {
        p_grConstantColorValue(code_to_argb(line_code(y)));
        quad(0, (float)y, (float)O.w, (float)y + 1);
    }
    p_grFinish();
    say("step: grLfbReadRegion back buffer %dx%d", O.w, O.h);
    if (!p_grLfbReadRegion(GR_BUFFER_BACKBUFFER, 0, 0, O.w, O.h, O.w * 2, buf)) {
        say("RESULT {\"mode\":\"bands\",\"error\":\"grLfbReadRegion refused\"}");
        free(buf);
        return 6;
    }
    for (y = 0; y < O.h; y++) {
        int x, ok = 1, row = y;
        unsigned want = line_code(y);
        for (x = 0; x < O.w; x += 7)
            if (buf[(size_t)row * O.w + x] != want) {
                ok = 0;
                break;
            }
        if (!ok) {
            bad++;
            badc[(y / (nl ? nl : 1)) % (chips > 0 && chips <= 8 ? chips : 1)]++;
            if (first_bad < 0) {
                first_bad = y;
                say("  first bad line %d: read %04x at x=%d, wanted %04x", y,
                    buf[(size_t)row * O.w + x], x, want);
            }
        }
    }
    p_grBufferSwap(0);
    say("RESULT {\"mode\":\"bands\",\"res\":\"%s\",\"cfg\":%d,\"chips\":%d,\"origin\":\"%s\","
        "\"band_lines\":%d,\"bad_lines\":%d,\"bad_per_chip\":[%d,%d,%d,%d],\"first_bad\":%d}",
        O.res, O.cfg, chips, O.lower ? "lower" : "upper", nl, bad, badc[0], badc[1], badc[2],
        badc[3], first_bad);
    free(buf);
    return bad ? 7 : 0;
}

int main(int argc, char **argv)
{
    int i, rescode = -1, hzcode = GR_REFRESH_60Hz, rc = 0, c;
    HWND hwnd;
    FxU32 ctx;

    for (i = 1; i < argc; i++) {
        const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(a, "--res") && v) { O.res = v; i++; }
        else if (!strcmp(a, "--refresh") && v) { O.hz = atoi(v); i++; }
        else if (!strcmp(a, "--cfg") && v) { O.cfg = atoi(v); i++; }
        else if (!strcmp(a, "--frames") && v) { O.frames = atoi(v); i++; }
        else if (!strcmp(a, "--layers") && v) { O.layers = atoi(v); i++; }
        else if (!strcmp(a, "--cycles") && v) { O.cycles = atoi(v); i++; }
        else if (!strcmp(a, "--dll") && v) { O.dll = v; i++; }
        else if (!strcmp(a, "--log") && v) { strncpy(g_logpath, v, sizeof g_logpath - 1); i++; }
        else if (!strcmp(a, "--origin") && v) { O.lower = !strcmp(v, "lower"); i++; }
        else if (!strcmp(a, "--blend")) O.blend = 1;
        else if (a[0] != '-') O.mode = a;
    }
    g_log = fopen(g_logpath, "w");
    sscanf(O.res, "%dx%d", &O.w, &O.h);
    for (i = 0; RES[i].name; i++)
        if (!strcmp(RES[i].name, O.res))
            rescode = RES[i].code;
    for (i = 0; HZ[i].hz; i++)
        if (HZ[i].hz == O.hz)
            hzcode = HZ[i].code;
    say("glidelab %s: res=%s refresh=%d cfg=%d dll=%s", O.mode, O.res, O.hz, O.cfg, O.dll);
    if (rescode < 0) {
        say("RESULT {\"error\":\"no GR_RESOLUTION for %s\"}", O.res);
        return 2;
    }
    if (O.cfg >= 0) {
        char env[16];
        _snprintf(env, sizeof env, "%d", O.cfg);
        SetEnvironmentVariableA("SSTH3_SLI_AA_CONFIGURATION", env);
    }
    g_dll = LoadLibraryA(O.dll);
    if (!g_dll || !bind_glide()) {
        say("RESULT {\"error\":\"cannot load %s (%lu)\"}", O.dll, GetLastError());
        return 3;
    }
    say("step: grGlideInit");
    p_grGlideInit();
    if (p_grSstSelect)
        p_grSstSelect(0);
    if (p_grGetString)
        say("GR_HARDWARE: %s", p_grGetString(GR_HARDWARE));
    hwnd = make_window(O.w, O.h);

    if (!strcmp(O.mode, "cycle")) {
        int ok = 0;
        double t0 = now_s();
        for (c = 0; c < O.cycles; c++) {
            int f;
            ctx = open_board(hwnd, rescode, hzcode);
            if (!ctx) {
                say("  cycle %d: open REFUSED", c);
                break;
            }
            flat_state();
            for (f = 0; f < 20; f++) {
                p_grBufferClear(0x00102030u + (unsigned)f, 0, 0xffff);
                p_grConstantColorValue(0xff000000u | (unsigned)(c * 997 + f * 31));
                quad(0, 0, (float)O.w, (float)O.h);
                p_grBufferSwap(0);
                pump();
            }
            p_grFinish();
            say("  cycle %d: chips %d, closing", c, chips_in_use());
            p_grSstWinClose(ctx);
            pump();
            ok++;
        }
        say("RESULT {\"mode\":\"cycle\",\"res\":\"%s\",\"cfg\":%d,\"cycles\":%d,\"ok\":%d,"
            "\"seconds\":%.2f}", O.res, O.cfg, O.cycles, ok, now_s() - t0);
        rc = ok == O.cycles ? 0 : 8;
    } else {
        ctx = open_board(hwnd, rescode, hzcode);
        if (!ctx) {
            say("RESULT {\"mode\":\"%s\",\"error\":\"grSstWinOpen refused\"}", O.mode);
            if (p_grGlideShutdown)
                p_grGlideShutdown();
            return 5;
        }
        say("chips in use: %d", chips_in_use());
        if (!strcmp(O.mode, "fill")) {
            rc = do_fill();
        } else if (!strcmp(O.mode, "bands")) {
            rc = do_bands();
        } else if (!strcmp(O.mode, "abandon")) {
            int f;
            flat_state();
            for (f = 0; f < 30; f++) {
                p_grBufferClear(0x00400000u, 0, 0xffff);
                p_grConstantColorValue(0xff00ff00u);
                quad(0, 0, (float)O.w / 2, (float)O.h / 2);
                p_grBufferSwap(1);
                pump();
            }
            say("RESULT {\"mode\":\"abandon\",\"res\":\"%s\",\"cfg\":%d,\"chips\":%d,"
                "\"note\":\"exiting WITHOUT grSstWinClose/grGlideShutdown\"}",
                O.res, O.cfg, chips_in_use());
            ExitProcess(0);
        } else {
            say("RESULT {\"error\":\"unknown mode %s\"}", O.mode);
            rc = 2;
        }
        say("step: grSstWinClose");
        p_grSstWinClose(ctx);
    }
    say("step: grGlideShutdown");
    if (p_grGlideShutdown)
        p_grGlideShutdown();
    if (hwnd)
        DestroyWindow(hwnd);
    say("done rc=%d", rc);
    return rc;
}
