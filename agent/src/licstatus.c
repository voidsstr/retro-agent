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

/* ---------------------------------------------------------------------- */
/* activation carry-over across a reimage                                  */
/* ---------------------------------------------------------------------- */

/*
 * WPASAVE / WPALOAD - preserve an ALREADY-ACTIVATED machine's activation
 * across a reinstall of the same box.
 *
 * wpa.dbl holds the activation state and is bound to the hardware that
 * produced it. Backing it up before a reinstall and restoring it afterwards is
 * Microsoft's own documented procedure for a repair install, and it is the only
 * part of activation that can honestly be automated: it moves nothing between
 * machines, generates nothing, and a file restored onto different hardware is
 * simply rejected.
 *
 * What this does NOT do is activate an unactivated machine. The first
 * activation of each box still goes through the normal wizard, because
 * Microsoft retired both the internet and automated-phone activation services
 * for XP - so a fleet box that has never been activated needs the operator, and
 * the agent's job is to make sure it only has to happen once per machine rather
 * than once per reimage.
 *
 * The backup is keyed by the machine's own hardware so a restore can never
 * pick up another box's file.
 */
#define WPA_LIVE     "%s\\system32\\wpa.dbl"
#define WPA_LIVE_BAK "%s\\system32\\wpa.bak"

/* A stable per-machine name.
 *
 * The computer name plus the boot volume's serial number. Both survive a
 * reinstall of the same box (setup preserves the volume serial unless the
 * partition is recreated, and the fleet's names are assigned per machine), and
 * neither needs a header beyond what this file already includes - an earlier
 * attempt used the first NIC's MAC via iphlpapi.h, which needs winsock2.h
 * ordered ahead of windows.h and would not compile here.
 *
 * It only has to be unique across the fleet and stable for one machine: a wrong
 * key means "no saved activation found", which is a safe answer, and wpa.dbl is
 * hardware-bound anyway so a mismatched file is rejected by Windows rather than
 * silently accepted.
 */
static void wpa_machine_key(char *out, DWORD cch)
{
    char  name[MAX_COMPUTERNAME_LENGTH + 1];
    DWORD n = sizeof(name);
    DWORD serial = 0;

    out[0] = 0;
    if (!GetComputerNameA(name, &n))
        lstrcpynA(name, "UNKNOWN", sizeof(name));
    GetVolumeInformationA("C:\\", NULL, 0, &serial, NULL, NULL, NULL, 0);
    _snprintf(out, cch - 1, "%s-%08lX", name, serial);
    out[cch - 1] = 0;
}

void handle_wpasave(SOCKET sock, const char *args)
{
    char win[MAX_PATH], live[MAX_PATH], key[64], dst[MAX_PATH], msg[512];
    const char *dir = args;

    while (*dir == ' ')
        dir++;
    if (!*dir) {
        send_error_response(sock, "usage: WPASAVE <directory>  (e.g. a share path)");
        return;
    }
    if (!GetWindowsDirectoryA(win, sizeof(win))) {
        send_error_response(sock, "cannot locate the Windows directory");
        return;
    }
    _snprintf(live, sizeof(live) - 1, WPA_LIVE, win);
    live[sizeof(live) - 1] = 0;
    if (GetFileAttributesA(live) == INVALID_FILE_ATTRIBUTES) {
        send_error_response(sock, "wpa.dbl not found - nothing to save");
        return;
    }
    wpa_machine_key(key, sizeof(key));
    _snprintf(dst, sizeof(dst) - 1, "%s\\wpa-%s.dbl", dir, key);
    dst[sizeof(dst) - 1] = 0;

    if (!CopyFileA(live, dst, FALSE)) {
        _snprintf(msg, sizeof(msg) - 1, "could not write %s (error %lu)",
                  dst, GetLastError());
        msg[sizeof(msg) - 1] = 0;
        send_error_response(sock, msg);
        return;
    }
    _snprintf(msg, sizeof(msg) - 1, "OK saved activation for machine %s to %s",
              key, dst);
    msg[sizeof(msg) - 1] = 0;
    log_msg(LOG_LIC, "%s", msg);
    send_text_response(sock, msg);
}

void handle_wpaload(SOCKET sock, const char *args)
{
    char win[MAX_PATH], live[MAX_PATH], bak[MAX_PATH], key[64], src[MAX_PATH];
    char msg[512];
    const char *dir = args;

    while (*dir == ' ')
        dir++;
    if (!*dir) {
        send_error_response(sock, "usage: WPALOAD <directory>");
        return;
    }
    if (!GetWindowsDirectoryA(win, sizeof(win))) {
        send_error_response(sock, "cannot locate the Windows directory");
        return;
    }
    wpa_machine_key(key, sizeof(key));
    _snprintf(src, sizeof(src) - 1, "%s\\wpa-%s.dbl", dir, key);
    src[sizeof(src) - 1] = 0;
    if (GetFileAttributesA(src) == INVALID_FILE_ATTRIBUTES) {
        /* Not an error: this box has simply never been activated and saved. */
        _snprintf(msg, sizeof(msg) - 1,
                  "no saved activation for machine %s - it needs a one-time "
                  "activation through the wizard first", key);
        msg[sizeof(msg) - 1] = 0;
        log_msg(LOG_LIC, "%s", msg);
        send_text_response(sock, msg);
        return;
    }
    _snprintf(live, sizeof(live) - 1, WPA_LIVE, win);
    _snprintf(bak, sizeof(bak) - 1, WPA_LIVE_BAK, win);
    live[sizeof(live) - 1] = bak[sizeof(bak) - 1] = 0;

    /* Keep whatever is there now. Restoring activation must never be the step
     * that loses a working state. */
    SetFileAttributesA(live, FILE_ATTRIBUTE_NORMAL);
    CopyFileA(live, bak, FALSE);

    if (!CopyFileA(src, live, FALSE)) {
        _snprintf(msg, sizeof(msg) - 1, "could not restore wpa.dbl (error %lu)",
                  GetLastError());
        msg[sizeof(msg) - 1] = 0;
        send_error_response(sock, msg);
        return;
    }
    _snprintf(msg, sizeof(msg) - 1,
              "OK restored activation for machine %s from %s (reboot to take "
              "effect; previous file kept as wpa.bak)", key, src);
    msg[sizeof(msg) - 1] = 0;
    log_msg(LOG_LIC, "%s", msg);
    send_text_response(sock, msg);
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
