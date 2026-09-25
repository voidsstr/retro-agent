/* deskfix9x - recover a Win9x desktop left black/garbled by a fullscreen Glide/GL
 * game (found on .243 after GLQuake on its Voodoo 2, 2026-09-24): re-apply the
 * registry display mode, restore the static system palette, and force every
 * window to repaint. Changes no settings. Log: C:\RETRO_AGENT\DESKFIX.TXT */
#include <windows.h>
static HANDLE out; static char line[200];
static void w(const char *s) { DWORD n; WriteFile(out, s, lstrlenA(s), &n, NULL); WriteFile(out, "\r\n", 2, &n, NULL); }
void __stdcall start(void) {
    HDC dc; LONG r; UINT prev;
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    out = CreateFileA("C:\\RETRO_AGENT\\DESKFIX.TXT", GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    r = ChangeDisplaySettingsA(NULL, 0);
    wsprintfA(line, "ChangeDisplaySettings(NULL) = %ld", r); w(line);
    dc = GetDC(NULL);
    prev = SetSystemPaletteUse(dc, SYSPAL_STATIC);
    wsprintfA(line, "SetSystemPaletteUse(STATIC) prev=%u", prev); w(line);
    ReleaseDC(NULL, dc);
    SendMessageTimeoutA(HWND_BROADCAST, WM_SYSCOLORCHANGE, 0, 0, SMTO_ABORTIFHUNG, 2000, NULL);
    SendMessageTimeoutA(HWND_BROADCAST, WM_PALETTECHANGED, (WPARAM)GetDesktopWindow(), 0, SMTO_ABORTIFHUNG, 2000, NULL);
    SendMessageTimeoutA(HWND_BROADCAST, WM_QUERYNEWPALETTE, 0, 0, SMTO_ABORTIFHUNG, 2000, NULL);
    RedrawWindow(NULL, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN | RDW_UPDATENOW);
    w("broadcast + full redraw done");
    CloseHandle(out); ExitProcess(0);
}
