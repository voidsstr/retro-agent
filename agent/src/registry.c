/*
 * registry.c - Windows registry operations (ANSI)
 * RegOpenKeyExA, RegQueryValueExA, RegSetValueExA, RegEnumKeyExA, RegEnumValueA
 */

#include "handlers.h"
#include "protocol.h"
#include "util.h"
#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static HKEY parse_root_key(const char *name)
{
    if (_stricmp(name, "HKLM") == 0 ||
        _stricmp(name, "HKEY_LOCAL_MACHINE") == 0)
        return HKEY_LOCAL_MACHINE;
    if (_stricmp(name, "HKCU") == 0 ||
        _stricmp(name, "HKEY_CURRENT_USER") == 0)
        return HKEY_CURRENT_USER;
    if (_stricmp(name, "HKCR") == 0 ||
        _stricmp(name, "HKEY_CLASSES_ROOT") == 0)
        return HKEY_CLASSES_ROOT;
    if (_stricmp(name, "HKU") == 0 ||
        _stricmp(name, "HKEY_USERS") == 0)
        return HKEY_USERS;
    if (_stricmp(name, "HKCC") == 0 ||
        _stricmp(name, "HKEY_CURRENT_CONFIG") == 0)
        return HKEY_CURRENT_CONFIG;
    return NULL;
}

static const char *type_name(DWORD type)
{
    switch (type) {
    case REG_SZ:          return "REG_SZ";
    case REG_EXPAND_SZ:   return "REG_EXPAND_SZ";
    case REG_DWORD:       return "REG_DWORD";
    case REG_BINARY:      return "REG_BINARY";
    case REG_MULTI_SZ:    return "REG_MULTI_SZ";
    case REG_NONE:        return "REG_NONE";
    default:              return "UNKNOWN";
    }
}

static void add_reg_value(json_t *j, HKEY hkey, const char *name)
{
    BYTE data[4096];
    DWORD type = 0, size = sizeof(data);
    LONG rc;

    rc = RegQueryValueExA(hkey, name, NULL, &type, data, &size);
    if (rc != ERROR_SUCCESS) return;

    log_msg(LOG_REG, "  Value: \"%s\" type=%s size=%lu",
            name[0] ? name : "(Default)", type_name(type), (unsigned long)size);

    json_object_start(j);
    json_kv_str(j, "name", name[0] ? name : "(Default)");
    json_kv_str(j, "type", type_name(type));

    switch (type) {
    case REG_SZ:
    case REG_EXPAND_SZ:
        data[size < sizeof(data) ? size : sizeof(data) - 1] = '\0';
        json_kv_str(j, "data", (const char *)data);
        break;
    case REG_DWORD:
        if (size >= 4) {
            DWORD val = *(DWORD *)data;
            json_kv_uint(j, "data", val);
        }
        break;
    case REG_BINARY:
    case REG_MULTI_SZ:
    default: {
        /* Hex encode binary data */
        char *hex = (char *)HeapAlloc(GetProcessHeap(), 0, size * 3 + 1);
        if (hex) {
            DWORD i;
            for (i = 0; i < size; i++)
                _snprintf(hex + i * 3, 4, "%02X ", data[i]);
            if (size > 0) hex[size * 3 - 1] = '\0';
            else hex[0] = '\0';
            json_kv_str(j, "data", hex);
            HeapFree(GetProcessHeap(), 0, hex);
        }
        break;
    }
    }

    json_object_end(j);
}

/*
 * REGREAD <root> <path> [value]
 * If value specified: read single value
 * If no value: enumerate all values and subkeys
 */
/* ---------- argument parsing ----------
 *
 * The registry commands used to parse their arguments with
 * sscanf("%31s %511s ...").  %s stops at whitespace, so ANY key path with a
 * space in it was silently truncated at the space:
 *
 *   REGREAD HKLM SOFTWARE\Microsoft\Windows NT\CurrentVersion
 *     -> path = "SOFTWARE\Microsoft\Windows", value = "NT\CurrentVersion"
 *
 * which fails with "Cannot open key" - indistinguishable from the key not
 * existing.  "Windows NT" is one of the most common paths on the system, and
 * this cost real time: a staged game's install.reg was nearly reported as
 * never merged because the key it wrote could not be read back.
 *
 * A path may now be QUOTED to say exactly where it ends, which is the
 * unambiguous form and the one to prefer:
 *
 *   REGREAD HKLM "SOFTWARE\Microsoft\Windows NT\CurrentVersion" ProductName
 *
 * Unquoted input still works, and REGREAD additionally recovers the common
 * unquoted case by trying the whole remainder as a path first - see
 * handle_regread().
 */

/* Copy one argument out of *pp into buf.  A leading '"' takes everything up to
 * the closing quote (so the argument may contain spaces); otherwise it takes
 * one whitespace-delimited token.  Advances *pp past it.  Returns 1 if
 * anything was read, and sets *was_quoted when the quoted form was used. */
static int reg_next_arg(const char **pp, char *buf, size_t cap, int *was_quoted)
{
    const char *p = *pp;
    size_t n = 0;

    if (was_quoted) *was_quoted = 0;
    while (*p == ' ' || *p == '\t') p++;
    if (!*p) { *pp = p; buf[0] = '\0'; return 0; }

    if (*p == '"') {
        p++;
        if (was_quoted) *was_quoted = 1;
        while (*p && *p != '"') {
            if (n + 1 < cap) buf[n++] = *p;
            p++;
        }
        if (*p == '"') p++;
    } else {
        while (*p && *p != ' ' && *p != '\t') {
            if (n + 1 < cap) buf[n++] = *p;
            p++;
        }
    }
    buf[n] = '\0';
    *pp = p;
    return 1;
}

/* Everything left, with surrounding whitespace and optional quotes removed. */
static void reg_rest_arg(const char *p, char *buf, size_t cap)
{
    size_t n;

    while (*p == ' ' || *p == '\t') p++;
    safe_strncpy(buf, p, cap);

    n = strlen(buf);
    while (n > 0 && (buf[n-1] == ' ' || buf[n-1] == '\t' ||
                     buf[n-1] == '\r' || buf[n-1] == '\n'))
        buf[--n] = '\0';

    if (buf[0] == '"' && n >= 2 && buf[n-1] == '"') {
        memmove(buf, buf + 1, n - 2);
        buf[n-2] = '\0';
    }
}

static void reg_slashes(char *path)
{
    char *p;
    for (p = path; *p; p++)
        if (*p == '/') *p = '\\';
}

void handle_regread(SOCKET sock, const char *args)
{
    char root_str[32], path[512], value_name[256];
    HKEY root, hkey;
    json_t j;
    char *result;
    int nargs;

    value_name[0] = '\0';
    path[0] = '\0';
    {
        const char *p = args ? args : "";
        int quoted = 0;

        if (!reg_next_arg(&p, root_str, sizeof(root_str), NULL) ||
            !reg_next_arg(&p, path, sizeof(path), &quoted)) {
            send_error_response(sock, "REGREAD requires <root> <path> [value]");
            return;
        }
        nargs = 2;

        root = parse_root_key(root_str);
        if (!root) {
            send_error_response(sock, "Invalid registry root");
            return;
        }

        if (quoted) {
            /* The quotes said exactly where the path ended. */
            reg_next_arg(&p, value_name, sizeof(value_name), NULL);
        } else {
            /* Unquoted, and there is more input. It is either a value name or
             * the rest of a path containing spaces ("Windows NT\..."), and we
             * cannot tell by looking. Ask the registry: try the WHOLE
             * remainder as a path first, and only fall back to treating the
             * last token as a value name. Trying the long form first is what
             * makes `REGREAD HKLM SOFTWARE\...\Windows NT\CurrentVersion`
             * work without quotes. */
            char rest[512];
            reg_rest_arg(p, rest, sizeof(rest));
            if (rest[0]) {
                char joined[512];
                HKEY probe;

                _snprintf(joined, sizeof(joined), "%s %s", path, rest);
                joined[sizeof(joined)-1] = '\0';
                reg_slashes(joined);

                if (RegOpenKeyExA(root, joined, 0, KEY_READ, &probe)
                        == ERROR_SUCCESS) {
                    RegCloseKey(probe);
                    safe_strncpy(path, joined, sizeof(path));
                    /* whole thing was the path; no value name */
                } else {
                    /* Split the remainder at its LAST space: everything before
                     * belongs to the path, the final token is the value. */
                    char *sp = strrchr(rest, ' ');
                    if (sp) {
                        *sp = '\0';
                        _snprintf(joined, sizeof(joined), "%s %s", path, rest);
                        joined[sizeof(joined)-1] = '\0';
                        safe_strncpy(path, joined, sizeof(path));
                        safe_strncpy(value_name, sp + 1, sizeof(value_name));
                    } else {
                        safe_strncpy(value_name, rest, sizeof(value_name));
                    }
                    nargs = 3;
                }
            }
        }
    }

    reg_slashes(path);

    if (RegOpenKeyExA(root, path, 0, KEY_READ, &hkey) != ERROR_SUCCESS) {
        char err[256];
        DWORD gle = GetLastError();
        log_msg(LOG_REG, "REGREAD: RegOpenKeyExA(\"%s\\%s\") failed, error %lu",
                root_str, path, (unsigned long)gle);
        _snprintf(err, sizeof(err), "Cannot open key: error %lu", gle);
        send_error_response(sock, err);
        return;
    }
    log_msg(LOG_REG, "REGREAD: RegOpenKeyExA(\"%s\\%s\") = OK", root_str, path);

    json_init(&j);
    json_object_start(&j);
    json_kv_str(&j, "root", root_str);
    json_kv_str(&j, "path", path);

    if (value_name[0]) {
        /* Read single value */
        json_key(&j, "value");
        add_reg_value(&j, hkey, value_name);
    } else {
        /* Enumerate values */
        json_key(&j, "values");
        json_array_start(&j);
        {
            DWORD idx;
            char name[256];
            DWORD name_len;
            for (idx = 0; ; idx++) {
                name_len = sizeof(name);
                if (RegEnumValueA(hkey, idx, name, &name_len,
                                  NULL, NULL, NULL, NULL) != ERROR_SUCCESS)
                    break;
                add_reg_value(&j, hkey, name);
            }
        }
        json_array_end(&j);

        /* Enumerate subkeys */
        json_key(&j, "subkeys");
        json_array_start(&j);
        {
            DWORD idx;
            char name[256];
            DWORD name_len;
            for (idx = 0; ; idx++) {
                name_len = sizeof(name);
                if (RegEnumKeyExA(hkey, idx, name, &name_len,
                                  NULL, NULL, NULL, NULL) != ERROR_SUCCESS)
                    break;
                json_str(&j, name);
            }
        }
        json_array_end(&j);
    }

    json_object_end(&j);
    RegCloseKey(hkey);

    result = json_finish(&j);
    send_text_response(sock, result);
    json_free(&j);
}

/*
 * REGDELETE <root> <path>
 * Recursively delete a registry key and all subkeys.
 * Tries SHDeleteKeyA from shlwapi.dll first, falls back to manual recursion.
 */

typedef LONG (WINAPI *PFN_SHDeleteKeyA)(HKEY hkey, LPCSTR pszSubKey);
static PFN_SHDeleteKeyA pfn_SHDeleteKeyA = NULL;
static int g_shdel_loaded = 0;

static LONG delete_key_recursive(HKEY root, const char *path)
{
    HKEY hkey;
    char child[256];
    DWORD child_len;
    LONG rc;

    rc = RegOpenKeyExA(root, path, 0, KEY_READ | KEY_WRITE, &hkey);
    if (rc != ERROR_SUCCESS) return rc;

    /* Delete all subkeys first (enumerate index 0 repeatedly since
     * deleting shifts the remaining subkeys down) */
    for (;;) {
        child_len = sizeof(child);
        rc = RegEnumKeyExA(hkey, 0, child, &child_len, NULL, NULL, NULL, NULL);
        if (rc != ERROR_SUCCESS) break;

        rc = delete_key_recursive(hkey, child);
        if (rc != ERROR_SUCCESS) {
            RegCloseKey(hkey);
            return rc;
        }
    }

    RegCloseKey(hkey);
    return RegDeleteKeyA(root, path);
}

void handle_regdelete(SOCKET sock, const char *args)
{
    char root_str[32], path[512];
    HKEY root;
    LONG rc;

    {
        /* Unlike REGREAD there is nothing after the path, so the whole
         * remainder IS the path - no ambiguity, spaces and all. */
        const char *p = args ? args : "";

        if (!reg_next_arg(&p, root_str, sizeof(root_str), NULL)) {
            send_error_response(sock, "REGDELETE requires <root> <path>");
            return;
        }
        reg_rest_arg(p, path, sizeof(path));
        if (!path[0]) {
            send_error_response(sock, "REGDELETE requires <root> <path>");
            return;
        }
    }

    reg_slashes(path);

    root = parse_root_key(root_str);
    if (!root) {
        send_error_response(sock, "Invalid registry root");
        return;
    }

    log_msg(LOG_REG, "REGDELETE: \"%s\\%s\"", root_str, path);

    /* Try SHDeleteKeyA first (recursive delete in one call) */
    if (!g_shdel_loaded) {
        HMODULE hmod = LoadLibraryA("shlwapi.dll");
        if (hmod)
            pfn_SHDeleteKeyA = (PFN_SHDeleteKeyA)
                GetProcAddress(hmod, "SHDeleteKeyA");
        g_shdel_loaded = 1;
    }

    if (pfn_SHDeleteKeyA) {
        rc = pfn_SHDeleteKeyA(root, path);
        log_msg(LOG_REG, "REGDELETE: SHDeleteKeyA = %ld", rc);
    } else {
        rc = delete_key_recursive(root, path);
        log_msg(LOG_REG, "REGDELETE: recursive delete = %ld", rc);
    }

    if (rc == ERROR_SUCCESS) {
        send_text_response(sock, "OK");
    } else {
        char err[128];
        _snprintf(err, sizeof(err), "REGDELETE failed: error %ld", rc);
        send_error_response(sock, err);
    }
}

/*
 * REGWRITE <root> <path> <name> <type> <data>
 */
void handle_regwrite(SOCKET sock, const char *args)
{
    char root_str[32], path[512], name[256], type_str[32];
    const char *data_start;
    HKEY root, hkey;
    DWORD type, disposition;

    {
        /* <name> <type> <data> follow the path, so an UNQUOTED path containing
         * spaces is genuinely ambiguous - quote it:
         *   REGWRITE HKLM "SOFTWARE\...\Windows NT\CurrentVersion" X REG_SZ v
         * Unquoted input parses exactly as it always did. */
        const char *p = args ? args : "";

        if (!reg_next_arg(&p, root_str, sizeof(root_str), NULL) ||
            !reg_next_arg(&p, path,     sizeof(path),     NULL) ||
            !reg_next_arg(&p, name,     sizeof(name),     NULL) ||
            !reg_next_arg(&p, type_str, sizeof(type_str), NULL)) {
            send_error_response(sock, "REGWRITE requires <root> <path> <name> <type> <data>");
            return;
        }
        while (*p == ' ' || *p == '\t') p++;
        data_start = p;
    }

    reg_slashes(path);

    root = parse_root_key(root_str);
    if (!root) {
        send_error_response(sock, "Invalid registry root");
        return;
    }

    if (_stricmp(type_str, "REG_SZ") == 0) type = REG_SZ;
    else if (_stricmp(type_str, "REG_EXPAND_SZ") == 0) type = REG_EXPAND_SZ;
    else if (_stricmp(type_str, "REG_DWORD") == 0) type = REG_DWORD;
    else if (_stricmp(type_str, "REG_BINARY") == 0) type = REG_BINARY;
    else {
        send_error_response(sock, "Unsupported type (use REG_SZ, REG_EXPAND_SZ, REG_DWORD, REG_BINARY)");
        return;
    }

    log_msg(LOG_REG, "REGWRITE: key=\"%s\\%s\" name=\"%s\" type=%s",
            root_str, path, name, type_str);

    if (RegCreateKeyExA(root, path, 0, NULL, 0, KEY_WRITE,
                        NULL, &hkey, &disposition) != ERROR_SUCCESS) {
        char err[256];
        DWORD gle = GetLastError();
        log_msg(LOG_REG, "REGWRITE: RegCreateKeyExA failed, error %lu", (unsigned long)gle);
        _snprintf(err, sizeof(err), "Cannot open/create key: error %lu", gle);
        send_error_response(sock, err);
        return;
    }

    {
        LONG rc;
        switch (type) {
        case REG_SZ:
        case REG_EXPAND_SZ:
            rc = RegSetValueExA(hkey, name, 0, type,
                                (const BYTE *)data_start,
                                (DWORD)strlen(data_start) + 1);
            break;
        case REG_DWORD: {
            DWORD val = (DWORD)strtoul(data_start, NULL, 0);
            rc = RegSetValueExA(hkey, name, 0, REG_DWORD,
                                (const BYTE *)&val, sizeof(val));
            break;
        }
        default:
            rc = ERROR_INVALID_PARAMETER;
            break;
        }

        RegCloseKey(hkey);

        if (rc == ERROR_SUCCESS) {
            log_msg(LOG_REG, "REGWRITE: RegSetValueExA = OK");
            send_text_response(sock, "OK");
        } else {
            char err[128];
            log_msg(LOG_REG, "REGWRITE: RegSetValueExA failed: %ld", rc);
            _snprintf(err, sizeof(err), "RegSetValueEx failed: %ld", rc);
            send_error_response(sock, err);
        }
    }
}
