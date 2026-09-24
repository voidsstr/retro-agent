/* wintext9x - dump every visible top-level dialog (#32770) and its child controls
 * (class, id, text, enabled/checked) to C:\RETRO_AGENT\WINTEXT.TXT. No C runtime. */
#include <windows.h>
static HANDLE out; static char line[700], cls[64], txt[512];
static void w(const char *s) { DWORD n; WriteFile(out, s, lstrlenA(s), &n, NULL); WriteFile(out, "\r\n", 2, &n, NULL); }
static BOOL CALLBACK child(HWND h, LPARAM depth) {
    RECT r; LRESULT chk = -1;
    if (!IsWindowVisible(h)) return TRUE;
    GetClassNameA(h, cls, sizeof(cls));
    txt[0] = 0; SendMessageTimeoutA(h, WM_GETTEXT, sizeof(txt), (LPARAM)txt, SMTO_ABORTIFHUNG, 500, NULL);
    GetWindowRect(h, &r);
    if (!lstrcmpiA(cls, "Button")) SendMessageTimeoutA(h, BM_GETCHECK, 0, 0, SMTO_ABORTIFHUNG, 500, (PDWORD_PTR)&chk);
    wsprintfA(line, "  [%s] id=%d %s%s rect=%d,%d,%d,%d text=\"%s\"", cls, GetDlgCtrlID(h),
              IsWindowEnabled(h) ? "" : "DISABLED ", chk == 1 ? "CHECKED " : "",
              r.left, r.top, r.right, r.bottom, txt);
    w(line);
    return TRUE;
}
static BOOL CALLBACK top(HWND h, LPARAM l) {
    RECT r;
    if (!IsWindowVisible(h)) return TRUE;
    GetClassNameA(h, cls, sizeof(cls));
    if (lstrcmpA(cls, "#32770")) return TRUE;
    txt[0] = 0; GetWindowTextA(h, txt, sizeof(txt)); GetWindowRect(h, &r);
    wsprintfA(line, "DIALOG hwnd=%08lX \"%s\" rect=%d,%d,%d,%d fg=%d", (DWORD)h, txt, r.left, r.top, r.right, r.bottom, GetForegroundWindow() == h);
    w(line);
    EnumChildWindows(h, child, 1);
    return TRUE;
}
void __stdcall start(void) {
    out = CreateFileA("C:\\RETRO_AGENT\\WINTEXT.TXT", GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (out == INVALID_HANDLE_VALUE) ExitProcess(3);
    EnumWindows(top, 0);
    w("--end--");
    CloseHandle(out);
    ExitProcess(0);
}
