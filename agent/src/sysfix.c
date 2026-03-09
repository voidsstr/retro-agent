/*
 * sysfix.c - Standard system fixes for retro OS compatibility
 *
 * SYSFIX [check|apply]
 *   check (default) - report what needs fixing without changing anything
 *   apply           - apply all needed fixes
 *
 * Each fix targets a known Win9x/ME issue that causes instability.
 * Returns JSON with status of each fix checked.
 */

#include "handlers.h"
#include "protocol.h"
#include "util.h"
#include "log.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define LOG_SYSFIX "SYSFIX"

/* Status strings */
#define FIX_OK       "ok"        /* already correct, no action needed */
#define FIX_NEEDED   "needed"    /* needs fixing (check mode) */
#define FIX_APPLIED  "applied"   /* fix was applied (apply mode) */
#define FIX_FAILED   "failed"    /* fix attempted but failed */
#define FIX_SKIPPED  "skipped"   /* not applicable to this system */

/* ---- helpers ---- */

static int is_win9x(void)
{
    OSVERSIONINFOA osvi;
    osvi.dwOSVersionInfoSize = sizeof(osvi);
    GetVersionExA(&osvi);
    return (osvi.dwPlatformId == VER_PLATFORM_WIN32_WINDOWS);
}

static DWORD get_ram_mb(void)
{
    MEMORYSTATUS ms;
    ms.dwLength = sizeof(ms);
    GlobalMemoryStatus(&ms);
    return (DWORD)(ms.dwTotalPhys / (1024 * 1024));
}

static void get_system_ini_path(char *buf, int bufsize)
{
    char windir[MAX_PATH];
    GetWindowsDirectoryA(windir, sizeof(windir));
    _snprintf(buf, bufsize, "%s\\SYSTEM.INI", windir);
    buf[bufsize - 1] = '\0';
}

/* ---- fix: vcache ---- */

/*
 * Win9x with >512MB RAM: if [vcache] MaxFileCache is not limited,
 * VxD address space exhaustion causes Windows Protection Errors
 * (especially during video driver init — NVCORE.VXD, etc).
 *
 * Fix: set MaxFileCache=262144 (256MB limit in KB).
 */
static void fix_vcache(json_t *j, int apply)
{
    char ini_path[MAX_PATH];
    char current[64];
    DWORD ram_mb;
    DWORD current_val;
    char detail[256];

    json_object_start(j);
    json_kv_str(j, "id", "vcache");
    json_kv_str(j, "description", "Limit vcache for >512MB RAM (prevents VxD address space exhaustion)");

    if (!is_win9x()) {
        json_kv_str(j, "status", FIX_SKIPPED);
        json_kv_str(j, "detail", "Not Win9x, vcache fix not applicable");
        json_object_end(j);
        return;
    }

    ram_mb = get_ram_mb();
    if (ram_mb <= 512) {
        json_kv_str(j, "status", FIX_SKIPPED);
        _snprintf(detail, sizeof(detail), "RAM is %luMB (<=512MB), vcache limit not needed",
                  (unsigned long)ram_mb);
        json_kv_str(j, "detail", detail);
        json_object_end(j);
        return;
    }

    get_system_ini_path(ini_path, sizeof(ini_path));

    /* Read current value */
    GetPrivateProfileStringA("vcache", "MaxFileCache", "", current, sizeof(current), ini_path);

    if (current[0] != '\0') {
        current_val = (DWORD)atol(current);
        /* Check if the existing value is reasonable (<=512MB = 524288KB) */
        if (current_val > 0 && current_val <= 524288) {
            _snprintf(detail, sizeof(detail),
                      "MaxFileCache=%lu already set (RAM=%luMB)",
                      (unsigned long)current_val, (unsigned long)ram_mb);
            json_kv_str(j, "status", FIX_OK);
            json_kv_str(j, "detail", detail);
            json_object_end(j);
            return;
        }
        /* Value is set but too large */
        _snprintf(detail, sizeof(detail),
                  "MaxFileCache=%lu is too large (RAM=%luMB)",
                  (unsigned long)current_val, (unsigned long)ram_mb);
    } else {
        _snprintf(detail, sizeof(detail),
                  "MaxFileCache not set (RAM=%luMB, needs limit)",
                  (unsigned long)ram_mb);
    }

    if (!apply) {
        json_kv_str(j, "status", FIX_NEEDED);
        json_kv_str(j, "detail", detail);
        json_object_end(j);
        return;
    }

    /* Apply: write MaxFileCache=262144 (256MB in KB) */
    log_msg(LOG_SYSFIX, "Applying vcache fix: MaxFileCache=262144 in %s", ini_path);
    if (WritePrivateProfileStringA("vcache", "MaxFileCache", "262144", ini_path)) {
        _snprintf(detail, sizeof(detail),
                  "Set MaxFileCache=262144 in %s (RAM=%luMB, reboot required)",
                  ini_path, (unsigned long)ram_mb);
        json_kv_str(j, "status", FIX_APPLIED);
        json_kv_str(j, "detail", detail);
    } else {
        _snprintf(detail, sizeof(detail),
                  "WritePrivateProfileString failed: error %lu",
                  (unsigned long)GetLastError());
        json_kv_str(j, "status", FIX_FAILED);
        json_kv_str(j, "detail", detail);
    }

    json_object_end(j);
}

/* ---- fix: contig_swap ---- */

/*
 * Win9x: ConservativeSwapfileUsage=1 in [386Enh] prevents Windows
 * from aggressively paging to the swap file, improving responsiveness
 * on machines with plenty of RAM.
 */
static void fix_contig_swap(json_t *j, int apply)
{
    char ini_path[MAX_PATH];
    char current[64];
    char detail[256];

    json_object_start(j);
    json_kv_str(j, "id", "conservative_swap");
    json_kv_str(j, "description", "Set ConservativeSwapfileUsage=1 to reduce unnecessary paging");

    if (!is_win9x()) {
        json_kv_str(j, "status", FIX_SKIPPED);
        json_kv_str(j, "detail", "Not Win9x");
        json_object_end(j);
        return;
    }

    get_system_ini_path(ini_path, sizeof(ini_path));
    GetPrivateProfileStringA("386Enh", "ConservativeSwapfileUsage", "", current, sizeof(current), ini_path);

    if (current[0] != '\0' && atoi(current) == 1) {
        json_kv_str(j, "status", FIX_OK);
        json_kv_str(j, "detail", "ConservativeSwapfileUsage=1 already set");
        json_object_end(j);
        return;
    }

    if (!apply) {
        json_kv_str(j, "status", FIX_NEEDED);
        _snprintf(detail, sizeof(detail), "ConservativeSwapfileUsage not set to 1");
        json_kv_str(j, "detail", detail);
        json_object_end(j);
        return;
    }

    log_msg(LOG_SYSFIX, "Applying conservative swap fix in %s", ini_path);
    if (WritePrivateProfileStringA("386Enh", "ConservativeSwapfileUsage", "1", ini_path)) {
        json_kv_str(j, "status", FIX_APPLIED);
        json_kv_str(j, "detail", "Set ConservativeSwapfileUsage=1 (reboot required)");
    } else {
        json_kv_str(j, "status", FIX_FAILED);
        _snprintf(detail, sizeof(detail), "Write failed: error %lu", (unsigned long)GetLastError());
        json_kv_str(j, "detail", detail);
    }

    json_object_end(j);
}

/* ---- fix: udma ---- */

/*
 * Win98SE: enable UDMA/DMA on IDE channels via NOIDE registry key.
 * Without this, IDE transfers may fall back to PIO mode, which is
 * extremely slow and causes high CPU usage during disk I/O.
 *
 * Sets DMACurrentlyUsed=1 for primary and secondary IDE masters.
 */
static void fix_udma(json_t *j, int apply)
{
    static const char *ide_paths[] = {
        "System\\CurrentControlSet\\Services\\VxD\\IOS\\NOIDE\\0000",
        "System\\CurrentControlSet\\Services\\VxD\\IOS\\NOIDE\\0001",
        NULL
    };
    int any_needed = 0;
    int any_failed = 0;
    int all_ok = 1;
    int i;

    json_object_start(j);
    json_kv_str(j, "id", "udma");
    json_kv_str(j, "description", "Enable DMA on IDE channels (prevents PIO mode fallback)");

    if (!is_win9x()) {
        json_kv_str(j, "status", FIX_SKIPPED);
        json_kv_str(j, "detail", "Not Win9x");
        json_object_end(j);
        return;
    }

    for (i = 0; ide_paths[i]; i++) {
        HKEY hk;
        DWORD val, size, type;

        if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, ide_paths[i], 0, KEY_READ, &hk) != ERROR_SUCCESS) {
            /* Key doesn't exist — no IDE channel at this index */
            continue;
        }

        all_ok = 0;  /* found at least one IDE channel to check */
        size = sizeof(val);
        if (RegQueryValueExA(hk, "DMACurrentlyUsed", NULL, &type, (BYTE *)&val, &size) == ERROR_SUCCESS
            && type == REG_BINARY && val == 1) {
            RegCloseKey(hk);
            continue;  /* already enabled */
        }
        RegCloseKey(hk);

        any_needed = 1;
        if (!apply) continue;

        /* Apply: set DMACurrentlyUsed=01 (REG_BINARY, 1 byte) */
        if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, ide_paths[i], 0, KEY_WRITE, &hk) == ERROR_SUCCESS) {
            BYTE dma_on = 1;
            if (RegSetValueExA(hk, "DMACurrentlyUsed", 0, REG_BINARY, &dma_on, 1) == ERROR_SUCCESS) {
                log_msg(LOG_SYSFIX, "Enabled DMA on %s", ide_paths[i]);
            } else {
                any_failed = 1;
            }
            RegCloseKey(hk);
        } else {
            any_failed = 1;
        }
    }

    if (all_ok && !any_needed) {
        json_kv_str(j, "status", FIX_SKIPPED);
        json_kv_str(j, "detail", "No IDE NOIDE keys found (no IDE channels to configure)");
    } else if (!any_needed) {
        json_kv_str(j, "status", FIX_OK);
        json_kv_str(j, "detail", "DMA already enabled on all IDE channels");
    } else if (!apply) {
        json_kv_str(j, "status", FIX_NEEDED);
        json_kv_str(j, "detail", "One or more IDE channels have DMA disabled");
    } else if (any_failed) {
        json_kv_str(j, "status", FIX_FAILED);
        json_kv_str(j, "detail", "Some IDE channels could not be updated");
    } else {
        json_kv_str(j, "status", FIX_APPLIED);
        json_kv_str(j, "detail", "DMA enabled on IDE channels (reboot required)");
    }

    json_object_end(j);
}

/* ---- fix: autologon ---- */

/*
 * Win9x/XP: Set AutoLogon=1 in Network\Logon (Win9x) or Winlogon (NT)
 * to suppress the network login prompt at boot.
 * Without this, Windows shows "Enter Network Password" on every boot,
 * requiring manual interaction before the desktop (and agent) can start.
 */
static void fix_autologon(json_t *j, int apply)
{
    char detail[256];
    HKEY hKey;
    DWORD size, type;
    int is_9x = is_win9x();

    json_object_start(j);
    json_kv_str(j, "id", "autologon");
    json_kv_str(j, "description",
                "Suppress network login prompt at boot (auto-logon)");

    if (is_9x) {
        /*
         * Win9x login prompt suppression requires TWO things:
         *
         * 1. PrimaryProvider="MSNP32" in Network\Logon — tells Win98 to
         *    route boot logon through Microsoft Network client (MSNP32).
         *    Without this, Win98 uses "Windows Logon" which doesn't
         *    establish a VREDIR/SMB session, and all share access fails
         *    with error 3787 ("You must log on") or 1222 ("Network not
         *    available").
         *
         * 2. AutoLogon=1 — uses cached .pwl credentials to skip the
         *    MSNP32 "Enter Network Password" dialog entirely.
         *
         * Also: MustBeValidated=0 (no domain validation needed in
         * workgroup), LMLogon=0 (no domain auth), username="admin".
         *
         * autologon.exe in RunServices is a secondary fallback that
         * dismisses the dialog if it appears despite AutoLogon=1.
         */
        DWORD autologon_val = 0;
        char provider[64] = "";
        int autologon_ok = 0;
        int provider_ok = 0;

        /* Check current state */
        if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "Network\\Logon", 0,
                          KEY_READ, &hKey) == ERROR_SUCCESS) {
            size = sizeof(autologon_val);
            if (RegQueryValueExA(hKey, "AutoLogon", NULL, &type,
                                 (BYTE *)&autologon_val, &size) == ERROR_SUCCESS
                && type == REG_DWORD && autologon_val == 1) {
                autologon_ok = 1;
            }
            size = sizeof(provider);
            if (RegQueryValueExA(hKey, "PrimaryProvider", NULL, &type,
                                 (BYTE *)provider, &size) == ERROR_SUCCESS
                && type == REG_SZ && _stricmp(provider, "MSNP32") == 0) {
                provider_ok = 1;
            }
            RegCloseKey(hKey);
        }

        if (autologon_ok && provider_ok) {
            json_kv_str(j, "status", FIX_OK);
            json_kv_str(j, "detail",
                        "AutoLogon=1 and PrimaryProvider=MSNP32 set");
            json_object_end(j);
            return;
        }

        if (!apply) {
            json_kv_str(j, "status", FIX_NEEDED);
            if (!autologon_ok && !provider_ok)
                json_kv_str(j, "detail",
                    "AutoLogon and PrimaryProvider both need setting");
            else if (!provider_ok)
                json_kv_str(j, "detail",
                    "PrimaryProvider not set to MSNP32 (SMB shares won't work)");
            else
                json_kv_str(j, "detail",
                    "AutoLogon not enabled, boot will show login prompt");
            json_object_end(j);
            return;
        }

        /* Apply: set PrimaryProvider, AutoLogon=1, LMLogon=0 */
        {
            BYTE lm_off[4] = {0, 0, 0, 0};
            DWORD autologon_on = 1;
            DWORD must_validate = 0;
            if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "Network\\Logon", 0,
                              KEY_WRITE, &hKey) == ERROR_SUCCESS) {
                RegSetValueExA(hKey, "PrimaryProvider", 0, REG_SZ,
                               (BYTE *)"MSNP32", 7);
                RegSetValueExA(hKey, "AutoLogon", 0, REG_DWORD,
                               (BYTE *)&autologon_on, sizeof(autologon_on));
                RegSetValueExA(hKey, "LMLogon", 0, REG_BINARY, lm_off, sizeof(lm_off));
                RegSetValueExA(hKey, "MustBeValidated", 0, REG_DWORD,
                               (BYTE *)&must_validate, sizeof(must_validate));
                RegCloseKey(hKey);
                log_msg(LOG_SYSFIX, "Applied autologon fix (Win9x): "
                        "PrimaryProvider=MSNP32, AutoLogon=1");
                json_kv_str(j, "status", FIX_APPLIED);
                json_kv_str(j, "detail",
                    "Set PrimaryProvider=MSNP32 and AutoLogon=1 "
                    "(effective next boot)");
            } else {
                _snprintf(detail, sizeof(detail), "RegOpenKeyEx write failed: %lu",
                          (unsigned long)GetLastError());
                json_kv_str(j, "status", FIX_FAILED);
                json_kv_str(j, "detail", detail);
            }
        }

    } else {
        /* NT (XP/2000): check HKLM\...\Winlogon for AutoAdminLogon.
         * Requires DefaultUserName + DefaultPassword to actually work;
         * without them XP shows "username/domain must be correct" error
         * on every boot. */
        char autologon[16] = "";
        char def_user[128] = "";
        char def_pass[128] = "";

        if (RegOpenKeyExA(HKEY_LOCAL_MACHINE,
                          "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Winlogon",
                          0, KEY_READ, &hKey) != ERROR_SUCCESS) {
            json_kv_str(j, "status", FIX_SKIPPED);
            json_kv_str(j, "detail", "Winlogon key not found");
            json_object_end(j);
            return;
        }

        size = sizeof(autologon);
        RegQueryValueExA(hKey, "AutoAdminLogon", NULL, &type,
                         (BYTE *)autologon, &size);
        size = sizeof(def_user);
        RegQueryValueExA(hKey, "DefaultUserName", NULL, &type,
                         (BYTE *)def_user, &size);
        size = sizeof(def_pass);
        RegQueryValueExA(hKey, "DefaultPassword", NULL, &type,
                         (BYTE *)def_pass, &size);
        RegCloseKey(hKey);

        if (autologon[0] == '1' && def_user[0] && def_pass[0]) {
            json_kv_str(j, "status", FIX_OK);
            _snprintf(detail, sizeof(detail),
                      "AutoAdminLogon=1, user=%s", def_user);
            json_kv_str(j, "detail", detail);
            json_object_end(j);
            return;
        }

        if (!apply) {
            json_kv_str(j, "status", FIX_NEEDED);
            if (autologon[0] == '1' && !def_pass[0])
                json_kv_str(j, "detail",
                    "AutoAdminLogon=1 but DefaultPassword missing");
            else
                json_kv_str(j, "detail",
                    "AutoAdminLogon not fully configured");
            json_object_end(j);
            return;
        }

        /* Apply: set AutoAdminLogon + DefaultUserName + DefaultPassword.
         * Use "Administrator" as default username if none is set. */
        if (RegOpenKeyExA(HKEY_LOCAL_MACHINE,
                          "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Winlogon",
                          0, KEY_WRITE, &hKey) == ERROR_SUCCESS) {
            RegSetValueExA(hKey, "AutoAdminLogon", 0, REG_SZ,
                           (BYTE *)"1", 2);
            if (!def_user[0]) {
                RegSetValueExA(hKey, "DefaultUserName", 0, REG_SZ,
                               (BYTE *)"admin",
                               (DWORD)strlen("admin") + 1);
                log_msg(LOG_SYSFIX, "Set DefaultUserName=admin");
            }
            if (!def_pass[0]) {
                RegSetValueExA(hKey, "DefaultPassword", 0, REG_SZ,
                               (BYTE *)"password",
                               (DWORD)strlen("password") + 1);
                log_msg(LOG_SYSFIX, "Set DefaultPassword");
            }
            RegCloseKey(hKey);
            log_msg(LOG_SYSFIX, "Applied autologon fix (NT)");
            json_kv_str(j, "status", FIX_APPLIED);
            json_kv_str(j, "detail",
                "Set AutoAdminLogon=1 with credentials (effective next boot)");
        } else {
            json_kv_str(j, "status", FIX_FAILED);
            _snprintf(detail, sizeof(detail), "RegOpenKeyEx write failed: %lu",
                      (unsigned long)GetLastError());
            json_kv_str(j, "detail", detail);
        }
    }

    json_object_end(j);
}

/* ---- main handler ---- */

typedef void (*fix_func_t)(json_t *j, int apply);

static const struct {
    const char *id;
    fix_func_t  func;
} all_fixes[] = {
    { "vcache",            fix_vcache },
    { "conservative_swap", fix_contig_swap },
    { "udma",              fix_udma },
    { "autologon",         fix_autologon },
    { NULL, NULL }
};

/*
 * sysfix_apply_startup - Called at agent startup (no socket).
 * Silently applies all fixes. Each fix logs what it does.
 * Uses a throwaway JSON builder since fix functions expect one,
 * but the output is discarded — only log messages matter.
 */
void sysfix_apply_startup(void)
{
    json_t j;
    int i;

    log_msg(LOG_SYSFIX, "Startup auto-apply running");

    for (i = 0; all_fixes[i].func; i++) {
        json_init(&j);
        json_array_start(&j);
        all_fixes[i].func(&j, 1);  /* apply=1 */
        json_array_end(&j);
        json_free(&j);
    }

    log_msg(LOG_SYSFIX, "Startup auto-apply done");
}

void handle_sysfix(SOCKET sock, const char *args)
{
    json_t j;
    char *result;
    int apply = 0;
    int i;

    if (args && _stricmp(args, "apply") == 0) {
        apply = 1;
    }

    log_msg(LOG_SYSFIX, "SYSFIX %s", apply ? "apply" : "check");

    json_init(&j);
    json_object_start(&j);

    json_kv_str(&j, "mode", apply ? "apply" : "check");
    json_kv_bool(&j, "is_win9x", is_win9x());
    json_kv_uint(&j, "ram_mb", get_ram_mb());

    json_key(&j, "fixes");
    json_array_start(&j);

    for (i = 0; all_fixes[i].func; i++) {
        all_fixes[i].func(&j, apply);
    }

    json_array_end(&j);
    json_object_end(&j);

    result = json_finish(&j);
    send_text_response(sock, result);
    json_free(&j);
}
