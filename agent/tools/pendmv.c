/*
 * pendmv - schedule a file replace/delete for the next boot via MoveFileEx
 *          with MOVEFILE_DELAY_UNTIL_REBOOT. The Session Manager performs the
 *          op at boot, BEFORE win32k loads the display driver and BEFORE WFP
 *          scans - which is how you swap a WFP-protected, in-use driver file
 *          (e.g. system32\3dfxvs.dll) without WFP reverting it.
 *
 *   pendmv <src> <dst>     at boot: rename/replace dst with src (both paths)
 *   pendmv <path> -        at boot: delete path (dst "-" or omitted)
 *
 * Prints:
 *   PENDMV: OK <src> -> <dst>      scheduled
 *   PENDMV: OK DELETE <path>       scheduled
 *   PENDMV: FAIL code=0x........   MoveFileEx rejected it (bad path, etc.)
 *
 * Build (mingw, no CRT - matches updrv.c):
 *   i686-w64-mingw32-gcc -Wall -Wextra -Os -s -nostdlib -DWIN32_LEAN_AND_MEAN \
 *     -DWINVER=0x0500 -D_WIN32_WINNT=0x0500 -march=i586 -mtune=pentium3 \
 *     -fno-stack-protector -o pendmv.exe pendmv.c -Wl,-e,_mainCRTStartup \
 *     -lkernel32
 */
#include <windows.h>

#ifndef MOVEFILE_DELAY_UNTIL_REBOOT
#define MOVEFILE_DELAY_UNTIL_REBOOT 0x00000004
#endif
#ifndef MOVEFILE_REPLACE_EXISTING
#define MOVEFILE_REPLACE_EXISTING 0x00000001
#endif

static void out(const char *s)
{
    DWORD written;
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    if (h != INVALID_HANDLE_VALUE && h != NULL)
        WriteFile(h, s, (DWORD)lstrlenA(s), &written, NULL);
}

/* Next whitespace-delimited token, honoring double quotes (stripped). */
static int next_arg(char **pp, char *dst, int dstlen)
{
    char *p = *pp;
    int n = 0, inq = 0;

    while (*p == ' ' || *p == '\t')
        p++;
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
    char src[520], dst[520], msg[1100];
    char *cmdline = GetCommandLineA();
    BOOL del, ok;
    DWORD err;

    next_arg(&cmdline, msg, sizeof(msg));               /* argv[0] */
    if (!next_arg(&cmdline, src, sizeof(src))) {
        out("pendmv - schedule a boot-time file replace/delete (MoveFileEx)\r\n"
            "usage: pendmv <src> <dst>   (replace dst with src at next boot)\r\n"
            "       pendmv <path> -      (delete path at next boot)\r\n");
        ExitProcess(1);
    }
    if (!next_arg(&cmdline, dst, sizeof(dst)))
        lstrcpyA(dst, "-");

    del = (dst[0] == '-' && dst[1] == '\0');

    SetLastError(0);
    if (del)
        ok = MoveFileExA(src, NULL, MOVEFILE_DELAY_UNTIL_REBOOT);
    else
        ok = MoveFileExA(src, dst,
                         MOVEFILE_DELAY_UNTIL_REBOOT | MOVEFILE_REPLACE_EXISTING);

    if (ok) {
        if (del) wsprintfA(msg, "PENDMV: OK DELETE %s\r\n", src);
        else     wsprintfA(msg, "PENDMV: OK %s -> %s\r\n", src, dst);
        out(msg);
        ExitProcess(0);
    }

    err = GetLastError();
    wsprintfA(msg, "PENDMV: FAIL code=0x%08X\r\n", (unsigned)err);
    out(msg);
    ExitProcess(1);
}
