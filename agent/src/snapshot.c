/*
 * snapshot.c - DRVSNAPSHOT: capture driver configuration state
 *
 * Usage: DRVSNAPSHOT <video|audio|lan> [VEN:DEV]
 *
 * Captures the complete driver configuration for devices of the specified
 * class, optionally filtered by PCI vendor:device ID.  Returns JSON with:
 *   - Full recursive registry dump of each device's class entry
 *   - The hardware enum entry (PCI/ISAPNP/etc.) binding
 *   - INF file contents (inline)
 *   - List of driver files with paths, sizes, and timestamps
 *
 * Each driver class (video, audio, lan) is independent — you can snapshot
 * and restore them separately per device ID.
 */

#include "handlers.h"
#include "protocol.h"
#include "util.h"
#include "log.h"
#include <stdio.h>
#include <string.h>

#define LOG_SNAP "SNAP"

/* ------------------------------------------------------------------ */
/*  Class definitions                                                  */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *arg;           /* command argument */
    const char *win9x_class;   /* Win9x class name under Services\Class */
    const char *nt_guid;       /* NT/2K/XP class GUID */
} snap_class_t;

static const snap_class_t snap_classes[] = {
    { "video", "Display", "{4D36E968-E325-11CE-BFC1-08002BE10318}" },
    { "audio", "MEDIA",   "{4D36E96C-E325-11CE-BFC1-08002BE10318}" },
    { "lan",   "Net",     "{4D36E972-E325-11CE-BFC1-08002BE10318}" },
    { NULL, NULL, NULL }
};

/* Extensions that indicate a driver-related file */
static const char *drv_exts[] = {
    ".drv", ".vxd", ".dll", ".sys", ".exe", ".inf", ".cat",
    ".pnf", ".ax",  ".ocx", ".cpl", ".mpd", ".386", ".ndi",
    NULL
};

/* ------------------------------------------------------------------ */
/*  Small helpers                                                      */
/* ------------------------------------------------------------------ */

static int has_driver_ext(const char *s)
{
    const char **e;
    int len;
    if (!s || !*s) return 0;
    len = (int)lstrlenA(s);
    if (len < 3 || len > 260) return 0;
    for (e = drv_exts; *e; e++) {
        int elen = (int)lstrlenA(*e);
        if (len >= elen && _stricmp(s + len - elen, *e) == 0)
            return 1;
    }
    return 0;
}

/* Case-insensitive substring search.  Returns 1 if needle is in haystack. */
static int stristr_found(const char *haystack, const char *needle)
{
    int hlen = (int)lstrlenA(haystack);
    int nlen = (int)lstrlenA(needle);
    int i;
    if (nlen == 0) return 1;
    if (nlen > hlen) return 0;
    for (i = 0; i <= hlen - nlen; i++) {
        if (_strnicmp(haystack + i, needle, nlen) == 0)
            return 1;
    }
    return 0;
}

/* Write binary data as a hex-encoded JSON string */
static void json_hex(json_t *j, const BYTE *data, DWORD len)
{
    static const char hx[] = "0123456789ABCDEF";
    char *buf;
    DWORD i;
    if (len == 0) { json_str(j, ""); return; }
    buf = (char *)HeapAlloc(GetProcessHeap(), 0, len * 2 + 1);
    if (!buf) { json_str(j, ""); return; }
    for (i = 0; i < len; i++) {
        buf[i * 2]     = hx[data[i] >> 4];
        buf[i * 2 + 1] = hx[data[i] & 0xF];
    }
    buf[len * 2] = '\0';
    json_str(j, buf);
    HeapFree(GetProcessHeap(), 0, buf);
}

/* ------------------------------------------------------------------ */
/*  Registry dump helpers                                              */
/* ------------------------------------------------------------------ */

/* Dump all values of an open key as a JSON array of
   { "name", "type", "data" } objects.  */
static void dump_reg_values(json_t *j, HKEY hkey)
{
    DWORD idx, nlen, dlen, type;
    char  name[512];
    BYTE  data[8192];

    json_array_start(j);
    for (idx = 0; ; idx++) {
        nlen = sizeof(name);
        dlen = sizeof(data);
        if (RegEnumValueA(hkey, idx, name, &nlen, NULL,
                          &type, data, &dlen) != ERROR_SUCCESS)
            break;

        json_object_start(j);
        json_kv_str(j, "name", name);

        switch (type) {
        case REG_SZ:
        case REG_EXPAND_SZ:
            json_kv_str(j, "type",
                        type == REG_SZ ? "REG_SZ" : "REG_EXPAND_SZ");
            data[dlen < sizeof(data) ? dlen : sizeof(data) - 1] = '\0';
            json_kv_str(j, "data", (const char *)data);
            break;

        case REG_DWORD:
            json_kv_str(j, "type", "REG_DWORD");
            json_key(j, "data");
            json_uint(j, dlen >= 4 ? *(DWORD *)data : 0);
            break;

        case REG_BINARY:
            json_kv_str(j, "type", "REG_BINARY");
            json_key(j, "data");
            json_hex(j, data, dlen);
            break;

        case REG_MULTI_SZ:
            json_kv_str(j, "type", "REG_MULTI_SZ");
            json_key(j, "data");
            json_array_start(j);
            {
                const char *p = (const char *)data;
                while (*p) {
                    json_str(j, p);
                    p += lstrlenA(p) + 1;
                }
            }
            json_array_end(j);
            break;

        default:
            json_kv_str(j, "type", "REG_OTHER");
            json_kv_uint(j, "type_id", type);
            json_key(j, "data");
            json_hex(j, data, dlen);
            break;
        }
        json_object_end(j);
    }
    json_array_end(j);
}

/* Recursively dump a registry key: values + subkeys (depth-limited). */
static void dump_reg_tree(json_t *j, HKEY hkey, int depth)
{
    DWORD idx, slen;
    char  sname[256];
    HKEY  sub;

    json_key(j, "values");
    dump_reg_values(j, hkey);

    if (depth >= 3) return;   /* prevent runaway recursion */

    json_key(j, "subkeys");
    json_object_start(j);
    for (idx = 0; ; idx++) {
        slen = sizeof(sname);
        if (RegEnumKeyExA(hkey, idx, sname, &slen,
                          NULL, NULL, NULL, NULL) != ERROR_SUCCESS)
            break;
        if (RegOpenKeyExA(hkey, sname, 0, KEY_READ, &sub) == ERROR_SUCCESS) {
            json_key(j, sname);
            json_object_start(j);
            dump_reg_tree(j, sub, depth + 1);
            json_object_end(j);
            RegCloseKey(sub);
        }
    }
    json_object_end(j);
}

/* ------------------------------------------------------------------ */
/*  Enum-entry lookup                                                  */
/* ------------------------------------------------------------------ */

/*
 * Scan HKLM\Enum (Win9x) or HKLM\SYSTEM\CCS\Enum (NT) for an entry
 * whose "Driver" value matches class_ref (e.g. "Display\0000" on Win9x,
 * or "{GUID}\0000" on NT).
 *
 * Fills enum_path (HKLM-relative) and returns 1 if found.
 */
static int find_enum_entry(int is_nt, const char *class_ref,
                           char *enum_path, int path_size)
{
    const char *root = is_nt
        ? "SYSTEM\\CurrentControlSet\\Enum"
        : "Enum";
    HKEY hRoot, hBus, hDev, hInst;
    DWORD bi, di, ii;
    char bname[128], dname[256], iname[128];
    DWORD blen, dlen, ilen;
    char drv[256];
    DWORD drv_len, drv_type;
    int found = 0;

    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, root, 0,
                      KEY_READ, &hRoot) != ERROR_SUCCESS)
        return 0;

    for (bi = 0; !found; bi++) {
        blen = sizeof(bname);
        if (RegEnumKeyExA(hRoot, bi, bname, &blen,
                          NULL, NULL, NULL, NULL) != ERROR_SUCCESS)
            break;
        if (RegOpenKeyExA(hRoot, bname, 0, KEY_READ, &hBus) != ERROR_SUCCESS)
            continue;

        for (di = 0; !found; di++) {
            dlen = sizeof(dname);
            if (RegEnumKeyExA(hBus, di, dname, &dlen,
                              NULL, NULL, NULL, NULL) != ERROR_SUCCESS)
                break;
            if (RegOpenKeyExA(hBus, dname, 0, KEY_READ, &hDev) != ERROR_SUCCESS)
                continue;

            for (ii = 0; !found; ii++) {
                ilen = sizeof(iname);
                if (RegEnumKeyExA(hDev, ii, iname, &ilen,
                                  NULL, NULL, NULL, NULL) != ERROR_SUCCESS)
                    break;
                if (RegOpenKeyExA(hDev, iname, 0,
                                  KEY_READ, &hInst) != ERROR_SUCCESS)
                    continue;

                drv_len = sizeof(drv);
                if (RegQueryValueExA(hInst, "Driver", NULL, &drv_type,
                                     (BYTE *)drv, &drv_len) == ERROR_SUCCESS
                    && (drv_type == REG_SZ || drv_type == REG_EXPAND_SZ)
                    && _stricmp(drv, class_ref) == 0) {
                    _snprintf(enum_path, path_size, "%s\\%s\\%s\\%s",
                              root, bname, dname, iname);
                    enum_path[path_size - 1] = '\0';
                    found = 1;
                }
                RegCloseKey(hInst);
            }
            RegCloseKey(hDev);
        }
        RegCloseKey(hBus);
    }
    RegCloseKey(hRoot);
    return found;
}

/* ------------------------------------------------------------------ */
/*  Driver file collection                                             */
/* ------------------------------------------------------------------ */

/*
 * Scan all string values of hkey for filenames with driver extensions.
 * Unique names are appended to names[]; returns updated count.
 */
static int scan_values_for_files(HKEY hkey,
                                 char names[][128], int count, int max_names)
{
    DWORD idx, nlen, dlen, type;
    char  vname[256];
    char  vdata[512];

    for (idx = 0; count < max_names; idx++) {
        nlen = sizeof(vname);
        dlen = sizeof(vdata);
        if (RegEnumValueA(hkey, idx, vname, &nlen, NULL,
                          &type, (BYTE *)vdata, &dlen) != ERROR_SUCCESS)
            break;
        if (type != REG_SZ && type != REG_EXPAND_SZ) continue;
        vdata[dlen < (DWORD)sizeof(vdata) ? dlen : sizeof(vdata) - 1] = '\0';
        if (!has_driver_ext(vdata)) continue;

        /* Deduplicate */
        {
            int k, dup = 0;
            for (k = 0; k < count; k++) {
                if (_stricmp(names[k], vdata) == 0) { dup = 1; break; }
            }
            if (!dup) {
                lstrcpynA(names[count], vdata, 128);
                count++;
            }
        }
    }
    return count;
}

/*
 * Emit "driver_files" JSON array: for each filename found in the class
 * key's values, search system directories and report path/size/modified.
 */
static void emit_driver_files(json_t *j, HKEY class_key,
                               const char *sys_dir, const char *win_dir)
{
    char names[64][128];
    int  nfiles = 0;
    HKEY sub;
    int  i;
    char drivers_dir[MAX_PATH], inf_dir[MAX_PATH];
    const char *dirs[5];

    /* Collect filenames from root key values */
    nfiles = scan_values_for_files(class_key, names, nfiles, 60);

    /* Scan common subkeys that may contain driver filenames */
    {
        static const char *subkey_names[] = {
            "DEFAULT", "Ndi", "Ndi\\Interfaces", NULL
        };
        const char **sk;
        for (sk = subkey_names; *sk; sk++) {
            if (RegOpenKeyExA(class_key, *sk, 0,
                              KEY_READ, &sub) == ERROR_SUCCESS) {
                nfiles = scan_values_for_files(sub, names, nfiles, 60);
                RegCloseKey(sub);
            }
        }
    }

    /* Build search directory list */
    _snprintf(drivers_dir, sizeof(drivers_dir), "%s\\drivers", sys_dir);
    drivers_dir[sizeof(drivers_dir) - 1] = '\0';
    _snprintf(inf_dir, sizeof(inf_dir), "%s\\INF", win_dir);
    inf_dir[sizeof(inf_dir) - 1] = '\0';

    dirs[0] = sys_dir;         /* C:\WINDOWS\SYSTEM or system32 */
    dirs[1] = drivers_dir;     /* system32\drivers (NT) */
    dirs[2] = inf_dir;         /* C:\WINDOWS\INF */
    dirs[3] = win_dir;         /* C:\WINDOWS */
    dirs[4] = NULL;

    json_key(j, "driver_files");
    json_array_start(j);

    for (i = 0; i < nfiles; i++) {
        int d;
        for (d = 0; dirs[d]; d++) {
            char fpath[MAX_PATH];
            WIN32_FIND_DATAA fd;
            HANDLE hf;
            SYSTEMTIME st;
            char tbuf[32];

            _snprintf(fpath, sizeof(fpath), "%s\\%s", dirs[d], names[i]);
            fpath[sizeof(fpath) - 1] = '\0';

            hf = FindFirstFileA(fpath, &fd);
            if (hf == INVALID_HANDLE_VALUE) continue;
            FindClose(hf);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;

            json_object_start(j);
            json_kv_str(j, "name", names[i]);
            json_kv_str(j, "path", fpath);
            json_kv_uint(j, "size", fd.nFileSizeLow);

            FileTimeToSystemTime(&fd.ftLastWriteTime, &st);
            _snprintf(tbuf, sizeof(tbuf), "%04d-%02d-%02dT%02d:%02d:%02d",
                      st.wYear, st.wMonth, st.wDay,
                      st.wHour, st.wMinute, st.wSecond);
            json_kv_str(j, "modified", tbuf);
            json_object_end(j);
            break;   /* found in this dir, skip remaining dirs */
        }
    }
    json_array_end(j);
}

/* ------------------------------------------------------------------ */
/*  INF file reader                                                    */
/* ------------------------------------------------------------------ */

static void emit_inf_file(json_t *j, const char *win_dir, const char *inf_name)
{
    char  inf_path[MAX_PATH];
    HANDLE hf;
    DWORD fsize, nread;
    char *content;

    json_key(j, "inf_file");
    json_object_start(j);

    if (!inf_name || !*inf_name) {
        json_kv_str(j, "status", "no_inf_path");
        json_object_end(j);
        return;
    }

    json_kv_str(j, "name", inf_name);

    /* Try C:\WINDOWS\INF\<name> first */
    _snprintf(inf_path, sizeof(inf_path), "%s\\INF\\%s", win_dir, inf_name);
    inf_path[sizeof(inf_path) - 1] = '\0';
    hf = CreateFileA(inf_path, GENERIC_READ, FILE_SHARE_READ,
                     NULL, OPEN_EXISTING, 0, NULL);

    /* Fallback: INF\OTHER\ */
    if (hf == INVALID_HANDLE_VALUE) {
        _snprintf(inf_path, sizeof(inf_path),
                  "%s\\INF\\OTHER\\%s", win_dir, inf_name);
        inf_path[sizeof(inf_path) - 1] = '\0';
        hf = CreateFileA(inf_path, GENERIC_READ, FILE_SHARE_READ,
                         NULL, OPEN_EXISTING, 0, NULL);
    }

    json_kv_str(j, "path", inf_path);

    if (hf == INVALID_HANDLE_VALUE) {
        json_kv_str(j, "status", "not_found");
        json_object_end(j);
        return;
    }

    fsize = GetFileSize(hf, NULL);
    json_kv_uint(j, "size", fsize);

    if (fsize > 256 * 1024) {   /* Don't inline INFs > 256 KB */
        json_kv_str(j, "status", "too_large");
        CloseHandle(hf);
        json_object_end(j);
        return;
    }

    content = (char *)HeapAlloc(GetProcessHeap(), 0, fsize + 1);
    if (!content) {
        json_kv_str(j, "status", "alloc_failed");
        CloseHandle(hf);
        json_object_end(j);
        return;
    }

    ReadFile(hf, content, fsize, &nread, NULL);
    CloseHandle(hf);
    content[nread] = '\0';

    json_key(j, "content");
    json_str(j, content);
    json_kv_str(j, "status", "ok");

    HeapFree(GetProcessHeap(), 0, content);

    /* Note PNF companion filename */
    {
        int namelen = (int)lstrlenA(inf_name);
        if (namelen > 4) {
            char pnf_name[MAX_PATH];
            lstrcpynA(pnf_name, inf_name, sizeof(pnf_name));
            lstrcpynA(pnf_name + namelen - 4, ".pnf", 5);
            json_kv_str(j, "pnf_name", pnf_name);
        }
    }

    json_object_end(j);
}

/* ------------------------------------------------------------------ */
/*  PCI-ID extraction from enum path                                   */
/* ------------------------------------------------------------------ */

/*
 * Try to extract "XXXX:YYYY" from a path containing VEN_XXXX&DEV_YYYY.
 * Returns 1 and fills pci_id (must be >= 10 bytes) on success.
 */
static int extract_pci_id(const char *enum_path, char *pci_id, int id_size)
{
    const char *vp = enum_path;
    /* Walk through the path looking for VEN_ (case-insensitive) */
    while (*vp) {
        if (_strnicmp(vp, "VEN_", 4) == 0 && lstrlenA(vp) >= 21) {
            /* VEN_XXXX&DEV_YYYY */
            if (_strnicmp(vp + 8, "&DEV_", 5) == 0) {
                char v4[5] = {0}, d4[5] = {0};
                memcpy(v4, vp + 4, 4);
                memcpy(d4, vp + 13, 4);
                _snprintf(pci_id, id_size, "%s:%s", v4, d4);
                pci_id[id_size - 1] = '\0';
                return 1;
            }
        }
        vp++;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Main handler: DRVSNAPSHOT                                          */
/* ------------------------------------------------------------------ */

void handle_drvsnapshot(SOCKET sock, const char *args)
{
    char class_arg[32]  = {0};
    char dev_filter[32] = {0};
    const snap_class_t *cls = NULL;
    const snap_class_t *c;
    OSVERSIONINFOA osv;
    int  is_nt;
    char class_path[512];
    char sys_dir[MAX_PATH], win_dir[MAX_PATH];
    HKEY hClass;
    DWORD idx, slen;
    char  sname[64];
    json_t j;
    char *result;
    int device_count = 0;
    const char *p;

    /* ---- Parse arguments ---- */
    if (!args || !*args) {
        send_error_response(sock,
            "Usage: DRVSNAPSHOT <video|audio|lan> [VEN:DEV]");
        return;
    }

    p = str_skip_spaces(args);
    {
        int i = 0;
        while (*p && *p != ' ' && i < (int)sizeof(class_arg) - 1)
            class_arg[i++] = *p++;
        class_arg[i] = '\0';
    }
    p = str_skip_spaces(p);
    if (*p) {
        int i = 0;
        while (*p && *p != ' ' && i < (int)sizeof(dev_filter) - 1)
            dev_filter[i++] = *p++;
        dev_filter[i] = '\0';
    }

    /* Look up class */
    for (c = snap_classes; c->arg; c++) {
        if (_stricmp(class_arg, c->arg) == 0) { cls = c; break; }
    }
    if (!cls) {
        send_error_response(sock,
            "Unknown driver class. Use: video, audio, or lan");
        return;
    }

    /* ---- Detect OS ---- */
    osv.dwOSVersionInfoSize = sizeof(osv);
    GetVersionExA(&osv);
    is_nt = (osv.dwPlatformId == VER_PLATFORM_WIN32_NT);

    /* ---- Build paths ---- */
    if (is_nt) {
        _snprintf(class_path, sizeof(class_path),
                  "SYSTEM\\CurrentControlSet\\Control\\Class\\%s",
                  cls->nt_guid);
    } else {
        _snprintf(class_path, sizeof(class_path),
                  "System\\CurrentControlSet\\Services\\Class\\%s",
                  cls->win9x_class);
    }
    class_path[sizeof(class_path) - 1] = '\0';

    GetSystemDirectoryA(sys_dir, sizeof(sys_dir));
    GetWindowsDirectoryA(win_dir, sizeof(win_dir));

    log_msg(LOG_SNAP, "DRVSNAPSHOT: class=%s filter=%s path=%s",
            class_arg, dev_filter[0] ? dev_filter : "(none)", class_path);

    /* ---- Open class key ---- */
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, class_path, 0,
                      KEY_READ, &hClass) != ERROR_SUCCESS) {
        char err[256];
        _snprintf(err, sizeof(err),
                  "Cannot open class key HKLM\\%s (error %lu)",
                  class_path, (unsigned long)GetLastError());
        send_error_response(sock, err);
        return;
    }

    /* ---- Build JSON response ---- */
    json_init(&j);
    json_object_start(&j);
    json_kv_str(&j, "class",       class_arg);
    json_kv_str(&j, "os_type",     is_nt ? "nt" : "win9x");
    json_kv_str(&j, "class_path",  class_path);
    json_kv_str(&j, "system_dir",  sys_dir);
    json_kv_str(&j, "windows_dir", win_dir);
    if (dev_filter[0])
        json_kv_str(&j, "filter", dev_filter);

    json_key(&j, "devices");
    json_array_start(&j);

    /* ---- Enumerate class subkeys (0000, 0001, ...) ---- */
    for (idx = 0; ; idx++) {
        HKEY hEntry;
        char class_ref[256];
        char enum_path[512];
        char desc[256]     = "";
        char inf_name[128] = "";
        char pci_id[16]    = "";
        DWORD vlen, vtype;

        slen = sizeof(sname);
        if (RegEnumKeyExA(hClass, idx, sname, &slen,
                          NULL, NULL, NULL, NULL) != ERROR_SUCCESS)
            break;

        /* Skip non-numeric subkeys (e.g. "Properties") */
        if (sname[0] < '0' || sname[0] > '9') continue;

        if (RegOpenKeyExA(hClass, sname, 0, KEY_READ,
                          &hEntry) != ERROR_SUCCESS)
            continue;

        /* Build driver reference for enum lookup */
        if (is_nt)
            _snprintf(class_ref, sizeof(class_ref),
                      "%s\\%s", cls->nt_guid, sname);
        else
            _snprintf(class_ref, sizeof(class_ref),
                      "%s\\%s", cls->win9x_class, sname);
        class_ref[sizeof(class_ref) - 1] = '\0';

        /* Find corresponding hardware enum entry */
        enum_path[0] = '\0';
        find_enum_entry(is_nt, class_ref, enum_path, sizeof(enum_path));

        /* Apply device filter */
        if (dev_filter[0]) {
            char filter_pat[64];
            const char *colon = strchr(dev_filter, ':');
            if (colon) {
                char ven[16] = {0}, dev[16] = {0};
                int n = (int)(colon - dev_filter);
                if (n > 15) n = 15;
                memcpy(ven, dev_filter, n);
                ven[n] = '\0';
                lstrcpynA(dev, colon + 1, 16);
                _snprintf(filter_pat, sizeof(filter_pat),
                          "VEN_%s&DEV_%s", ven, dev);
                filter_pat[sizeof(filter_pat) - 1] = '\0';

                if (!enum_path[0] ||
                    !stristr_found(enum_path, filter_pat)) {
                    RegCloseKey(hEntry);
                    continue;   /* doesn't match device filter */
                }
            }
        }

        /* Read description and INF path */
        vlen = sizeof(desc);
        if (RegQueryValueExA(hEntry, "DriverDesc", NULL, &vtype,
                             (BYTE *)desc, &vlen) != ERROR_SUCCESS)
            desc[0] = '\0';

        vlen = sizeof(inf_name);
        if (RegQueryValueExA(hEntry, "InfPath", NULL, &vtype,
                             (BYTE *)inf_name, &vlen) != ERROR_SUCCESS)
            inf_name[0] = '\0';

        /* ---- Emit this device ---- */
        json_object_start(&j);
        json_kv_str(&j, "class_index", sname);
        json_kv_str(&j, "class_ref",   class_ref);
        json_kv_str(&j, "description", desc);

        /* Enum entry info */
        json_kv_str(&j, "enum_path", enum_path);

        /* Extract PCI VEN:DEV from enum path */
        if (enum_path[0] && extract_pci_id(enum_path, pci_id, sizeof(pci_id)))
            json_kv_str(&j, "pci_id", pci_id);

        /* Dump hardware enum registry */
        if (enum_path[0]) {
            HKEY hEnumKey;
            if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, enum_path, 0,
                              KEY_READ, &hEnumKey) == ERROR_SUCCESS) {
                json_key(&j, "enum_registry");
                json_object_start(&j);
                dump_reg_tree(&j, hEnumKey, 0);
                json_object_end(&j);
                RegCloseKey(hEnumKey);
            }
        }

        /* Dump class registry (full recursive) */
        json_key(&j, "class_registry");
        json_object_start(&j);
        dump_reg_tree(&j, hEntry, 0);
        json_object_end(&j);

        /* INF file content */
        emit_inf_file(&j, win_dir, inf_name);

        /* Driver files */
        emit_driver_files(&j, hEntry, sys_dir, win_dir);

        json_object_end(&j);
        device_count++;

        RegCloseKey(hEntry);
    }

    json_array_end(&j);   /* devices */
    json_kv_int(&j, "device_count", device_count);
    json_object_end(&j);

    RegCloseKey(hClass);

    result = json_finish(&j);
    send_text_response(sock, result);
    json_free(&j);

    log_msg(LOG_SNAP, "DRVSNAPSHOT: done, %d device(s)", device_count);
}
