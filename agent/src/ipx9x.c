/*
 * ipx9x.c - IPX/SPX on Windows 98 SE (agent 1.97.0): the registry template and
 * VxD payload from agent/shared/ipxplan.h, applied the way NETDI would.
 * Orchestration and the reboot hand-off: ipxsetup.c.
 *
 * The template was validated against a golden Network-applet install and a real
 * agent install on 2026-10-01 (ipx_9x_writes_allowed in ipxplan.h; the caller
 * checks it - IpxSetup9xTemplateOk=0 shuts this off). A bad binding on .243 can take the box off the network until a
 * person visits it, so every step that can refuse comes BEFORE the first write:
 *
 *   1. exactly ONE physical adapter with TCP/IP bound (Dial-Up / VPN excluded);
 *   2. render the template with this box's indices and adapter, and check it
 *      (only HKLM values SET, nothing deleted, no VREDIR / NWREDIR / NWNBLINK /
 *      VSERVER / NWSERVER, the binding the LAST value);
 *   3. ADDITIVE preflight: every value is either already exactly right or
 *      absent - a value present with OTHER data refuses the whole install;
 *   4. stage NWLINK.VXD + WSIPX.VXD into SYSTEM (copied as .IPN, size and
 *      CRC-32 checked against the compiled-in manifest, then renamed - an
 *      existing file that is not exactly that build is never replaced);
 *   5. write C:\RETRO_AGENT\IPX9X.BAK, a REGEDIT4 file that UNDOES the install
 *      (written once, before the first registry write);
 * then the writes: every key the template names (the empty ones too), every
 * missing value in template order - class key, VxD keys, devnode, Winsock
 * entries, the queued WSCInstallProvider + RunOnce FirstBootCall - and the
 * adapter binding LAST, each read back as it is written; RegFlushKey (Win98
 * writes its registry lazily and .243 has lost writes to a Registry Checker
 * restore); and every value read back once more.
 *
 * Win9x-safe: Reg* / file calls the agent already imports, no child process
 * (regedit is fragile on 9x - CLAUDE.md, regmerge.h), no new DLL.
 */
#include "ipxsetup.h"
#include "log.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#define X9_NETTRANS   "System\\CurrentControlSet\\Services\\Class\\NetTrans"
#define X9_NWLINK     "Enum\\Network\\NWLINK"
#define X9_QUEUED     "Software\\Microsoft\\Windows\\CurrentVersion\\Setup\\NetSetup\\QueuedAPI"
#define X9_TEXT_MAX   16384
#define X9_MAX_KEYS   64
#define X9_MAX_NICS   4

/* ---- small registry helpers (drv9x.c's shapes) --------------------------- */

static void x9_str(HKEY k, const char *name, char *out, DWORD cch)
{
    DWORD type = 0, sz = cch - 2;
    memset(out, 0, cch);
    if (RegQueryValueExA(k, name, NULL, &type, (LPBYTE)out, &sz) != ERROR_SUCCESS || type != REG_SZ)
        memset(out, 0, cch);
    out[cch - 1] = out[cch - 2] = 0;
}

static DWORD x9_dword(HKEY k, const char *name)
{
    BYTE b[4] = { 0, 0, 0, 0 };
    DWORD type = 0, sz = sizeof(b);
    if (RegQueryValueExA(k, name, NULL, &type, b, &sz) != ERROR_SUCCESS || sz < 4)
        return 0;
    return (DWORD)b[0] | ((DWORD)b[1] << 8) | ((DWORD)b[2] << 16) | ((DWORD)b[3] << 24);
}

static int x9_key_exists(const char *path)
{
    HKEY k;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, path, 0, KEY_READ, &k) != ERROR_SUCCESS)
        return 0;
    RegCloseKey(k);
    return 1;
}

/* Does a value of <key>\Bindings start with `prefix` (exact = BE it), any case? */
static int x9_binding_like(const char *key, const char *prefix, int exact)
{
    char path[300], name[128];
    HKEY k;
    DWORD i, n;
    int hit = 0;
    size_t pl = strlen(prefix);
    _snprintf(path, sizeof(path) - 1, "%s\\Bindings", key);
    path[sizeof(path) - 1] = 0;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, path, 0, KEY_READ, &k) != ERROR_SUCCESS)
        return 0;
    for (i = 0; !hit; i++) {
        n = sizeof(name);
        if (RegEnumValueA(k, i, name, &n, NULL, NULL, NULL, NULL) != ERROR_SUCCESS)
            break;
        if (_strnicmp(name, prefix, pl) == 0 && (!exact || !name[pl]))
            hit = 1;
    }
    RegCloseKey(k);
    return hit;
}

/* ---- observe ------------------------------------------------------------- */

typedef struct {
    int  n;
    char path[X9_MAX_NICS][200];
    char desc[X9_MAX_NICS][96];
} x9_nics_t;

/* Live network devnodes (HKEY_DYN_DATA) that are physical, TCP/IP-bound LAN
 * adapters (ipx9x_nic_candidate). */
static void x9_scan_nics(x9_nics_t *out)
{
    HKEY hdd, hn, he;
    DWORD i;
    memset(out, 0, sizeof(*out));
    if (RegOpenKeyExA(HKEY_DYN_DATA, "Config Manager\\Enum", 0, KEY_READ, &hdd) != ERROR_SUCCESS)
        return;
    for (i = 0; ; i++) {
        char node[32], hw[200], path[220], cls[32], ids[512], compat[512], desc[96];
        DWORD cch = sizeof(node), problem;
        if (RegEnumKeyExA(hdd, i, node, &cch, NULL, NULL, NULL, NULL) != ERROR_SUCCESS)
            break;
        if (RegOpenKeyExA(hdd, node, 0, KEY_READ, &hn) != ERROR_SUCCESS)
            continue;
        x9_str(hn, "HardWareKey", hw, sizeof(hw));
        problem = x9_dword(hn, "Problem");
        RegCloseKey(hn);
        if (!hw[0])
            continue;
        _snprintf(path, sizeof(path) - 1, "Enum\\%s", hw);
        path[sizeof(path) - 1] = 0;
        if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, path, 0, KEY_READ, &he) != ERROR_SUCCESS)
            continue;
        x9_str(he, "Class", cls, sizeof(cls));
        x9_str(he, "HardwareID", ids, sizeof(ids));
        x9_str(he, "CompatibleIDs", compat, sizeof(compat));
        x9_str(he, "DeviceDesc", desc, sizeof(desc));
        RegCloseKey(he);
        if (!ipx9x_nic_candidate(cls, problem, x9_binding_like(path, "MSTCP\\", 0),
                                 ids, compat, desc))
            continue;
        if (out->n < X9_MAX_NICS) {
            lstrcpynA(out->path[out->n], hw, sizeof(out->path[0]));
            lstrcpynA(out->desc[out->n], desc, sizeof(out->desc[0]));
        }
        out->n++;
    }
    RegCloseKey(hdd);
}

/* A live devnode for this hardware key with problem 0? */
static int x9_live(const char *want)
{
    HKEY hdd, hn;
    DWORD i;
    int ok = 0;
    if (RegOpenKeyExA(HKEY_DYN_DATA, "Config Manager\\Enum", 0, KEY_READ, &hdd) != ERROR_SUCCESS)
        return 0;
    for (i = 0; !ok; i++) {
        char node[32], hw[200];
        DWORD cch = sizeof(node);
        if (RegEnumKeyExA(hdd, i, node, &cch, NULL, NULL, NULL, NULL) != ERROR_SUCCESS)
            break;
        if (RegOpenKeyExA(hdd, node, 0, KEY_READ, &hn) != ERROR_SUCCESS)
            continue;
        x9_str(hn, "HardWareKey", hw, sizeof(hw));
        if (_stricmp(hw, want) == 0)
            ok = x9_dword(hn, "Problem") == 0;
        RegCloseKey(hn);
    }
    RegCloseKey(hdd);
    return ok;
}

/* The NWLINK devnode + its class key, or an orphan NWLINK class key a failed
 * earlier attempt left (its index is reused, never duplicated). */
static void x9_find_nwlink(ipx9x_info_t *x)
{
    HKEY h, k;
    DWORD i;
    char path[200], drv[64], v[64];

    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, X9_NWLINK, 0, KEY_READ, &h) == ERROR_SUCCESS) {
        for (i = 0; !x->devnode_present; i++) {
            char inst[16];
            DWORD n = sizeof(inst);
            if (RegEnumKeyExA(h, i, inst, &n, NULL, NULL, NULL, NULL) != ERROR_SUCCESS)
                break;
            if (!ipx_index_ok(inst) || RegOpenKeyExA(h, inst, 0, KEY_READ, &k) != ERROR_SUCCESS)
                continue;
            x9_str(k, "Driver", drv, sizeof(drv));
            RegCloseKey(k);
            if (_strnicmp(drv, "NetTrans\\", 9) != 0 || !ipx_index_ok(drv + 9))
                continue;
            _snprintf(path, sizeof(path) - 1, X9_NETTRANS "\\%s", drv + 9);
            path[sizeof(path) - 1] = 0;
            if (!x9_key_exists(path))
                continue;
            lstrcpynA(x->cls, drv + 9, sizeof(x->cls));
            lstrcpynA(x->inst, inst, sizeof(x->inst));
            x->devnode_present = 1;
        }
        RegCloseKey(h);
    }
    if (!x->cls[0] && RegOpenKeyExA(HKEY_LOCAL_MACHINE, X9_NETTRANS, 0, KEY_READ, &h) == ERROR_SUCCESS) {
        for (i = 0; !x->cls[0]; i++) {
            char sub[16];
            DWORD n = sizeof(sub);
            if (RegEnumKeyExA(h, i, sub, &n, NULL, NULL, NULL, NULL) != ERROR_SUCCESS)
                break;
            if (!ipx_index_ok(sub) || RegOpenKeyExA(h, sub, 0, KEY_READ, &k) != ERROR_SUCCESS)
                continue;
            x9_str(k, "InfSection", v, sizeof(v));
            if (_stricmp(v, "NWLINK.ndi") == 0)
                lstrcpynA(x->cls, sub, sizeof(x->cls));
            RegCloseKey(k);
        }
        RegCloseKey(h);
    }
    x->frame = IPX_FRAME_AUTO;
    if (x->cls[0]) {
        _snprintf(path, sizeof(path) - 1, X9_NETTRANS "\\%s", x->cls);
        path[sizeof(path) - 1] = 0;
        if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, path, 0, KEY_READ, &k) == ERROR_SUCCESS) {
            x9_str(k, "Frame_Type", v, sizeof(v));
            x->frame = ipx_frame_from_9x(v);
            RegCloseKey(k);
        }
    }
}

static int x9_file_exists(const char *path)
{
    DWORD a = GetFileAttributesA(path);
    return a != 0xFFFFFFFF && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

void ipx9x_observe(ipx9x_info_t *x)
{
    x9_nics_t nics;
    char sys[MAX_PATH], path[MAX_PATH + 32], hw[64];
    int i;

    memset(x, 0, sizeof(*x));
    x9_scan_nics(&nics);
    x->nic_count = nics.n;
    if (nics.n == 1) {
        lstrcpynA(x->nic, nics.path[0], sizeof(x->nic));
        lstrcpynA(x->nic_desc, nics.desc[0], sizeof(x->nic_desc));
    }
    x9_find_nwlink(x);
    if (x->devnode_present) {
        char name[32];
        _snprintf(name, sizeof(name) - 1, "NWLINK\\%s", x->inst);
        name[sizeof(name) - 1] = 0;
        for (i = 0; i < nics.n && i < X9_MAX_NICS && !x->bound; i++) {
            char key[220];
            _snprintf(key, sizeof(key) - 1, "Enum\\%s", nics.path[i]);
            key[sizeof(key) - 1] = 0;
            x->bound = x9_binding_like(key, name, 1);
        }
        _snprintf(hw, sizeof(hw) - 1, "Network\\NWLINK\\%s", x->inst);
        hw[sizeof(hw) - 1] = 0;
        x->stack_loaded = x9_live(hw);
    }
    sys[0] = 0;
    GetSystemDirectoryA(sys, sizeof(sys));
    x->files_present = 1;
    for (i = 0; i < IPX9X_NPAYLOAD; i++) {
        _snprintf(path, sizeof(path) - 1, "%s\\%s", sys, ipx9x_payload[i].name);
        path[sizeof(path) - 1] = 0;
        if (!x9_file_exists(path))
            x->files_present = 0;
    }
}

/* ---- the payload --------------------------------------------------------- */

static int x9_file_crc(const char *path, unsigned long *size, unsigned long *crc)
{
    HANDLE h;
    unsigned char buf[8192];
    DWORD got;
    *size = 0;
    *crc = 0;
    h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return 0;
    for (;;) {
        if (!ReadFile(h, buf, sizeof(buf), &got, NULL)) {
            CloseHandle(h);
            return 0;
        }
        if (!got)
            break;
        *crc = ipx_crc32(*crc, buf, got);
        *size += got;
    }
    CloseHandle(h);
    return 1;
}

static int x9_root_ok(const char *root)
{
    char p[MAX_PATH + 32];
    _snprintf(p, sizeof(p) - 1, "%s\\%s", root, IPX_PAYLOAD_MANIFEST);
    p[sizeof(p) - 1] = 0;
    return x9_file_exists(p);
}

/* The payload folder: IpxPayloadPath, else the default UNC - and when a bare
 * UNC is not readable (Win9x without an authenticated session, the dosstage.c
 * lesson), the same sub-path on whichever mapped network drive holds it. */
static int x9_payload_root(char *out, size_t cch)
{
    char cfg[MAX_PATH];
    const char *sub;
    char letter;
    HKEY h;

    lstrcpynA(cfg, IPX_DEFAULT_PAYLOAD, sizeof(cfg));
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "Software\\RetroAgent", 0, KEY_READ, &h) == ERROR_SUCCESS) {
        char v[MAX_PATH];
        x9_str(h, IPX_REG_PAYLOAD, v, sizeof(v));
        if (v[0])
            lstrcpynA(cfg, v, sizeof(cfg));
        RegCloseKey(h);
    }
    if (x9_root_ok(cfg)) {
        lstrcpynA(out, cfg, (int)cch);
        return 1;
    }
    if (!(cfg[0] == '\\' && cfg[1] == '\\'))
        return 0;
    sub = cfg + 2;
    while (*sub && *sub != '\\') sub++;
    if (*sub) sub++;
    while (*sub && *sub != '\\') sub++;
    if (*sub) sub++;
    if (!*sub)
        return 0;
    for (letter = 'C'; letter <= 'Z'; letter++) {
        char root[8], probe[MAX_PATH];
        _snprintf(root, sizeof(root) - 1, "%c:\\", letter);
        root[sizeof(root) - 1] = 0;
        if (GetDriveTypeA(root) != DRIVE_REMOTE)
            continue;
        _snprintf(probe, sizeof(probe) - 1, "%c:\\%s", letter, sub);
        probe[sizeof(probe) - 1] = 0;
        if (x9_root_ok(probe)) {
            lstrcpynA(out, probe, (int)cch);
            log_msg(LOG_IPX, "payload reached via mapped drive %c: (the UNC is not readable)", letter);
            return 1;
        }
    }
    return 0;
}

/* Put each payload file in SYSTEM. 1 = every file is there and exactly the
 * manifest's build; 0 = refused (why says what). */
static int x9_stage(const char *sys, ipx9x_result_t *r, char *why, size_t whycch)
{
    char root[MAX_PATH];
    int have_root = 0, i;

    for (i = 0; i < IPX9X_NPAYLOAD; i++) {
        const ipx_file_t *f = &ipx9x_payload[i];
        char dst[MAX_PATH + 32], src[MAX_PATH + 32], tmp[MAX_PATH + 32];
        unsigned long size, crc;
        char base[16];
        const char *dot = strchr(f->name, '.');

        _snprintf(dst, sizeof(dst) - 1, "%s\\%s", sys, f->name);
        dst[sizeof(dst) - 1] = 0;
        if (x9_file_exists(dst)) {
            if (x9_file_crc(dst, &size, &crc) && ipx_payload_ok(f, size, crc))
                continue;
            _snprintf(why, whycch - 1, "%s exists but is not the Windows 98 SE build the template "
                      "was captured with (%lu B, crc %08lX; want %lu B, %08lX) - not replaced",
                      dst, size, crc, f->size, f->crc32);
            why[whycch - 1] = 0;
            return 0;
        }
        if (!have_root) {
            if (!x9_payload_root(root, sizeof(root))) {
                _snprintf(why, whycch - 1, "the IPX payload folder is not reachable (%s, or IpxPayloadPath)"
                          " - no %s", IPX_DEFAULT_PAYLOAD, IPX_PAYLOAD_MANIFEST);
                why[whycch - 1] = 0;
                return 0;
            }
            have_root = 1;
        }
        lstrcpynA(base, f->name, dot ? (int)(dot - f->name) + 1 : (int)sizeof(base));
        _snprintf(src, sizeof(src) - 1, "%s\\%s", root, f->name);
        src[sizeof(src) - 1] = 0;
        _snprintf(tmp, sizeof(tmp) - 1, "%s\\%s.IPN", sys, base);
        tmp[sizeof(tmp) - 1] = 0;
        SetFileAttributesA(tmp, FILE_ATTRIBUTE_NORMAL);
        DeleteFileA(tmp);
        if (!CopyFileA(src, tmp, FALSE)) {
            _snprintf(why, whycch - 1, "cannot copy %s (error %lu)", src, (unsigned long)GetLastError());
            why[whycch - 1] = 0;
            return 0;
        }
        if (!x9_file_crc(tmp, &size, &crc) || !ipx_payload_ok(f, size, crc)) {
            _snprintf(why, whycch - 1, "%s on the share is not the expected build (%lu B, crc %08lX; "
                      "want %lu B, %08lX)", src, size, crc, f->size, f->crc32);
            why[whycch - 1] = 0;
            DeleteFileA(tmp);
            return 0;
        }
        if (!MoveFileA(tmp, dst)) {
            _snprintf(why, whycch - 1, "cannot rename %s to %s (error %lu)", tmp, dst,
                      (unsigned long)GetLastError());
            why[whycch - 1] = 0;
            DeleteFileA(tmp);
            return 0;
        }
        if (!x9_file_crc(dst, &size, &crc) || !ipx_payload_ok(f, size, crc)) {
            _snprintf(why, whycch - 1, "%s does not read back as the staged build", dst);
            why[whycch - 1] = 0;
            return 0;
        }
        r->files_copied++;
        log_msg(LOG_IPX, "%s staged from %s (%lu B, crc %08lX)", dst, root, f->size, f->crc32);
    }
    return 1;
}

/* ---- indices --------------------------------------------------------------- */

/* The lowest free 4-digit subkey index under HKLM\<parent>. */
static int x9_free_index(const char *parent, char *out)
{
    HKEY h;
    DWORD i;
    int used[512], n = 0, idx;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, parent, 0, KEY_READ, &h) == ERROR_SUCCESS) {
        for (i = 0; n < 512; i++) {
            char sub[32];
            DWORD cch = sizeof(sub);
            if (RegEnumKeyExA(h, i, sub, &cch, NULL, NULL, NULL, NULL) != ERROR_SUCCESS)
                break;
            if (ipx_index_ok(sub))
                used[n++] = atoi(sub);
        }
        RegCloseKey(h);
    }
    idx = ipx_first_free(used, n);
    if (idx < 0)
        return 0;
    _snprintf(out, 8, "%04d", idx);
    out[7] = 0;
    return 1;
}

/* The QueuedAPI item: one already queued by an earlier attempt of ours (still
 * waiting for its FirstBootCall) is reused; otherwise the first free ItemN. */
static void x9_queue_item(char *out, size_t cch)
{
    HKEY h, k;
    DWORD i;
    int used[64], n = 0, idx;

    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, X9_QUEUED, 0, KEY_READ, &h) == ERROR_SUCCESS) {
        for (i = 0; n < 64; i++) {
            char sub[32], path[96], api[64], ps[64];
            DWORD c = sizeof(sub);
            if (RegEnumKeyExA(h, i, sub, &c, NULL, NULL, NULL, NULL) != ERROR_SUCCESS)
                break;
            if (_strnicmp(sub, "Item", 4) != 0 || sub[4] < '0' || sub[4] > '9')
                continue;
            used[n++] = atoi(sub + 4);
            _snprintf(path, sizeof(path) - 1, "%s\\1", sub);
            path[sizeof(path) - 1] = 0;
            api[0] = ps[0] = 0;
            if (RegOpenKeyExA(h, path, 0, KEY_READ, &k) == ERROR_SUCCESS) {
                x9_str(k, "", api, sizeof(api));
                RegCloseKey(k);
            }
            _snprintf(path, sizeof(path) - 1, "%s\\1\\ProtocolInfo\\3", sub);
            path[sizeof(path) - 1] = 0;
            if (RegOpenKeyExA(h, path, 0, KEY_READ, &k) == ERROR_SUCCESS) {
                x9_str(k, "ProtocolString", ps, sizeof(ps));
                RegCloseKey(k);
            }
            if (_stricmp(api, "WSCInstallProvider") == 0 && _stricmp(ps, "MS.w95.spi.ipx") == 0) {
                _snprintf(out, cch - 1, "%d", atoi(sub + 4));
                out[cch - 1] = 0;
                RegCloseKey(h);
                return;
            }
        }
        RegCloseKey(h);
    }
    idx = ipx_first_free(used, n);
    _snprintf(out, cch - 1, "%d", idx < 0 ? 0 : idx);
    out[cch - 1] = 0;
}

/* ---- one template value --------------------------------------------------- */

enum { X9_HOLDS = 0, X9_MISSING, X9_CONFLICT };

static int x9_value_state(const rm_entry_t *e)
{
    HKEY k;
    unsigned char buf[RM_DATA_MAX + 4];
    DWORD type = 0, len = sizeof(buf);
    LONG rc;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, e->key, 0, KEY_QUERY_VALUE, &k) != ERROR_SUCCESS)
        return X9_MISSING;
    rc = RegQueryValueExA(k, e->name[0] ? e->name : NULL, NULL, &type, buf, &len);
    RegCloseKey(k);
    if (rc == ERROR_FILE_NOT_FOUND)
        return X9_MISSING;
    /* Win98 reports an empty default value as present-and-empty; a key whose
     * default was never set reads the same way. Treat "" as absent only for
     * the default value, which is never data anyone relies on being "". */
    if (rc == ERROR_SUCCESS && !e->name[0] && type == REG_SZ && (len == 0 || (len == 1 && !buf[0]))
            && !(e->type == RM_REG_SZ && e->len == 0))
        return X9_MISSING;
    if (rc != ERROR_SUCCESS && rc != ERROR_MORE_DATA)
        return X9_MISSING;
    if (rc == ERROR_MORE_DATA)
        return X9_CONFLICT;
    return rm_value_matches(e, 1, (unsigned)type, buf, (unsigned)len) ? X9_HOLDS : X9_CONFLICT;
}

static int x9_value_write(const rm_entry_t *e, LONG *err)
{
    HKEY k;
    DWORD disp;
    LONG rc;
    *err = 0;
    rc = RegCreateKeyExA(HKEY_LOCAL_MACHINE, e->key, 0, NULL, 0, KEY_SET_VALUE | KEY_QUERY_VALUE,
                         NULL, &k, &disp);
    if (rc != ERROR_SUCCESS) {
        *err = rc;
        return 0;
    }
    rc = RegSetValueExA(k, e->name[0] ? e->name : NULL, 0, e->type, e->data,
                        e->type == RM_REG_SZ ? e->len + 1 : e->len);
    RegCloseKey(k);
    if (rc != ERROR_SUCCESS) {
        *err = rc;
        return 0;
    }
    return 1;
}

/* ---- the undo file --------------------------------------------------------- */

typedef struct {
    char key[RM_KEY_MAX];
    int  existed;
} x9_key_t;

static void x9_cat(char *buf, size_t cap, size_t *len, const char *s)
{
    size_t n = strlen(s);
    if (*len + n + 1 > cap)
        return;
    memcpy(buf + *len, s, n + 1);
    *len += n;
}

/* The highest ancestor of `key` (itself included) that does not exist yet -
 * what RegCreateKeyEx will create first, so what the undo must delete. Called
 * BEFORE any key is created. */
static void x9_topmost_new(const char *key, char *out, size_t cch)
{
    char tmp[RM_KEY_MAX];
    size_t i, n = strlen(key);
    lstrcpynA(out, key, (int)cch);
    for (i = 1; i <= n && i < sizeof(tmp); i++) {
        if (key[i] != '\\' && key[i] != 0)
            continue;
        memcpy(tmp, key, i);
        tmp[i] = 0;
        if (!x9_key_exists(tmp)) {
            lstrcpynA(out, tmp, (int)cch);
            return;
        }
    }
}

/* REGEDIT4 text that removes what the install adds: the values it adds to
 * keys that existed (the binding first - it is what turns IPX on), then every
 * key chain it creates, from the highest ancestor that does not exist yet.
 * Written ONCE, before the first write: a later repair run keeps the first
 * file, which already covers everything the template can add. */
static int x9_write_undo(const char *const *parts, int nparts, const x9_key_t *keys, int nkeys,
                         rm_parser_t *ps, rm_entry_t *e, int copied_files)
{
    char *buf;
    size_t len = 0, cap = X9_TEXT_MAX;
    HANDLE h;
    DWORD put = 0;
    int i, p, ok;
    SYSTEMTIME st;
    char line[RM_KEY_MAX + RM_NAME_MAX + 64], qname[RM_NAME_MAX * 2];

    if (x9_file_exists(IPX_BACKUP_FILE))
        return 1;                       /* the first install's undo covers everything */
    buf = (char *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, cap);
    if (!buf)
        return 0;
    GetLocalTime(&st);
    _snprintf(line, sizeof(line) - 1,
              "REGEDIT4\r\n\r\n"
              "; IPX9X.BAK - UNDOES the IPX/SPX protocol the retro agent installed (%04u-%02u-%02u %02u:%02u).\r\n"
              "; Written before the first change. Import it with REGEDIT, then restart Windows.\r\n"
              "; The install only ADDED: it refuses to change a value that already exists.\r\n"
              "; %s\r\n\r\n",
              st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
              copied_files ? "It also copied NWLINK.VXD / WSIPX.VXD into SYSTEM - delete them too."
                           : "NWLINK.VXD / WSIPX.VXD were already in SYSTEM - leave them.");
    line[sizeof(line) - 1] = 0;
    x9_cat(buf, cap, &len, line);

    /* values added to keys that already existed, last part first */
    for (p = nparts - 1; p >= 0; p--) {
        if (!parts[p])
            continue;
        rm_init(ps, parts[p], strlen(parts[p]));
        while (rm_next(ps, e)) {
            int existed = 0;
            for (i = 0; i < nkeys; i++)
                if (_stricmp(keys[i].key, e->key) == 0)
                    existed = keys[i].existed;
            if (!existed || x9_value_state(e) == X9_HOLDS)
                continue;
            if (ipx_quote_backslashes(e->name, qname, sizeof(qname)) < 0)
                continue;
            _snprintf(line, sizeof(line) - 1, "[HKEY_LOCAL_MACHINE\\%s]\r\n\"%s\"=-\r\n\r\n",
                      e->key, qname);
            line[sizeof(line) - 1] = 0;
            x9_cat(buf, cap, &len, line);
        }
    }
    /* every key chain the install creates, from its highest NEW ancestor (an
     * intermediate key like QueuedAPI\ItemN is created too and must go too),
     * each once, last created first */
    for (i = nkeys - 1; i >= 0; i--) {
        char top[RM_KEY_MAX], marker[RM_KEY_MAX + 32];
        if (keys[i].existed)
            continue;
        x9_topmost_new(keys[i].key, top, sizeof(top));
        _snprintf(marker, sizeof(marker) - 1, "[-HKEY_LOCAL_MACHINE\\%s]\r\n", top);
        marker[sizeof(marker) - 1] = 0;
        if (strstr(buf, marker))
            continue;
        _snprintf(line, sizeof(line) - 1, "%s\r\n", marker);
        line[sizeof(line) - 1] = 0;
        x9_cat(buf, cap, &len, line);
    }

    h = CreateFileA(IPX_BACKUP_FILE, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        HeapFree(GetProcessHeap(), 0, buf);
        return 0;
    }
    ok = WriteFile(h, buf, (DWORD)len, &put, NULL) && put == (DWORD)len;
    if (ok)
        FlushFileBuffers(h);
    CloseHandle(h);
    HeapFree(GetProcessHeap(), 0, buf);
    if (!ok)
        DeleteFileA(IPX_BACKUP_FILE);
    return ok;
}

/* ---- the install ------------------------------------------------------------ */

static void x9_why(ipx9x_result_t *r, const char *fmt, const char *a, const char *b)
{
    _snprintf(r->msg, sizeof(r->msg) - 1, fmt, a ? a : "", b ? b : "");
    r->msg[sizeof(r->msg) - 1] = 0;
}

int ipx9x_install(ipx9x_result_t *r, int catalog_has_ipx)
{
    ipx9x_info_t x;
    char K[8], I[8], Q[12], sys[MAX_PATH], sysq[MAX_PATH * 2], why[400];
    char *text[IPX9X_NPARTS];
    const char *parts[IPX9X_NPARTS];
    ipx_var_t vars[6];
    rm_parser_t *ps = NULL;
    rm_entry_t *e = NULL;
    x9_key_t *keys = NULL;
    int nkeys = 0, p, i, missing = 0, conflicts = 0, total;
    const char *cwhy;

    memset(r, 0, sizeof(*r));
    memset(text, 0, sizeof(text));
    memset(parts, 0, sizeof(parts));

    ipx9x_observe(&x);
    if (x.nic_count != 1) {
        if (x.nic_count)
            _snprintf(r->msg, sizeof(r->msg) - 1, "REFUSED: %d TCP/IP-bound network adapters - "
                      "which one carries IPX is not decided automatically", x.nic_count);
        else
            lstrcpynA(r->msg, "REFUSED: no TCP/IP-bound physical network adapter found",
                      sizeof(r->msg));
        r->msg[sizeof(r->msg) - 1] = 0;
        return 0;
    }

    /* indices: reuse an earlier install's, never duplicate */
    if (x.cls[0])
        lstrcpynA(K, x.cls, sizeof(K));
    else if (!x9_free_index(X9_NETTRANS, K)) {
        x9_why(r, "REFUSED: no free NetTrans class index%s%s", NULL, NULL);
        return 0;
    }
    if (x.devnode_present)
        lstrcpynA(I, x.inst, sizeof(I));
    else if (!x9_free_index(X9_NWLINK, I)) {
        x9_why(r, "REFUSED: no free NWLINK instance%s%s", NULL, NULL);
        return 0;
    }
    x9_queue_item(Q, sizeof(Q));
    sys[0] = 0;
    GetSystemDirectoryA(sys, sizeof(sys));
    if (!sys[0] || ipx_quote_backslashes(sys, sysq, sizeof(sysq)) < 0) {
        x9_why(r, "REFUSED: no system directory%s%s", NULL, NULL);
        return 0;
    }
    vars[0].name = "K";        vars[0].value = K;
    vars[1].name = "I";        vars[1].value = I;
    vars[2].name = "Q";        vars[2].value = Q;
    vars[3].name = "FRAME";    vars[3].value = ipx_frame_to_9x(IPX_FRAME_8022);
    vars[4].name = "NIC";      vars[4].value = x.nic;
    vars[5].name = "SYSDIR:Q"; vars[5].value = sysq;

    ps = (rm_parser_t *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*ps));
    e = (rm_entry_t *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*e));
    keys = (x9_key_t *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, X9_MAX_KEYS * sizeof(*keys));
    if (!ps || !e || !keys) {
        x9_why(r, "FAILED: out of memory%s%s", NULL, NULL);
        goto out;
    }
    for (p = 0; p < IPX9X_NPARTS; p++) {
        if (p == IPX9X_PART_WS2 && catalog_has_ipx)
            continue;               /* the catalog already lists IPX: queue nothing */
        text[p] = (char *)HeapAlloc(GetProcessHeap(), 0, X9_TEXT_MAX);
        if (!text[p] || ipx9x_render(ipx9x_part(p), vars, 6, text[p], X9_TEXT_MAX) < 0) {
            x9_why(r, "REFUSED: the template part %s did not render%s",
                   p == 0 ? "stack" : p == 1 ? "ws2" : "bind", NULL);
            goto out;
        }
        parts[p] = text[p];
    }

    /* the rules, on what will really be written */
    total = ipx9x_template_check(parts, IPX9X_NPARTS, x.nic, I, ps, e, &cwhy);
    if (total < 0) {
        x9_why(r, "REFUSED: %s%s", cwhy, NULL);
        goto out;
    }

    /* ADDITIVE preflight: keys that exist, values that hold / are missing /
     * would CHANGE (= refuse). Nothing has been written yet. */
    for (p = 0; p < IPX9X_NPARTS; p++) {
        const char *q = parts[p];
        char key[RM_KEY_MAX];
        if (!q)
            continue;
        while (ipx9x_next_key(&q, key, sizeof(key))) {
            for (i = 0; i < nkeys; i++)
                if (_stricmp(keys[i].key, key) == 0)
                    break;
            if (i < nkeys)
                continue;
            if (nkeys >= X9_MAX_KEYS) {
                x9_why(r, "REFUSED: the template names more than 64 keys%s%s", NULL, NULL);
                goto out;
            }
            lstrcpynA(keys[nkeys].key, key, sizeof(keys[0].key));
            keys[nkeys].existed = x9_key_exists(key);
            nkeys++;
        }
        rm_init(ps, parts[p], strlen(parts[p]));
        while (rm_next(ps, e)) {
            int st = x9_value_state(e);
            if (st == X9_MISSING)
                missing++;
            else if (st == X9_CONFLICT && !conflicts++) {
                _snprintf(why, sizeof(why) - 1, "HKLM\\%s\\%s", e->key, e->name[0] ? e->name : "@");
                why[sizeof(why) - 1] = 0;
            }
        }
    }
    if (conflicts) {
        _snprintf(r->msg, sizeof(r->msg) - 1, "REFUSED: %d existing value(s) hold OTHER data - the "
                  "install only adds, it never changes network configuration (first: %s)",
                  conflicts, why);
        r->msg[sizeof(r->msg) - 1] = 0;
        goto out;
    }

    /* the payload, before any registry write */
    if (!x9_stage(sys, r, why, sizeof(why))) {
        x9_why(r, "REFUSED: %s%s", why, NULL);
        goto out;
    }
    if (!missing) {
        r->ok = 1;
        _snprintf(r->msg, sizeof(r->msg) - 1, "already in place: all %d value(s) hold "
                  "(NetTrans\\%s, NWLINK\\%s), %d file(s) copied", total, K, I, r->files_copied);
        r->msg[sizeof(r->msg) - 1] = 0;
        goto out;
    }
    if (!x9_write_undo(parts, IPX9X_NPARTS, keys, nkeys, ps, e, r->files_copied)) {
        x9_why(r, "REFUSED: cannot write the undo file %s%s", IPX_BACKUP_FILE, NULL);
        goto out;
    }

    /* the writes: keys in template order (empty ones too), then values in
     * template order - the binding is the last value of the last part */
    for (i = 0; i < nkeys; i++) {
        HKEY k;
        DWORD disp;
        if (keys[i].existed)
            continue;
        if (RegCreateKeyExA(HKEY_LOCAL_MACHINE, keys[i].key, 0, NULL, 0, KEY_ALL_ACCESS, NULL,
                            &k, &disp) != ERROR_SUCCESS) {
            x9_why(r, "FAILED: cannot create HKLM\\%s - nothing bound, see %s", keys[i].key,
                   IPX_BACKUP_FILE);
            RegFlushKey(HKEY_LOCAL_MACHINE);
            goto out;
        }
        RegCloseKey(k);
        r->keys_created++;
    }
    for (p = 0; p < IPX9X_NPARTS; p++) {
        if (!parts[p])
            continue;
        rm_init(ps, parts[p], strlen(parts[p]));
        while (rm_next(ps, e)) {
            LONG err = 0;
            if (x9_value_state(e) == X9_HOLDS)
                continue;
            if (!x9_value_write(e, &err) || x9_value_state(e) != X9_HOLDS) {
                char what[RM_KEY_MAX + RM_NAME_MAX + 32];
                _snprintf(what, sizeof(what) - 1, "HKLM\\%s\\%s (error %ld)", e->key,
                          e->name[0] ? e->name : "@", (long)err);
                what[sizeof(what) - 1] = 0;
                x9_why(r, "FAILED: %s did not read back - the adapter binding was NOT written; "
                       "undo: %s", what, IPX_BACKUP_FILE);
                RegFlushKey(HKEY_LOCAL_MACHINE);
                goto out;
            }
            r->values_written++;
        }
    }
    RegFlushKey(HKEY_LOCAL_MACHINE);    /* Win98 writes its registry lazily */

    /* the post-condition: every value, read back once more */
    for (p = 0; p < IPX9X_NPARTS; p++) {
        if (!parts[p])
            continue;
        rm_init(ps, parts[p], strlen(parts[p]));
        while (rm_next(ps, e))
            if (x9_value_state(e) != X9_HOLDS) {
                x9_why(r, "FAILED: HKLM\\%s\\%s changed after it was written", e->key,
                       e->name[0] ? e->name : "@");
                goto out;
            }
    }
    r->ok = 1;
    _snprintf(r->msg, sizeof(r->msg) - 1, "installed: %d value(s) written in %d new key(s), "
              "%d file(s) copied, NetTrans\\%s, NWLINK\\%s bound to %s, frame 802.2%s",
              r->values_written, r->keys_created, r->files_copied, K, I, x.nic,
              catalog_has_ipx ? "" : ", Winsock 2 provider queued for the next boot");
    r->msg[sizeof(r->msg) - 1] = 0;

out:
    for (p = 0; p < IPX9X_NPARTS; p++)
        if (text[p])
            HeapFree(GetProcessHeap(), 0, text[p]);
    if (ps) HeapFree(GetProcessHeap(), 0, ps);
    if (e) HeapFree(GetProcessHeap(), 0, e);
    if (keys) HeapFree(GetProcessHeap(), 0, keys);
    return r->ok;
}
