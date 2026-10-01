/* cwtest - exercises agent/src/consolewin.c exactly as the agent does, as its
 * own console process, so WINLIST can see what happened to the window. */
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include "consolewin.h"

static char g_logpath[MAX_PATH] = "C:\\cwtest.log";

void log_msg(const char *tag, const char *fmt, ...)
{
    va_list ap; FILE *f = fopen(g_logpath, "a");
    if (!f) return;
    fprintf(f, "[%s] ", tag);
    va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
    fprintf(f, "\n"); fclose(f);
}

int main(int argc, char **argv)
{
    HWND h; char t[64];
    if (argc > 1) lstrcpynA(t, argv[1], sizeof(t)); else lstrcpynA(t, "cwtest", sizeof(t));
    SetConsoleTitleA(t);
    printf("%s\n", t);
    log_msg("TEST", "---- %s start, pid %lu", t, (unsigned long)GetCurrentProcessId());
    consolewin_startup(0);
    Sleep(2000);
    {
        typedef HWND (WINAPI *gcw_t)(void);
        gcw_t gcw = (gcw_t)GetProcAddress(GetModuleHandleA("kernel32.dll"), "GetConsoleWindow");
        h = gcw ? gcw() : NULL;
        log_msg("TEST", "%s after 2s: hwnd=%p IsIconic=%d", t, (void*)h, h ? (int)IsIconic(h) : -1);
    }
    Sleep(600000);
    return 0;
}
