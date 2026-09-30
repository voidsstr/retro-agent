/* glreset9x - hand the screen back from a Voodoo 1/2 that a killed Glide program left switched in.
 *
 *   glreset9x            -> C:\RETRO_AGENT\GLRESET.TXT
 *
 * A Voodoo 2 is a PASSTHROUGH card: while a Glide program has it open, its
 * relay feeds the monitor the 3D framebuffer instead of the 2D card. The
 * program's grSstWinClose()/grGlideShutdown() switch the relay back. A program
 * that is KILLED never makes those calls, and the monitor keeps showing its
 * last 3D frame over a perfectly healthy Windows desktop - measured
 * 2026-09-30 in the Win98 build VM: Quake II (3dfx) ended by PROCKILL left its
 * "LOADING" frame on screen, and every later screenshot showed it.
 *
 * This opens Glide 2 (glide2x.dll, found the normal way: beside us, then
 * SYSTEM), opens one 640x480 window on board 0, closes it and shuts Glide
 * down - the same calls a well-behaved game makes on exit. Nothing is
 * installed or configured; no driver or registry is touched.
 *
 * NOT for a Voodoo whose BAR0 is 0 (.243's BIOS leaves its card unconfigured
 * until the agent's PCIRESCAN has run): Glide on an unmapped board maps it
 * over RAM and kills the box (CLAUDE.md, PCIRESCAN). Run it only where Glide
 * games already run.
 *
 * No C runtime:
 *   i686-w64-mingw32-gcc -O1 -march=i586 -mwindows -nostdlib -fno-builtin -e _start@0 \
 *       -o glreset9x.exe glreset9x.c -lkernel32 -s
 */
#include <windows.h>

typedef void (__stdcall *v_fn)(void);
typedef int  (__stdcall *i_ptr_fn)(void *);
typedef int  (__stdcall *i_int_fn)(int);
typedef int  (__stdcall *open_fn)(DWORD, int, int, int, int, int, int);

#define GR_RESOLUTION_640x480 0x7
#define GR_REFRESH_60Hz       0x0
#define GR_COLORFORMAT_ARGB   0x0
#define GR_ORIGIN_UPPER_LEFT  0x0

static HANDLE out;
static char hw[8192];          /* GrHwConfiguration - generously sized */

static void w(const char *s)
{
    DWORD n;
    WriteFile(out, s, lstrlenA(s), &n, NULL);
    WriteFile(out, "\r\n", 2, &n, NULL);
}

void WINAPI _start(void)
{
    HMODULE g;
    v_fn init, shut, wclose;
    i_ptr_fn query;
    i_int_fn sel;
    open_fn wopen;
    int ok;

    out = CreateFileA("C:\\RETRO_AGENT\\GLRESET.TXT", GENERIC_WRITE, FILE_SHARE_READ, NULL,
                      CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    /* no 3dfx splash: Glide plays it on every grSstWinOpen, several seconds of
     * logo that a screenshot taken meanwhile would record as "the desktop" */
    SetEnvironmentVariableA("FX_GLIDE_NO_SPLASH", "1");
    g = LoadLibraryA("glide2x.dll");
    if (!g) { w("FAIL: glide2x.dll not found - no Glide on this box"); ExitProcess(2); }
    init   = (v_fn)GetProcAddress(g, "_grGlideInit@0");
    shut   = (v_fn)GetProcAddress(g, "_grGlideShutdown@0");
    wclose = (v_fn)GetProcAddress(g, "_grSstWinClose@0");
    query  = (i_ptr_fn)GetProcAddress(g, "_grSstQueryHardware@4");
    sel    = (i_int_fn)GetProcAddress(g, "_grSstSelect@4");
    wopen  = (open_fn)GetProcAddress(g, "_grSstWinOpen@28");
    if (!init || !shut || !wclose || !query || !sel || !wopen) {
        w("FAIL: glide2x.dll lacks a Glide 2 entry point"); ExitProcess(3);
    }
    init();
    if (!query(hw)) { w("FAIL: grSstQueryHardware found no board"); shut(); ExitProcess(4); }
    sel(0);
    ok = wopen(0, GR_RESOLUTION_640x480, GR_REFRESH_60Hz, GR_COLORFORMAT_ARGB, GR_ORIGIN_UPPER_LEFT, 2, 1);
    if (!ok) {
        /* a Glide program still holds the board - nothing to hand back yet */
        w("FAIL: grSstWinOpen refused - is a Glide program still running?");
        shut();
        ExitProcess(5);
    }
    wclose();
    shut();
    w("OK: Glide opened and closed on board 0 - the passthrough is back on the 2D card");
    ExitProcess(0);
}
