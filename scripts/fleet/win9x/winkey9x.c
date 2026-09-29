/* winkey9x "<title substring>" <key> [max|restore] - bring a window to the front and press ONE key.
 *
 * Written 2026-09-28 for .243: a LAUNCHed DOS program starts in a MINIMIZED
 * DOS box there (every new console window does), so the agent's UIKEY - which
 * types into whatever has focus - never reaches it. Found with DOS Quake
 * sitting at its "really quit? (Y/N)" menu. This restores the first visible
 * top-level window whose title contains the substring (case-insensitive),
 * makes it the foreground window, waits, and sends one virtual key.
 * <key> is a single character (A-Z, 0-9) or RETURN / ESCAPE / SPACE, or CLOSE
 * to post WM_CLOSE instead (Win98 then asks to terminate a running DOS box), or
 * PID to report the process that owns the window (a DOS box: its WINOA386.MOD).
 * No C runtime. Output: C:\RETRO_AGENT\WINKEY.TXT
 *   i686-w64-mingw32-gcc -O1 -march=i586 -mwindows -nostdlib -fno-builtin -e _start@0 \
 *       -o winkey9x.exe winkey9x.c -lkernel32 -luser32 -s */
#include <windows.h>
int memcmp(const void *a, const void *b, unsigned int n);
static HANDLE out; static char line[512], want[128], title[256]; static HWND found;
static void w(const char *s) { DWORD n; WriteFile(out, s, lstrlenA(s), &n, NULL); WriteFile(out, "\r\n", 2, &n, NULL); }
static BOOL CALLBACK each(HWND h, LPARAM l)
{
    (void)l;
    if (!IsWindowVisible(h)) return TRUE;
    title[0] = 0; GetWindowTextA(h, title, sizeof(title)); CharUpperA(title);
    /* the console that launched us is titled with our own command line */
    { char *p = title; for (; *p; p++) if (!memcmp(p, "WINKEY9X", 8)) return TRUE; }
    { char *p = title; int n = lstrlenA(want);
      for (; *p; p++) if (!memcmp(p, want, n)) { found = h; return FALSE; } }
    return TRUE;
}
static const char *tok(const char *p, char *d, int max)
{
    int i = 0; while (*p == ' ') p++;
    if (*p == '"') { p++; while (*p && *p != '"' && i < max - 1) d[i++] = *p++; if (*p) p++; }
    else while (*p && *p != ' ' && i < max - 1) d[i++] = *p++;
    d[i] = 0; return p;
}
int memcmp(const void *a, const void *b, unsigned int n)
{ const unsigned char *x = a, *y = b; while (n--) { if (*x != *y) return *x - *y; x++; y++; } return 0; }
void __stdcall start(void)
{
    char *c = GetCommandLineA(), key[16], mode[16];
    BYTE vk;
    if (*c == '"') { c++; while (*c && *c != '"') c++; if (*c) c++; } else while (*c && *c != ' ') c++;
    c = (char *)tok(c, want, sizeof(want)); c = (char *)tok(c, key, sizeof(key)); tok(c, mode, sizeof(mode));
    CharUpperA(want); CharUpperA(key);
    out = CreateFileA("C:\\RETRO_AGENT\\WINKEY.TXT", GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, 0, NULL);
    if (!want[0] || !key[0]) { w("usage: winkey9x \"<title substring>\" <key> [max|restore]"); ExitProcess(1); }
    EnumWindows(each, 0);
    if (!found) { wsprintfA(line, "no visible window titled *%s*", want); w(line); ExitProcess(2); }
    if (!lstrcmpA(key, "PID")) {
        /* which process owns the window: for a DOS box, its WINOA386.MOD VM -
         * the one PROCKILL target that is certainly that program's */
        DWORD pid = 0;
        GetWindowThreadProcessId(found, &pid);
        wsprintfA(line, "window %08lX pid %lu", (DWORD)found, pid);
        w(line); CloseHandle(out); ExitProcess(0);
    }
    if (!lstrcmpA(key, "CLOSE")) {
        /* a minimized graphics-mode DOS box never gets keys; closing its window
         * makes Win98 offer to terminate the program, which destroys the VM
         * properly - unlike killing a process on Win9x */
        PostMessageA(found, WM_CLOSE, 0, 0);
        wsprintfA(line, "window %08lX - WM_CLOSE posted", (DWORD)found);
        w(line); CloseHandle(out); ExitProcess(0);
    }
    vk = !lstrcmpA(key, "RETURN") ? VK_RETURN : !lstrcmpA(key, "ESCAPE") ? VK_ESCAPE
       : !lstrcmpA(key, "SPACE") ? VK_SPACE : (BYTE)key[0];
    ShowWindow(found, !lstrcmpiA(mode, "max") ? SW_SHOWMAXIMIZED : SW_RESTORE);
    SetForegroundWindow(found);
    Sleep(2500);
    wsprintfA(line, "window %08lX foreground=%d iconic=%d - sending vk %02X", (DWORD)found,
              GetForegroundWindow() == found, IsIconic(found), vk);
    w(line);
    keybd_event(vk, (BYTE)MapVirtualKeyA(vk, 0), 0, 0);
    Sleep(80);
    keybd_event(vk, (BYTE)MapVirtualKeyA(vk, 0), KEYEVENTF_KEYUP, 0);
    w("done");
    CloseHandle(out);
    ExitProcess(0);
}
