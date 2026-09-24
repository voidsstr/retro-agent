/* fleet9x - two Win9x chores the 1.78.x agent cannot do by itself.
 *   fleet9x attrib <path> <hex FILE_ATTRIBUTE_* mask>   (writes FLEET9X.TXT)
 *   fleet9x reboot                                      (clean forced reboot)
 * The reboot path is the agent's Win9x logic: a message queue, kill the
 * console windows that veto shutdown, ExitWindowsEx, then keep pumping. */
#include <windows.h>
#include <tlhelp32.h>

/* No C runtime at all: Win98's MSVCRT.DLL lacks exports that mingw-w64's
 * startup code wants, and the loader then sits on a modal dialog forever. */
static void note(const char *s) {
    DWORD n;
    HANDLE f = CreateFileA("C:\\RETRO_AGENT\\FLEET9X.TXT", GENERIC_WRITE, 0, NULL,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    SetFilePointer(f, 0, NULL, FILE_END);
    WriteFile(f, s, lstrlenA(s), &n, NULL);
    WriteFile(f, "\r\n", 2, &n, NULL);
    CloseHandle(f);
}
static const char *skip(const char *p) { while (*p == ' ') p++; return p; }
static int starts(const char *p, const char *w) {
    while (*w) if (*p++ != *w++) return 0;
    return 1;
}

static void kill_consoles(void) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32 pe; DWORD me = GetCurrentProcessId();
    if (snap == INVALID_HANDLE_VALUE) return;
    pe.dwSize = sizeof(pe);
    if (Process32First(snap, &pe)) do {
        const char *n = pe.szExeFile, *q;
        for (q = pe.szExeFile; *q; q++) if (*q == '\\') n = q + 1;
        if (pe.th32ProcessID != me &&
            (!lstrcmpiA(n, "COMMAND.COM") || !lstrcmpiA(n, "CONAGENT.EXE") ||
             !lstrcmpiA(n, "RETRO_CHAT.EXE"))) {
            HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, pe.th32ProcessID);
            if (h) { TerminateProcess(h, 1); CloseHandle(h); }
        }
    } while (Process32Next(snap, &pe));
    CloseHandle(snap);
}

static int run(HINSTANCE hi, HINSTANCE hp, LPSTR cmd, int show) {
    char buf[300];
    (void)hp; (void)show;
    cmd = (LPSTR)skip(cmd);
    if (starts(cmd, "attrib ")) {
        char path[MAX_PATH]; unsigned long mask = 0; int i = 0;
        const char *p = skip(cmd + 7);
        while (*p && *p != ' ' && i < MAX_PATH - 1) path[i++] = *p++;
        path[i] = 0; p = skip(p);
        for (; *p; p++) {
            int d = (*p >= '0' && *p <= '9') ? *p - '0' :
                    (*p >= 'a' && *p <= 'f') ? *p - 'a' + 10 :
                    (*p >= 'A' && *p <= 'F') ? *p - 'A' + 10 : -1;
            if (d < 0) break;
            mask = mask * 16 + d;
        }
        if (!path[0]) { note("attrib: bad args"); return 2; }
        {
            DWORD before = GetFileAttributesA(path);
            BOOL ok = SetFileAttributesA(path, mask);
            wsprintfA(buf, "attrib %s before=%lx set=%lx ok=%d now=%lx", path,
                      (unsigned long)before, mask, (int)ok,
                      (unsigned long)GetFileAttributesA(path));
        }
        note(buf);
        return 0;
    }
    if (starts(cmd, "reboot")) {
        MSG m; DWORD end; BOOL ok;
        HWND w = CreateWindowA("STATIC", "", WS_POPUP, 0, 0, 0, 0, NULL, NULL, hi, NULL);
        kill_consoles(); Sleep(500);
        ok = ExitWindowsEx(EWX_REBOOT | EWX_FORCE, 0);
        wsprintfA(buf, "reboot: ExitWindowsEx(REBOOT|FORCE) = %d err=%lu", ok, GetLastError()); note(buf);
        if (!ok) { Sleep(1000); ok = ExitWindowsEx(EWX_REBOOT, 0);
                   wsprintfA(buf, "reboot: retry without FORCE = %d", ok); note(buf); }
        end = GetTickCount() + 90000;
        while ((long)(GetTickCount() - end) < 0) {
            while (PeekMessageA(&m, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageA(&m); }
            Sleep(50);
        }
        note("reboot: still running after 90s - did not take");
        if (w) DestroyWindow(w);
        return 1;
    }
    note("usage: fleet9x attrib <path> <hexmask> | reboot");
    return 2;
}

/* Entry point with no CRT: skip the program name, run, exit. */
void __stdcall start(void) {
    char *c = GetCommandLineA();
    if (*c == '"') { c++; while (*c && *c != '"') c++; if (*c) c++; }
    else while (*c && *c != ' ') c++;
    ExitProcess(run(GetModuleHandleA(NULL), NULL, c, SW_SHOW));
}
