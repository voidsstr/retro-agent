/* regdump9x <HKDD|HKLM|HKCC> <subkey> <outfile>  - recursive dump, no C runtime. */
#include <windows.h>
static HANDLE out;
static void w(const char *s) { DWORD n; WriteFile(out, s, lstrlenA(s), &n, NULL); }
static void hexv(const BYTE *d, DWORD len) {
    char b[8]; DWORD i;
    for (i = 0; i < len && i < 256; i++) { wsprintfA(b, "%02x ", d[i]); w(b); }
    if (len > 256) w("...");
}
static void dump(HKEY root, const char *path, int depth) {
    HKEY k; DWORD i; char name[512], sub[1024]; static BYTE data[4096];
    if (depth > 12) return;
    if (RegOpenKeyExA(root, path, 0, KEY_READ, &k) != ERROR_SUCCESS) { w("[cannot open] "); w(path); w("\r\n"); return; }
    w("["); w(path); w("]\r\n");
    for (i = 0;; i++) {
        DWORD nl = sizeof(name), dl = sizeof(data), type = 0; char line[64];
        LONG r = RegEnumValueA(k, i, name, &nl, NULL, &type, data, &dl);
        if (r == ERROR_NO_MORE_ITEMS) break;
        if (r != ERROR_SUCCESS && r != ERROR_MORE_DATA) break;
        w("  \""); w(name); w("\"");
        wsprintfA(line, " t=%lu l=%lu = ", type, dl); w(line);
        if (type == REG_SZ && r == ERROR_SUCCESS) { data[dl < sizeof(data) ? dl : sizeof(data) - 1] = 0; w((char *)data); }
        else if (r == ERROR_SUCCESS) hexv(data, dl);
        w("\r\n");
    }
    for (i = 0;; i++) {
        DWORD nl = sizeof(name);
        if (RegEnumKeyExA(k, i, name, &nl, NULL, NULL, NULL, NULL) != ERROR_SUCCESS) break;
        if (path[0]) wsprintfA(sub, "%s\\%s", path, name); else lstrcpyA(sub, name);
        dump(root, sub, depth + 1);
    }
    RegCloseKey(k);
}
void __stdcall start(void) {
    char *c = GetCommandLineA(), root[16], key[512], file[260]; int i; HKEY r = NULL;
    if (*c == '"') { c++; while (*c && *c != '"') c++; if (*c) c++; } else while (*c && *c != ' ') c++;
    while (*c == ' ') c++;
    for (i = 0; *c && *c != ' ' && i < 15; ) root[i++] = *c++; root[i] = 0; while (*c == ' ') c++;
    if (*c == '"') { c++; for (i = 0; *c && *c != '"' && i < 511; ) key[i++] = *c++; key[i] = 0; if (*c) c++; }
    else { for (i = 0; *c && *c != ' ' && i < 511; ) key[i++] = *c++; key[i] = 0; }
    while (*c == ' ') c++;
    for (i = 0; *c && *c != ' ' && i < 259; ) file[i++] = *c++; file[i] = 0;
    if (!lstrcmpiA(root, "HKDD")) r = HKEY_DYN_DATA;
    else if (!lstrcmpiA(root, "HKLM")) r = HKEY_LOCAL_MACHINE;
    else if (!lstrcmpiA(root, "HKCC")) r = HKEY_CURRENT_CONFIG;
    out = CreateFileA(file[0] ? file : "C:\\RETRO_AGENT\\REGDUMP.TXT", GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (out == INVALID_HANDLE_VALUE) ExitProcess(3);
    if (!r) { w("bad root\r\n"); CloseHandle(out); ExitProcess(2); }
    dump(r, key, 0);
    w("--end--\r\n");
    CloseHandle(out);
    ExitProcess(0);
}
