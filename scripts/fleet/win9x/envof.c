/* envof - what a RUNNING process's environment says about one variable (NT only).
 *
 *   envof <image> <NAME>      -> C:\RETRO_AGENT\ENVOF.TXT
 *   e.g. envof explorer.exe FX_GLIDE_SWAPINTERVAL
 *
 * Writes one line per process with that image name: "<pid> NAME=value" or
 * "<pid> NAME not set". It reads the process's own environment block (PEB ->
 * ProcessParameters -> Environment, the block CreateProcess hands to every
 * child), so it answers "what will a program Explorer starts get?" without
 * starting anything - no keystrokes into a desktop somebody may be using.
 * The question it was written for (2026-10-02, .124): did Explorer pick up a
 * system variable set with reg, after envbcast's WM_SETTINGCHANGE?
 *
 * 32-bit NT (XP): PEB+0x10 = ProcessParameters, +0x48 = Environment, which is
 * UTF-16. The block's size is not stored, so it is read a page at a time up to
 * the double NUL (ReadProcessMemory fails on a range that crosses into an
 * unmapped page). Needs PROCESS_QUERY_INFORMATION | PROCESS_VM_READ: the same
 * user, which the retro agent is. NOT for Win9x (no PEB of this shape) - it
 * says so and exits 3.
 *
 * No C runtime:
 *   i686-w64-mingw32-gcc -O1 -march=i586 -mwindows -nostdlib -fno-builtin -e _start@0 \
 *       -o envof.exe envof.c -lkernel32 -luser32 -s
 */
#include <windows.h>
#include <tlhelp32.h>

typedef LONG (WINAPI *nqip_fn)(HANDLE, ULONG, PVOID, ULONG, PULONG);

static HANDLE out = INVALID_HANDLE_VALUE;
static WCHAR env[32768];               /* static: no __chkstk */
static char line[1024];

static void w(const char *s)
{
    DWORD n;
    WriteFile(out, s, lstrlenA(s), &n, NULL);
    WriteFile(out, "\r\n", 2, &n, NULL);
}

/* the n-th whitespace-separated argument of the command line (0 = program) */
static int arg(const char *cl, int n, char *buf, int cap)
{
    int i = 0, k;
    for (;;) {
        while (*cl == ' ' || *cl == '\t') cl++;
        if (!*cl) return 0;
        k = 0;
        if (*cl == '"') {
            cl++;
            while (*cl && *cl != '"') { if (i == n && k < cap - 1) buf[k++] = *cl; cl++; }
            if (*cl) cl++;
        } else {
            while (*cl && *cl != ' ' && *cl != '\t') { if (i == n && k < cap - 1) buf[k++] = *cl; cl++; }
        }
        if (i == n) { buf[k] = 0; return 1; }
        i++;
    }
}

static int read_env(HANDLE p, BYTE *base)
{
    DWORD got = 0;
    SIZE_T n;
    while (got + 4096 <= sizeof env) {
        /* to the end of the current page, at most a page */
        DWORD chunk = 4096 - ((DWORD)(ULONG_PTR)(base + got) & 4095);
        DWORD i;
        if (!ReadProcessMemory(p, base + got, (BYTE *)env + got, chunk, &n) || n != chunk)
            return got > 0 ? (int)got : -1;
        got += chunk;
        for (i = 2; i + 1 < got / 2; i++)
            if (env[i] == 0 && env[i - 1] == 0) return (int)got;
    }
    return (int)got;
}

static void report(DWORD pid, HANDLE p, nqip_fn nqip, const char *name)
{
    struct { LONG ExitStatus; PVOID Peb; ULONG_PTR a, b, c, d; } pbi;
    BYTE *params = NULL, *envp = NULL;
    SIZE_T n;
    ULONG rl;
    int got, i, nl = lstrlenA(name);
    WCHAR *s;

    if (nqip(p, 0, &pbi, sizeof pbi, &rl) != 0 || !pbi.Peb ||
        !ReadProcessMemory(p, (BYTE *)pbi.Peb + 0x10, &params, sizeof params, &n) || !params ||
        !ReadProcessMemory(p, params + 0x48, &envp, sizeof envp, &n) || !envp) {
        wsprintfA(line, "%lu FAIL: could not read its PEB (error %lu)", pid, GetLastError());
        w(line);
        return;
    }
    got = read_env(p, envp);
    if (got <= 0) {
        wsprintfA(line, "%lu FAIL: could not read its environment block", pid);
        w(line);
        return;
    }
    env[(sizeof env / 2) - 1] = 0;
    for (s = env; *s; s += lstrlenW(s) + 1) {
        for (i = 0; i < nl; i++) {
            WCHAR a = s[i], b = (WCHAR)(BYTE)name[i];
            if (a >= 'a' && a <= 'z') a -= 32;
            if (b >= 'a' && b <= 'z') b -= 32;
            if (a != b) break;
        }
        if (i == nl && s[nl] == '=') {
            char v[512];
            WideCharToMultiByte(CP_ACP, 0, s, -1, v, sizeof v, NULL, NULL);
            wsprintfA(line, "%lu %s", pid, v);
            w(line);
            return;
        }
    }
    wsprintfA(line, "%lu %s not set", pid, name);
    w(line);
}

void WINAPI _start(void)
{
    char image[MAX_PATH], name[256];
    const char *cl = GetCommandLineA();
    HANDLE snap;
    PROCESSENTRY32 pe;
    nqip_fn nqip;
    OSVERSIONINFOA ov;
    int found = 0;

    out = CreateFileA("C:\\RETRO_AGENT\\ENVOF.TXT", GENERIC_WRITE, FILE_SHARE_READ, NULL,
                      CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (!arg(cl, 1, image, sizeof image) || !arg(cl, 2, name, sizeof name)) {
        w("usage: envof <image> <NAME>");
        ExitProcess(2);
    }
    ov.dwOSVersionInfoSize = sizeof ov;
    nqip = (nqip_fn)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQueryInformationProcess");
    if (!GetVersionExA(&ov) || ov.dwPlatformId != VER_PLATFORM_WIN32_NT || !nqip) {
        w("FAIL: NT only (no PEB environment to read here)");
        ExitProcess(3);
    }
    snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    pe.dwSize = sizeof pe;
    if (snap != INVALID_HANDLE_VALUE && Process32First(snap, &pe)) {
        do {
            if (lstrcmpiA(pe.szExeFile, image) == 0) {
                HANDLE p = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE,
                                       pe.th32ProcessID);
                found++;
                if (!p) {
                    wsprintfA(line, "%lu FAIL: OpenProcess error %lu", pe.th32ProcessID, GetLastError());
                    w(line);
                    continue;
                }
                report(pe.th32ProcessID, p, nqip, name);
                CloseHandle(p);
            }
        } while (Process32Next(snap, &pe));
    }
    if (!found) {
        wsprintfA(line, "no process named %s", image);
        w(line);
    }
    CloseHandle(out);
    ExitProcess(found ? 0 : 1);
}
