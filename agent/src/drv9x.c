/*
 * drv9x.c - DRIVERS STATUS / PLAN on Windows 9x (agent 1.88.0).
 *
 * Win9x has no SetupDi device list worth trusting and no newdev; its truth is
 * the Configuration Manager's live devnode table, HKEY_DYN_DATA\Config
 * Manager\Enum (HardWareKey, Problem, Status - what REGEDIT /e exported on
 * .243 to prove the USB card came up problem 0), plus each devnode's
 * HKLM\Enum\<HardWareKey> (ids, class, Driver, ConfigFlags) and its class key
 * System\CurrentControlSet\Services\Class\<Driver>.
 *
 * Report-only. Every device gets one state from agent/shared/drvplan.h; the
 * 3dfx rule (agent/shared/drvsafe.h) is judged from its ids and its bound
 * driver's strings. Win9x ids are comma-separated REG_SZ - drvsafe handles
 * both shapes. There is no driver store for 9x yet, so PLAN says so.
 */

#include "handlers.h"
#include "protocol.h"
#include "util.h"
#include "log.h"
#include "../shared/drvsafe.h"
#include "../shared/drvplan.h"
#include <string.h>
#include <stdio.h>

#define DRV9X_MAX 300

typedef struct {
    char          id[160], desc[128], cls[32], prov[64], date[24], inf[32], match[128];
    unsigned long problem;
    int           state, excl;
} drv9x_rec;

/* Clears the WHOLE buffer first, not just out[0]. drvsafe_ids_3dfx() walks an
 * id list NT-style - NUL-separated strings up to an empty one - so a REG_SZ
 * read into a reused buffer must end in TWO NULs. With only out[0] reset, a
 * shorter HardwareID left the previous device's longer tail after its NUL and
 * that tail was scanned as more ids: on .243 (2026-09-28) the VIA USB
 * controller, enumerated after the Voodoo 2, read "excluded_3dfx" from the
 * Voodoo's leftover VEN_121A. The loss is capped at cch - 2 characters. */
static void drv9x_str(HKEY k, const char *name, char *out, DWORD cch)
{
    DWORD type = 0, sz = cch - 2;
    memset(out, 0, cch);
    if (RegQueryValueExA(k, name, NULL, &type, (LPBYTE)out, &sz) != ERROR_SUCCESS || type != REG_SZ)
        memset(out, 0, cch);
    out[cch - 1] = out[cch - 2] = 0;
}

static DWORD drv9x_dword(HKEY k, const char *name)
{
    BYTE  b[4] = { 0, 0, 0, 0 };
    DWORD type = 0, sz = sizeof(b);
    if (RegQueryValueExA(k, name, NULL, &type, b, &sz) != ERROR_SUCCESS || sz < 4)
        return 0;
    return (DWORD)b[0] | ((DWORD)b[1] << 8) | ((DWORD)b[2] << 16) | ((DWORD)b[3] << 24);
}

void drv9x_status(SOCKET sock, int plan)
{
    HKEY       hdd;
    DWORD      i;
    drv9x_rec *rec;
    int        n = 0, k, counts[DRVST_COUNT], truncated = 0;
    json_t     j;
    char      *out;

    memset(counts, 0, sizeof(counts));
    if (RegOpenKeyExA(HKEY_DYN_DATA, "Config Manager\\Enum", 0, KEY_READ, &hdd) != ERROR_SUCCESS) {
        send_error_response(sock, "DRIVERS STATUS: cannot open HKEY_DYN_DATA\\Config Manager\\Enum");
        return;
    }
    rec = (drv9x_rec *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, DRV9X_MAX * sizeof(drv9x_rec));
    if (!rec) {
        RegCloseKey(hdd);
        send_error_response(sock, "DRIVERS STATUS: out of memory");
        return;
    }
    for (i = 0; ; i++) {
        char  node[32], hwkey[160], path[200], drv[80], hwids[512], compat[512], mfg[64], ddesc[128];
        DWORD cch = sizeof(node);
        HKEY  hn, he, hc;
        drv9x_rec *r;

        if (RegEnumKeyExA(hdd, i, node, &cch, NULL, NULL, NULL, NULL) != ERROR_SUCCESS)
            break;
        if (RegOpenKeyExA(hdd, node, 0, KEY_READ, &hn) != ERROR_SUCCESS)
            continue;
        drv9x_str(hn, "HardWareKey", hwkey, sizeof(hwkey));
        if (!hwkey[0]) { RegCloseKey(hn); continue; }
        if (n >= DRV9X_MAX) { truncated = 1; RegCloseKey(hn); break; }
        r = &rec[n];
        lstrcpynA(r->id, hwkey, sizeof(r->id));
        r->problem = drv9x_dword(hn, "Problem");
        RegCloseKey(hn);

        drv[0] = hwids[0] = compat[0] = mfg[0] = 0;
        _snprintf(path, sizeof(path) - 1, "Enum\\%s", hwkey);
        path[sizeof(path) - 1] = 0;
        if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, path, 0, KEY_READ, &he) == ERROR_SUCCESS) {
            drv9x_str(he, "DeviceDesc", r->desc, sizeof(r->desc));
            drv9x_str(he, "Class", r->cls, sizeof(r->cls));
            drv9x_str(he, "Driver", drv, sizeof(drv));
            drv9x_str(he, "HardwareID", hwids, sizeof(hwids));
            drv9x_str(he, "CompatibleIDs", compat, sizeof(compat));
            drv9x_str(he, "Mfg", mfg, sizeof(mfg));
            if (drv9x_dword(he, "ConfigFlags") & 0x01)
                r->problem = r->problem ? r->problem : 22;     /* disabled at boot */
            RegCloseKey(he);
        }
        ddesc[0] = 0;
        if (drv[0]) {
            _snprintf(path, sizeof(path) - 1, "System\\CurrentControlSet\\Services\\Class\\%s", drv);
            path[sizeof(path) - 1] = 0;
            if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, path, 0, KEY_READ, &hc) == ERROR_SUCCESS) {
                drv9x_str(hc, "ProviderName", r->prov, sizeof(r->prov));
                drv9x_str(hc, "DriverDate", r->date, sizeof(r->date));
                drv9x_str(hc, "InfPath", r->inf, sizeof(r->inf));
                drv9x_str(hc, "MatchingDeviceId", r->match, sizeof(r->match));
                drv9x_str(hc, "DriverDesc", ddesc, sizeof(ddesc));
                RegCloseKey(hc);
            }
        }
        r->excl = drvsafe_ids_3dfx(hwids, compat) || drvsafe_id_is_3dfx(hwkey) ? DRVSAFE_3DFX_ID
                : drvsafe_driver_is_3dfx(r->prov, mfg, ddesc, r->inf, NULL) ? DRVSAFE_3DFX_DRIVER : DRVSAFE_OK;
        r->state = drvplan_state(r->problem, drv[0] != 0, 0, r->excl, r->cls, r->match,
                                 ddesc[0] ? ddesc : r->desc);
        counts[r->state]++;
        n++;
    }
    RegCloseKey(hdd);

    json_init(&j);
    json_object_start(&j);
    json_kv_str(&j, "os", "win9x");
    json_kv_bool(&j, "plan", plan);
    json_kv_str(&j, "store", "none");      /* no Win9x driver store yet */
    json_kv_bool(&j, "truncated", truncated);
    json_key(&j, "counts");
    json_object_start(&j);
    for (k = 0; k < DRVST_COUNT; k++)
        json_kv_int(&j, drvst_name(k), counts[k]);
    json_object_end(&j);
    json_key(&j, "devices");
    json_array_start(&j);
    for (k = 0; k < n; k++) {
        drv9x_rec *r = &rec[k];
        json_object_start(&j);
        json_kv_str(&j, "id", r->id);
        json_kv_str(&j, "desc", r->desc);
        json_kv_str(&j, "class", r->cls);
        json_kv_str(&j, "state", drvst_name(r->state));
        json_kv_uint(&j, "problem", r->problem);
        json_kv_str(&j, "provider", r->prov);
        json_kv_str(&j, "date", r->date);
        json_kv_str(&j, "inf", r->inf);
        json_kv_str(&j, "matching", r->match);
        if (r->excl)
            json_kv_str(&j, "excluded", drvsafe_reason_name(r->excl));
        json_object_end(&j);
    }
    json_array_end(&j);
    json_object_end(&j);
    out = json_finish(&j);
    if (out) {
        send_text_response(sock, out);
        HeapFree(GetProcessHeap(), 0, out);
    } else {
        send_error_response(sock, "DRIVERS STATUS: out of memory");
    }
    HeapFree(GetProcessHeap(), 0, rec);
}
