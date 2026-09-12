/*
 * glideprobe.c - ask Glide directly what a VSA-100 board will and will not do.
 *
 * WHY THIS EXISTS
 * ---------------
 * On the Voodoo 5 6000 at .191, asking for 4-chip 8-sample AA
 * (SSTH3_SLI_AA_CONFIGURATION=8) takes the display driver down hard enough to
 * kill the retro agent with it - twice, through two different code paths, so it
 * is not an artifact of how the game was launched. A game tells you almost
 * nothing about that: Quake III's console goes quiet during a timedemo, so
 * "hung in Glide init" and "rendering slowly" look identical from the log, and
 * the box has to be attended before anything else can be measured.
 *
 * This probe removes the engine, the ICD and the resolution logic from the
 * question and calls Glide itself, ONE STEP AT A TIME, writing a line for each
 * step and FLUSHING IT TO DISK BEFORE TAKING THE NEXT ONE. That is the whole
 * design: when the board wedges the machine, the last line in the log names the
 * exact call that did it. A buffered log would lose precisely the line worth
 * having.
 *
 * Glide is resolved with LoadLibrary/GetProcAddress rather than linked, for
 * three reasons that all matter here:
 *   - it can be pointed at ANY glide3x.dll (--dll), which is how the retail
 *     AmigaMerlin Glide and our clean-room build get compared on one box;
 *   - the two builds disagree on symbol decoration (grFoo@N vs _grFoo@N), and
 *     a probe that failed to link would report a driver problem that is really
 *     a build problem;
 *   - a MISSING export is then a finding the probe can report, instead of a
 *     load-time failure with no log at all.
 *
 * Usage:
 *   glideprobe.exe [--res 1024x768] [--refresh 60|75|85|100|120] [--aa N]
 *                  [--dll <path to glide3x.dll>] [--log <path>] [--noopen]
 *
 * --aa only RECORDS what the caller intends and sets the environment variable
 * in-process; the chip/AA topology is chosen by the driver from
 * SSTH3_SLI_AA_CONFIGURATION, so the caller should also have written the
 * registry value. --noopen stops before grSstWinOpen, which is the call that
 * touches the board, so it is the safe way to confirm the probe itself works.
 *
 * Build (from the repo root):
 *   i686-w64-mingw32-gcc -O1 -o voodoo-cleanroom/out/glideprobe.exe \
 *       voodoo-cleanroom/tools/glideprobe.c \
 *       -Ivoodoo-cleanroom/out/sdk/include -lgdi32 -luser32
 */

#include <windows.h>
#include <io.h>      /* _get_osfhandle - the log must reach DISK, not just stdio */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Only the constants are wanted from the SDK; every entry point is looked up
 * at run time, so nothing here creates a link dependency on a Glide build. */
#include "glide.h"
#include "sst1vid.h"

static FILE *g_log;
static char  g_logpath[MAX_PATH] = "C:\\glideprobe.log";

/* Write a line and make sure it is ON DISK before returning. If the next call
 * hangs the box, this line is the evidence - so fflush is not enough on its
 * own, the OS buffers too. */
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

/* ------------------------------------------------------------------ */
/* Glide entry points, all late-bound                                  */
/* ------------------------------------------------------------------ */
typedef void  (__stdcall *pfn_void)(void);
typedef void  (__stdcall *pfn_sel)(int);
typedef FxU32 (__stdcall *pfn_winopen)(FxU32, GrScreenResolution_t,
                                       GrScreenRefresh_t, GrColorFormat_t,
                                       GrOriginLocation_t, int, int);
typedef void  (__stdcall *pfn_winclose)(FxU32);
typedef FxBool(__stdcall *pfn_get)(FxU32, FxU32, FxI32 *);
typedef const char *(__stdcall *pfn_getstring)(FxU32);
typedef void  (__stdcall *pfn_bufclear)(GrColor_t, GrAlpha_t, FxU32);
typedef void  (__stdcall *pfn_bufswap)(FxU32);
typedef char *(__stdcall *pfn_version)(char *);

static HMODULE g_dll;

/* Try every decoration the two Glide builds are known to use. A symbol that is
 * genuinely absent is reported rather than silently treated as a driver fault. */
static FARPROC sym(const char *name)
{
    char buf[128];
    FARPROC p = GetProcAddress(g_dll, name);
    if (p) return p;
    _snprintf(buf, sizeof buf, "_%s", name);
    if ((p = GetProcAddress(g_dll, buf))) return p;
    /* @N stdcall decorations, byte counts unknown to us - probe the plausible
     * ones rather than hardcoding a table per symbol. */
    for (int n = 0; n <= 32; n += 4) {
        _snprintf(buf, sizeof buf, "%s@%d", name, n);
        if ((p = GetProcAddress(g_dll, buf))) return p;
        _snprintf(buf, sizeof buf, "_%s@%d", name, n);
        if ((p = GetProcAddress(g_dll, buf))) return p;
    }
    return NULL;
}

struct res_ent { const char *name; int code; };
static const struct res_ent RESOLUTIONS[] = {
    { "640x480",   GR_RESOLUTION_640x480   },
    { "800x600",   GR_RESOLUTION_800x600   },
    { "1024x768",  GR_RESOLUTION_1024x768  },
    { "1280x960",  GR_RESOLUTION_1280x960  },
    { "1280x1024", GR_RESOLUTION_1280x1024 },
    { "1600x1200", GR_RESOLUTION_1600x1200 },
    { "2048x1536", GR_RESOLUTION_2048x1536 },
    { NULL, 0 }
};

struct hz_ent { int hz; int code; };
static const struct hz_ent REFRESH[] = {
    { 60, GR_REFRESH_60Hz }, { 75, GR_REFRESH_75Hz }, { 85, GR_REFRESH_85Hz },
    { 100, GR_REFRESH_100Hz }, { 120, GR_REFRESH_120Hz }, { 0, 0 }
};

int main(int argc, char **argv)
{
    const char *dllpath = "glide3x.dll";
    const char *resname = "640x480";
    int hz = 60, aa = -1, noopen = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--res") && i + 1 < argc)          resname = argv[++i];
        else if (!strcmp(argv[i], "--refresh") && i + 1 < argc) hz = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--aa") && i + 1 < argc)      aa = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--dll") && i + 1 < argc)     dllpath = argv[++i];
        else if (!strcmp(argv[i], "--log") && i + 1 < argc)
            strncpy(g_logpath, argv[++i], sizeof g_logpath - 1);
        else if (!strcmp(argv[i], "--noopen"))                  noopen = 1;
    }

    g_log = fopen(g_logpath, "w");
    say("glideprobe: res=%s refresh=%dHz aa=%d dll=%s noopen=%d",
        resname, hz, aa, dllpath, noopen);

    /* The driver reads the topology from this; set it in-process too so the
     * value the Glide in THIS process sees is not left to chance. */
    if (aa >= 0) {
        char env[64];
        _snprintf(env, sizeof env, "%d", aa);
        SetEnvironmentVariableA("SSTH3_SLI_AA_CONFIGURATION", env);
        say("set SSTH3_SLI_AA_CONFIGURATION=%s in-process", env);
    }
    {
        char cur[64] = "(unset)";
        GetEnvironmentVariableA("SSTH3_SLI_AA_CONFIGURATION", cur, sizeof cur);
        say("env SSTH3_SLI_AA_CONFIGURATION=%s", cur);
    }

    int rescode = -1;
    for (const struct res_ent *r = RESOLUTIONS; r->name; r++)
        if (!strcmp(r->name, resname)) { rescode = r->code; break; }
    if (rescode < 0) { say("FAIL: no GR_RESOLUTION constant for %s", resname); return 2; }
    int hzcode = GR_REFRESH_60Hz;
    for (const struct hz_ent *h = REFRESH; h->hz; h++)
        if (h->hz == hz) { hzcode = h->code; break; }
    say("resolution code 0x%x, refresh code 0x%x", rescode, hzcode);

    say("step: LoadLibrary(%s)", dllpath);
    g_dll = LoadLibraryA(dllpath);
    if (!g_dll) { say("FAIL: LoadLibrary failed, GetLastError=%lu", GetLastError()); return 3; }
    {
        char got[MAX_PATH] = "";
        GetModuleFileNameA(g_dll, got, sizeof got);
        say("loaded: %s", got);
    }

    pfn_version  p_ver    = (pfn_version) sym("grGlideGetVersion");
    pfn_void     p_init   = (pfn_void)    sym("grGlideInit");
    pfn_void     p_shut   = (pfn_void)    sym("grGlideShutdown");
    pfn_sel      p_select = (pfn_sel)     sym("grSstSelect");
    pfn_get      p_get    = (pfn_get)     sym("grGet");
    pfn_getstring p_gstr  = (pfn_getstring) sym("grGetString");
    pfn_winopen  p_open   = (pfn_winopen) sym("grSstWinOpen");
    pfn_winclose p_close  = (pfn_winclose)sym("grSstWinClose");
    pfn_bufclear p_clear  = (pfn_bufclear)sym("grBufferClear");
    pfn_bufswap  p_swap   = (pfn_bufswap) sym("grBufferSwap");

    say("exports: version=%p init=%p shutdown=%p select=%p get=%p getstring=%p",
        p_ver, p_init, p_shut, p_select, p_get, p_gstr);
    say("exports: winopen=%p winclose=%p bufclear=%p bufswap=%p",
        p_open, p_close, p_clear, p_swap);
    if (!p_init || !p_open) { say("FAIL: this DLL exports no usable Glide entry points"); return 4; }

    if (p_ver) { char v[128] = ""; p_ver(v); say("grGlideGetVersion: %s", v); }

    say("step: grGlideInit()");
    p_init();
    say("  grGlideInit returned");

    if (p_get) {
        FxI32 v = -1;
        if (p_get(GR_NUM_BOARDS, sizeof v, &v)) say("GR_NUM_BOARDS = %ld", (long)v);
        else say("GR_NUM_BOARDS query failed");
    }

    say("step: grSstSelect(0)");
    if (p_select) p_select(0);
    say("  grSstSelect returned");

    if (p_get) {
        FxI32 v = -1;
        if (p_get(GR_NUM_FB, sizeof v, &v)) say("GR_NUM_FB (chips in use) = %ld", (long)v);
        else say("GR_NUM_FB query failed");
    }
    if (p_gstr) {
        const char *s;
        if ((s = p_gstr(GR_HARDWARE)) != NULL) say("GR_HARDWARE: %s", s);
        if ((s = p_gstr(GR_RENDERER)) != NULL) say("GR_RENDERER: %s", s);
        if ((s = p_gstr(GR_VERSION))  != NULL) say("GR_VERSION: %s", s);
    }

    if (noopen) {
        say("--noopen: stopping before grSstWinOpen (the call that touches the board)");
        if (p_shut) { p_shut(); say("grGlideShutdown returned"); }
        say("RESULT: probe-ok-noopen");
        return 0;
    }

    /* THE DANGEROUS CALL. Everything above is bookkeeping; this is where the
     * board is actually programmed, and where an unsupported AA topology has
     * taken the machine down. The log is already on disk. */
    say("step: grSstWinOpen(res=%s, %dHz, ABGR, UPPER_LEFT, 2 colour, 1 aux)",
        resname, hz);
    FxU32 ctx = p_open(0, (GrScreenResolution_t)rescode, (GrScreenRefresh_t)hzcode,
                       GR_COLORFORMAT_ABGR, GR_ORIGIN_UPPER_LEFT, 2, 1);
    say("  grSstWinOpen returned 0x%lx (%s)", (unsigned long)ctx,
        ctx ? "context created" : "REFUSED - this mode/AA combination is not available");

    if (!ctx) {
        say("RESULT: winopen-refused");
        if (p_shut) p_shut();
        return 5;
    }

    if (p_get) {
        FxI32 v = -1;
        if (p_get(GR_NUM_FB, sizeof v, &v))
            say("GR_NUM_FB after open (chips ganged) = %ld", (long)v);
    }

    say("step: grBufferClear + grBufferSwap x3");
    for (int i = 0; i < 3; i++) {
        if (p_clear) p_clear(0x00204060, 0xff, 0xffff);
        if (p_swap)  p_swap(1);
        say("  frame %d done", i + 1);
    }

    say("step: grSstWinClose");
    if (p_close) p_close(ctx);
    say("  grSstWinClose returned");

    say("step: grGlideShutdown");
    if (p_shut) p_shut();
    say("  grGlideShutdown returned");

    say("RESULT: ok");
    return 0;
}
