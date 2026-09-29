/* mkpif9x - make a desktop shortcut with an icon on Windows 9x, and have a DOS
 * program's window close when the program ends.
 *
 *   mkpif9x "<target>" "<working dir>" "<shortcut path, no extension>" "<icon file>" [nocloseonexit]
 *   log: C:\RETRO_AGENT\MKPIF9X.TXT (appended)
 *
 * Written 2026-09-29 for .243 (Win98 SE): the DOS games already installed on
 * its C: and D: had no desktop icons - they were reachable only through the
 * DOSGAME menu in MS-DOS mode, and D: is no longer visible there at all (the
 * 80 GB disk is Windows' now). The user asked for every game on the box to
 * have a desktop icon with the game's own art.
 *
 * Same calls GAMESYNC makes (agent/src/gamesync.c gs_make_shortcut):
 * IShellLink SetPath / SetWorkingDirectory / SetIconLocation, IPersistFile
 * Save to "<path>.lnk". For an MS-DOS target the Win9x shell writes a PIF
 * ("<path>.pif") instead. That PIF then gets "Close on exit" (basic section
 * byte 63h bit 4) so quitting the game does not leave a "Finished - ..." DOS
 * window behind; byte 01h is not a checksum on Win9x (it reads 78h in every
 * PIF the shell writes), so nothing else changes.
 *
 * No C runtime. Build:
 *   i686-w64-mingw32-gcc -O1 -march=i586 -mwindows -nostdlib -e _start@0 \
 *       -o mkpif9x.exe mkpif9x.c -lkernel32 -luser32 -lole32 -luuid -s
 */
#define COBJMACROS
#include <windows.h>
#include <shlobj.h>

static const CLSID CLSID_SL = { 0x00021401, 0, 0, { 0xC0, 0, 0, 0, 0, 0, 0, 0x46 } };
static const IID   IID_SLA  = { 0x000214EE, 0, 0, { 0xC0, 0, 0, 0, 0, 0, 0, 0x46 } };
static const IID   IID_PF   = { 0x0000010B, 0, 0, { 0xC0, 0, 0, 0, 0, 0, 0, 0x46 } };

static HANDLE g_log = INVALID_HANDLE_VALUE;
static char   g_line[1024];

static void emit(const char *s)
{
    DWORD n;
    if (g_log == INVALID_HANDLE_VALUE) return;
    WriteFile(g_log, s, lstrlenA(s), &n, NULL);
    WriteFile(g_log, "\r\n", 2, &n, NULL);
    FlushFileBuffers(g_log);
}

static int next_arg(const char **p, char *out, int cap)
{
    const char *s = *p;
    int n = 0, q = 0;
    while (*s == ' ' || *s == '\t') s++;
    if (!*s) { *p = s; return 0; }
    if (*s == '"') { q = 1; s++; }
    while (*s && (q ? *s != '"' : (*s != ' ' && *s != '\t'))) {
        if (n < cap - 1) out[n++] = *s;
        s++;
    }
    if (q && *s == '"') s++;
    out[n] = 0;
    *p = s;
    return 1;
}

/* Set "Close on exit" in a PIF's basic section. 1 = set (or already set). */
static int pif_close_on_exit(const char *pif)
{
    HANDLE h = CreateFileA(pif, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    unsigned char b;
    DWORD n;
    int ok = 0;
    if (h == INVALID_HANDLE_VALUE) return 0;
    if (GetFileSize(h, NULL) >= 0x171 && SetFilePointer(h, 0x63, NULL, FILE_BEGIN) == 0x63
            && ReadFile(h, &b, 1, &n, NULL) && n == 1) {
        if (b & 0x10) ok = 1;
        else {
            b |= 0x10;
            if (SetFilePointer(h, 0x63, NULL, FILE_BEGIN) == 0x63 && WriteFile(h, &b, 1, &n, NULL) && n == 1)
                ok = 1;
        }
    }
    CloseHandle(h);
    return ok;
}

void WINAPI _start(void)
{
    const char *cmd = GetCommandLineA();
    char exe[MAX_PATH], target[MAX_PATH], wdir[MAX_PATH], base[MAX_PATH], icon[MAX_PATH], opt[32];
    char lnk[MAX_PATH + 8], pif[MAX_PATH + 8];
    WCHAR wlnk[MAX_PATH + 8];
    IShellLinkA *sl = NULL;
    IPersistFile *pf = NULL;
    HRESULT hr;
    int rc = 1;

    g_log = CreateFileA("C:\\RETRO_AGENT\\MKPIF9X.TXT", GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_ALWAYS, 0, NULL);
    if (g_log != INVALID_HANDLE_VALUE) SetFilePointer(g_log, 0, NULL, FILE_END);
    next_arg(&cmd, exe, sizeof(exe));
    target[0] = wdir[0] = base[0] = icon[0] = opt[0] = 0;
    next_arg(&cmd, target, sizeof(target));
    next_arg(&cmd, wdir, sizeof(wdir));
    next_arg(&cmd, base, sizeof(base));
    next_arg(&cmd, icon, sizeof(icon));
    next_arg(&cmd, opt, sizeof(opt));
    if (!target[0] || !base[0]) {
        emit("usage: mkpif9x \"<target>\" \"<working dir>\" \"<shortcut path, no extension>\" \"<icon>\" [nocloseonexit]");
        goto out;
    }
    wsprintfA(lnk, "%s.lnk", base);
    wsprintfA(pif, "%s.pif", base);
    if (GetFileAttributesA(target) == 0xFFFFFFFF) {
        wsprintfA(g_line, "FAIL %s: target %s does not exist", base, target);
        emit(g_line);
        goto out;
    }
    if (FAILED(CoInitialize(NULL))) { emit("FAIL CoInitialize"); goto out; }
    hr = CoCreateInstance(&CLSID_SL, NULL, CLSCTX_INPROC_SERVER, &IID_SLA, (void **)&sl);
    if (FAILED(hr) || !sl) { wsprintfA(g_line, "FAIL %s: CoCreateInstance %08lX", base, (unsigned long)hr); emit(g_line); goto uninit; }
    IShellLinkA_SetPath(sl, target);
    if (wdir[0]) IShellLinkA_SetWorkingDirectory(sl, wdir);
    if (icon[0]) IShellLinkA_SetIconLocation(sl, icon, 0);
    hr = IShellLinkA_QueryInterface(sl, &IID_PF, (void **)&pf);
    if (SUCCEEDED(hr) && pf) {
        MultiByteToWideChar(CP_ACP, 0, lnk, -1, wlnk, MAX_PATH + 8);
        hr = IPersistFile_Save(pf, wlnk, TRUE);
        IPersistFile_Release(pf);
    }
    IShellLinkA_Release(sl);
    if (FAILED(hr)) { wsprintfA(g_line, "FAIL %s: Save %08lX", base, (unsigned long)hr); emit(g_line); goto uninit; }
    if (GetFileAttributesA(pif) != 0xFFFFFFFF) {
        int closed = lstrcmpiA(opt, "nocloseonexit") == 0 ? -1 : pif_close_on_exit(pif);
        wsprintfA(g_line, "OK %s.pif -> %s (icon %s)%s", base, target, icon[0] ? icon : "-",
                  closed == 1 ? ", close on exit" : closed == 0 ? ", close-on-exit NOT set" : "");
        rc = closed == 0 ? 2 : 0;
    } else if (GetFileAttributesA(lnk) != 0xFFFFFFFF) {
        wsprintfA(g_line, "OK %s.lnk -> %s (icon %s)", base, target, icon[0] ? icon : "-");
        rc = 0;
    } else {
        wsprintfA(g_line, "FAIL %s: Save reported success but neither .pif nor .lnk exists", base);
    }
    emit(g_line);
uninit:
    CoUninitialize();
out:
    emit("done");
    if (g_log != INVALID_HANDLE_VALUE) CloseHandle(g_log);
    ExitProcess((UINT)rc);
}
