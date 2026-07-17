/*
 * setmode - restore/change the Windows desktop display mode via
 *           ChangeDisplaySettings. Used to recover the desktop resolution
 *           after a 3dfx Glide fullscreen app (Quake III) exits abnormally
 *           and leaves the Voodoo stuck in its low-res Glide mode.
 *
 *   setmode <width> <height> <bpp> [freq]
 *   e.g. setmode 1024 768 32 85
 *
 * Prints: SETMODE: OK  |  SETMODE: FAIL code=<n>  (ChangeDisplaySettings ret)
 *
 * Build (mingw, no CRT - matches updrv.c/pendmv.c):
 *   i686-w64-mingw32-gcc -Wall -Wextra -Os -s -nostdlib -DWIN32_LEAN_AND_MEAN \
 *     -DWINVER=0x0500 -D_WIN32_WINNT=0x0500 -march=i586 -mtune=pentium3 \
 *     -fno-stack-protector -o setmode.exe setmode.c -Wl,-e,_mainCRTStartup \
 *     -lkernel32 -luser32
 */
#include <windows.h>

static void out(const char *s)
{
    DWORD w;
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    if (h != INVALID_HANDLE_VALUE && h != NULL)
        WriteFile(h, s, (DWORD)lstrlenA(s), &w, NULL);
}

/* tiny non-negative atoi */
static int a2i(const char *p)
{
    int n = 0;
    while (*p >= '0' && *p <= '9') { n = n * 10 + (*p - '0'); p++; }
    return n;
}

static int next_arg(char **pp, char *dst, int dstlen)
{
    char *p = *pp;
    int n = 0, inq = 0;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '\0') { *pp = p; return 0; }
    while (*p != '\0' && (inq || (*p != ' ' && *p != '\t'))) {
        if (*p == '"') inq = !inq;
        else if (n < dstlen - 1) dst[n++] = *p;
        p++;
    }
    dst[n] = '\0';
    *pp = p;
    return 1;
}

void __cdecl mainCRTStartup(void)
{
    char a[64], b[64], c[64], f[64], msg[128];
    char *cmd = GetCommandLineA();
    DEVMODEA dm;
    LONG r;

    next_arg(&cmd, msg, sizeof(msg));          /* argv[0] */
    if (!next_arg(&cmd, a, sizeof(a)) || !next_arg(&cmd, b, sizeof(b)) ||
        !next_arg(&cmd, c, sizeof(c))) {
        out("setmode - restore the Windows desktop display mode\r\n"
            "usage: setmode <width> <height> <bpp> [freq]\r\n"
            "  e.g. setmode 1024 768 32 85\r\n");
        ExitProcess(1);
    }
    next_arg(&cmd, f, sizeof(f));               /* optional freq */

    { int i; char *q = (char *)&dm; for (i = 0; i < (int)sizeof(dm); i++) q[i] = 0; }
    dm.dmSize = sizeof(dm);
    dm.dmPelsWidth  = (DWORD)a2i(a);
    dm.dmPelsHeight = (DWORD)a2i(b);
    dm.dmBitsPerPel = (DWORD)a2i(c);
    dm.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_BITSPERPEL;
    if (f[0]) {
        dm.dmDisplayFrequency = (DWORD)a2i(f);
        dm.dmFields |= DM_DISPLAYFREQUENCY;
    }

    r = ChangeDisplaySettingsA(&dm, 0);        /* 0 = update registry + apply now */
    if (r == DISP_CHANGE_SUCCESSFUL) {
        out("SETMODE: OK\r\n");
        ExitProcess(0);
    }
    wsprintfA(msg, "SETMODE: FAIL code=%d\r\n", (int)r);
    out(msg);
    ExitProcess(1);
}
