/*
 * video.c - Video card diagnostics
 * Display adapter enumeration via registry, driver info, resolution,
 * DirectX version, PCI IDs, error codes.
 */

#include "handlers.h"
#include "protocol.h"
#include "util.h"
#include "log.h"
#include <windows.h>
#include <setupapi.h>
#include <cfgmgr32.h>
#include <stdio.h>
#include <string.h>

/* Display class GUID: {4D36E968-E325-11CE-BFC1-08002BE10318} */
static const GUID GUID_DISPLAY = {
    0x4D36E968, 0xE325, 0x11CE,
    { 0xBF, 0xC1, 0x08, 0x00, 0x2B, 0xE1, 0x03, 0x18 }
};

/*
 * CM_Get_DevNode_Status - dynamically loaded for Win98SE compatibility.
 * On Win98SE this is in cfgmgr32.dll, on NT in setupapi.dll or cfgmgr32.dll.
 * We try both. If unavailable, device status checks are skipped.
 */
typedef DWORD (WINAPI *PFN_CM_Get_DevNode_Status)(
    PULONG pulStatus, PULONG pulProblemNumber, DWORD dnDevInst, ULONG ulFlags);
static PFN_CM_Get_DevNode_Status pfn_CM_Get_DevNode_Status = NULL;
static int g_cm_loaded = 0;

static void load_cfgmgr(void)
{
    HMODULE hmod;
    if (g_cm_loaded) return;
    g_cm_loaded = 1;

    hmod = LoadLibraryA("cfgmgr32.dll");
    if (hmod) {
        pfn_CM_Get_DevNode_Status = (PFN_CM_Get_DevNode_Status)
            GetProcAddress(hmod, "CM_Get_DevNode_Status");
        if (pfn_CM_Get_DevNode_Status) {
            log_msg(LOG_VIDEO, "CM_Get_DevNode_Status loaded from cfgmgr32.dll");
            return;
        }
    }

    hmod = LoadLibraryA("setupapi.dll");
    if (hmod) {
        pfn_CM_Get_DevNode_Status = (PFN_CM_Get_DevNode_Status)
            GetProcAddress(hmod, "CM_Get_DevNode_Status");
        if (pfn_CM_Get_DevNode_Status) {
            log_msg(LOG_VIDEO, "CM_Get_DevNode_Status loaded from setupapi.dll");
            return;
        }
    }

    log_msg(LOG_VIDEO, "CM_Get_DevNode_Status not available");
}

static void read_reg_string(HKEY hkey, const char *name, char *buf, int bufsize)
{
    DWORD type = REG_SZ, size = (DWORD)bufsize;
    buf[0] = '\0';
    RegQueryValueExA(hkey, name, NULL, &type, (BYTE *)buf, &size);
}

static void add_display_info(json_t *j)
{
    HDC hdc = GetDC(NULL);
    if (hdc) {
        char res[32];
        int width  = GetDeviceCaps(hdc, HORZRES);
        int height = GetDeviceCaps(hdc, VERTRES);
        int depth  = GetDeviceCaps(hdc, BITSPIXEL) * GetDeviceCaps(hdc, PLANES);
        int refresh = GetDeviceCaps(hdc, VREFRESH);

        _snprintf(res, sizeof(res), "%dx%d", width, height);

        json_key(j, "display");
        json_object_start(j);
        json_kv_str(j, "resolution", res);
        json_kv_int(j, "color_depth", depth);
        json_kv_int(j, "refresh_rate", refresh);

        {
            /*
             * Get driver description. EnumDisplayDevicesA is Win2000+ only,
             * so on Win98 we fall back to reading the registry directly.
             */
            char drv_desc[256] = "";
            typedef BOOL (WINAPI *PFN_EnumDisplayDevicesA)(LPCSTR, DWORD, PDISPLAY_DEVICEA, DWORD);
            PFN_EnumDisplayDevicesA pfnEnum;
            pfnEnum = (PFN_EnumDisplayDevicesA)GetProcAddress(
                GetModuleHandleA("user32.dll"), "EnumDisplayDevicesA");
            if (pfnEnum) {
                DISPLAY_DEVICEA dd;
                dd.cb = sizeof(dd);
                if (pfnEnum(NULL, 0, &dd, 0)) {
                    safe_strncpy(drv_desc, dd.DeviceString, sizeof(drv_desc));
                }
            }
            if (drv_desc[0] == '\0') {
                /* Fallback: read from registry (Win98 path) */
                HKEY hk;
                if (RegOpenKeyExA(HKEY_LOCAL_MACHINE,
                    "System\\CurrentControlSet\\Services\\Class\\Display\\0000",
                    0, KEY_READ, &hk) == ERROR_SUCCESS) {
                    read_reg_string(hk, "DriverDesc", drv_desc, sizeof(drv_desc));
                    RegCloseKey(hk);
                }
            }
            json_kv_str(j, "driver_desc", drv_desc);
        }

        json_object_end(j);
        ReleaseDC(NULL, hdc);
    }
}

static void add_directx_info(json_t *j)
{
    HKEY hkey;
    char version[64] = "unknown";

    json_key(j, "directx");
    json_object_start(j);

    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE,
                      "SOFTWARE\\Microsoft\\DirectX",
                      0, KEY_READ, &hkey) == ERROR_SUCCESS) {
        read_reg_string(hkey, "Version", version, sizeof(version));
        json_kv_str(j, "version", version);
        json_kv_str(j, "registry_path", "HKLM\\SOFTWARE\\Microsoft\\DirectX");
        RegCloseKey(hkey);
    } else {
        json_kv_str(j, "version", "not found");
    }

    json_object_end(j);
}

/*
 * Enumerate display adapters from registry.
 * Win98: HKLM\System\CurrentControlSet\Services\Class\Display\*
 * WinXP: HKLM\SYSTEM\CurrentControlSet\Control\Class\{4D36E968-E325-11CE-BFC1-08002BE10318}\*
 */
static void add_adapters_from_registry(json_t *j)
{
    HKEY hkey;
    OSVERSIONINFOA osvi;
    const char *base_path;
    char subkey_name[64];
    DWORD index, name_len;
    int is_nt;

    osvi.dwOSVersionInfoSize = sizeof(osvi);
    GetVersionExA(&osvi);
    is_nt = (osvi.dwPlatformId == VER_PLATFORM_WIN32_NT);

    if (is_nt)
        base_path = "SYSTEM\\CurrentControlSet\\Control\\Class\\"
                    "{4D36E968-E325-11CE-BFC1-08002BE10318}";
    else
        base_path = "System\\CurrentControlSet\\Services\\Class\\Display";

    json_key(j, "adapters");
    json_array_start(j);

    log_msg(LOG_VIDEO, "Scanning adapters: base_path=\"HKLM\\%s\" (is_nt=%d)", base_path, is_nt);

    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, base_path, 0, KEY_READ, &hkey)
        == ERROR_SUCCESS) {

        for (index = 0; ; index++) {
            HKEY hsubkey;
            char full_path[512];
            char name[256], mfg[256], drv_ver[64], drv_date[64];
            char inf_path[256], hw_id[256];
            char vid[16], did[16];
            DWORD problem = 0;

            name_len = sizeof(subkey_name);
            if (RegEnumKeyExA(hkey, index, subkey_name, &name_len,
                              NULL, NULL, NULL, NULL) != ERROR_SUCCESS)
                break;

            _snprintf(full_path, sizeof(full_path), "%s\\%s",
                      base_path, subkey_name);

            if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, full_path, 0, KEY_READ,
                              &hsubkey) != ERROR_SUCCESS) {
                log_msg(LOG_VIDEO, "RegOpenKeyExA(\"HKLM\\%s\") failed", full_path);
                continue;
            }
            log_msg(LOG_VIDEO, "RegOpenKeyExA(\"HKLM\\%s\") = OK", full_path);

            read_reg_string(hsubkey, "DriverDesc", name, sizeof(name));
            if (name[0] == '\0') {
                /* Skip non-adapter entries (like "Properties") */
                RegCloseKey(hsubkey);
                continue;
            }

            read_reg_string(hsubkey, "ProviderName", mfg, sizeof(mfg));
            if (mfg[0] == '\0')
                read_reg_string(hsubkey, "Mfg", mfg, sizeof(mfg));
            read_reg_string(hsubkey, "DriverVersion", drv_ver, sizeof(drv_ver));
            read_reg_string(hsubkey, "DriverDate", drv_date, sizeof(drv_date));
            read_reg_string(hsubkey, "InfPath", inf_path, sizeof(inf_path));
            read_reg_string(hsubkey, "MatchingDeviceId", hw_id, sizeof(hw_id));

            log_msg(LOG_VIDEO, "Adapter: name=\"%s\" mfg=\"%s\" hw_id=\"%s\"",
                    name, mfg, hw_id);

            /* Parse PCI VEN/DEV from hardware ID (e.g. "pci\ven_10de&dev_0028") */
            vid[0] = did[0] = '\0';
            {
                const char *p;
                p = strstr(hw_id, "ven_");
                if (!p) p = strstr(hw_id, "VEN_");
                if (p) {
                    _snprintf(vid, sizeof(vid), "0x%c%c%c%c",
                              p[4], p[5], p[6], p[7]);
                }
                p = strstr(hw_id, "dev_");
                if (!p) p = strstr(hw_id, "DEV_");
                if (p) {
                    _snprintf(did, sizeof(did), "0x%c%c%c%c",
                              p[4], p[5], p[6], p[7]);
                }
            }

            log_msg(LOG_VIDEO, "PCI IDs: vendor=%s device=%s",
                    vid[0] ? vid : "unknown", did[0] ? did : "unknown");

            json_object_start(j);
            json_kv_str(j, "name", name);
            json_kv_str(j, "manufacturer", mfg);
            json_kv_str(j, "pci_vendor_id", vid[0] ? vid : "unknown");
            json_kv_str(j, "pci_device_id", did[0] ? did : "unknown");
            json_kv_str(j, "driver_version", drv_ver[0] ? drv_ver : "unknown");
            json_kv_str(j, "driver_date", drv_date[0] ? drv_date : "unknown");
            json_kv_str(j, "inf_path", inf_path);
            json_kv_str(j, "hardware_id", hw_id);

            {
                char reg_path[512];
                _snprintf(reg_path, sizeof(reg_path), "HKLM\\%s", full_path);
                json_kv_str(j, "registry_path", reg_path);
            }

            /* Try to get device status via SetupAPI / CfgMgr */
            load_cfgmgr();
            if (pfn_CM_Get_DevNode_Status) {
                HDEVINFO devs;
                SP_DEVINFO_DATA devinfo;
                DWORD dev_idx;

                devs = SetupDiGetClassDevsA(&GUID_DISPLAY, NULL, NULL,
                                            DIGCF_PRESENT);
                if (devs != INVALID_HANDLE_VALUE) {
                    devinfo.cbSize = sizeof(devinfo);
                    for (dev_idx = 0;
                         SetupDiEnumDeviceInfo(devs, dev_idx, &devinfo);
                         dev_idx++) {
                        ULONG dn_status = 0, dn_problem = 0;
                        if (pfn_CM_Get_DevNode_Status(&dn_status, &dn_problem,
                                                      devinfo.DevInst, 0)
                            == CR_SUCCESS) {
                            (void)dn_status;
                            problem = dn_problem;
                            break;  /* Use first match */
                        }
                    }
                    SetupDiDestroyDeviceInfoList(devs);
                }
            }

            log_msg(LOG_VIDEO, "Device status: %s (error_code=%lu)",
                    problem == 0 ? "OK" : "ERROR", (unsigned long)problem);

            json_kv_str(j, "status", problem == 0 ? "OK" : "ERROR");
            json_kv_uint(j, "error_code", problem);

            json_object_end(j);
            RegCloseKey(hsubkey);
        }

        RegCloseKey(hkey);
    }

    json_array_end(j);
}

void handle_videodiag(SOCKET sock)
{
    json_t j;
    char *result;

    json_init(&j);
    json_object_start(&j);

    add_adapters_from_registry(&j);
    add_display_info(&j);
    add_directx_info(&j);

    json_object_end(&j);

    result = json_finish(&j);
    send_text_response(sock, result);
    json_free(&j);
}

/*
 * PCISCAN - PCI display device scan with cross-referencing.
 *
 * Reads:
 *   1. HKLM\Enum\PCI\* — all PCI device entries (Win98) or similar (NT)
 *   2. Display class entries — installed display drivers
 *   3. SetupDiGetClassDevsA(DIGCF_PRESENT) — physically present devices
 *
 * Cross-reference produces status for each device:
 *   active       — PCI entry + valid Display binding + physically present
 *   ghost        — PCI entry with display Driver binding but NOT physically present
 *   orphan       — PCI entry with Driver pointing to nonexistent Display class entry
 *   new          — PCI display vendor entry with no Driver binding
 *   stale_driver — Display class entry with no PCI entry referencing it
 */

/* Track which Display class indices are referenced by PCI entries */
#define MAX_DISPLAY_ENTRIES 32

typedef struct {
    char class_index[8];       /* "0000", "0001", etc. */
    char driver_desc[256];
    char matching_id[256];
    char inf_path[256];
    char referenced_by[256];   /* PCI ID + instance that references this */
    int  is_referenced;
} display_entry_t;

typedef struct {
    char hw_id[128];           /* e.g. "pci\\ven_121a&dev_0005" */
    int  present;
} present_device_t;

#define MAX_PRESENT_DEVICES 32

void handle_pciscan(SOCKET sock)
{
    json_t j;
    char *result;
    OSVERSIONINFOA osvi;
    int is_nt;
    HKEY hkey_pci, hkey_display_base;
    const char *pci_enum_path;
    const char *display_class_path;
    DWORD ven_idx, inst_idx, disp_idx;
    display_entry_t displays[MAX_DISPLAY_ENTRIES];
    int display_count = 0;
    present_device_t present[MAX_PRESENT_DEVICES];
    int present_count = 0;

    /* Summary counters */
    char active_ids[512] = "";
    char ghost_ids[512] = "";
    char new_ids[512] = "";
    char stale_indices[512] = "";
    int active_count = 0, ghost_count = 0, new_count = 0, stale_count = 0;

    osvi.dwOSVersionInfoSize = sizeof(osvi);
    GetVersionExA(&osvi);
    is_nt = (osvi.dwPlatformId == VER_PLATFORM_WIN32_NT);

    if (is_nt) {
        pci_enum_path = "SYSTEM\\CurrentControlSet\\Enum\\PCI";
        display_class_path = "SYSTEM\\CurrentControlSet\\Control\\Class\\"
                             "{4D36E968-E325-11CE-BFC1-08002BE10318}";
    } else {
        pci_enum_path = "Enum\\PCI";
        display_class_path = "System\\CurrentControlSet\\Services\\Class\\Display";
    }

    log_msg(LOG_VIDEO, "PCISCAN: pci_enum=\"HKLM\\%s\" display_class=\"HKLM\\%s\"",
            pci_enum_path, display_class_path);

    /* ---- Phase 1: Collect physically present display devices ---- */
    load_cfgmgr();
    {
        HDEVINFO devs;
        SP_DEVINFO_DATA devinfo;
        DWORD dev_idx;

        devs = SetupDiGetClassDevsA(&GUID_DISPLAY, NULL, NULL, DIGCF_PRESENT);
        if (devs != INVALID_HANDLE_VALUE) {
            devinfo.cbSize = sizeof(devinfo);
            for (dev_idx = 0;
                 SetupDiEnumDeviceInfo(devs, dev_idx, &devinfo) &&
                 present_count < MAX_PRESENT_DEVICES;
                 dev_idx++) {
                char hw_id[256] = "";
                SetupDiGetDeviceRegistryPropertyA(devs, &devinfo,
                    SPDRP_HARDWAREID, NULL, (BYTE *)hw_id, sizeof(hw_id), NULL);
                if (hw_id[0]) {
                    /* Lowercase for comparison */
                    char *p;
                    safe_strncpy(present[present_count].hw_id, hw_id,
                                 sizeof(present[present_count].hw_id));
                    for (p = present[present_count].hw_id; *p; p++)
                        *p = (*p >= 'A' && *p <= 'Z') ? (*p + 32) : *p;
                    present[present_count].present = 1;
                    log_msg(LOG_VIDEO, "PCISCAN: present device: \"%s\"",
                            present[present_count].hw_id);
                    present_count++;
                }
            }
            SetupDiDestroyDeviceInfoList(devs);
        }
    }

    /* ---- Phase 2: Collect Display class entries ---- */
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, display_class_path, 0, KEY_READ,
                      &hkey_display_base) == ERROR_SUCCESS) {
        char subkey_name[64];
        DWORD name_len;

        for (disp_idx = 0; display_count < MAX_DISPLAY_ENTRIES; disp_idx++) {
            HKEY hsubkey;
            char full_path[512];

            name_len = sizeof(subkey_name);
            if (RegEnumKeyExA(hkey_display_base, disp_idx, subkey_name, &name_len,
                              NULL, NULL, NULL, NULL) != ERROR_SUCCESS)
                break;

            /* Skip non-numeric entries like "Properties" */
            if (subkey_name[0] < '0' || subkey_name[0] > '9')
                continue;

            _snprintf(full_path, sizeof(full_path), "%s\\%s",
                      display_class_path, subkey_name);

            if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, full_path, 0, KEY_READ,
                              &hsubkey) == ERROR_SUCCESS) {
                display_entry_t *de = &displays[display_count];

                safe_strncpy(de->class_index, subkey_name, sizeof(de->class_index));
                read_reg_string(hsubkey, "DriverDesc", de->driver_desc,
                                sizeof(de->driver_desc));
                read_reg_string(hsubkey, "MatchingDeviceId", de->matching_id,
                                sizeof(de->matching_id));
                read_reg_string(hsubkey, "InfPath", de->inf_path,
                                sizeof(de->inf_path));
                de->referenced_by[0] = '\0';
                de->is_referenced = 0;

                /* Only count entries that have a driver description */
                if (de->driver_desc[0]) {
                    log_msg(LOG_VIDEO, "PCISCAN: display[%s] desc=\"%s\" match=\"%s\"",
                            de->class_index, de->driver_desc, de->matching_id);
                    display_count++;
                }

                RegCloseKey(hsubkey);
            }
        }
        RegCloseKey(hkey_display_base);
    }

    /* ---- Phase 3: Scan PCI enum and cross-reference ---- */
    json_init(&j);
    json_object_start(&j);

    json_key(&j, "pci_display_devices");
    json_array_start(&j);

    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, pci_enum_path, 0, KEY_READ,
                      &hkey_pci) == ERROR_SUCCESS) {
        char ven_name[128];
        DWORD ven_name_len;

        for (ven_idx = 0; ; ven_idx++) {
            HKEY hkey_ven;
            char ven_path[512];
            char vid[8] = "", did[8] = "";
            const char *p;
            int is_display_vendor;

            ven_name_len = sizeof(ven_name);
            if (RegEnumKeyExA(hkey_pci, ven_idx, ven_name, &ven_name_len,
                              NULL, NULL, NULL, NULL) != ERROR_SUCCESS)
                break;

            /* Parse VEN_ and DEV_ from subkey name */
            p = strstr(ven_name, "VEN_");
            if (!p) p = strstr(ven_name, "ven_");
            if (p) {
                vid[0] = p[4]; vid[1] = p[5]; vid[2] = p[6]; vid[3] = p[7];
                vid[4] = '\0';
            }
            p = strstr(ven_name, "DEV_");
            if (!p) p = strstr(ven_name, "dev_");
            if (p) {
                did[0] = p[4]; did[1] = p[5]; did[2] = p[6]; did[3] = p[7];
                did[4] = '\0';
            }

            if (!vid[0] || !did[0]) continue;

            /* Check if any Display class entry references this VEN/DEV combo */
            is_display_vendor = 0;
            {
                char match_lower[128];
                int di;
                _snprintf(match_lower, sizeof(match_lower),
                          "pci\\ven_%s&dev_%s", vid, did);
                /* Lowercase for comparison */
                {
                    char *q;
                    for (q = match_lower; *q; q++)
                        *q = (*q >= 'A' && *q <= 'Z') ? (*q + 32) : *q;
                }
                for (di = 0; di < display_count; di++) {
                    char mi_lower[256];
                    char *q;
                    safe_strncpy(mi_lower, displays[di].matching_id, sizeof(mi_lower));
                    for (q = mi_lower; *q; q++)
                        *q = (*q >= 'A' && *q <= 'Z') ? (*q + 32) : *q;
                    if (strstr(mi_lower, match_lower)) {
                        is_display_vendor = 1;
                        break;
                    }
                }
            }

            /* Enumerate instance subkeys */
            _snprintf(ven_path, sizeof(ven_path), "%s\\%s", pci_enum_path, ven_name);
            if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, ven_path, 0, KEY_READ,
                              &hkey_ven) == ERROR_SUCCESS) {
                char inst_name[64];
                DWORD inst_name_len;

                for (inst_idx = 0; ; inst_idx++) {
                    HKEY hkey_inst;
                    char inst_path[512];
                    char driver_binding[64] = "";
                    char desc[256] = "";
                    const char *status_str;
                    int has_display_binding = 0;
                    int is_present = 0;
                    int binding_valid = 0;

                    inst_name_len = sizeof(inst_name);
                    if (RegEnumKeyExA(hkey_ven, inst_idx, inst_name, &inst_name_len,
                                      NULL, NULL, NULL, NULL) != ERROR_SUCCESS)
                        break;

                    _snprintf(inst_path, sizeof(inst_path), "%s\\%s",
                              ven_path, inst_name);

                    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, inst_path, 0, KEY_READ,
                                      &hkey_inst) != ERROR_SUCCESS)
                        continue;

                    read_reg_string(hkey_inst, "Driver", driver_binding,
                                    sizeof(driver_binding));
                    read_reg_string(hkey_inst, "DeviceDesc", desc, sizeof(desc));

                    RegCloseKey(hkey_inst);

                    /* Check if this has a Display driver binding */
                    if (driver_binding[0]) {
                        if (_strnicmp(driver_binding, "Display\\", 8) == 0 ||
                            _strnicmp(driver_binding, "DISPLAY\\", 8) == 0) {
                            has_display_binding = 1;

                            /* Check if the referenced Display class entry exists */
                            {
                                const char *idx_part = driver_binding + 8;
                                int di;
                                for (di = 0; di < display_count; di++) {
                                    if (_stricmp(displays[di].class_index,
                                                 idx_part) == 0) {
                                        binding_valid = 1;
                                        displays[di].is_referenced = 1;
                                        _snprintf(displays[di].referenced_by,
                                                  sizeof(displays[di].referenced_by),
                                                  "%s\\%s", ven_name, inst_name);
                                        break;
                                    }
                                }
                            }
                        }
                    } else if (!is_display_vendor) {
                        /* No driver binding and not a known display vendor — skip */
                        continue;
                    }

                    /* Skip non-display devices that have non-display bindings */
                    if (driver_binding[0] && !has_display_binding && !is_display_vendor)
                        continue;

                    /* Check physical presence against SetupDi results */
                    {
                        char check_id[128];
                        char *q;
                        int pi;
                        _snprintf(check_id, sizeof(check_id),
                                  "pci\\ven_%s&dev_%s", vid, did);
                        for (q = check_id; *q; q++)
                            *q = (*q >= 'A' && *q <= 'Z') ? (*q + 32) : *q;
                        for (pi = 0; pi < present_count; pi++) {
                            if (strstr(present[pi].hw_id, check_id)) {
                                is_present = 1;
                                break;
                            }
                        }
                    }

                    /* Determine status */
                    if (!driver_binding[0]) {
                        status_str = "new";
                    } else if (has_display_binding && !binding_valid) {
                        status_str = "orphan";
                    } else if (has_display_binding && is_present) {
                        status_str = "active";
                    } else if (has_display_binding && !is_present) {
                        status_str = "ghost";
                    } else {
                        continue;  /* Non-display device */
                    }

                    log_msg(LOG_VIDEO, "PCISCAN: PCI %s\\%s driver=\"%s\" "
                            "present=%d status=%s",
                            ven_name, inst_name, driver_binding,
                            is_present, status_str);

                    /* Emit JSON for this device */
                    json_object_start(&j);
                    json_kv_str(&j, "pci_id", ven_name);
                    json_kv_str(&j, "instance", inst_name);
                    json_kv_str(&j, "vendor_id", vid);
                    json_kv_str(&j, "device_id", did);
                    json_kv_str(&j, "description", desc);
                    json_kv_str(&j, "driver_binding", driver_binding);
                    json_kv_str(&j, "status", status_str);
                    json_object_end(&j);

                    /* Track summary */
                    {
                        char id_pair[16];
                        _snprintf(id_pair, sizeof(id_pair), "%s:%s", vid, did);

                        if (strcmp(status_str, "active") == 0 && active_count < 10) {
                            if (active_ids[0]) strcat(active_ids, ",");
                            strcat(active_ids, id_pair);
                            active_count++;
                        } else if (strcmp(status_str, "ghost") == 0 && ghost_count < 10) {
                            if (ghost_ids[0]) strcat(ghost_ids, ",");
                            strcat(ghost_ids, id_pair);
                            ghost_count++;
                        } else if (strcmp(status_str, "new") == 0 && new_count < 10) {
                            if (new_ids[0]) strcat(new_ids, ",");
                            strcat(new_ids, id_pair);
                            new_count++;
                        }
                    }
                }
                RegCloseKey(hkey_ven);
            }
        }
        RegCloseKey(hkey_pci);
    }

    json_array_end(&j);

    /* ---- Phase 4: Emit Display class entries ---- */
    json_key(&j, "display_drivers");
    json_array_start(&j);
    {
        int di;
        for (di = 0; di < display_count; di++) {
            json_object_start(&j);
            json_kv_str(&j, "class_index", displays[di].class_index);
            json_kv_str(&j, "driver_desc", displays[di].driver_desc);
            json_kv_str(&j, "matching_device_id", displays[di].matching_id);
            json_kv_str(&j, "inf_path", displays[di].inf_path);
            json_kv_str(&j, "referenced_by",
                         displays[di].is_referenced
                             ? displays[di].referenced_by : "");
            json_object_end(&j);

            /* Track stale drivers */
            if (!displays[di].is_referenced && stale_count < 10) {
                if (stale_indices[0]) strcat(stale_indices, ",");
                strcat(stale_indices, displays[di].class_index);
                stale_count++;
            }
        }
    }
    json_array_end(&j);

    /* ---- Phase 5: Summary ---- */
    json_key(&j, "summary");
    json_object_start(&j);
    {
        /* Emit each summary field as a JSON array of strings */
        char *lists[] = { active_ids, ghost_ids, new_ids, stale_indices };
        const char *keys[] = { "active", "ghost", "new_hardware", "stale_drivers" };
        int li;

        for (li = 0; li < 4; li++) {
            json_key(&j, keys[li]);
            json_array_start(&j);
            if (lists[li][0]) {
                /* Split comma-separated list */
                char tmp[512];
                char *tok, *next;
                safe_strncpy(tmp, lists[li], sizeof(tmp));
                tok = tmp;
                while (tok && *tok) {
                    next = strchr(tok, ',');
                    if (next) *next++ = '\0';
                    json_str(&j, tok);
                    tok = next;
                }
            }
            json_array_end(&j);
        }
    }
    json_object_end(&j);

    json_object_end(&j);

    result = json_finish(&j);
    send_text_response(sock, result);
    json_free(&j);
}

void handle_drivers(SOCKET sock, const char *args)
{
    HDEVINFO devs;
    SP_DEVINFO_DATA devinfo;
    DWORD idx;
    json_t j;
    char *result;
    GUID *pguid = NULL;
    char filter[128] = "";

    if (args && args[0]) {
        safe_strncpy(filter, args, sizeof(filter));
        /* If it looks like a GUID, parse it */
        if (filter[0] == '{') {
            /* Try SetupDiClassGuidsFromNameA if it's a class name instead */
            /* For now, just list all and filter display */
        }
    }

    json_init(&j);
    json_array_start(&j);

    devs = SetupDiGetClassDevsA(pguid, NULL, NULL,
                                DIGCF_PRESENT | DIGCF_ALLCLASSES);
    if (devs != INVALID_HANDLE_VALUE) {
        devinfo.cbSize = sizeof(devinfo);
        for (idx = 0; SetupDiEnumDeviceInfo(devs, idx, &devinfo); idx++) {
            char desc[256] = "", cls[128] = "", mfg[256] = "";
            char hw_id[256] = "";
            ULONG status = 0, problem = 0;

            SetupDiGetDeviceRegistryPropertyA(devs, &devinfo,
                SPDRP_DEVICEDESC, NULL, (BYTE *)desc, sizeof(desc), NULL);
            SetupDiGetDeviceRegistryPropertyA(devs, &devinfo,
                SPDRP_CLASS, NULL, (BYTE *)cls, sizeof(cls), NULL);
            SetupDiGetDeviceRegistryPropertyA(devs, &devinfo,
                SPDRP_MFG, NULL, (BYTE *)mfg, sizeof(mfg), NULL);
            SetupDiGetDeviceRegistryPropertyA(devs, &devinfo,
                SPDRP_HARDWAREID, NULL, (BYTE *)hw_id, sizeof(hw_id), NULL);

            /* Apply class filter if specified */
            if (filter[0] && _stricmp(cls, filter) != 0)
                continue;

            load_cfgmgr();
            if (pfn_CM_Get_DevNode_Status)
                pfn_CM_Get_DevNode_Status(&status, &problem, devinfo.DevInst, 0);

            json_object_start(&j);
            json_kv_str(&j, "description", desc);
            json_kv_str(&j, "class", cls);
            json_kv_str(&j, "manufacturer", mfg);
            json_kv_str(&j, "hardware_id", hw_id);
            json_kv_str(&j, "status", problem == 0 ? "OK" : "ERROR");
            json_kv_uint(&j, "error_code", problem);
            json_object_end(&j);
        }
        SetupDiDestroyDeviceInfoList(devs);
    }

    json_array_end(&j);

    result = json_finish(&j);
    send_text_response(sock, result);
    json_free(&j);
}
