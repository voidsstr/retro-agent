/*
 * fxpanel.c - put the 3dfx Control Panel on every box that has a card it
 * serves, at agent start (agent 1.94.0). The decision is
 * agent/shared/fxpanel.h; this is the Win32 half.
 *
 * Called from the gamesync startup thread after its network delay and its
 * modern-host check, right before gs_place_tool_shortcuts() - which then puts
 * the "3dfx Control Panel" desktop shortcut down with the 3dfx logo
 * (3dfxlogo.ico) and claims it, so the end-of-sync sweep keeps it.
 *
 * Source: the share's panel directory (HKLM\Software\RetroAgent\FxPanelPath
 * overrides), which push_3dfxctl.py publishes. Files: 3dfxctl.exe and
 * 3dfxlogo.ico into C:\RETRO_AGENT. Each is copied through a .new file and
 * renamed over the old one, then its size is read back - the post-condition,
 * not CopyFile's return. A running panel holds its exe; that copy fails, is
 * logged, and is retried at the next agent start.
 *
 * Off switch: HKLM\Software\RetroAgent\FxPanel = 0. The outcome of each start
 * is in HKLM\Software\RetroAgent\FxPanelBoot.
 */
#include <windows.h>
#include <stdio.h>
#include <string.h>

#include "../shared/fxpanel.h"
#include "fxpanel.h"
#include "hwextra.h"
#include "log.h"

#define LOG_FXP          "FXPANEL"
#define FXP_KEY          "Software\\RetroAgent"
#define FXP_DEFAULT_SRC  "\\\\192.168.1.122\\files\\Utility\\Retro Automation\\3dfx"
#define FXP_DEST         "C:\\RETRO_AGENT"

static const char *const g_fxp_files[] = { "3dfxctl.exe", "3dfxlogo.ico" };

static DWORD fxp_reg_dword(const char *name, DWORD dflt)
{
    HKEY k;
    DWORD v = dflt, t = 0, n = sizeof(v);
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, FXP_KEY, 0, KEY_READ, &k) != ERROR_SUCCESS)
        return dflt;
    if (RegQueryValueExA(k, name, NULL, &t, (LPBYTE)&v, &n) != ERROR_SUCCESS || t != REG_DWORD)
        v = dflt;
    RegCloseKey(k);
    return v;
}

static void fxp_reg_str(const char *name, char *out, DWORD cap, const char *dflt)
{
    HKEY k;
    DWORD t = 0, n = cap;
    lstrcpynA(out, dflt, (int)cap);
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, FXP_KEY, 0, KEY_READ, &k) != ERROR_SUCCESS)
        return;
    if (RegQueryValueExA(k, name, NULL, &t, (LPBYTE)out, &n) != ERROR_SUCCESS ||
        t != REG_SZ || !out[0])
        lstrcpynA(out, dflt, (int)cap);
    out[cap - 1] = 0;
    RegCloseKey(k);
}

static void fxp_record(const char *text)
{
    HKEY k;
    if (RegCreateKeyExA(HKEY_LOCAL_MACHINE, FXP_KEY, 0, NULL, 0, KEY_WRITE, NULL, &k,
                        NULL) != ERROR_SUCCESS)
        return;
    RegSetValueExA(k, "FxPanelBoot", 0, REG_SZ, (const BYTE *)text,
                   (DWORD)lstrlenA(text) + 1);
    RegCloseKey(k);
}

static int fxp_stat(const char *path, unsigned long long *size, unsigned long long *time)
{
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &a) ||
        (a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
        return 0;
    *size = ((unsigned long long)a.nFileSizeHigh << 32) | a.nFileSizeLow;
    *time = ((unsigned long long)a.ftLastWriteTime.dwHighDateTime << 32) |
            a.ftLastWriteTime.dwLowDateTime;
    return 1;
}

/* 1 copied, 0 already current, -1 failed (reason in `why`). */
static int fxp_sync_file(const char *srcdir, const char *name, char *why, int cap)
{
    char src[MAX_PATH], dst[MAX_PATH], tmp[MAX_PATH];
    unsigned long long ss, st, ds = 0, dt = 0, gs, gt;
    int have;

    _snprintf(src, sizeof(src) - 1, "%s\\%s", srcdir, name); src[sizeof(src) - 1] = 0;
    _snprintf(dst, sizeof(dst) - 1, "%s\\%s", FXP_DEST, name); dst[sizeof(dst) - 1] = 0;
    _snprintf(tmp, sizeof(tmp) - 1, "%s.new", dst);           tmp[sizeof(tmp) - 1] = 0;

    if (!fxp_stat(src, &ss, &st)) {
        _snprintf(why, cap - 1, "%s: share copy unreadable (%lu)", name,
                  (unsigned long)GetLastError());
        why[cap - 1] = 0;
        return -1;
    }
    have = fxp_stat(dst, &ds, &dt);
    if (!fxpanel_need_copy(have, ds, dt, ss, st))
        return 0;

    DeleteFileA(tmp);
    if (!CopyFileA(src, tmp, FALSE)) {
        _snprintf(why, cap - 1, "%s: copy from the share failed (%lu)", name,
                  (unsigned long)GetLastError());
        why[cap - 1] = 0;
        return -1;
    }
    if (!fxp_stat(tmp, &gs, &gt) || gs != ss) {
        DeleteFileA(tmp);
        _snprintf(why, cap - 1, "%s: the copy is %I64u bytes, the share's %I64u", name,
                  gs, ss);
        why[cap - 1] = 0;
        return -1;
    }
    if (!MoveFileExA(tmp, dst, MOVEFILE_REPLACE_EXISTING)) {
        DWORD e = GetLastError();
        DeleteFileA(tmp);
        _snprintf(why, cap - 1, "%s: could not replace the old copy (%lu%s)", name,
                  (unsigned long)e,
                  e == ERROR_ACCESS_DENIED || e == ERROR_SHARING_VIOLATION
                      ? " - the panel is probably open; next start" : "");
        why[cap - 1] = 0;
        return -1;
    }
    if (!fxp_stat(dst, &gs, &gt) || gs != ss) {
        _snprintf(why, cap - 1, "%s: not in place after the rename", name);
        why[cap - 1] = 0;
        return -1;
    }
    return 1;
}

void fxpanel_ensure(void)
{
    OSVERSIONINFOA vi;
    char card[300], srcdir[MAX_PATH], result[512], why[300];
    int decision, i, copied = 0, failed = 0;

    ZeroMemory(&vi, sizeof(vi));
    vi.dwOSVersionInfoSize = sizeof(vi);
    GetVersionExA(&vi);                 /* only NT 5.x is wanted - no shim issue */

    card[0] = 0;
    decision = fxpanel_decide(fxp_reg_dword("FxPanel", 1) == 0,
                              vi.dwPlatformId == VER_PLATFORM_WIN32_NT,
                              (unsigned)vi.dwMajorVersion,
                              (vi.dwPlatformId == VER_PLATFORM_WIN32_NT &&
                               vi.dwMajorVersion == 5)
                                  ? hwextra_3dfx_panel_card(card, sizeof(card)) : 0);
    switch (decision) {
    case FXP_OFF:
        fxp_record("off (HKLM\\Software\\RetroAgent\\FxPanel=0)");
        log_msg(LOG_FXP, "switched off (FxPanel=0) - not deploying the 3dfx Control Panel");
        return;
    case FXP_NOT_OS:
        fxp_record("not applicable: not Windows 2000/XP");
        return;                          /* every 9x/Win7 box: quiet */
    case FXP_NO_CARD:
        fxp_record("not applicable: no present Banshee/Voodoo3/4/5 with a driver");
        return;
    }

    fxp_reg_str("FxPanelPath", srcdir, sizeof(srcdir), FXP_DEFAULT_SRC);
    result[0] = 0;
    for (i = 0; i < (int)(sizeof(g_fxp_files) / sizeof(g_fxp_files[0])); i++) {
        int r;
        why[0] = 0;
        r = fxp_sync_file(srcdir, g_fxp_files[i], why, sizeof(why));
        if (r > 0) {
            copied++;
            log_msg(LOG_FXP, "%s deployed to %s from %s", g_fxp_files[i], FXP_DEST, srcdir);
        } else if (r < 0) {
            failed++;
            log_msg(LOG_FXP, "FAILED: %s", why);
            _snprintf(result + lstrlenA(result), sizeof(result) - lstrlenA(result) - 1,
                      "%s; ", why);
        }
    }
    result[sizeof(result) - 1] = 0;
    {
        char rec[700];
        _snprintf(rec, sizeof(rec) - 1, "%s: %d copied, %d current, %d failed%s%s (card %s)",
                  failed ? "PARTIAL" : "ok", copied,
                  (int)(sizeof(g_fxp_files) / sizeof(g_fxp_files[0])) - copied - failed,
                  failed, failed ? " - " : "", result, card);
        rec[sizeof(rec) - 1] = 0;
        fxp_record(rec);
        if (copied || failed)
            log_msg(LOG_FXP, "%s", rec);
    }
}
