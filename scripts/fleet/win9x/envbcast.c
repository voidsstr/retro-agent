/* envbcast - tell Explorer the environment changed: WM_SETTINGCHANGE("Environment").
 *
 *   envbcast            -> C:\RETRO_AGENT\ENVBCAST.TXT
 *
 * A variable written straight into the registry - REGWRITE or `reg add` on
 * HKLM\SYSTEM\CurrentControlSet\Control\Session Manager\Environment or
 * HKCU\Environment - reaches nothing that is already running. Explorer keeps
 * the environment it started with, so every game started from the desktop or
 * the Start menu goes on without the variable until the next log-on. The
 * System control panel (and setx, which XP does not ship) end with a
 * WM_SETTINGCHANGE broadcast whose lParam is "Environment", on which Explorer
 * re-reads the environment from the registry. This sends exactly that
 * broadcast and nothing else. Found 2026-10-02 on .124: the system
 * FX_GLIDE_SWAPINTERVAL=1 (vsync for every Glide/OpenGL game, the user's
 * choice) had been set with reg, and Explorer never saw it.
 *
 * A program that is already running - the retro agent included - keeps its
 * old environment; only what Explorer starts AFTER this sees the change. The
 * 3dfx Control Panel sends the same broadcast on Apply (3dfxctl.c).
 *
 * Works on 9x and NT. No C runtime:
 *   i686-w64-mingw32-gcc -O1 -march=i586 -mwindows -nostdlib -fno-builtin -e _start@0 \
 *       -o envbcast.exe envbcast.c -luser32 -lkernel32 -s
 */
#include <windows.h>

static HANDLE out = INVALID_HANDLE_VALUE;

static void w(const char *s)
{
    DWORD n;
    WriteFile(out, s, lstrlenA(s), &n, NULL);
    WriteFile(out, "\r\n", 2, &n, NULL);
}

void WINAPI _start(void)
{
    DWORD_PTR res = 0;
    LRESULT ok;

    out = CreateFileA("C:\\RETRO_AGENT\\ENVBCAST.TXT", GENERIC_WRITE, FILE_SHARE_READ, NULL,
                      CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    /* SMTO_ABORTIFHUNG: one hung top-level window must not hold the broadcast
     * (and this process) forever; 5 s per window is what the panel uses. */
    ok = SendMessageTimeoutA(HWND_BROADCAST, WM_SETTINGCHANGE, 0, (LPARAM)"Environment",
                             SMTO_ABORTIFHUNG, 5000, &res);
    w(ok ? "OK: WM_SETTINGCHANGE(\"Environment\") broadcast - programs Explorer starts from now on see the registry environment"
         : "FAIL: the broadcast timed out or failed - Explorer may not have re-read the environment");
    CloseHandle(out);
    ExitProcess(ok ? 0 : 1);
}
