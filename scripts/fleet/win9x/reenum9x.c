/* reenum9x [devid-to-check] - ask Win9x Config Manager to re-enumerate the PCI
 * bus (BIOS\*PNP0A03\0C) and report the status of a device id afterwards.
 * No C runtime; cfgmgr32 is loaded dynamically. Output: C:\RETRO_AGENT\REENUM.TXT */
#include <windows.h>
typedef DWORD (WINAPI *loc_t)(DWORD *, const char *, ULONG);
typedef DWORD (WINAPI *ren_t)(DWORD, ULONG);
typedef DWORD (WINAPI *sta_t)(ULONG *, ULONG *, DWORD, ULONG);
static HANDLE out; static char line[512];
static void w(const char *s) { DWORD n; WriteFile(out, s, lstrlenA(s), &n, NULL); WriteFile(out, "\r\n", 2, &n, NULL); FlushFileBuffers(out); }
static void check(HMODULE h, const char *id) {
    loc_t loc = (loc_t)GetProcAddress(h, "CM_Locate_DevNodeA");
    sta_t sta = (sta_t)GetProcAddress(h, "CM_Get_DevNode_Status");
    DWORD dn = 0, cr; ULONG st = 0, pr = 0;
    if (!loc || !sta) { w("check: missing export"); return; }
    cr = loc(&dn, id, 0);
    wsprintfA(line, "locate %s -> cr=%lu dn=%08lX", id, cr, dn); w(line);
    if (cr == 0) { cr = sta(&st, &pr, dn, 0); wsprintfA(line, "  status cr=%lu status=%08lX problem=%lu", cr, st, pr); w(line); }
}
void __stdcall start(void) {
    char *c = GetCommandLineA(); HMODULE h; loc_t loc; ren_t ren; DWORD dn = 0, cr;
    if (*c == '"') { c++; while (*c && *c != '"') c++; if (*c) c++; } else while (*c && *c != ' ') c++;
    while (*c == ' ') c++;
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    out = CreateFileA("C:\\RETRO_AGENT\\REENUM.TXT", GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (out == INVALID_HANDLE_VALUE) ExitProcess(3);
    h = LoadLibraryA("cfgmgr32.dll");
    wsprintfA(line, "reenum9x 1.0  cfgmgr32=%08lX", (DWORD)h); w(line);
    if (!h) { CloseHandle(out); ExitProcess(2); }
    if (*c) { w("before:"); check(h, c); }
    if (*c && !lstrcmpiA(c + lstrlenA(c) - 6, "noscan")) { CloseHandle(out); ExitProcess(0); }
    loc = (loc_t)GetProcAddress(h, "CM_Locate_DevNodeA");
    ren = (ren_t)GetProcAddress(h, "CM_Reenumerate_DevNode");
    wsprintfA(line, "exports: locate=%08lX reenumerate=%08lX", (DWORD)loc, (DWORD)ren); w(line);
    if (!loc || !ren) { CloseHandle(out); ExitProcess(4); }
    cr = loc(&dn, "BIOS\\*PNP0A03\\0C", 0);
    wsprintfA(line, "locate PCI bus -> cr=%lu dn=%08lX", cr, dn); w(line);
    if (cr) { CloseHandle(out); ExitProcess(5); }
    w("calling CM_Reenumerate_DevNode(pci bus, 0)");
    cr = ren(dn, 0);
    wsprintfA(line, "reenumerate -> cr=%lu", cr); w(line);
    Sleep(5000);
    if (*c) { w("after:"); check(h, c); }
    w("done");
    CloseHandle(out);
    ExitProcess(cr ? 6 : 0);
}
