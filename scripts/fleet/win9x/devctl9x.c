/* devctl9x disable|enable|status|persistoff|persiston <id> [<id> ...] | @<file of ids>
 *   (COMMAND.COM caps a command line at ~127 characters, hence @file)
 *
 * persistoff / persiston write HKLM\Enum\<id> ConfigFlags = 01 / 00 and
 * RegFlushKey BEFORE anything else, touching no devnode: on a box that
 * freezes seconds after logon, a lazily flushed registry change is lost.
 *
 * Win9x: disable or enable a devnode NOW through Config Manager
 * (CM_Disable_DevNode / CM_Enable_DevNode) and report its status before and
 * after. Written 2026-09-27 for .243, which hard-froze within about two
 * minutes of every boot once its NEC uPD720101 USB card had drivers: a
 * ConfigFlags change only takes effect at the next boot, and the box froze
 * before it got there. A live disable stops the device's driver within
 * seconds of logon. Persist the choice separately (Enum ConfigFlags = 01).
 *
 * No C runtime; cfgmgr32 is loaded dynamically (a static import Win9x cannot
 * resolve kills the exe at load). Output: C:\RETRO_AGENT\DEVCTL.TXT */
#include <windows.h>

typedef DWORD (WINAPI *loc_t)(DWORD *, const char *, ULONG);
typedef DWORD (WINAPI *dev_t)(DWORD, ULONG);
typedef DWORD (WINAPI *sta_t)(ULONG *, ULONG *, DWORD, ULONG);

static HANDLE out;
static char line[600];
static char ids[4096];

static void w(const char *s)
{
    DWORD n;
    WriteFile(out, s, lstrlenA(s), &n, NULL);
    WriteFile(out, "\r\n", 2, &n, NULL);
    FlushFileBuffers(out);
}

static void status(sta_t sta, DWORD dn, const char *when)
{
    ULONG st = 0, pr = 0;
    DWORD cr = sta(&st, &pr, dn, 0);
    wsprintfA(line, "  %s: cr=%lu status=%08lX problem=%lu", when, cr, st, pr);
    w(line);
}

void __stdcall start(void)
{
    char *c = GetCommandLineA(), *id, verb[16];
    int i, mode, bad = 0;
    HMODULE h;
    loc_t loc; dev_t dis, ena; sta_t sta;

    if (*c == '"') { c++; while (*c && *c != '"') c++; if (*c) c++; } else while (*c && *c != ' ') c++;
    while (*c == ' ') c++;
    for (i = 0; i < 15 && c[i] && c[i] != ' '; i++) verb[i] = c[i];
    verb[i] = 0;
    c += i;
    mode = !lstrcmpiA(verb, "disable") ? 1 : !lstrcmpiA(verb, "enable") ? 2 : !lstrcmpiA(verb, "status") ? 3
         : !lstrcmpiA(verb, "persistoff") ? 4 : !lstrcmpiA(verb, "persiston") ? 5 : 0;

    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    out = CreateFileA("C:\\RETRO_AGENT\\DEVCTL.TXT", GENERIC_WRITE, FILE_SHARE_READ, NULL,
                      CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (out == INVALID_HANDLE_VALUE) ExitProcess(3);
    wsprintfA(line, "devctl9x 1.0 %s", verb);
    w(line);
    if (!mode) { w("usage: devctl9x disable|enable|status|persistoff|persiston <id> [<id> ...] | @file"); CloseHandle(out); ExitProcess(1); }

    h = LoadLibraryA("cfgmgr32.dll");
    loc = h ? (loc_t)GetProcAddress(h, "CM_Locate_DevNodeA") : NULL;
    dis = h ? (dev_t)GetProcAddress(h, "CM_Disable_DevNode") : NULL;
    ena = h ? (dev_t)GetProcAddress(h, "CM_Enable_DevNode") : NULL;
    sta = h ? (sta_t)GetProcAddress(h, "CM_Get_DevNode_Status") : NULL;
    if (mode < 4 && (!loc || !dis || !ena || !sta)) { w("cfgmgr32.dll or its exports unavailable"); CloseHandle(out); ExitProcess(2); }

    while (*c == ' ') c++;
    if (*c == '@') {                    /* ids from a file, whitespace separated */
        DWORD n = 0;
        HANDLE f = CreateFileA(c + 1, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
        if (f == INVALID_HANDLE_VALUE) { w("cannot open the id file"); CloseHandle(out); ExitProcess(4); }
        ReadFile(f, ids, sizeof(ids) - 1, &n, NULL);
        CloseHandle(f);
        ids[n] = 0;
        for (c = ids; *c; c++) if (*c == '\r' || *c == '\n' || *c == '\t') *c = ' ';
        c = ids;
    }
    for (;;) {
        DWORD dn = 0, cr;
        while (*c == ' ') c++;
        if (!*c) break;
        id = c;
        while (*c && *c != ' ') c++;
        if (*c) *c++ = 0;
        if (mode >= 4) {
            HKEY k;
            BYTE flags[4] = { (BYTE)(mode == 4 ? 1 : 0), 0, 0, 0 }, back[4] = { 0xFF, 0xFF, 0xFF, 0xFF };
            DWORD type = 0, sz = sizeof(back);
            lstrcpyA(line, "Enum\\");
            lstrcatA(line, id);
            cr = RegOpenKeyExA(HKEY_LOCAL_MACHINE, line, 0, KEY_READ | KEY_WRITE, &k);
            if (!cr) {
                cr = RegSetValueExA(k, "ConfigFlags", 0, REG_BINARY, flags, 4);
                RegFlushKey(k);
                RegQueryValueExA(k, "ConfigFlags", NULL, &type, back, &sz);
                RegCloseKey(k);
            }
            RegFlushKey(HKEY_LOCAL_MACHINE);
            wsprintfA(line, "%s: ConfigFlags set cr=%lu, read back %02X %02X %02X %02X (type %lu)",
                      id, cr, back[0], back[1], back[2], back[3], type);
            w(line);
            if (cr || back[0] != flags[0]) bad++;
            continue;
        }
        cr = loc(&dn, id, 0);
        wsprintfA(line, "%s: locate cr=%lu", id, cr);
        w(line);
        if (cr) { bad++; continue; }
        status(sta, dn, "before");
        if (mode == 1) cr = dis(dn, 0);
        else if (mode == 2) cr = ena(dn, 0);
        if (mode != 3) {
            wsprintfA(line, "  %s -> cr=%lu", mode == 1 ? "CM_Disable_DevNode" : "CM_Enable_DevNode", cr);
            w(line);
            if (cr) bad++;
            Sleep(1000);
            status(sta, dn, "after");
        }
    }
    w(bad ? "done - with errors" : "done");
    CloseHandle(out);
    ExitProcess(bad ? 6 : 0);
}
