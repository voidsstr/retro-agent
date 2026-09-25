/*
 * pcirescue.c - Win9x: bring back an INSTALLED PCI device the boot missed
 *
 * Found on .243 (Compaq Deskpro-class 430HX, Win98 SE) with a Voodoo 2 on
 * 2026-09-24. On every observed boot the BIOS left the card unconfigured
 * (command 0000, BAR0 0) and Win98's boot-time PCI enumeration created no
 * devnode for it - yet the card answered config cycles later in the session,
 * and a CM_Reenumerate_DevNode of the PCI bus found it, installed the driver
 * and assigned BAR0 (0x09000000, decode on, problem 0). So the device is
 * fine; the boot simply does not see it.
 *
 * Leaving it like that is not merely "no 3D". Glide finds Voodoo boards by
 * scanning config space itself, not through Windows, so a Glide game started
 * on such a boot enables a card whose BAR0 is 0 - i.e. over system RAM. That
 * is what took .243 down when glide2x.dll was first loaded there.
 *
 * So at every agent startup (the agent starts at logon, before anyone can
 * launch a game) this checks every PCI device that HAS a driver installed
 * (an Enum\PCI instance with a Driver value) for a live devnode. If one is
 * missing it re-enumerates the PCI bus and checks again. A device that was
 * physically removed just stays missing - re-enumerating a bus is harmless -
 * and is reported as such, not as a failure of this module.
 *
 * NT/2000/XP and later enumerate PCI themselves; this is a no-op there.
 * cfgmgr32 is resolved with LoadLibrary/GetProcAddress, never imported: a
 * static import Win9x cannot resolve kills the whole exe at load (CLAUDE.md).
 *
 * Registry (HKLM\Software\RetroAgent):
 *   PciRescue      REG_DWORD  0 = do not run at startup (PCIRESCAN still works)
 *   PciRescueBoot  REG_SZ     what the STARTUP pass found and did, written by
 *                             the agent. The log cannot be relied on for this:
 *                             on .243 the chat polls rotated the whole boot out
 *                             of agent.log + agent.log.1 within two hours, so
 *                             "did the rescue run, or did Windows find the card
 *                             itself?" had no answer. PCIRESCAN reports it as
 *                             last_boot.
 *
 * Command: PCIRESCAN - run it now; answers with JSON describing what was
 * missing before, which buses were re-enumerated, and what is missing after,
 * plus last_boot (the stored startup summary).
 */

#include "handlers.h"
#include "protocol.h"
#include "util.h"
#include "log.h"
#include <string.h>
#include <stdio.h>

#define LOG_PCIR "PCIRESCUE"
#define PCIR_MAX 16
#define PCIR_ID  200

typedef DWORD (WINAPI *pcir_locate_t)(DWORD *, const char *, ULONG);
typedef DWORD (WINAPI *pcir_reenum_t)(DWORD, ULONG);

typedef struct {
    int  installed;                     /* Enum\PCI instances with a Driver */
    int  n_missing_before, n_missing_after, n_buses;
    char missing_before[PCIR_MAX][PCIR_ID];
    char missing_after[PCIR_MAX][PCIR_ID];
    char buses[PCIR_MAX][PCIR_ID];
    DWORD bus_cr[PCIR_MAX];
    const char *error;                  /* static string, or NULL */
} pcir_result_t;

static volatile LONG g_pcir_busy = 0;

static int pcir_is_win9x(void)
{
    OSVERSIONINFOA o;
    o.dwOSVersionInfoSize = sizeof(o);
    if (!GetVersionExA(&o)) return 0;
    return o.dwPlatformId == VER_PLATFORM_WIN32_WINDOWS;
}

static int pcir_load(pcir_locate_t *loc, pcir_reenum_t *ren)
{
    HMODULE h = LoadLibraryA("cfgmgr32.dll");
    if (!h) return 0;
    *loc = (pcir_locate_t)GetProcAddress(h, "CM_Locate_DevNodeA");
    *ren = (pcir_reenum_t)GetProcAddress(h, "CM_Reenumerate_DevNode");
    return *loc && *ren;
}

/* Collect "PCI\<device>\<instance>" for every installed PCI instance whose
 * devnode Config Manager cannot locate. Returns the number found. */
static int pcir_find_missing(pcir_locate_t loc, char out[][PCIR_ID], int *installed)
{
    HKEY hpci;
    DWORD i;
    int n = 0;

    if (installed) *installed = 0;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "Enum\\PCI", 0, KEY_READ, &hpci)
            != ERROR_SUCCESS)
        return 0;
    for (i = 0; i < 256; i++) {
        char dev[96];
        DWORD cch = sizeof(dev), k;
        HKEY hdev;
        if (RegEnumKeyExA(hpci, i, dev, &cch, NULL, NULL, NULL, NULL) != ERROR_SUCCESS)
            break;
        if (_strnicmp(dev, "VEN_", 4) != 0) continue;       /* IRQHOLDER etc. */
        if (RegOpenKeyExA(hpci, dev, 0, KEY_READ, &hdev) != ERROR_SUCCESS) continue;
        for (k = 0; k < 16; k++) {
            char inst[64], drv[128], id[PCIR_ID];
            DWORD icch = sizeof(inst), dl = sizeof(drv), type = 0, dn = 0;
            HKEY hinst;
            if (RegEnumKeyExA(hdev, k, inst, &icch, NULL, NULL, NULL, NULL) != ERROR_SUCCESS)
                break;
            if (RegOpenKeyExA(hdev, inst, 0, KEY_READ, &hinst) != ERROR_SUCCESS) continue;
            drv[0] = 0;
            if (RegQueryValueExA(hinst, "Driver", NULL, &type, (LPBYTE)drv, &dl) != ERROR_SUCCESS
                    || type != REG_SZ)
                drv[0] = 0;
            RegCloseKey(hinst);
            if (!drv[0]) continue;                          /* never installed */
            if (installed) (*installed)++;
            _snprintf(id, sizeof(id) - 1, "PCI\\%s\\%s", dev, inst);
            id[sizeof(id) - 1] = 0;
            if (loc(&dn, id, 0) != 0 && n < PCIR_MAX) {     /* CR_SUCCESS == 0 */
                safe_strncpy(out[n], id, PCIR_ID);
                n++;
            }
        }
        RegCloseKey(hdev);
    }
    RegCloseKey(hpci);
    return n;
}

/* Re-enumerate every PCI bus devnode (*PNP0A03 under BIOS, ACPI or ROOT). */
static void pcir_reenumerate_buses(pcir_locate_t loc, pcir_reenum_t ren, pcir_result_t *r)
{
    static const char *const enumerators[] = { "BIOS", "ACPI", "ROOT" };
    int e;
    for (e = 0; e < 3; e++) {
        char path[64];
        HKEY hbus;
        DWORD i;
        _snprintf(path, sizeof(path) - 1, "Enum\\%s\\*PNP0A03", enumerators[e]);
        path[sizeof(path) - 1] = 0;
        if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, path, 0, KEY_READ, &hbus) != ERROR_SUCCESS)
            continue;
        for (i = 0; i < 8 && r->n_buses < PCIR_MAX; i++) {
            char inst[64], id[PCIR_ID];
            DWORD cch = sizeof(inst), dn = 0, cr;
            if (RegEnumKeyExA(hbus, i, inst, &cch, NULL, NULL, NULL, NULL) != ERROR_SUCCESS)
                break;
            _snprintf(id, sizeof(id) - 1, "%s\\*PNP0A03\\%s", enumerators[e], inst);
            id[sizeof(id) - 1] = 0;
            cr = loc(&dn, id, 0);
            if (cr == 0) cr = ren(dn, 0);
            else cr |= 0x80000000UL;          /* could not even locate it */
            safe_strncpy(r->buses[r->n_buses], id, PCIR_ID);
            r->bus_cr[r->n_buses] = cr;
            r->n_buses++;
            log_msg(LOG_PCIR, "re-enumerated %s -> cr=%08lX", id, (unsigned long)cr);
        }
        RegCloseKey(hbus);
    }
}

static void pcir_run(pcir_result_t *r, int force)
{
    pcir_locate_t loc = NULL;
    pcir_reenum_t ren = NULL;
    int i;

    memset(r, 0, sizeof(*r));
    if (!pcir_is_win9x()) { r->error = "not Win9x - Windows NT enumerates PCI itself"; return; }
    if (!pcir_load(&loc, &ren)) { r->error = "cfgmgr32.dll or its exports unavailable"; return; }

    r->n_missing_before = pcir_find_missing(loc, r->missing_before, &r->installed);
    for (i = 0; i < r->n_missing_before; i++)
        log_msg(LOG_PCIR, "installed but no devnode: %s", r->missing_before[i]);
    if (!r->n_missing_before && !force) {
        log_msg(LOG_PCIR, "all %d installed PCI device(s) present - nothing to do", r->installed);
        return;
    }

    pcir_reenumerate_buses(loc, ren, r);
    Sleep(3000);                        /* let Config Manager settle */
    r->n_missing_after = pcir_find_missing(loc, r->missing_after, NULL);
    log_msg(LOG_PCIR, "after re-enumeration: %d missing (was %d)%s",
            r->n_missing_after, r->n_missing_before,
            r->n_missing_after ? " - still missing means not answering on the bus (removed, or not seated)" : "");
}

/* One line saying what the startup pass saw and did, for PciRescueBoot. */
static void pcir_summary(const pcir_result_t *r, char *out, size_t cch)
{
    size_t n;
    int i;
    _snprintf(out, cch - 1, "agent %s, uptime %lus: ",
#ifdef AGENT_VERSION
              AGENT_VERSION,
#else
              "?",
#endif
              (unsigned long)(GetTickCount() / 1000));
    out[cch - 1] = 0;
    n = strlen(out);
    if (r->error) {
        _snprintf(out + n, cch - n - 1, "error: %s", r->error);
    } else if (!r->n_missing_before) {
        _snprintf(out + n, cch - n - 1,
                  "all %d installed PCI device(s) present - nothing to do", r->installed);
    } else {
        _snprintf(out + n, cch - n - 1,
                  "%d of %d installed PCI device(s) had no devnode, re-enumerated %d bus(es), "
                  "%d still missing -> %s;",
                  r->n_missing_before, r->installed, r->n_buses, r->n_missing_after,
                  r->n_missing_after < r->n_missing_before ? "RESCUED" : "NOT rescued");
        for (i = 0; i < r->n_missing_before; i++) {
            n = strlen(out);
            if (n + 2 >= cch) break;
            _snprintf(out + n, cch - n - 1, " %s", r->missing_before[i]);
        }
    }
    out[cch - 1] = 0;
}

static void pcir_store_boot(const char *summary)
{
    HKEY h;
    if (RegCreateKeyExA(HKEY_LOCAL_MACHINE, "Software\\RetroAgent", 0, NULL, 0,
                        KEY_WRITE, NULL, &h, NULL) != ERROR_SUCCESS) {
        log_msg(LOG_PCIR, "could not open HKLM\\Software\\RetroAgent to record the result");
        return;
    }
    RegSetValueExA(h, "PciRescueBoot", 0, REG_SZ, (const BYTE *)summary,
                   (DWORD)strlen(summary) + 1);
    RegCloseKey(h);
}

static void pcir_load_boot(char *out, DWORD cch)
{
    HKEY h;
    DWORD type = 0, sz = cch;
    out[0] = 0;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "Software\\RetroAgent", 0, KEY_READ, &h) != ERROR_SUCCESS)
        return;
    if (RegQueryValueExA(h, "PciRescueBoot", NULL, &type, (LPBYTE)out, &sz) != ERROR_SUCCESS
            || type != REG_SZ)
        out[0] = 0;
    out[cch - 1] = 0;
    RegCloseKey(h);
}

DWORD WINAPI pcirescue_thread(LPVOID param)
{
    char summary[1024];
    pcir_result_t r;
    DWORD enabled = 1, sz = sizeof(enabled), type = 0;
    HKEY h;
    (void)param;
    if (!pcir_is_win9x()) return 0;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "Software\\RetroAgent", 0, KEY_READ, &h) == ERROR_SUCCESS) {
        if (RegQueryValueExA(h, "PciRescue", NULL, &type, (LPBYTE)&enabled, &sz) != ERROR_SUCCESS
                || type != REG_DWORD)
            enabled = 1;
        RegCloseKey(h);
    }
    if (!enabled) {
        log_msg(LOG_PCIR, "disabled by PciRescue=0");
        pcir_store_boot("disabled by PciRescue=0");
        return 0;
    }
    if (InterlockedExchange((LONG *)&g_pcir_busy, 1)) return 0;
    pcir_run(&r, 0);
    InterlockedExchange((LONG *)&g_pcir_busy, 0);
    pcir_summary(&r, summary, sizeof(summary));
    pcir_store_boot(summary);
    return 0;
}

static void pcir_emit_list(json_t *j, const char *key, char list[][PCIR_ID], int n)
{
    int i;
    json_key(j, key);
    json_array_start(j);
    for (i = 0; i < n; i++) json_str(j, list[i]);
    json_array_end(j);
}

void handle_pcirescan(SOCKET sock, const char *args)
{
    char last_boot[1024];
    pcir_result_t r;
    json_t j;
    char *out;
    int i, force = args && _stricmp(args, "force") == 0;

    if (InterlockedExchange((LONG *)&g_pcir_busy, 1)) {
        send_error_response(sock, "PCIRESCAN already running");
        return;
    }
    pcir_run(&r, force);
    InterlockedExchange((LONG *)&g_pcir_busy, 0);

    json_init(&j);
    json_object_start(&j);
    if (r.error) json_kv_str(&j, "error", r.error);
    json_kv_int(&j, "installed_pci_devices", r.installed);
    pcir_emit_list(&j, "missing_before", r.missing_before, r.n_missing_before);
    json_key(&j, "buses_reenumerated");
    json_array_start(&j);
    for (i = 0; i < r.n_buses; i++) {
        json_object_start(&j);
        json_kv_str(&j, "id", r.buses[i]);
        json_kv_uint(&j, "cr", r.bus_cr[i]);
        json_object_end(&j);
    }
    json_array_end(&j);
    pcir_emit_list(&j, "missing_after", r.missing_after, r.n_missing_after);
    json_kv_bool(&j, "rescued", r.n_missing_before > 0 && r.n_missing_after < r.n_missing_before);
    pcir_load_boot(last_boot, sizeof(last_boot));
    json_kv_str(&j, "last_boot", last_boot);
    json_object_end(&j);
    out = json_finish(&j);
    if (!out) { send_error_response(sock, "PCIRESCAN: out of memory"); return; }
    send_text_response(sock, out);
    HeapFree(GetProcessHeap(), 0, out);
}
