/* glide2probe.c - the Glide 2 API, step by step, through whatever glide2x.dll
 * the loader finds (on a V5 box: AmigaMerlin's "Glide2 to Glide3 Translator",
 * which forwards to glide3x.dll). Each step's result is logged, flushed, so
 * the step that fails - or the last one reached before a fault - is on disk.
 *
 *   glide2probe.exe [--dll glide2x.dll] [--res 7] [--ref 0] [--log path]
 *
 * res/ref are Glide 2 enums (GR_RESOLUTION_640x480 = 7, GR_REFRESH_60Hz = 0).
 * Opens fullscreen, clears and swaps 60 frames, closes. Clean-room: only the
 * public Glide 2 API names and signatures (sst1/cvg/h3 glide.h in the GPL
 * release). */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef int FxBool;
typedef unsigned long FxU32;
typedef void (__stdcall *pv0)(void);
typedef FxBool (__stdcall *pqh)(void *);
typedef void (__stdcall *psel)(int);
typedef FxBool (__stdcall *pwo)(FxU32, int, int, int, int, int, int);
typedef void (__stdcall *pclr)(FxU32, unsigned char, unsigned short);
typedef void (__stdcall *pswp)(int);

static FILE *L;
static void say(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(L ? L : stdout, fmt, ap);
    va_end(ap);
    fputc('\n', L ? L : stdout);
    fflush(L ? L : stdout);
}

static FARPROC sym(HMODULE h, const char *dec, const char *plain)
{
    FARPROC p = GetProcAddress(h, dec);
    if (!p)
        p = GetProcAddress(h, plain);
    say("  %s -> %p", dec, (void *)p);
    return p;
}

static LRESULT CALLBACK wp(HWND w, UINT m, WPARAM a, LPARAM b)
{
    return DefWindowProcA(w, m, a, b);
}

int main(int argc, char **argv)
{
    const char *dll = "glide2x.dll", *logp = NULL;
    int res = 7, ref = 0, i;
    HMODULE h;
    WNDCLASSA wc;
    HWND w;
    unsigned char hw[4096];
    pv0 init, shut;
    pqh query;
    psel sel;
    pwo open;
    pv0 close_;
    pclr clr;
    pswp swp;
    FxBool ok;
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--dll") && i + 1 < argc) dll = argv[++i];
        else if (!strcmp(argv[i], "--res") && i + 1 < argc) res = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--ref") && i + 1 < argc) ref = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--log") && i + 1 < argc) logp = argv[++i];
    }
    if (logp)
        L = fopen(logp, "w");
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    say("glide2probe: dll %s res %d ref %d", dll, res, ref);
    h = LoadLibraryA(dll);
    say("LoadLibrary -> %p (err %lu)", (void *)h, h ? 0 : GetLastError());
    if (!h)
        return 1;
    {
        char path[MAX_PATH];
        HMODULE g3 = GetModuleHandleA("glide3x.dll");
        GetModuleFileNameA(h, path, sizeof path);
        say("  loaded %s", path);
        if (g3) {
            GetModuleFileNameA(g3, path, sizeof path);
            say("  glide3x already loaded by it: %s", path);
        }
    }
    init = (pv0)sym(h, "_grGlideInit@0", "grGlideInit");
    query = (pqh)sym(h, "_grSstQueryHardware@4", "grSstQueryHardware");
    sel = (psel)sym(h, "_grSstSelect@4", "grSstSelect");
    open = (pwo)sym(h, "_grSstWinOpen@28", "grSstWinOpen");
    close_ = (pv0)sym(h, "_grSstWinClose@0", "grSstWinClose");
    clr = (pclr)sym(h, "_grBufferClear@12", "grBufferClear");
    swp = (pswp)sym(h, "_grBufferSwap@4", "grBufferSwap");
    shut = (pv0)sym(h, "_grGlideShutdown@0", "grGlideShutdown");
    if (!init || !query || !sel || !open || !shut)
        return 2;
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = wp;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "glide2probe";
    RegisterClassA(&wc);
    w = CreateWindowExA(WS_EX_TOPMOST, "glide2probe", "glide2probe", WS_POPUP | WS_VISIBLE,
                        0, 0, 640, 480, NULL, NULL, wc.hInstance, NULL);
    say("window %p", (void *)w);
    say("grGlideInit ...");
    init();
    say("grGlideInit returned");
    memset(hw, 0, sizeof hw);
    ok = query(hw);
    say("grSstQueryHardware -> %d, num_sst %lu, type %lu", ok, *(FxU32 *)hw, *(FxU32 *)(hw + 4));
    sel(0);
    say("grSstSelect(0) returned");
    ok = open((FxU32)(ULONG_PTR)w, res, ref, 0 /* ARGB */, 0 /* upper left */, 2, 1);
    say("grSstWinOpen -> %d", ok);
    if (ok) {
        for (i = 0; i < 60; i++) {
            if (clr)
                clr(0x00400000u | (i * 4), 0, 0xffff);
            if (swp)
                swp(1);
        }
        say("60 clears/swaps done");
        if (close_)
            close_();
        say("grSstWinClose returned");
    }
    shut();
    say("grGlideShutdown returned");
    DestroyWindow(w);
    say("RESULT %s", ok ? "open-ok" : "open-FAILED");
    return ok ? 0 : 3;
}
