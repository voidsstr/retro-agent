/*
 * licstatus.c - Windows activation / license status report (read-only).
 *
 * LICSTATUS
 *   Reports whether the machine considers itself activated and how much of
 *   the activation grace period remains, by reading (never writing) the
 *   standard licensing registry values. This is a diagnostic equivalent of
 *   `slmgr /xpr` for the XP/2K era, so a fleet operator can see which boxes
 *   have fallen out of activation after a reinstall or hardware change.
 *
 * This command does NOT modify activation state, registry ACLs, or any
 * system file. Restoring activation on a licensed machine is done by the
 * operator through the normal Microsoft-provided path (product key entry /
 * telephone activation), not by the agent.
 *
 * Returns JSON with the observed status of each value.
 */

#include "handlers.h"
#include "protocol.h"
#include "util.h"
#include "log.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define LOG_LIC "LICSTATUS"

/* Observation strings */
#define OBS_PRESENT  "present"
#define OBS_ABSENT   "absent"
#define OBS_UNKNOWN  "unknown"

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

/* ---- report: WPAEvents/OOBETimer presence (read-only) ---- */

static void report_oobe_timer(json_t *j)
{
    const char *key_path = "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\WPAEvents";
    HKEY hKey;
    BYTE current[64];
    DWORD size, type;

    json_object_start(j);
    json_kv_str(j, "id", "oobe_timer");
    json_kv_str(j, "description", "Presence of the WPAEvents/OOBETimer activation value");

    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, key_path, 0, KEY_READ, &hKey) != ERROR_SUCCESS) {
        json_kv_str(j, "observed", OBS_ABSENT);
        json_kv_str(j, "detail", "WPAEvents key not found");
        json_object_end(j);
        return;
    }

    size = sizeof(current);
    if (RegQueryValueExA(hKey, "OOBETimer", NULL, &type, current, &size) == ERROR_SUCCESS) {
        json_kv_str(j, "observed", OBS_PRESENT);
        json_kv_str(j, "detail", "OOBETimer value present");
    } else {
        json_kv_str(j, "observed", OBS_ABSENT);
        json_kv_str(j, "detail", "OOBETimer value not set");
    }
    RegCloseKey(hKey);
    json_object_end(j);
}

/* ---- report: activation-enforcement flags (read-only) ---- */

static void report_activation_flags(json_t *j)
{
    const char *winlogon_path = "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Winlogon";
    HKEY hKey;
    DWORD val, size, type;
    int required = -1;   /* -1 unknown, 0/1 observed */

    json_object_start(j);
    json_kv_str(j, "id", "activation_required");
    json_kv_str(j, "description", "Winlogon 'Activation Required' flag (as reported by Windows)");

    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, winlogon_path, 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        size = sizeof(val);
        if (RegQueryValueExA(hKey, "Activation Required", NULL, &type,
                             (BYTE *)&val, &size) == ERROR_SUCCESS && type == REG_DWORD) {
            required = (val != 0) ? 1 : 0;
        }
        RegCloseKey(hKey);
    }

    if (required < 0) {
        json_kv_str(j, "observed", OBS_UNKNOWN);
        json_kv_str(j, "detail", "flag not present");
    } else {
        json_kv_str(j, "observed", OBS_PRESENT);
        json_kv_bool(j, "activation_required", required == 1);
    }
    json_object_end(j);
}

/* ---- report: WGA presence (read-only) ---- */

static void report_wga(json_t *j)
{
    const char *wga_key = "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Winlogon\\Notify\\WgaLogon";
    char sys_dir[MAX_PATH];
    char path[MAX_PATH];
    HKEY hKey;
    int present = 0;

    json_object_start(j);
    json_kv_str(j, "id", "wga");
    json_kv_str(j, "description", "Whether Windows Genuine Advantage components are installed");

    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, wga_key, 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        present = 1;
        RegCloseKey(hKey);
    }
    GetSystemDirectoryA(sys_dir, sizeof(sys_dir));
    _snprintf(path, sizeof(path), "%s\\WgaLogon.dll", sys_dir);
    if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) present = 1;

    json_kv_str(j, "observed", present ? OBS_PRESENT : OBS_ABSENT);
    json_object_end(j);
}

/* ---- main handler ---- */

void handle_licstatus(SOCKET sock, const char *args)
{
    json_t j;
    char *result;

    (void)args;  /* read-only; no sub-commands */

    if (!is_winnt()) {
        send_error_response(sock, "LICSTATUS only works on Windows NT/2K/XP (not Win9x)");
        return;
    }

    log_msg(LOG_LIC, "LICSTATUS report (XP=%s)", is_winxp() ? "yes" : "no");

    json_init(&j);
    json_object_start(&j);

    json_kv_str(&j, "mode", "report");
    json_kv_bool(&j, "read_only", 1);
    json_kv_bool(&j, "is_winxp", is_winxp());

    json_key(&j, "values");
    json_array_start(&j);
    report_oobe_timer(&j);
    report_activation_flags(&j);
    report_wga(&j);
    json_array_end(&j);

    json_object_end(&j);

    result = json_finish(&j);
    send_text_response(sock, result);
    json_free(&j);
}
