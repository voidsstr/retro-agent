/*
 * netshare.c - Network share operations (NETMAP, NETUNMAP, FILECOPY, AUTOMAP)
 * Uses dynamic loading of mpr.dll for Win98SE compatibility.
 */

#include "handlers.h"
#include "protocol.h"
#include "util.h"
#include "log.h"
#include <stdio.h>
#include <string.h>

/* WNet types from winnetwk.h - defined here to avoid header issues */
#ifndef RESOURCETYPE_DISK
#define RESOURCETYPE_DISK       0x00000001
#endif

typedef struct {
    DWORD  dwScope;
    DWORD  dwType;
    DWORD  dwDisplayType;
    DWORD  dwUsage;
    LPSTR  lpLocalName;
    LPSTR  lpRemoteName;
    LPSTR  lpComment;
    LPSTR  lpProvider;
} NETRESOURCEA_COMPAT;

/* Function pointer types for mpr.dll */
typedef DWORD (WINAPI *PFN_WNetAddConnection2A)(
    const NETRESOURCEA_COMPAT *lpNetResource,
    LPCSTR lpPassword, LPCSTR lpUserName, DWORD dwFlags);
typedef DWORD (WINAPI *PFN_WNetCancelConnection2A)(
    LPCSTR lpName, DWORD dwFlags, BOOL fForce);

static HMODULE g_mpr = NULL;
static PFN_WNetAddConnection2A pfn_WNetAddConnection2A = NULL;
static PFN_WNetCancelConnection2A pfn_WNetCancelConnection2A = NULL;

static int load_mpr(void)
{
    if (g_mpr) return 1;

    g_mpr = LoadLibraryA("mpr.dll");
    if (!g_mpr) {
        log_msg(LOG_NET, "LoadLibraryA(\"mpr.dll\") failed: %lu", GetLastError());
        return 0;
    }

    pfn_WNetAddConnection2A = (PFN_WNetAddConnection2A)
        GetProcAddress(g_mpr, "WNetAddConnection2A");
    pfn_WNetCancelConnection2A = (PFN_WNetCancelConnection2A)
        GetProcAddress(g_mpr, "WNetCancelConnection2A");

    if (!pfn_WNetAddConnection2A || !pfn_WNetCancelConnection2A) {
        log_msg(LOG_NET, "mpr.dll missing WNet functions");
        FreeLibrary(g_mpr);
        g_mpr = NULL;
        return 0;
    }

    log_msg(LOG_NET, "mpr.dll loaded OK");
    return 1;
}

static const char *wnet_error_msg(DWORD err)
{
    switch (err) {
    case 0:    return "OK";
    case 5:    return "Access denied";
    case 53:   return "Network path not found";
    case 67:   return "Bad network name";
    case 85:   return "Drive already in use";
    case 86:   return "Invalid password";
    case 1202: return "Device already remembered";
    case 1203: return "No network provider";
    case 1204: return "Network provider not ready";
    case 1205: return "Bad net name";
    case 1219: return "Multiple connections not allowed";
    case 1222: return "Network not available";
    case 2250: return "Network connection not found";
    default:   return "Unknown error";
    }
}

/*
 * NETMAP <unc_path> [drive] [user] [password]
 * Maps a network share via WNetAddConnection2A.
 * Returns JSON: {"status":"OK","unc_path":"\\\\...","drive":"Z:"} or error.
 */
void handle_netmap(SOCKET sock, const char *args)
{
    char unc_path[512] = "";
    char drive[8] = "";
    char user[128] = "";
    char password[128] = "";
    NETRESOURCEA_COMPAT nr;
    DWORD result;
    json_t j;
    char *json_result;

    if (!args || !args[0]) {
        send_error_response(sock, "NETMAP requires a UNC path");
        return;
    }

    if (!load_mpr()) {
        send_error_response(sock, "mpr.dll not available");
        return;
    }

    /* Parse: unc_path [drive] [user] [password] */
    sscanf(args, "%511s %7s %127s %127s", unc_path, drive, user, password);

    log_msg(LOG_NET, "NETMAP: path=\"%s\" drive=\"%s\" user=\"%s\"",
            unc_path, drive, user[0] ? user : "(none)");

    /* Cancel the drive letter mapping only.  Do NOT cancel UNC paths —
     * that would destroy the MSNP32 logon session, after which neither
     * explicit nor session-auth connections work (error 5 on everything).
     * The drive letter cancel handles "drive already in use" (error 85). */
    if (drive[0])
        pfn_WNetCancelConnection2A(drive, 0, TRUE);

    memset(&nr, 0, sizeof(nr));
    nr.dwType = RESOURCETYPE_DISK;
    nr.lpRemoteName = unc_path;
    nr.lpLocalName = drive[0] ? drive : NULL;

    /* Try session auth (NULL creds) first — this uses the MSNP32 logon
     * session which is always valid on Win9x.  We MUST try this before
     * explicit creds because a failed WNetAddConnection2A with explicit
     * creds poisons the session on Win9x (subsequent NULL-cred attempts
     * also fail with error 5 until reboot). */
    if (user[0] || password[0]) {
        result = pfn_WNetAddConnection2A(&nr, NULL, NULL, 0);
        if (result != 0) {
            log_msg(LOG_NET, "NETMAP: session auth failed (%lu), trying explicit creds",
                    (unsigned long)result);
            result = pfn_WNetAddConnection2A(&nr,
                password[0] ? password : NULL,
                user[0] ? user : NULL,
                0);
        }
    } else {
        result = pfn_WNetAddConnection2A(&nr, NULL, NULL, 0);
    }

    log_msg(LOG_NET, "NETMAP: WNetAddConnection2A(\"%s\") = %lu (%s)",
            unc_path, (unsigned long)result, wnet_error_msg(result));

    json_init(&j);
    json_object_start(&j);

    if (result == 0) {
        json_kv_str(&j, "status", "OK");
        json_kv_str(&j, "unc_path", unc_path);
        json_kv_str(&j, "drive", drive[0] ? drive : "(none)");
    } else {
        json_kv_str(&j, "status", "ERROR");
        json_kv_uint(&j, "error_code", result);
        json_kv_str(&j, "error_msg", wnet_error_msg(result));
    }

    json_object_end(&j);
    json_result = json_finish(&j);

    if (result == 0)
        send_text_response(sock, json_result);
    else
        send_error_response(sock, json_result);

    json_free(&j);
}

/*
 * NETUNMAP <drive_or_unc>
 * Unmaps a network share via WNetCancelConnection2A.
 */
void handle_netunmap(SOCKET sock, const char *args)
{
    DWORD result;

    if (!args || !args[0]) {
        send_error_response(sock, "NETUNMAP requires a drive letter or UNC path");
        return;
    }

    if (!load_mpr()) {
        send_error_response(sock, "mpr.dll not available");
        return;
    }

    log_msg(LOG_NET, "NETUNMAP: target=\"%s\"", args);

    result = pfn_WNetCancelConnection2A(args, 0, TRUE);

    log_msg(LOG_NET, "NETUNMAP: WNetCancelConnection2A(\"%s\") = %lu (%s)",
            args, (unsigned long)result, wnet_error_msg(result));

    if (result == 0) {
        send_text_response(sock, "OK");
    } else {
        char err[256];
        _snprintf(err, sizeof(err),
                  "{\"status\":\"ERROR\",\"error_code\":%lu,\"error_msg\":\"%s\"}",
                  (unsigned long)result, wnet_error_msg(result));
        send_error_response(sock, err);
    }
}

/*
 * FILECOPY <source>|<dest>
 * Copies a file using CopyFileA. Pipe delimiter between src and dest.
 * Falls back to space delimiter if no pipe found.
 * Works with UNC paths, mapped drives, and local paths.
 */
void handle_filecopy(SOCKET sock, const char *args)
{
    char source[512], dest[512];
    const char *delim;
    DWORD file_size;
    HANDLE hFile;
    json_t j;
    char *json_result;

    if (!args || !args[0]) {
        send_error_response(sock, "FILECOPY requires source|dest");
        return;
    }

    /* Find delimiter: pipe preferred, space fallback */
    delim = strchr(args, '|');
    if (delim) {
        int src_len = (int)(delim - args);
        if (src_len >= (int)sizeof(source)) src_len = (int)sizeof(source) - 1;
        memcpy(source, args, src_len);
        source[src_len] = '\0';
        safe_strncpy(dest, delim + 1, sizeof(dest));
    } else {
        /* Space fallback: first token is source, rest is dest */
        if (sscanf(args, "%511s %511s", source, dest) < 2) {
            send_error_response(sock, "FILECOPY requires source and dest separated by | or space");
            return;
        }
    }

    /* Trim leading/trailing spaces */
    {
        const char *s = str_skip_spaces(source);
        if (s != source) {
            int len = (int)strlen(s);
            memmove(source, s, len + 1);
        }
        s = str_skip_spaces(dest);
        if (s != dest) {
            int len = (int)strlen(s);
            memmove(dest, s, len + 1);
        }
    }

    log_msg(LOG_FILE, "FILECOPY: \"%s\" -> \"%s\"", source, dest);

    if (!CopyFileA(source, dest, FALSE)) {
        DWORD err = GetLastError();
        char errmsg[256];

        log_msg(LOG_FILE, "FILECOPY: CopyFileA failed, error %lu", (unsigned long)err);

        _snprintf(errmsg, sizeof(errmsg),
                  "{\"status\":\"ERROR\",\"error_code\":%lu,\"source\":\"%s\",\"dest\":\"%s\"}",
                  (unsigned long)err, source, dest);
        send_error_response(sock, errmsg);
        return;
    }

    /* Get file size for response */
    file_size = 0;
    hFile = CreateFileA(dest, GENERIC_READ, FILE_SHARE_READ, NULL,
                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile != INVALID_HANDLE_VALUE) {
        file_size = GetFileSize(hFile, NULL);
        if (file_size == INVALID_FILE_SIZE) file_size = 0;
        CloseHandle(hFile);
    }

    log_msg(LOG_FILE, "FILECOPY: OK, %lu bytes copied", (unsigned long)file_size);

    json_init(&j);
    json_object_start(&j);
    json_kv_str(&j, "status", "OK");
    json_kv_uint(&j, "bytes_copied", file_size);
    json_kv_str(&j, "source", source);
    json_kv_str(&j, "dest", dest);
    json_object_end(&j);

    json_result = json_finish(&j);
    send_text_response(sock, json_result);
    json_free(&j);
}

/* ---- AUTOMAP: persistent drive mappings via registry ---- */

#define AUTOMAP_KEY "Software\\RetroAgent\\AutoMap"

/*
 * Map a single drive from stored config.
 * Cancels any stale cached connection first (by UNC and by drive letter)
 * to avoid "Access denied" from reused unauthenticated sessions.
 * Returns WNet error code (0 = success).
 */
static DWORD automap_map_one(const char *drive, const char *unc,
                             const char *user, const char *pass)
{
    NETRESOURCEA_COMPAT nr;
    DWORD result;

    /* Cancel drive letter only — do NOT cancel UNC paths, as that
     * destroys the MSNP32 logon session on Win9x. */
    pfn_WNetCancelConnection2A(drive, 0, TRUE);

    memset(&nr, 0, sizeof(nr));
    nr.dwType = RESOURCETYPE_DISK;
    nr.lpRemoteName = (LPSTR)unc;
    nr.lpLocalName = (LPSTR)drive;

    /* Try session auth first, fall back to explicit creds.
     * See handle_netmap() comment for why this order matters. */
    if (user[0] || pass[0]) {
        result = pfn_WNetAddConnection2A(&nr, NULL, NULL, 0);
        if (result != 0) {
            result = pfn_WNetAddConnection2A(&nr,
                pass[0] ? pass : NULL,
                user[0] ? user : NULL,
                0);
        }
    } else {
        result = pfn_WNetAddConnection2A(&nr, NULL, NULL, 0);
    }

    return result;
}

/*
 * automap_run_all - Called at agent startup (no socket).
 * Reads all stored mappings from registry and maps them.
 * Retries once after 2s if any mapping fails (network may not be ready).
 */
void automap_run_all(void)
{
    HKEY hKey;
    DWORD idx, name_len, data_len, type;
    char name[16], data[768];
    int mapped = 0, failed = 0;
    int need_retry = 0;
    char retry_names[8][16];
    char retry_data[8][768];
    int retry_count = 0;

    if (!load_mpr()) {
        log_msg(LOG_NET, "AUTOMAP: mpr.dll not available, skipping");
        return;
    }

    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, AUTOMAP_KEY, 0,
                      KEY_READ, &hKey) != ERROR_SUCCESS) {
        /* No automap key = nothing to do */
        return;
    }

    log_msg(LOG_NET, "AUTOMAP: checking stored drive mappings");

    for (idx = 0; ; idx++) {
        char unc[512], user[128], pass[128];
        DWORD result;

        name_len = sizeof(name);
        data_len = sizeof(data);
        if (RegEnumValueA(hKey, idx, name, &name_len, NULL,
                          &type, (LPBYTE)data, &data_len) != ERROR_SUCCESS)
            break;

        if (type != REG_SZ) continue;

        /* Parse: "unc user pass" */
        unc[0] = user[0] = pass[0] = '\0';
        sscanf(data, "%511s %127s %127s", unc, user, pass);
        if (!unc[0]) continue;

        result = automap_map_one(name, unc, user, pass);
        log_msg(LOG_NET, "AUTOMAP: %s -> %s = %lu (%s)",
                name, unc, (unsigned long)result, wnet_error_msg(result));

        if (result == 0) {
            mapped++;
        } else {
            failed++;
            /* Queue for retry (network may not be ready yet) */
            if (retry_count < 8) {
                safe_strncpy(retry_names[retry_count], name, sizeof(retry_names[0]));
                safe_strncpy(retry_data[retry_count], data, sizeof(retry_data[0]));
                retry_count++;
                need_retry = 1;
            }
        }
    }

    RegCloseKey(hKey);

    /* Retry failed mappings after a short delay */
    if (need_retry) {
        int i;
        log_msg(LOG_NET, "AUTOMAP: %d failed, retrying in 2s...", retry_count);
        Sleep(2000);

        for (i = 0; i < retry_count; i++) {
            char unc[512], user[128], pass[128];
            DWORD result;

            unc[0] = user[0] = pass[0] = '\0';
            sscanf(retry_data[i], "%511s %127s %127s", unc, user, pass);

            result = automap_map_one(retry_names[i], unc, user, pass);
            log_msg(LOG_NET, "AUTOMAP: retry %s -> %s = %lu (%s)",
                    retry_names[i], unc, (unsigned long)result,
                    wnet_error_msg(result));

            if (result == 0) {
                mapped++;
                failed--;
            }
        }
    }

    log_msg(LOG_NET, "AUTOMAP: done, %d mapped, %d failed", mapped, failed);
}

/*
 * AUTOMAP ADD <drive> <unc> <user> <pass>
 * AUTOMAP REMOVE <drive>
 * AUTOMAP LIST
 * AUTOMAP RUN
 */
void handle_automap(SOCKET sock, const char *args)
{
    char subcmd[16] = "";

    if (!args || !args[0]) {
        send_error_response(sock, "AUTOMAP requires ADD|REMOVE|LIST|RUN");
        return;
    }

    sscanf(args, "%15s", subcmd);

    if (_stricmp(subcmd, "ADD") == 0) {
        char drive[16] = "", unc[512] = "", user[128] = "", pass[128] = "";
        char regdata[768];
        HKEY hKey;
        DWORD disp, result;
        const char *rest = str_skip_spaces(args + 3);

        if (sscanf(rest, "%15s %511s %127s %127s", drive, unc, user, pass) < 2) {
            send_error_response(sock, "AUTOMAP ADD <drive> <unc> [user] [pass]");
            return;
        }

        if (!load_mpr()) {
            send_error_response(sock, "mpr.dll not available");
            return;
        }

        /* Store in registry */
        _snprintf(regdata, sizeof(regdata), "%s %s %s", unc,
                  user[0] ? user : "", pass[0] ? pass : "");

        if (RegCreateKeyExA(HKEY_LOCAL_MACHINE, AUTOMAP_KEY, 0, NULL,
                            0, KEY_WRITE, NULL, &hKey, &disp) != ERROR_SUCCESS) {
            send_error_response(sock, "Failed to create registry key");
            return;
        }

        RegSetValueExA(hKey, drive, 0, REG_SZ,
                       (const BYTE *)regdata, (DWORD)strlen(regdata) + 1);
        RegCloseKey(hKey);

        log_msg(LOG_NET, "AUTOMAP: stored %s -> %s user=%s",
                drive, unc, user[0] ? user : "(none)");

        /* Also map it immediately */
        result = automap_map_one(drive, unc, user, pass);

        {
            json_t j;
            char *json_result;
            json_init(&j);
            json_object_start(&j);
            json_kv_str(&j, "status", result == 0 ? "OK" : "STORED");
            json_kv_str(&j, "drive", drive);
            json_kv_str(&j, "unc", unc);
            if (result != 0) {
                json_kv_uint(&j, "map_error", result);
                json_kv_str(&j, "map_error_msg", wnet_error_msg(result));
            }
            json_object_end(&j);
            json_result = json_finish(&j);
            send_text_response(sock, json_result);
            json_free(&j);
        }

    } else if (_stricmp(subcmd, "REMOVE") == 0) {
        char drive[16] = "";
        HKEY hKey;
        const char *rest = str_skip_spaces(args + 6);

        sscanf(rest, "%15s", drive);
        if (!drive[0]) {
            send_error_response(sock, "AUTOMAP REMOVE <drive>");
            return;
        }

        /* Remove from registry */
        if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, AUTOMAP_KEY, 0,
                          KEY_WRITE, &hKey) == ERROR_SUCCESS) {
            RegDeleteValueA(hKey, drive);
            RegCloseKey(hKey);
        }

        /* Also unmap now if mpr is available */
        if (load_mpr()) {
            pfn_WNetCancelConnection2A(drive, 0, TRUE);
        }

        log_msg(LOG_NET, "AUTOMAP: removed %s", drive);

        {
            json_t j;
            char *json_result;
            json_init(&j);
            json_object_start(&j);
            json_kv_str(&j, "status", "OK");
            json_kv_str(&j, "drive", drive);
            json_kv_str(&j, "action", "removed");
            json_object_end(&j);
            json_result = json_finish(&j);
            send_text_response(sock, json_result);
            json_free(&j);
        }

    } else if (_stricmp(subcmd, "LIST") == 0) {
        HKEY hKey;
        json_t j;
        char *json_result;
        DWORD idx, name_len, data_len, type;
        char name[16], data[768];

        json_init(&j);
        json_array_start(&j);

        if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, AUTOMAP_KEY, 0,
                          KEY_READ, &hKey) == ERROR_SUCCESS) {
            for (idx = 0; ; idx++) {
                char unc[512], user[128];

                name_len = sizeof(name);
                data_len = sizeof(data);
                if (RegEnumValueA(hKey, idx, name, &name_len, NULL,
                                  &type, (LPBYTE)data, &data_len) != ERROR_SUCCESS)
                    break;
                if (type != REG_SZ) continue;

                unc[0] = user[0] = '\0';
                sscanf(data, "%511s %127s", unc, user);

                json_object_start(&j);
                json_kv_str(&j, "drive", name);
                json_kv_str(&j, "unc", unc);
                json_kv_str(&j, "user", user[0] ? user : "");
                json_kv_str(&j, "password", "***");
                json_object_end(&j);
            }
            RegCloseKey(hKey);
        }

        json_array_end(&j);
        json_result = json_finish(&j);
        send_text_response(sock, json_result);
        json_free(&j);

    } else if (_stricmp(subcmd, "RUN") == 0) {
        HKEY hKey;
        json_t j;
        char *json_result;
        DWORD idx, name_len, data_len, type;
        char name[16], data[768];

        if (!load_mpr()) {
            send_error_response(sock, "mpr.dll not available");
            return;
        }

        json_init(&j);
        json_array_start(&j);

        if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, AUTOMAP_KEY, 0,
                          KEY_READ, &hKey) == ERROR_SUCCESS) {
            for (idx = 0; ; idx++) {
                char unc[512], user[128], pass[128];
                DWORD result;

                name_len = sizeof(name);
                data_len = sizeof(data);
                if (RegEnumValueA(hKey, idx, name, &name_len, NULL,
                                  &type, (LPBYTE)data, &data_len) != ERROR_SUCCESS)
                    break;
                if (type != REG_SZ) continue;

                unc[0] = user[0] = pass[0] = '\0';
                sscanf(data, "%511s %127s %127s", unc, user, pass);
                if (!unc[0]) continue;

                result = automap_map_one(name, unc, user, pass);

                json_object_start(&j);
                json_kv_str(&j, "drive", name);
                json_kv_str(&j, "unc", unc);
                json_kv_str(&j, "status", result == 0 ? "OK" : "ERROR");
                if (result != 0) {
                    json_kv_uint(&j, "error_code", result);
                    json_kv_str(&j, "error_msg", wnet_error_msg(result));
                }
                json_object_end(&j);
            }
            RegCloseKey(hKey);
        }

        json_array_end(&j);
        json_result = json_finish(&j);
        send_text_response(sock, json_result);
        json_free(&j);

    } else {
        send_error_response(sock, "AUTOMAP: unknown sub-command (use ADD|REMOVE|LIST|RUN)");
    }
}
