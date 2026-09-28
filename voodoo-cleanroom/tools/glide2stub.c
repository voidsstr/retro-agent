/* glide2stub.c - a FAKE glide2x.dll: the Glide 2 entry points glide2probe
 * calls, with no hardware behind them. It exists to check the HARNESS off the
 * box (under Wine in the retro-wine container, tools/glide2probe_wine.sh):
 * UE1's call sequence, the re-open inside WM_KILLFOCUS, the pace gate, the
 * watchdog's stack dump and hard timeout - never to say anything about a
 * driver. Every call is logged (GLIDE2STUB_LOG).
 *
 *   GLIDE2STUB_HANG_OPEN=N   the Nth grSstWinOpen never returns (Sleep, a block)
 *   GLIDE2STUB_SPIN_OPEN=N   the Nth grSstWinOpen spins forever (a busy hang)
 *
 * Build: i686-w64-mingw32-gcc -shared -O1 -o out/glide2stub/glide2x.dll
 *        tools/glide2stub.c -Wl,--add-stdcall-alias
 * Clean-room: the public Glide 2 names and signatures only. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

typedef int FxBool;
typedef unsigned long FxU32;
typedef void (*G2ErrCb)(const char *, FxBool);

static int g_opens;

static void slog(const char *fmt, ...)
{
    const char *p = getenv("GLIDE2STUB_LOG");
    FILE *f;
    va_list ap;
    if (!p || !(f = fopen(p, "a")))
        return;
    fprintf(f, "%lu stub ", (unsigned long)GetTickCount());
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

static int env_n(const char *name)
{
    const char *v = getenv(name);
    return v ? atoi(v) : 0;
}

__declspec(dllexport) void __stdcall grGlideGetVersion(char *v)
{
    strcpy(v, "2.70 glide2stub");
    slog("grGlideGetVersion");
}

__declspec(dllexport) FxBool __stdcall grSstQueryBoards(void *hw)
{
    *(FxU32 *)hw = 1;
    slog("grSstQueryBoards");
    return 1;
}

__declspec(dllexport) void __stdcall grGlideInit(void)
{
    slog("grGlideInit");
}

__declspec(dllexport) void __stdcall grErrorSetCallback(G2ErrCb cb)
{
    slog("grErrorSetCallback %p", (void *)cb);
}

__declspec(dllexport) FxBool __stdcall grSstQueryHardware(void *hw)
{
    FxU32 *w = (FxU32 *)hw;
    w[0] = 1;           /* num_sst */
    w[1] = 0;           /* GR_SSTTYPE_VOODOO */
    w[2] = 60817408;    /* fbRam, as the translator reports the V5 6000 */
    slog("grSstQueryHardware");
    return 1;
}

__declspec(dllexport) void __stdcall grSstSelect(int which)
{
    slog("grSstSelect %d", which);
}

__declspec(dllexport) FxBool __stdcall grSstWinOpen(FxU32 hwnd, int res, int ref, int cformat,
                                                   int origin, int nbufs, int naux)
{
    g_opens++;
    slog("grSstWinOpen #%d hwnd %08lx res %d ref %d cformat %d origin %d bufs %d aux %d "
         "(foreground %s)", g_opens, hwnd, res, ref, cformat, origin, nbufs, naux,
         GetForegroundWindow() == (HWND)hwnd ? "YES" : "no");
    if (env_n("GLIDE2STUB_HANG_OPEN") == g_opens) {
        slog("grSstWinOpen #%d: HANG (Sleep INFINITE) as asked", g_opens);
        Sleep(INFINITE);
    }
    if (env_n("GLIDE2STUB_SPIN_OPEN") == g_opens) {
        volatile unsigned long n = 0;
        slog("grSstWinOpen #%d: SPIN as asked", g_opens);
        for (;;)
            n++;
    }
    return 1;
}

__declspec(dllexport) void __stdcall grSstWinClose(void)
{
    slog("grSstWinClose");
}

__declspec(dllexport) void __stdcall grBufferClear(FxU32 color, unsigned char alpha,
                                                   unsigned short depth)
{
    (void)color; (void)alpha; (void)depth;
}

__declspec(dllexport) void __stdcall grBufferSwap(int interval)
{
    (void)interval;
    Sleep(1);
}

__declspec(dllexport) void __stdcall grGlideShutdown(void)
{
    slog("grGlideShutdown");
}
