/*
 * xpactivate.c - Windows XP activation bypass
 *
 * XPACTIVATE [check|apply]
 *   check (default) - report activation status without changing anything
 *   apply           - apply all activation bypass steps
 *
 * Applies the OOBETimer reset, sets Activation Required=0,
 * removes WGA enforcement, and denies SYSTEM write access to
 * the WPAEvents key so Windows cannot revert the bypass.
 *
 * CRITICAL: Never make wpa.dbl read-only. Windows needs write
 * access to it at all times or it causes "system files missing
 * or damaged" errors that block login in ALL modes.
 *
 * Returns JSON with status of each step.
 */

#include "handlers.h"
#include "protocol.h"
#include "util.h"
#include "log.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <aclapi.h>
#include <sddl.h>

#define LOG_XPA "XPACTIVATE"

/* Status strings (same as sysfix.c) */
#define FIX_OK       "ok"
#define FIX_NEEDED   "needed"
#define FIX_APPLIED  "applied"
#define FIX_FAILED   "failed"
#define FIX_SKIPPED  "skipped"

/* ---- helpers ---- */

static int is_winnt(void)
{
    OSVERSIONINFOA osvi;
    osvi.dwOSVersionInfoSize = sizeof(osvi);
    GetVersionExA(&osvi);
    return (osvi.dwPlatformId == VER_PLATFORM_WIN32_NT);
}

static int is_winxp(void)
{
    OSVERSIONINFOA osvi;
    osvi.dwOSVersionInfoSize = sizeof(osvi);
    GetVersionExA(&osvi);
    return (osvi.dwPlatformId == VER_PLATFORM_WIN32_NT &&
            osvi.dwMajorVersion == 5 && osvi.dwMinorVersion == 1);
}

/* ---- step 1: OOBETimer registry value ---- */

/*
 * The OOBETimer value in WPAEvents controls the activation grace period.
 * Setting it to FF D5 71 D6 8B 6A 8D 6F D5 33 93 FD signals
 * "not yet activated, timer reset" across XP SP1/SP2/SP3.
 */
static void step_oobe_timer(json_t *j, int apply)
{
    static const BYTE target_val[] = {
        0xFF, 0xD5, 0x71, 0xD6, 0x8B, 0x6A,
        0x8D, 0x6F, 0xD5, 0x33, 0x93, 0xFD
    };
    const char *key_path = "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\WPAEvents";
    HKEY hKey;
    BYTE current[32];
    DWORD size, type;
    char detail[256];

    json_object_start(j);
    json_kv_str(j, "id", "oobe_timer");
    json_kv_str(j, "description", "Reset OOBETimer activation value");

    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, key_path, 0, KEY_READ, &hKey) != ERROR_SUCCESS) {
        json_kv_str(j, "status", FIX_SKIPPED);
        json_kv_str(j, "detail", "WPAEvents key not found");
        json_object_end(j);
        return;
    }

    size = sizeof(current);
    memset(current, 0, sizeof(current));
    if (RegQueryValueExA(hKey, "OOBETimer", NULL, &type, current, &size) == ERROR_SUCCESS
        && size == sizeof(target_val)
        && memcmp(current, target_val, sizeof(target_val)) == 0) {
        json_kv_str(j, "status", FIX_OK);
        json_kv_str(j, "detail", "OOBETimer already set to bypass value");
        RegCloseKey(hKey);
        json_object_end(j);
        return;
    }
    RegCloseKey(hKey);

    if (!apply) {
        json_kv_str(j, "status", FIX_NEEDED);
        json_kv_str(j, "detail", "OOBETimer needs reset");
        json_object_end(j);
        return;
    }

    /* Apply: write the bypass value */
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, key_path, 0, KEY_WRITE, &hKey) == ERROR_SUCCESS) {
        if (RegSetValueExA(hKey, "OOBETimer", 0, REG_BINARY,
                           target_val, sizeof(target_val)) == ERROR_SUCCESS) {
            log_msg(LOG_XPA, "Set OOBETimer bypass value");
            json_kv_str(j, "status", FIX_APPLIED);
            json_kv_str(j, "detail", "OOBETimer set to bypass value");
        } else {
            _snprintf(detail, sizeof(detail), "RegSetValueEx failed: %lu",
                      (unsigned long)GetLastError());
            json_kv_str(j, "status", FIX_FAILED);
            json_kv_str(j, "detail", detail);
        }
        RegCloseKey(hKey);
    } else {
        _snprintf(detail, sizeof(detail), "Cannot open WPAEvents for write: %lu",
                  (unsigned long)GetLastError());
        json_kv_str(j, "status", FIX_FAILED);
        json_kv_str(j, "detail", detail);
    }

    json_object_end(j);
}

/* ---- step 2: Run SetupOobeBnk ---- */

/*
 * rundll32.exe syssetup,SetupOobeBnk resets the activation grace period.
 * Must be called AFTER setting OOBETimer — the registry value tells
 * SetupOobeBnk what state to initialize.
 */
static void step_setup_oobe_bnk(json_t *j, int apply)
{
    char detail[256];
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    char cmd[] = "rundll32.exe syssetup,SetupOobeBnk";
    DWORD exit_code;

    json_object_start(j);
    json_kv_str(j, "id", "setup_oobe_bnk");
    json_kv_str(j, "description", "Run SetupOobeBnk to reset activation grace period");

    if (!apply) {
        json_kv_str(j, "status", FIX_NEEDED);
        json_kv_str(j, "detail", "SetupOobeBnk needs to run after OOBETimer is set");
        json_object_end(j);
        return;
    }

    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    memset(&pi, 0, sizeof(pi));

    if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE,
                        CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        _snprintf(detail, sizeof(detail), "CreateProcess failed: %lu",
                  (unsigned long)GetLastError());
        json_kv_str(j, "status", FIX_FAILED);
        json_kv_str(j, "detail", detail);
        json_object_end(j);
        return;
    }

    /* Wait up to 30 seconds */
    WaitForSingleObject(pi.hProcess, 30000);
    GetExitCodeProcess(pi.hProcess, &exit_code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    log_msg(LOG_XPA, "SetupOobeBnk exited with code %lu", (unsigned long)exit_code);
    _snprintf(detail, sizeof(detail), "SetupOobeBnk completed (exit code %lu)",
              (unsigned long)exit_code);
    json_kv_str(j, "status", FIX_APPLIED);
    json_kv_str(j, "detail", detail);

    json_object_end(j);
}

/* ---- step 3: Activation Required registry flag ---- */

/*
 * Set "Activation Required"=0 in Winlogon to tell the login process
 * not to enforce activation. Also set RegDone="1" in the NT
 * CurrentVersion key to mark registration as complete.
 */
static void step_activation_required(json_t *j, int apply)
{
    const char *winlogon_path = "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Winlogon";
    const char *ntcv_path = "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
    HKEY hKey;
    DWORD val, size, type;
    char regdone[16] = "";
    char detail[256];
    int act_ok = 0, reg_ok = 0;

    json_object_start(j);
    json_kv_str(j, "id", "activation_required");
    json_kv_str(j, "description", "Disable activation enforcement in Winlogon");

    /* Check Activation Required */
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, winlogon_path, 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        size = sizeof(val);
        if (RegQueryValueExA(hKey, "Activation Required", NULL, &type,
                             (BYTE *)&val, &size) == ERROR_SUCCESS
            && type == REG_DWORD && val == 0) {
            act_ok = 1;
        }
        RegCloseKey(hKey);
    }

    /* Check RegDone */
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, ntcv_path, 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        size = sizeof(regdone);
        if (RegQueryValueExA(hKey, "RegDone", NULL, &type,
                             (BYTE *)regdone, &size) == ERROR_SUCCESS
            && type == REG_SZ && regdone[0] == '1') {
            reg_ok = 1;
        }
        RegCloseKey(hKey);
    }

    if (act_ok && reg_ok) {
        json_kv_str(j, "status", FIX_OK);
        json_kv_str(j, "detail", "Activation Required=0 and RegDone=1 already set");
        json_object_end(j);
        return;
    }

    if (!apply) {
        json_kv_str(j, "status", FIX_NEEDED);
        json_kv_str(j, "detail", "Activation enforcement flags need setting");
        json_object_end(j);
        return;
    }

    /* Apply */
    {
        DWORD zero = 0;
        int any_failed = 0;

        if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, winlogon_path, 0, KEY_WRITE, &hKey) == ERROR_SUCCESS) {
            if (RegSetValueExA(hKey, "Activation Required", 0, REG_DWORD,
                               (BYTE *)&zero, sizeof(zero)) != ERROR_SUCCESS)
                any_failed = 1;
            RegCloseKey(hKey);
        } else {
            any_failed = 1;
        }

        if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, ntcv_path, 0, KEY_WRITE, &hKey) == ERROR_SUCCESS) {
            if (RegSetValueExA(hKey, "RegDone", 0, REG_SZ,
                               (BYTE *)"1", 2) != ERROR_SUCCESS)
                any_failed = 1;
            RegCloseKey(hKey);
        } else {
            any_failed = 1;
        }

        if (any_failed) {
            _snprintf(detail, sizeof(detail), "Some registry writes failed: %lu",
                      (unsigned long)GetLastError());
            json_kv_str(j, "status", FIX_FAILED);
            json_kv_str(j, "detail", detail);
        } else {
            log_msg(LOG_XPA, "Set Activation Required=0, RegDone=1");
            json_kv_str(j, "status", FIX_APPLIED);
            json_kv_str(j, "detail", "Set Activation Required=0 and RegDone=1");
        }
    }

    json_object_end(j);
}

/* ---- step 4: Remove WGA enforcement ---- */

/*
 * WGA (Windows Genuine Advantage) adds a Winlogon notification DLL
 * that blocks login on non-activated systems. Remove the notify key
 * and delete the enforcement DLLs.
 */
static void step_remove_wga(json_t *j, int apply)
{
    const char *wga_key = "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Winlogon\\Notify\\WgaLogon";
    char sys_dir[MAX_PATH];
    char path[MAX_PATH];
    HKEY hKey;
    int wga_key_exists = 0;
    int files_exist = 0;
    char detail[512];

    json_object_start(j);
    json_kv_str(j, "id", "remove_wga");
    json_kv_str(j, "description", "Remove WGA login enforcement (WgaLogon, WgaTray, LegitCheckControl)");

    GetSystemDirectoryA(sys_dir, sizeof(sys_dir));

    /* Check if WGA notify key exists */
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, wga_key, 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        wga_key_exists = 1;
        RegCloseKey(hKey);
    }

    /* Check if WGA files exist */
    _snprintf(path, sizeof(path), "%s\\WgaLogon.dll", sys_dir);
    if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) files_exist = 1;
    _snprintf(path, sizeof(path), "%s\\WgaTray.exe", sys_dir);
    if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) files_exist = 1;
    _snprintf(path, sizeof(path), "%s\\LegitCheckControl.dll", sys_dir);
    if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) files_exist = 1;

    if (!wga_key_exists && !files_exist) {
        json_kv_str(j, "status", FIX_OK);
        json_kv_str(j, "detail", "WGA not installed (no notify key, no WGA files)");
        json_object_end(j);
        return;
    }

    if (!apply) {
        json_kv_str(j, "status", FIX_NEEDED);
        _snprintf(detail, sizeof(detail), "WGA present (key=%s, files=%s)",
                  wga_key_exists ? "yes" : "no", files_exist ? "yes" : "no");
        json_kv_str(j, "detail", detail);
        json_object_end(j);
        return;
    }

    /* Apply: delete key and files */
    detail[0] = '\0';

    if (wga_key_exists) {
        if (RegDeleteKeyA(HKEY_LOCAL_MACHINE, wga_key) == ERROR_SUCCESS) {
            log_msg(LOG_XPA, "Deleted WgaLogon notify key");
            strcat(detail, "Deleted WgaLogon notify key. ");
        } else {
            strcat(detail, "Failed to delete WgaLogon key. ");
        }
    }

    _snprintf(path, sizeof(path), "%s\\WgaLogon.dll", sys_dir);
    DeleteFileA(path);
    _snprintf(path, sizeof(path), "%s\\WgaTray.exe", sys_dir);
    DeleteFileA(path);
    _snprintf(path, sizeof(path), "%s\\LegitCheckControl.dll", sys_dir);
    DeleteFileA(path);
    strcat(detail, "Deleted WGA files.");

    log_msg(LOG_XPA, "Removed WGA enforcement files");
    json_kv_str(j, "status", FIX_APPLIED);
    json_kv_str(j, "detail", detail);

    json_object_end(j);
}

/* ---- step 5: Deny SYSTEM write to WPAEvents ---- */

/*
 * After setting OOBETimer, deny SYSTEM Full Control on the WPAEvents
 * registry key so Windows cannot revert the bypass value. This is
 * the critical step that makes the bypass persistent across reboots.
 *
 * Uses SetSecurityInfo to add a Deny ACE for SYSTEM on the key.
 */
static void step_deny_system_wpa(json_t *j, int apply)
{
    const char *key_path = "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\WPAEvents";
    HKEY hKey;
    char detail[256];
    PSID system_sid = NULL;
    SID_IDENTIFIER_AUTHORITY nt_auth = SECURITY_NT_AUTHORITY;
    EXPLICIT_ACCESSA ea;
    PACL old_dacl = NULL, new_dacl = NULL;
    PSECURITY_DESCRIPTOR psd = NULL;
    DWORD rc;

    json_object_start(j);
    json_kv_str(j, "id", "deny_system_wpa");
    json_kv_str(j, "description", "Deny SYSTEM write access to WPAEvents (prevents revert)");

    if (!apply) {
        json_kv_str(j, "status", FIX_NEEDED);
        json_kv_str(j, "detail", "SYSTEM deny ACE needs to be set on WPAEvents key");
        json_object_end(j);
        return;
    }

    /* Open key with WRITE_DAC permission */
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, key_path, 0,
                      KEY_READ | WRITE_DAC, &hKey) != ERROR_SUCCESS) {
        _snprintf(detail, sizeof(detail), "Cannot open WPAEvents with WRITE_DAC: %lu",
                  (unsigned long)GetLastError());
        json_kv_str(j, "status", FIX_FAILED);
        json_kv_str(j, "detail", detail);
        json_object_end(j);
        return;
    }

    /* Create SYSTEM SID */
    if (!AllocateAndInitializeSid(&nt_auth, 1, SECURITY_LOCAL_SYSTEM_RID,
                                   0, 0, 0, 0, 0, 0, 0, &system_sid)) {
        _snprintf(detail, sizeof(detail), "AllocateAndInitializeSid failed: %lu",
                  (unsigned long)GetLastError());
        json_kv_str(j, "status", FIX_FAILED);
        json_kv_str(j, "detail", detail);
        RegCloseKey(hKey);
        json_object_end(j);
        return;
    }

    /* Get existing DACL */
    rc = GetSecurityInfo(hKey, SE_REGISTRY_KEY, DACL_SECURITY_INFORMATION,
                         NULL, NULL, &old_dacl, NULL, &psd);
    if (rc != ERROR_SUCCESS) {
        _snprintf(detail, sizeof(detail), "GetSecurityInfo failed: %lu", (unsigned long)rc);
        json_kv_str(j, "status", FIX_FAILED);
        json_kv_str(j, "detail", detail);
        FreeSid(system_sid);
        RegCloseKey(hKey);
        json_object_end(j);
        return;
    }

    /* Build deny ACE for SYSTEM: deny Full Control */
    memset(&ea, 0, sizeof(ea));
    ea.grfAccessPermissions = KEY_ALL_ACCESS;
    ea.grfAccessMode = DENY_ACCESS;
    ea.grfInheritance = NO_INHERITANCE;
    ea.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    ea.Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
    ea.Trustee.ptstrName = (LPSTR)system_sid;

    rc = SetEntriesInAclA(1, &ea, old_dacl, &new_dacl);
    if (rc != ERROR_SUCCESS) {
        _snprintf(detail, sizeof(detail), "SetEntriesInAcl failed: %lu", (unsigned long)rc);
        json_kv_str(j, "status", FIX_FAILED);
        json_kv_str(j, "detail", detail);
        FreeSid(system_sid);
        if (psd) LocalFree(psd);
        RegCloseKey(hKey);
        json_object_end(j);
        return;
    }

    /* Apply new DACL */
    rc = SetSecurityInfo(hKey, SE_REGISTRY_KEY, DACL_SECURITY_INFORMATION,
                         NULL, NULL, new_dacl, NULL);
    if (rc == ERROR_SUCCESS) {
        log_msg(LOG_XPA, "Denied SYSTEM Full Control on WPAEvents key");
        json_kv_str(j, "status", FIX_APPLIED);
        json_kv_str(j, "detail", "SYSTEM denied Full Control on WPAEvents (bypass is persistent)");
    } else {
        _snprintf(detail, sizeof(detail), "SetSecurityInfo failed: %lu", (unsigned long)rc);
        json_kv_str(j, "status", FIX_FAILED);
        json_kv_str(j, "detail", detail);
    }

    /* Cleanup */
    if (new_dacl) LocalFree(new_dacl);
    if (psd) LocalFree(psd);
    FreeSid(system_sid);
    RegCloseKey(hKey);

    json_object_end(j);
}

/* ---- step 6: Re-register licensing DLLs ---- */

/*
 * Some SP3 installs have corrupted licensing DLL registrations,
 * causing SetupOobeBnk to silently fail. Re-register them first.
 */
static void step_register_dlls(json_t *j, int apply)
{
    char sys_dir[MAX_PATH];
    char cmd[MAX_PATH];
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;

    json_object_start(j);
    json_kv_str(j, "id", "register_licensing_dlls");
    json_kv_str(j, "description", "Re-register regwizc.dll and licdll.dll (fixes silent SetupOobeBnk failures)");

    if (!apply) {
        json_kv_str(j, "status", FIX_NEEDED);
        json_kv_str(j, "detail", "Licensing DLLs should be re-registered before SetupOobeBnk");
        json_object_end(j);
        return;
    }

    GetSystemDirectoryA(sys_dir, sizeof(sys_dir));

    /* Register regwizc.dll */
    _snprintf(cmd, sizeof(cmd), "regsvr32 /s \"%s\\regwizc.dll\"", sys_dir);
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    memset(&pi, 0, sizeof(pi));
    if (CreateProcessA(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, 10000);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }

    /* Register licdll.dll */
    _snprintf(cmd, sizeof(cmd), "regsvr32 /s \"%s\\licdll.dll\"", sys_dir);
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    memset(&pi, 0, sizeof(pi));
    if (CreateProcessA(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, 10000);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }

    log_msg(LOG_XPA, "Re-registered regwizc.dll and licdll.dll");
    json_kv_str(j, "status", FIX_APPLIED);
    json_kv_str(j, "detail", "Re-registered licensing DLLs");

    json_object_end(j);
}

/* ---- main handler ---- */

typedef void (*xpa_step_t)(json_t *j, int apply);

static const struct {
    const char *id;
    xpa_step_t  func;
} all_steps[] = {
    { "register_licensing_dlls", step_register_dlls },
    { "oobe_timer",              step_oobe_timer },
    { "setup_oobe_bnk",          step_setup_oobe_bnk },
    { "activation_required",     step_activation_required },
    { "remove_wga",              step_remove_wga },
    { "deny_system_wpa",         step_deny_system_wpa },
    { NULL, NULL }
};

void handle_xpactivate(SOCKET sock, const char *args)
{
    json_t j;
    char *result;
    int apply = 0;
    int i;

    if (!is_winnt()) {
        send_error_response(sock, "XPACTIVATE only works on Windows NT/XP (not Win9x)");
        return;
    }

    if (args && _stricmp(args, "apply") == 0) {
        apply = 1;
    }

    log_msg(LOG_XPA, "XPACTIVATE %s (XP=%s)", apply ? "apply" : "check",
            is_winxp() ? "yes" : "no");

    json_init(&j);
    json_object_start(&j);

    json_kv_str(&j, "mode", apply ? "apply" : "check");
    json_kv_bool(&j, "is_winxp", is_winxp());

    json_key(&j, "steps");
    json_array_start(&j);

    for (i = 0; all_steps[i].func; i++) {
        all_steps[i].func(&j, apply);
    }

    json_array_end(&j);
    json_object_end(&j);

    result = json_finish(&j);
    send_text_response(sock, result);
    json_free(&j);
}
