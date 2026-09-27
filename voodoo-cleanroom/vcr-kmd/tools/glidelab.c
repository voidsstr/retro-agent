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
 *            At most GLIDELAB_MAX_CYCLES, whatever is asked: every open and
 *            every close is a monitor re-sync.
 *   abandon  open, draw, and exit WITHOUT grSstWinClose/grGlideShutdown -
 *            what a crashed or force-killed game leaves; the kernel driver
 *            must put the desktop back and turn SLI off by itself. Run it,
 *            then anything else, and check the second run and the desktop.
 *
 * Every mode: every grSstWinOpen and grSstWinClose waits out vcr_pace.h's
 * floor (3 s, or `--pace MS` if longer - decimal, at most 30000, anything else
 * refused before anything switches) and the mode is held that long. A switch
 * the gate cannot pace (its lock held by a stuck tool, a stamp that never
 * stops moving) is not made: the run ends with a RESULT "error" naming why,
 * and a mode already up is left for the exit hold.
 *
 * The refresh: --refresh must be one of the HZ[] rates (refused otherwise,
 * before anything switches), it is handed to Glide as FX_GLIDE_REFRESH as well
 * as the grSstWinOpen code, and the rate the mode really opened at is read
 * back ("opened_hz"): a mode at any other rate is closed again, paced, and the
 * run fails - it is a rate no host gate checked.
 *
 * A window that loses the foreground while the board holds the mode
 * (DirectDraw gives the desktop back at that moment, unpaced) has that switch
 * recorded, ends the run through the paced close and says "focus_lost":true,
 * rather than let a re-activation switch in again.
 *
 * Every step is flushed to the log before the next (glideprobe's rule: when
 * the board wedges the box, the last line names the call). The final line is
 * `RESULT {json}`; the host reads the LAST one.
 *
 * --trace N (0-2) turns on our h5 Glide's own step trace (fork AA-TRACE:
 * FX_GLIDE_TRACE=N) into <log>.trace - each line flushed to the disk before
 * the hardware access it names, level 2 also waiting after every FIFO step
 * of the open until the chips have run it. That is what names the step
 * INSIDE grSstWinOpen / grLfbReadRegion a deep wedge froze on, where this
 * log can only name the call. For an AA open - by --cfg, or by what Glide
 * reads without it (effective_config) - it also sets FX_GLIDE_NO_SPLASH=1 and
 * FX_GLIDE_NO_PLUGIN=1: NO_SPLASH alone gates only grSplash(), and the
 * third-party plugin's init still drew through Glide inside the open.
 * --trace-cfg adds the fork's config-space dumps (FX_GLIDE_TRACE_CFG=1: 36
 * PCI_OP escapes per dump on the V5 6000); without it a level-1 trace adds
 * file writes only. A traced run is a diagnosis, not a benchmark: its RESULT
 * says "trace":N. A Glide that is not a traced build ignores it (the host
 * sees no trace header).
 *
 * An AA open is REFUSED (RESULT error, rc 13, before any window or mode) on a
 * Glide whose GR_EXTENSION lacks RETRO3DFX_SLIAA_GUARD: the fork commits that
 * add the guards were local-only, and a Glide built from origin before them
 * sent the request that froze .124 on cfg 1.
 *
 * Two more of our Glide's opt-in settings, made where Glide reads them - the
 * process environment (glide_env), never the registry, before the DLL loads:
 *   --aa-lfb-read --i-am-at-the-box   RETRO_GLIDE_AA_LFB_READ=1: our Glide
 *            then lets a READ lock through in a multi-chip AA mode - the read
 *            that froze .124 on cfg 3 (fork SLIAA-GUARD, glfb.c). Refused
 *            (rc 2, before anything loads) without the confirmation; without
 *            the flag an INHERITED value is set to 0, so only this command
 *            line can opt in. RESULT "aa_lfb_read":1 when it did.
 *   --maplog PATH   RETRO_GLIDE_MAPLOG=PATH: our Glide appends every mapping
 *            it dereferences (base0, base1, the slaves' registers, with what
 *            VirtualQuery says) - the file is deleted first, one run one log.
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
#include "vcr_pace.h"

/* 2 open/close pairs already exercise enable -> disable -> enable; the 10 this
 * used to default to were 20 re-syncs of .124's CRT ~0.3 s apart (2026-09-26) */
#define GLIDELAB_MAX_CYCLES 3
/* FX_GLIDE_TRACE levels the fork knows: 1 steps, 2 steps + a sync per step */
#define GLIDELAB_MAX_TRACE 2

static FILE *g_log;
static char  g_logpath[MAX_PATH] = "C:\\glidelab.log";
static char  g_tracepath[MAX_PATH + 8];         /* <log>.trace, --trace only */

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
static int g_held;              /* a context holds (or may hold) a fullscreen mode */
static int g_focus_lost;        /* ... and the window lost the foreground meanwhile */

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_CLOSE)
        return 0;
    /* Deactivated while the board holds the mode: DirectDraw's hook on this
     * window (Glide takes the screen through it) gives the desktop back right
     * now - a switch the gate did not pace. Record it, and end the run:
     * carrying on until something re-activates the window would switch the
     * mode back in, unpaced too. */
    if (m == WM_ACTIVATEAPP && !w && g_held) {
        vcr_pace_mark();
        g_focus_lost = 1;
    }
    return DefWindowProcA(h, m, w, l);
}

/* Once the foreground is lost, nothing more is dispatched: a re-activation
 * still queued would have DirectDraw switch the mode back in, unpaced, and
 * the run is ending anyway. */
static void pump(void)
{
    MSG msg;
    while (!g_focus_lost && PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
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
    int w, h, hz, cfg, frames, layers, blend, cycles, lower, trace, trace_cfg;
} O = { "fill", "glide3x.dll", "640x480", 640, 480, 60, -1, 200, 4, 0, 2, 0, 0, 0 };
/* Glide environment pass-throughs (kept out of O, whose layout the host tests
 * pin): --aa-lfb-read, its --i-am-at-the-box, --maplog PATH */
static int         g_aa_lfb_read, g_at_box;
static const char *g_maplog;

/* SSTH3_SLI_AA_CONFIGURATION values that turn anti-aliasing on (Glide's
 * gpci.c: 1 = 2-sample on one chip, 3/6 = 2-sample, 4/7 = 4-sample, 8 =
 * 8-sample). 0/2/5 are single chip / SLI without AA; -1 = not given. */
static int aa_config(int cfg)
{
    return cfg == 1 || cfg == 3 || cfg == 4 || cfg == 6 || cfg == 7 || cfg == 8;
}

/* s as the inside of a JSON string: a registry key or a DLL path is full of
 * backslashes, and a RESULT the host cannot parse reads as "unparsable" */
static const char *json_esc(char *dst, size_t n, const char *s)
{
    size_t o = 0;
    for (; s && *s && o + 2 < n; s++) {
        if (*s == '\\' || *s == '"')
            dst[o++] = '\\';
        dst[o++] = ((unsigned char)*s < 0x20) ? ' ' : *s;
    }
    dst[o] = 0;
    return dst;
}

/* 1 = the Glide says it carries the fork's SLIAA-GUARD (GR_EXTENSION, space-
 * delimited as Glide lists it): it refuses an SLI/AA request no kernel can
 * program and honours the kernel's refusal. A Glide without it - the fork
 * before 631221b, which is what build-stack.sh builds from origin until the
 * fork is pushed - sent cfg 1's {4 chips, no SLI, 2-sample AA} and froze .124
 * on two kernels. An AA open is refused without it. */
static int glide_guarded(const char *ext)
{
    return ext != NULL && strstr(ext, " RETRO3DFX_SLIAA_GUARD ") != NULL;
}

/* ---- the SLI/AA configuration Glide will really open ------------------------
 * --cfg sets SSTH3_SLI_AA_CONFIGURATION in-process. Without it Glide takes its
 * own chain - our fork's hwcGetenv() on NT 5.x (minihwc.c): the process
 * environment, then HKCU, then HKLM under getRegPath()'s key (Services\3dfxvs\
 * Device0\glide when Services\3dfxvs\Device0 exists, else Services\banshee\
 * Device0\glide), REG_SZ only - and FX_GLIDE_AA_SAMPLE, through the same
 * chain, overrides the sample count (gpci.c). The same lookups here, so what
 * glidelab decides for an AA open (the splash plugin, the guard) follows what
 * Glide will open, not only what --cfg said. */
#define GLIDE_KEY_3DFXVS  "SYSTEM\\CurrentControlSet\\Services\\3dfxvs\\Device0"
#define GLIDE_KEY_BANSHEE "SYSTEM\\CurrentControlSet\\Services\\banshee\\Device0\\glide"

static int reg_sz(HKEY root, const char *key, const char *name, char *val, DWORD vlen)
{
    HKEY h;
    DWORD type = 0, n = vlen - 1;
    LONG rc;
    if (RegOpenKeyExA(root, key, 0, KEY_READ, &h) != ERROR_SUCCESS)
        return 0;
    rc = RegQueryValueExA(h, name, NULL, &type, (BYTE *)val, &n);
    RegCloseKey(h);
    if (rc != ERROR_SUCCESS || type != REG_SZ)
        return 0;
    val[n < vlen ? n : vlen - 1] = 0;
    return 1;
}

/* 1 = found; val = its text, src = where (for the step log) */
static int glide_setting(const char *name, char *val, DWORD vlen, char *src, size_t slen)
{
    const char *e = getenv(name);
    char key[128];
    HKEY h;
    if (e) {
        _snprintf(val, vlen, "%s", e);
        val[vlen - 1] = 0;
        _snprintf(src, slen, "environment");
    } else {
        if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, GLIDE_KEY_3DFXVS, 0, KEY_READ, &h) == ERROR_SUCCESS) {
            RegCloseKey(h);
            _snprintf(key, sizeof key, "%s\\glide", GLIDE_KEY_3DFXVS);
        } else {
            _snprintf(key, sizeof key, "%s", GLIDE_KEY_BANSHEE);
        }
        key[sizeof key - 1] = 0;
        if (reg_sz(HKEY_CURRENT_USER, key, name, val, vlen))
            _snprintf(src, slen, "HKCU\\%s", key);
        else if (reg_sz(HKEY_LOCAL_MACHINE, key, name, val, vlen))
            _snprintf(src, slen, "HKLM\\%s", key);
        else
            return 0;
    }
    src[slen - 1] = 0;
    return 1;
}

/* the configuration Glide will open, and whether it anti-aliases; *why says
 * where each came from */
static int g_eff_cfg = 2, g_eff_aa;
static char g_eff_why[400];

static void effective_config(void)
{
    char v[64], src[180], asrc[180];
    int aa_set = 0, aa_samples = 0;
    if (O.cfg >= 0) {
        g_eff_cfg = O.cfg;
        _snprintf(src, sizeof src, "--cfg");
    } else if (glide_setting("SSTH3_SLI_AA_CONFIGURATION", v, sizeof v, src, sizeof src)) {
        g_eff_cfg = atoi(v);
    } else {
        g_eff_cfg = 2;                              /* GLIDE_GETENV(..., 2L) */
        _snprintf(src, sizeof src, "Glide's default");
    }
    src[sizeof src - 1] = 0;
    if (glide_setting("FX_GLIDE_AA_SAMPLE", v, sizeof v, asrc, sizeof asrc)) {
        aa_set = 1;
        aa_samples = atoi(v);
    }
    g_eff_aa = aa_set ? aa_samples > 1 : aa_config(g_eff_cfg);
    _snprintf(g_eff_why, sizeof g_eff_why, "cfg %d from %s%s%s%s -> %s", g_eff_cfg, src,
              aa_set ? ", FX_GLIDE_AA_SAMPLE=" : "", aa_set ? v : "",
              aa_set ? " overrides the samples" : "", g_eff_aa ? "AA" : "no AA");
    g_eff_why[sizeof g_eff_why - 1] = 0;
}

/* RETRO_GLIDE_AA_LFB_READ for our Glide (fork glfb.c: a READ lock in a
 * multi-chip AA mode is let through only for exactly "1", read from the
 * process environment). asked = --aa-lfb-read, at_box = --i-am-at-the-box,
 * inherited = the value glidelab started with. -> -1 refused (asked without
 * the confirmation); 0 and *value = what to set, or NULL = leave it: "1" only
 * when asked AND confirmed, "0" over any inherited value otherwise - so no
 * environment left on the box can opt a glidelab run in. */
static int aa_lfb_read_env(int asked, int at_box, const char *inherited, const char **value)
{
    *value = NULL;
    if (asked && !at_box)
        return -1;
    if (asked)
        *value = "1";
    else if (inherited && *inherited)
        *value = "0";
    return 0;
}

/* --trace's value: decimal 0..GLIDELAB_MAX_TRACE, nothing else */
static int parse_trace(const char *v, int *out)
{
    if (!v || v[0] < '0' || v[0] > '0' + GLIDELAB_MAX_TRACE || v[1])
        return 0;
    *out = v[0] - '0';
    return 1;
}

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

static double now_s(void)
{
    LARGE_INTEGER f, t;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart / (double)f.QuadPart;
}

/* ---- the pace (vcr_pace.h) ------------------------------------------------------------
 * Glide takes the screen with DirectDraw SetDisplayMode and gives it back with
 * RestoreDisplayMode, so every grSstWinOpen and every grSstWinClose is a
 * monitor re-sync - on 2026-09-26 a sweep re-synced .124's CRT ~250 times at
 * two a second. Every open and close goes through the gate, in every mode. */
static double g_paced_s;        /* spent waiting out the floor: not the board's time */
static int    g_opened_hz;      /* the refresh the last open really set, 0 = none read */

/* 1 = switch; 0 = vcr_pace.h refused (g_vcr_pace_why) and nothing may switch */
static int pace_before(void)
{
    double t = now_s();
    int go = vcr_pace_before_switch();
    g_paced_s += now_s() - t;
    return go;
}

/* A setting for Glide, where Glide will SEE it. Its hwcGetenv() calls the C
 * runtime's getenv(), which reads msvcrt's own copy of the environment taken
 * when the process started - SetEnvironmentVariableA does not reach it. So
 * every glidelab --cfg / --refresh set that way was invisible, and Glide
 * fell through to the registry: on .124 the vendor's control panel had left
 * FX_GLIDE_REFRESH=75 and SSTH3_SLI_AA_CONFIGURATION=5 under
 * Services\3dfxvs\Device0\glide, so a "cfg 0 at 60 Hz" run opened cfg 5 at
 * 75 Hz (caught by the opened_hz read-back, 2026-09-26). _putenv updates
 * msvcrt's copy - glidelab and our mingw Glide share msvcrt.dll - and the
 * Win32 block is set too for anything that asks the OS. */
static int glide_env(const char *name, const char *value)
{
    /* a path goes through here too (FX_GLIDE_TRACE_FILE): a truncated one
     * would send Glide's trace somewhere nobody looks - refused instead */
    char kv[MAX_PATH + 64];
    int n = _snprintf(kv, sizeof kv, "%s=%s", name, value);
    if (n < 0 || n >= (int)sizeof kv) {
        say("glide_env: %s is too long - NOT set", name);
        return 0;
    }
    SetEnvironmentVariableA(name, value);
    _putenv(kv);
    return 1;
}

/* The refresh the mode really opened at. FX_GLIDE_REFRESH and the HZ[] code
 * ask; the driver answers. */
static int current_hz(void)
{
    DEVMODEA dm;
    memset(&dm, 0, sizeof dm);
    dm.dmSize = sizeof dm;
    if (!EnumDisplaySettingsA(NULL, ENUM_CURRENT_SETTINGS, &dm))
        return 0;
    return (int)dm.dmDisplayFrequency;
}

/* 0 with g_vcr_pace_why set: the gate refused and nothing was switched;
 * 0 without it: Glide refused (after a switch that may have been made) */
static FxU32 open_board(HWND hwnd, int rescode, int hzcode)
{
    FxU32 ctx;
    say("step: grSstWinOpen %s %dHz origin %s", O.res, O.hz, O.lower ? "lower" : "upper");
    pump();
    if (!pace_before()) {
        say("  -> not opened: %s", g_vcr_pace_why);
        return 0;
    }
    ctx = p_grSstWinOpen((FxU32)(uintptr_t)hwnd, (GrScreenResolution_t)rescode,
                         (GrScreenRefresh_t)hzcode, GR_COLORFORMAT_ARGB,
                         O.lower ? GR_ORIGIN_LOWER_LEFT : GR_ORIGIN_UPPER_LEFT, 2, 1);
    /* even when refused: the refusal can come after Glide has set the mode,
     * i.e. after a re-sync, and may leave that mode set until shutdown or
     * exit. Counting it costs a failed run only the hold. */
    vcr_pace_after_switch();
    g_held = 1;
    pump();
    g_opened_hz = ctx ? current_hz() : 0;
    say("  -> context 0x%lx, %d Hz", (unsigned long)ctx, g_opened_hz);
    return ctx;
}

/* 1 = the opened mode is at the refresh asked for. Glide can quietly open at
 * another (a registry override, a rate the driver moved): the host gate
 * checked O.hz, not that one. */
static int refresh_ok(void)
{
    /* 0 / 1 is XP's "hardware default" answer, not a rate: unknown is not a
     * mismatch (unmeasured whether a Glide session on silicon reports one);
     * the host's lab_halt reads it the same way */
    if (g_opened_hz == O.hz || g_opened_hz == 0 || g_opened_hz == 1)
        return 1;
    say("  opened at %d Hz, not the %d Hz asked: closing it", g_opened_hz, O.hz);
    return 0;
}

/* 0 = the gate refused the switch out: NOT closed - grSstWinClose would give
 * the mode back unpaced - and left for the exit hold */
static int close_board(FxU32 ctx)
{
    if (!pace_before())             /* the hold: a short run still sits the floor */
        return 0;
    g_held = 0;                     /* a deactivation from here on is ours */
    p_grSstWinClose(ctx);
    vcr_pace_after_restore();
    return 1;
}

/* grGlideShutdown closes a context still open, and a refused open may have
 * left its mode set: while one may be held, the shutdown is the switch out.
 * 0 = the gate refused it, and Glide is left for the exit (see close_board) */
static int shutdown_glide(void)
{
    int held = g_held;
    if (!p_grGlideShutdown)
        return 1;                   /* the exit is the switch out: atexit holds it */
    if (held && !pace_before())
        return 0;
    g_held = 0;
    p_grGlideShutdown();
    if (held)
        vcr_pace_after_restore();
    return 1;
}

/* The switch out was refused by the gate: nothing more is switched - no
 * close, no shutdown, which would give the mode back unpaced - and a RESULT
 * of its own says so (the host reads the LAST one). Returning from main runs
 * the exit hold; glide3x's own DLL_PROCESS_DETACH gives the mode back after
 * it, inside the stamp the hold puts ahead. */
static int left_for_exit(const char *step)
{
    say("RESULT {\"mode\":\"%s\",\"error\":\"%s not made: %s - the mode is left for the exit"
        " hold\",\"opened_hz\":%d}", O.mode, step, g_vcr_pace_why, g_opened_hz);
    return 10;
}

/* ,"opened_hz":N,"focus_lost":... for a RESULT, with an "error" when the
 * focus was lost: the run was cut short, and its numbers are not a pass */
static const char *tail_json(void)
{
    static char buf[256];
    char tr[24] = "";
    /* a traced run flushes the disk at every step: its numbers are a
     * diagnosis, not a benchmark, and the RESULT says so */
    if (O.trace > 0)
        _snprintf(tr, sizeof tr, ",\"trace\":%d", O.trace);
    tr[sizeof tr - 1] = 0;
    /* ... as does a run that let multi-chip AA LFB reads through */
    _snprintf(buf, sizeof buf, ",\"opened_hz\":%d%s%s%s", g_opened_hz, tr,
              g_aa_lfb_read ? ",\"aa_lfb_read\":1" : "",
              g_focus_lost ? ",\"focus_lost\":true,\"error\":\"the window lost the foreground"
                             " while the board held the mode - run ended\""
                           : ",\"focus_lost\":false");
    buf[sizeof buf - 1] = 0;
    return buf;
}

/* --trace: did the Glide open the trace file? A Glide that is not a traced
 * build ignores FX_GLIDE_TRACE; say so in the step log (the host checks the
 * file's header as well) */
static void trace_check(const char *when)
{
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (O.trace <= 0)
        return;
    if (GetFileAttributesExA(g_tracepath, GetFileExInfoStandard, &fa))
        say("trace: %s: %s is %lu bytes", when, g_tracepath, (unsigned long)fa.nFileSizeLow);
    else
        say("trace: %s: %s MISSING - this glide3x is not a traced build, or it could not "
            "open the file", when, g_tracepath);
}

static int chips_in_use(void)
{
    FxI32 v = 1;
    if (p_grGet && p_grGet(GR_NUM_FB, sizeof v, &v))
        return (int)v;
    return 1;
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
    for (f = 0; f < 10 && !g_focus_lost; f++) {
        p_grBufferClear(0, 0, 0xffff);
        p_grConstantColorValue(0x80406080u);
        quad(0, 0, (float)O.w, (float)O.h);
        p_grBufferSwap(0);
        pump();
    }
    p_grFinish();
    say("step: %d frames x %d layers%s", O.frames, O.layers, O.blend ? " (blended)" : "");
    t0 = now_s();
    for (f = 0; f < O.frames && !g_focus_lost; f++) {
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
    pump();                         /* a deactivation still queued is seen now */
    say("RESULT {\"mode\":\"fill\",\"res\":\"%s\",\"cfg\":%d,\"chips\":%d,\"frames\":%d,"
        "\"frames_run\":%d,\"layers\":%d,\"blend\":%d,\"seconds\":%.4f,\"fps\":%.2f,"
        "\"mpix_s\":%.1f%s}",
        O.res, O.cfg, chips_in_use(), O.frames, f, O.layers, O.blend, t, t > 0 ? f / t : 0.0,
        t > 0 ? (double)O.w * O.h * O.layers * f / t / 1e6 : 0.0, tail_json());
    return g_focus_lost ? 12 : 0;
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
    pump();                         /* a deactivation still queued is seen now */
    say("RESULT {\"mode\":\"bands\",\"res\":\"%s\",\"cfg\":%d,\"chips\":%d,\"origin\":\"%s\","
        "\"band_lines\":%d,\"bad_lines\":%d,\"bad_per_chip\":[%d,%d,%d,%d],\"first_bad\":%d%s}",
        O.res, O.cfg, chips, O.lower ? "lower" : "upper", nl, bad, badc[0], badc[1], badc[2],
        badc[3], first_bad, tail_json());
    free(buf);
    return g_focus_lost ? 12 : bad ? 7 : 0;
}

int main(int argc, char **argv)
{
    int i, rescode = -1, hzcode = -1, rc = 0, c, cycles_asked;
    HWND hwnd;
    FxU32 ctx;
    DWORD pace;
    const char *bad_pace = NULL, *bad_trace = NULL, *lfb_env = NULL;
    char hz[16];

    /* A crash must die at once, not sit behind a Watson / "has encountered a
     * problem" box: that box keeps the process - and the fullscreen mode Glide
     * set - alive until someone at the box clicks it, and the fullscreen mode
     * hides it. The box is driven remotely; nobody is at .124's CRT. */
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    for (i = 1; i < argc; i++) {
        const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(a, "--res") && v) { O.res = v; i++; }
        else if (!strcmp(a, "--refresh") && v) { O.hz = atoi(v); i++; }
        else if (!strcmp(a, "--cfg") && v) { O.cfg = atoi(v); i++; }
        else if (!strcmp(a, "--frames") && v) { O.frames = atoi(v); i++; }
        else if (!strcmp(a, "--layers") && v) { O.layers = atoi(v); i++; }
        else if (!strcmp(a, "--cycles") && v) { O.cycles = atoi(v); i++; }
        else if (!strcmp(a, "--pace") && v) {
            /* atoi("-1") was a floor of 0xFFFFFFFF ms: refused, not wrapped */
            if (vcr_pace_parse_ms(v, &pace))
                vcr_pace_set_min(pace);
            else
                bad_pace = v;
            i++;
        }
        else if (!strcmp(a, "--dll") && v) { O.dll = v; i++; }
        else if (!strcmp(a, "--log") && v) { strncpy(g_logpath, v, sizeof g_logpath - 1); i++; }
        else if (!strcmp(a, "--origin") && v) { O.lower = !strcmp(v, "lower"); i++; }
        else if (!strcmp(a, "--trace") && v) {
            if (!parse_trace(v, &O.trace))
                bad_trace = v;
            i++;
        }
        else if (!strcmp(a, "--trace-cfg")) O.trace_cfg = 1;
        else if (!strcmp(a, "--aa-lfb-read")) g_aa_lfb_read = 1;
        else if (!strcmp(a, "--i-am-at-the-box")) g_at_box = 1;
        else if (!strcmp(a, "--maplog") && v) { g_maplog = v; i++; }
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
    cycles_asked = O.cycles;
    if (O.cycles > GLIDELAB_MAX_CYCLES) {
        say("cycles capped at %d (asked %d): every open and every close re-syncs the monitor",
            GLIDELAB_MAX_CYCLES, cycles_asked);
        O.cycles = GLIDELAB_MAX_CYCLES;
    }
    if (bad_pace) {
        say("RESULT {\"mode\":\"%s\",\"error\":\"--pace %s: decimal milliseconds, 0 to %u\"}",
            O.mode, bad_pace, VCR_PACE_MAX_MS);
        return 2;
    }
    if (bad_trace) {
        say("RESULT {\"mode\":\"%s\",\"error\":\"--trace %s: 0 to %d\"}", O.mode, bad_trace,
            GLIDELAB_MAX_TRACE);
        return 2;
    }
    /* the config-space dumps are an addition to a trace, never on their own */
    if (O.trace_cfg && O.trace <= 0) {
        say("RESULT {\"mode\":\"%s\",\"error\":\"--trace-cfg needs --trace 1 or 2\"}", O.mode);
        return 2;
    }
    /* a multi-chip AA LFB read froze .124 (cfg 3): opting our Glide back in
     * takes the confirmation, and nothing but this command line can opt in */
    if (aa_lfb_read_env(g_aa_lfb_read, g_at_box, getenv("RETRO_GLIDE_AA_LFB_READ"), &lfb_env) < 0) {
        say("RESULT {\"mode\":\"%s\",\"error\":\"--aa-lfb-read REFUSED without "
            "--i-am-at-the-box: a multi-chip AA LFB read froze .124 (cfg 3)\"}", O.mode);
        return 2;
    }
    if (g_maplog && !*g_maplog) {
        say("RESULT {\"mode\":\"%s\",\"error\":\"--maplog needs a file path\"}", O.mode);
        return 2;
    }
    if (rescode < 0) {
        say("RESULT {\"error\":\"no GR_RESOLUTION for %s\"}", O.res);
        return 2;
    }
    /* it fell through to 60 Hz without a word: a mode the host never gated */
    if (hzcode < 0) {
        say("RESULT {\"mode\":\"%s\",\"error\":\"no GR_REFRESH for %d Hz\"}", O.mode, O.hz);
        return 2;
    }
    /* Glide reads FX_GLIDE_REFRESH before its HKCU/HKLM overrides, so the
     * refresh asked for is the one it opens even on a box whose registry
     * pins another; set before the DLL loads, which may read it at attach */
    _snprintf(hz, sizeof hz, "%d", O.hz);
    hz[sizeof hz - 1] = 0;
    glide_env("FX_GLIDE_REFRESH", hz);
    if (O.cfg >= 0) {
        char env[16];
        _snprintf(env, sizeof env, "%d", O.cfg);
        glide_env("SSTH3_SLI_AA_CONFIGURATION", env);
    }
    /* what Glide will open: --cfg, or its own environment/registry chain */
    effective_config();
    say("sli/aa: %s", g_eff_why);
    /* the trace, before the DLL loads: Glide reads FX_GLIDE_TRACE once, at
     * its first trace point, from the process environment only */
    if (O.trace > 0) {
        char lv[8];
        int n = _snprintf(g_tracepath, sizeof g_tracepath, "%s.trace", g_logpath);
        if (n < 0 || n >= (int)sizeof g_tracepath) {
            say("RESULT {\"mode\":\"%s\",\"error\":\"--trace: the log path is too long for "
                "<log>.trace\"}", O.mode);
            return 2;
        }
        _snprintf(lv, sizeof lv, "%d", O.trace);
        lv[sizeof lv - 1] = 0;
        DeleteFileA(g_tracepath);               /* Glide appends: one run, one trace */
        if (!glide_env("FX_GLIDE_TRACE_FILE", g_tracepath) ||
            !glide_env("FX_GLIDE_TRACE", lv)) {
            say("RESULT {\"mode\":\"%s\",\"error\":\"--trace: the Glide settings could not "
                "be made\"}", O.mode);
            return 2;
        }
        /* an AA open - by --cfg or by what Glide reads without it - is traced
         * with no third-party splash plugin as a variable. FX_GLIDE_NO_SPLASH
         * gates grSplash() only; the plugin's init (3dfxspl3.dll, drawing
         * through Glide) still ran inside grSstWinOpen: FX_GLIDE_NO_PLUGIN=1
         * (fork gsst.c doSplash) is what keeps it from loading. The trace's
         * "splash: plugin present reg=0" line confirms it. */
        if (g_eff_aa) {
            glide_env("FX_GLIDE_NO_SPLASH", "1");
            glide_env("FX_GLIDE_NO_PLUGIN", "1");
        }
        /* the PCI config dumps (36 PCI_OP escapes a dump on the V5 6000),
         * only when asked: without them a level-1 trace adds file writes only */
        if (O.trace_cfg)
            glide_env("FX_GLIDE_TRACE_CFG", "1");
        say("trace: level %d -> %s%s%s", O.trace, g_tracepath,
            g_eff_aa ? " (AA open: FX_GLIDE_NO_SPLASH=1 FX_GLIDE_NO_PLUGIN=1)" : "",
            O.trace_cfg ? " (FX_GLIDE_TRACE_CFG=1: config-space dumps)" : "");
    }
    /* our Glide's opt-ins, where it reads them - the process environment,
     * never the registry - before the DLL loads */
    if (lfb_env) {
        if (!glide_env("RETRO_GLIDE_AA_LFB_READ", lfb_env)) {
            say("RESULT {\"mode\":\"%s\",\"error\":\"RETRO_GLIDE_AA_LFB_READ could not be "
                "set\"}", O.mode);
            return 2;
        }
        say("aa lfb read: RETRO_GLIDE_AA_LFB_READ=%s%s", lfb_env, g_aa_lfb_read
            ? " (--aa-lfb-read --i-am-at-the-box: a READ lock in multi-chip AA is let through)"
            : " (an inherited value overridden: only --aa-lfb-read --i-am-at-the-box opts in)");
    }
    if (g_maplog) {
        DeleteFileA(g_maplog);                  /* Glide appends: one run, one log */
        if (!glide_env("RETRO_GLIDE_MAPLOG", g_maplog)) {
            say("RESULT {\"mode\":\"%s\",\"error\":\"--maplog: the path is too long for "
                "the environment\"}", O.mode);
            return 2;
        }
        say("maplog: RETRO_GLIDE_MAPLOG=%s", g_maplog);
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
    {
        /* an AA open only on a Glide that says it carries the SLI/AA guards -
         * checked here, after grGlideInit and before anything opens: the
         * DLL on the box is whatever was staged, and nothing else checks it */
        const char *ext = p_grGetString ? p_grGetString(GR_EXTENSION) : NULL;
        const int guarded = glide_guarded(ext);
        say("GR_EXTENSION:%s", ext ? ext : " (none)");
        say("guard: RETRO3DFX_SLIAA_GUARD %s; RETRO3DFX_AA_TRACE %s", guarded ? "present" : "ABSENT",
            ext && strstr(ext, " RETRO3DFX_AA_TRACE ") ? "present" : "ABSENT");
        if (g_eff_aa && !guarded) {
            char jd[2 * MAX_PATH], jw[2 * sizeof g_eff_why];
            say("RESULT {\"mode\":\"%s\",\"error\":\"AA open refused: %s has no "
                "RETRO3DFX_SLIAA_GUARD in GR_EXTENSION - a Glide without the SLI/AA guards "
                "froze .124 on cfg 1 (%s)\",\"unguarded_glide\":true}", O.mode,
                json_esc(jd, sizeof jd, O.dll), json_esc(jw, sizeof jw, g_eff_why));
            if (!shutdown_glide())
                return left_for_exit("grGlideShutdown");
            return 13;
        }
    }
    hwnd = make_window(O.w, O.h);

    if (!strcmp(O.mode, "cycle")) {
        int ok = 0, wrong_hz = 0, glide_failed = 0;
        const char *refused = NULL;
        double t0 = now_s();
        for (c = 0; c < O.cycles && !g_focus_lost; c++) {
            int f;
            ctx = open_board(hwnd, rescode, hzcode);
            if (!ctx) {
                refused = g_vcr_pace_why;
                glide_failed = !refused;
                say("  cycle %d: open REFUSED%s%s", c, refused ? " by the pace gate: " : "",
                    refused ? refused : "");
                break;
            }
            if (!refresh_ok()) {
                wrong_hz = 1;
                if (!close_board(ctx))
                    return left_for_exit("grSstWinClose");
                break;
            }
            flat_state();
            for (f = 0; f < 20 && !g_focus_lost; f++) {
                p_grBufferClear(0x00102030u + (unsigned)f, 0, 0xffff);
                p_grConstantColorValue(0xff000000u | (unsigned)(c * 997 + f * 31));
                quad(0, 0, (float)O.w, (float)O.h);
                p_grBufferSwap(0);
                pump();
            }
            p_grFinish();
            say("  cycle %d: chips %d, closing", c, chips_in_use());
            if (!close_board(ctx))
                return left_for_exit("grSstWinClose");
            pump();
            if (!g_focus_lost)
                ok++;
        }
        trace_check("after the cycles");
        /* seconds is the wall clock, pace included; paced_s is the part spent
         * waiting out vcr_pace.h's floor, so seconds - paced_s is the board's */
        if (refused)
            say("RESULT {\"mode\":\"cycle\",\"res\":\"%s\",\"cfg\":%d,\"cycles\":%d,"
                "\"cycles_asked\":%d,\"ok\":%d,\"error\":\"grSstWinOpen not made: %s\"%s}",
                O.res, O.cfg, O.cycles, cycles_asked, ok, refused, tail_json());
        else if (glide_failed)
            /* Glide itself refused: a RESULT without "error" read as a pass
             * to every runner that counts errors (pre-existing, found 09-26) */
            say("RESULT {\"mode\":\"cycle\",\"res\":\"%s\",\"cfg\":%d,\"cycles\":%d,"
                "\"cycles_asked\":%d,\"ok\":%d,\"error\":\"grSstWinOpen failed at cycle %d\"%s}",
                O.res, O.cfg, O.cycles, cycles_asked, ok, ok, tail_json());
        else if (wrong_hz)
            say("RESULT {\"mode\":\"cycle\",\"res\":\"%s\",\"cfg\":%d,\"cycles\":%d,"
                "\"cycles_asked\":%d,\"ok\":%d,\"error\":\"opened at %d Hz, not the %d Hz"
                " asked\",\"opened_hz\":%d}", O.res, O.cfg, O.cycles, cycles_asked, ok,
                g_opened_hz, O.hz, g_opened_hz);
        else
            say("RESULT {\"mode\":\"cycle\",\"res\":\"%s\",\"cfg\":%d,\"cycles\":%d,"
                "\"cycles_asked\":%d,\"ok\":%d,\"seconds\":%.2f,\"paced_s\":%.2f%s}", O.res,
                O.cfg, O.cycles, cycles_asked, ok, now_s() - t0, g_paced_s, tail_json());
        rc = refused ? 10 : wrong_hz ? 11 : g_focus_lost ? 12 : ok == O.cycles ? 0 : 8;
    } else {
        ctx = open_board(hwnd, rescode, hzcode);
        if (!ctx) {
            if (g_vcr_pace_why) {
                say("RESULT {\"mode\":\"%s\",\"error\":\"grSstWinOpen not made: %s\"}",
                    O.mode, g_vcr_pace_why);
                return 10;
            }
            say("RESULT {\"mode\":\"%s\",\"error\":\"grSstWinOpen refused\"}", O.mode);
            if (!shutdown_glide())
                return left_for_exit("grGlideShutdown");
            return 5;
        }
        if (!refresh_ok()) {
            say("RESULT {\"mode\":\"%s\",\"error\":\"opened at %d Hz, not the %d Hz asked\","
                "\"opened_hz\":%d}", O.mode, g_opened_hz, O.hz, g_opened_hz);
            if (!close_board(ctx))
                return left_for_exit("grSstWinClose");
            if (!shutdown_glide())
                return left_for_exit("grGlideShutdown");
            return 11;
        }
        say("chips in use: %d", chips_in_use());
        trace_check("after grSstWinOpen");
        if (!strcmp(O.mode, "fill")) {
            rc = do_fill();
        } else if (!strcmp(O.mode, "bands")) {
            rc = do_bands();
        } else if (!strcmp(O.mode, "abandon")) {
            int f;
            flat_state();
            for (f = 0; f < 30 && !g_focus_lost; f++) {
                p_grBufferClear(0x00400000u, 0, 0xffff);
                p_grConstantColorValue(0xff00ff00u);
                quad(0, 0, (float)O.w / 2, (float)O.h / 2);
                p_grBufferSwap(1);
                pump();
            }
            say("RESULT {\"mode\":\"abandon\",\"res\":\"%s\",\"cfg\":%d,\"chips\":%d,"
                "\"note\":\"exiting WITHOUT grSstWinClose/grGlideShutdown\"%s}",
                O.res, O.cfg, chips_in_use(), tail_json());
            /* ExitProcess skips atexit, so vcr_pace.h's exit hold would not
             * run - and this exit IS the switch out: run that hold here. It
             * holds the mode for the floor and stamps the revert AHEAD, which
             * is what this exit needs: the dying glide3x's DLL_PROCESS_DETACH
             * shuts Glide down - seconds of idle waits - before its
             * RestoreDisplayMode, long after a stamp made now */
            vcr_pace_at_exit();
            ExitProcess(0);
        } else {
            say("RESULT {\"error\":\"unknown mode %s\"}", O.mode);
            rc = 2;
        }
        say("step: grSstWinClose");
        if (!close_board(ctx))
            return left_for_exit("grSstWinClose");
    }
    say("step: grGlideShutdown");
    if (!shutdown_glide())
        return left_for_exit("grGlideShutdown");
    if (hwnd)
        DestroyWindow(hwnd);
    say("done rc=%d", rc);
    return rc;
}
