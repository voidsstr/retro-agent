/*
 * ipxsetup.c - IPXSETUP: install the IPX/SPX protocol on every fleet box that
 * can have it, at every agent start and on demand (agent 1.97.0). The decisions
 * are agent/shared/ipxplan.h; NT 5.x installs through ipxnt.c (INetCfg),
 * Windows 98 SE through ipx9x.c (registry template + VxD payload).
 *
 *   IPXSETUP [status]               read-only: probe + registry + files, no COM
 *   IPXSETUP apply [force] [retry]  install now (refused on a modern host).
 *                                   force = ignore IpxSetup=0; retry = ignore the
 *                                   attempts cap / re-run a broken install. NT
 *                                   waits up to 120 s for the result; a Win9x
 *                                   (multiplex) agent never blocks its one
 *                                   serving thread - it answers "running" and the
 *                                   result is read with IPXSETUP status.
 *
 * The startup pass (ipxsetup_thread, spawn_helper'd from main.c) asks the host
 * policy FIRST, records PENDING, waits ~100 s and for clockfix, then does what
 * the command does with force/retry off - so a re-imaged XP box gets NWLink
 * back by itself (.123 lost its hand-installed NWLink to the 2026-08-27
 * re-image), and a box where it is already live writes nothing.
 *
 * ONE INSTALL AT A TIME: the INetCfg install and the Win98 registry install
 * take gamesync.c's driver-install lock (agent_install_lock_*), the one DRIVERS
 * UPDATE, DRVUPDATE and the startup driver passes share - they all flip
 * SetupAPI's non-interactive mode and would restore each other's saved state.
 *
 * REBOOTS: never on NT (reported as reboot_required - scripts/fleet/safe-reboot.py
 * owns that: the PXE hold and the activation check). On Win9x only with
 * IpxSetupReboot=1, ONCE per install, 20 minutes apart, never while GAMESYNC
 * copies, through the agent's own shell route (agent_self_reboot_9x) after
 * IpxRebootLast is written and the registry flushed - the postskip 1Bh pattern.
 * A self-reboot bypasses safe-reboot.py, so arm it only on a box that cannot
 * PXE-boot (.243's 3Com EtherLink III cannot).
 *
 * Registry (HKLM\Software\RetroAgent): IpxSetup, IpxSetupReboot,
 * IpxSetup9xTemplateOk, IpxPayloadPath (operator); IpxSetupBoot,
 * IpxSetupAttempts, IpxInstalledAt, IpxRebootLast (agent). See ipxplan.h.
 */
#include "ipxsetup.h"
#include "handlers.h"
#include "protocol.h"
#include "log.h"
#include "hostpolicy.h"
#include "bgwork.h"

#include <string.h>
#include <stdio.h>
#include <time.h>

/* GetProcAddress returns FARPROC; the casts below are the point (ntdyn.c). */
#pragma GCC diagnostic ignored "-Wcast-function-type"

/* After hwpublish (90 s) and before gameindex (120 s) - test_agent_startup_stagger.py. */
#define IPX_FIRST_DELAY_MS   100000
#define IPX_CLOCK_WAIT_MS    1200000     /* clockfix's worst case (postskip.c) */
#define IPX_NT_WATCHDOG_MS   600000      /* an INetCfg install that has not returned in 10 min hung */
#define IPX_CMD_WAIT_MS      120000      /* how long IPXSETUP apply waits on NT */
#define IPX_SYNC_WAIT_MS     7200000     /* a 9x reboot waits up to 2 h for GAMESYNC to finish */

enum { IPX_OUT_NONE = 0, IPX_OUT_NOTHING, IPX_OUT_INSTALLED, IPX_OUT_ALREADY, IPX_OUT_BUSY,
       IPX_OUT_REFUSED, IPX_OUT_FAILED, IPX_OUT_HUNG };

static const char *ipx_outcome_name(int o)
{
    switch (o) {
    case IPX_OUT_NOTHING:   return "nothing_to_do";
    case IPX_OUT_INSTALLED: return "installed";
    case IPX_OUT_ALREADY:   return "already_installed";
    case IPX_OUT_BUSY:      return "busy";
    case IPX_OUT_REFUSED:   return "refused";
    case IPX_OUT_FAILED:    return "failed";
    case IPX_OUT_HUNG:      return "hung";
    default:                return "none";
    }
}

typedef struct {
    int   outcome;              /* IPX_OUT_* */
    int   changed;              /* things the job changed (values + files, or 1 component) */
    int   reboot_required;
    char  msg[400];
    DWORD finished;             /* GetTickCount() */
} ipx_last_t;

static CRITICAL_SECTION g_ipx_cs;
static volatile LONG g_ipx_ready;
static HANDLE g_ipx_done;               /* manual-reset: signalled while no job runs */
static volatile LONG g_ipx_job_running;
static volatile LONG g_ipx_hung;        /* an INetCfg call never returned (this process) */
static ipx_last_t g_ipx_last;

void ipxsetup_init(void)
{
    if (InterlockedExchange(&g_ipx_ready, 1))
        return;
    InitializeCriticalSection(&g_ipx_cs);
    g_ipx_done = CreateEventA(NULL, TRUE, TRUE, NULL);
}

/* ---- registry ----------------------------------------------------------- */

static DWORD ipx_reg_dword(const char *name, DWORD dflt, int *present)
{
    HKEY h;
    DWORD v = dflt, sz = sizeof(v), type = 0;
    if (present)
        *present = 0;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "Software\\RetroAgent", 0, KEY_READ, &h) != ERROR_SUCCESS)
        return dflt;
    if (RegQueryValueExA(h, name, NULL, &type, (LPBYTE)&v, &sz) != ERROR_SUCCESS || type != REG_DWORD)
        v = dflt;
    else if (present)
        *present = 1;
    RegCloseKey(h);
    return v;
}

static void ipx_reg_set_dword(const char *name, DWORD v)
{
    HKEY h;
    if (RegCreateKeyExA(HKEY_LOCAL_MACHINE, "Software\\RetroAgent", 0, NULL, 0, KEY_SET_VALUE,
                        NULL, &h, NULL) != ERROR_SUCCESS)
        return;
    RegSetValueExA(h, name, 0, REG_DWORD, (const BYTE *)&v, sizeof(v));
    RegCloseKey(h);
}

static void ipx_store(const char *msg)
{
    HKEY h;
    SYSTEMTIME st;
    char line[480];
    GetLocalTime(&st);
    _snprintf(line, sizeof(line) - 1, "%04u-%02u-%02u %02u:%02u %s", st.wYear, st.wMonth, st.wDay,
              st.wHour, st.wMinute, msg);
    line[sizeof(line) - 1] = 0;
    if (RegCreateKeyExA(HKEY_LOCAL_MACHINE, "Software\\RetroAgent", 0, NULL, 0, KEY_SET_VALUE,
                        NULL, &h, NULL) != ERROR_SUCCESS)
        return;
    RegSetValueExA(h, IPX_REG_RESULT, 0, REG_SZ, (const BYTE *)line, (DWORD)strlen(line) + 1);
    RegCloseKey(h);
}

static void ipx_load(char *out, DWORD cch)
{
    HKEY h;
    DWORD type = 0, sz = cch - 1;
    out[0] = 0;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "Software\\RetroAgent", 0, KEY_READ, &h) != ERROR_SUCCESS)
        return;
    if (RegQueryValueExA(h, IPX_REG_RESULT, NULL, &type, (LPBYTE)out, &sz) != ERROR_SUCCESS
            || type != REG_SZ)
        out[0] = 0;
    out[cch - 1] = 0;
    RegCloseKey(h);
}

/* ---- the live probe ------------------------------------------------------ */

typedef struct {
    short          family;
    unsigned char  netnum[4];
    unsigned char  nodenum[6];
    unsigned short socket;
} ipx_sockaddr_t;               /* SOCKADDR_IPX, 14 bytes */

typedef int (WSAAPI *pWSAEnumProtocolsA_t)(LPINT, LPWSAPROTOCOL_INFOA, LPDWORD);

void ipx_probe(ipx_probe_t *p)
{
    SOCKET s;
    HMODULE ws;
    pWSAEnumProtocolsA_t enump = NULL;

    memset(p, 0, sizeof(*p));
    p->catalog_ipx = -1;
    s = socket(IPX_AF_IPX, SOCK_DGRAM, IPX_NSPROTO_IPX);
    if (s != INVALID_SOCKET) {
        ipx_sockaddr_t a;
        int len = sizeof(a);
        p->winsock_ipx = 1;
        memset(&a, 0, sizeof(a));
        a.family = IPX_AF_IPX;
        if (bind(s, (struct sockaddr *)&a, sizeof(a)) == 0 &&
            getsockname(s, (struct sockaddr *)&a, &len) == 0) {
            _snprintf(p->net, sizeof(p->net) - 1, "%02x%02x%02x%02x", a.netnum[0], a.netnum[1],
                      a.netnum[2], a.netnum[3]);
            _snprintf(p->node, sizeof(p->node) - 1, "%02x%02x%02x%02x%02x%02x", a.nodenum[0],
                      a.nodenum[1], a.nodenum[2], a.nodenum[3], a.nodenum[4], a.nodenum[5]);
        }
        closesocket(s);
    }
    /* The Winsock 2 catalog, through GetProcAddress (no new static import). */
    ws = GetModuleHandleA("ws2_32.dll");
    if (ws)
        enump = (pWSAEnumProtocolsA_t)GetProcAddress(ws, "WSAEnumProtocolsA");
    if (enump) {
        DWORD sz = 0;
        WSAPROTOCOL_INFOA *buf;
        enump(NULL, NULL, &sz);
        if (sz && sz < 256 * 1024) {
            buf = (WSAPROTOCOL_INFOA *)HeapAlloc(GetProcessHeap(), 0, sz);
            if (buf) {
                int n = enump(NULL, buf, &sz), i;
                if (n != SOCKET_ERROR) {
                    p->catalog_ipx = 0;
                    for (i = 0; i < n; i++)
                        if (buf[i].iAddressFamily == IPX_AF_IPX && buf[i].iSocketType == SOCK_DGRAM)
                            p->catalog_ipx = 1;
                }
                HeapFree(GetProcessHeap(), 0, buf);
            }
        }
    }
}

/* ---- observe ------------------------------------------------------------- */

typedef struct {
    int         is_nt;
    DWORD       major, minor, build;
    const char *why;
    ipx_obs_t   o;
    ipx_probe_t probe;
    ipxnt_info_t nt;
    ipx9x_info_t w9;
    int         state;
    int         template_ok;
    int         reboot_allowed;
} ipx_full_t;

static void ipx_os(int *is_nt, DWORD *major, DWORD *minor, DWORD *build)
{
    OSVERSIONINFOA v;
    memset(&v, 0, sizeof(v));
    v.dwOSVersionInfoSize = sizeof(v);
    GetVersionExA(&v);
    *is_nt = v.dwPlatformId == VER_PLATFORM_WIN32_NT;
    *major = v.dwMajorVersion;
    *minor = v.dwMinorVersion;
    *build = *is_nt ? v.dwBuildNumber : LOWORD(v.dwBuildNumber);
    if (*is_nt)
        host_os_version(major, minor, build);   /* the truth past Windows 8's shim */
}

static void ipx_observe(ipx_full_t *f)
{
    int present;
    DWORD v, ok;

    memset(f, 0, sizeof(*f));
    ipx_os(&f->is_nt, &f->major, &f->minor, &f->build);
    f->o.mech = ipx_mech_for(f->is_nt, f->major, f->minor, f->build, &f->why);
    f->o.managed = host_manages_this_box();
    v = ipx_reg_dword(IPX_REG_SWITCH, 1, &present);
    f->o.enabled = !present || v != 0;
    ipx_probe(&f->probe);
    f->o.winsock_ipx = f->probe.winsock_ipx;
    f->o.frame_configured = IPX_FRAME_AUTO;
    if (f->o.mech == IPX_MECH_NETCFG) {
        ipxnt_observe(&f->nt);
        f->o.component_present = f->nt.component_present;
        f->o.stack_loaded = f->nt.service_running == 1;
        f->o.files_present = f->nt.files_present;
        if (f->nt.nadapters)
            f->o.frame_configured = f->nt.ad[0].frame;
    } else if (f->o.mech == IPX_MECH_9X) {
        ipx9x_observe(&f->w9);
        /* the binding is what turns it on: keys without it are inert leftovers */
        f->o.component_present = f->w9.devnode_present && f->w9.bound;
        f->o.stack_loaded = f->w9.stack_loaded;
        f->o.files_present = f->w9.files_present;
        f->o.frame_configured = f->w9.frame;
    }
    f->o.installed_at = ipx_reg_dword(IPX_REG_INSTALLED, 0, NULL);
    f->o.boot_time = (unsigned long)time(NULL) - GetTickCount() / 1000UL;
    f->o.attempts = (int)ipx_reg_dword(IPX_REG_ATTEMPTS, 0, NULL);
    f->o.hung = g_ipx_hung != 0;
    f->state = ipx_decide(&f->o);
    ok = ipx_reg_dword(IPX_REG_TEMPLATE_OK, 0, &present);
    f->template_ok = ipx_9x_writes_allowed(present, ok);
    f->reboot_allowed = ipx_reg_dword(IPX_REG_REBOOT, 0, NULL) == 1;
}

static void ipx_describe(const ipx_full_t *f, char *out, size_t cch)
{
    switch (f->state) {
    case IPX_ST_NOT_SUPPORTED:
        _snprintf(out, cch - 1, "%s", f->why);
        break;
    case IPX_ST_POLICY:
        _snprintf(out, cch - 1, "modern Windows host - the agent does not manage it");
        break;
    case IPX_ST_ACTIVE:
        _snprintf(out, cch - 1, "ok: active (IPX net %s node %s, frame %s)",
                  f->probe.net[0] ? f->probe.net : "?", f->probe.node[0] ? f->probe.node : "?",
                  ipx_frame_name(f->o.frame_configured));
        break;
    case IPX_ST_FRAME_MISMATCH:
        _snprintf(out, cch - 1, "active, but frame type %s - the fleet uses %s; left as it is "
                  "(the install never changes an existing value)",
                  ipx_frame_name(f->o.frame_configured), ipx_frame_name(ipx_frame_wanted(f->o.mech)));
        break;
    case IPX_ST_PENDING_REBOOT:
        _snprintf(out, cch - 1, "installed - NOT ACTIVE UNTIL THE NEXT REBOOT%s",
                  f->o.mech == IPX_MECH_NETCFG ? " (use scripts/fleet/safe-reboot.py)"
                  : f->reboot_allowed ? "" : " (IpxSetupReboot is not 1: the agent will not reboot)");
        break;
    case IPX_ST_BROKEN:
        _snprintf(out, cch - 1, "BROKEN: installed but Winsock opens no IPX socket (stack %s, "
                  "Winsock 2 catalog %s) - IPXSETUP apply retry re-runs the install",
                  f->o.stack_loaded ? "loaded" : "not loaded",
                  f->probe.catalog_ipx == 1 ? "lists IPX" : f->probe.catalog_ipx == 0
                  ? "has no IPX" : "unknown");
        break;
    case IPX_ST_HUNG:
        _snprintf(out, cch - 1, "HUNG: an install never returned - restart the agent");
        break;
    case IPX_ST_DISABLED:
        _snprintf(out, cch - 1, "not installed - IpxSetup=0 (IPXSETUP apply force installs anyway)");
        break;
    case IPX_ST_GAVE_UP:
        _snprintf(out, cch - 1, "not installed after %d attempt(s) - IPXSETUP apply retry",
                  f->o.attempts);
        break;
    case IPX_ST_PAYLOAD_MISSING:
        _snprintf(out, cch - 1, "not installed - the in-box NWLink files are missing "
                  "(inf\\netnwlnk.inf, drivers\\nwlnkipx.sys or Driver Cache\\i386\\driver.cab)");
        break;
    default:
        _snprintf(out, cch - 1, "not installed");
        break;
    }
    out[cch - 1] = 0;
}

static void ipx_emit(json_t *j, const ipx_full_t *f)
{
    char os[96], last[480], desc[400];
    int i;

    _snprintf(os, sizeof(os) - 1, "%s %lu.%lu.%lu", ipx_os_name(f->is_nt, f->major, f->minor),
              (unsigned long)f->major, (unsigned long)f->minor, (unsigned long)f->build);
    os[sizeof(os) - 1] = 0;
    json_kv_str(j, "os", os);
    json_kv_str(j, "mechanism", ipx_mech_name(f->o.mech));
    json_kv_bool(j, "supported", f->o.mech != IPX_MECH_NONE);
    if (f->o.mech == IPX_MECH_NONE)
        json_kv_str(j, "why", f->why);
    json_kv_bool(j, "managed", f->o.managed);
    json_kv_bool(j, "enabled", f->o.enabled);
    json_kv_str(j, "state", ipx_state_name(f->state));
    ipx_describe(f, desc, sizeof(desc));
    json_kv_str(j, "detail", desc);
    json_kv_bool(j, "winsock_ipx", f->probe.winsock_ipx);
    json_key(j, "catalog_ipx");
    if (f->probe.catalog_ipx < 0)
        json_null(j);
    else
        json_bool(j, f->probe.catalog_ipx);
    json_kv_bool(j, "component_present", f->o.component_present);
    json_kv_bool(j, "stack_loaded", f->o.stack_loaded);
    json_kv_bool(j, "files_present", f->o.files_present);
    if (f->probe.winsock_ipx) {
        json_key(j, "address");
        json_object_start(j);
        json_kv_str(j, "net", f->probe.net);
        json_kv_str(j, "node", f->probe.node);
        json_object_end(j);
    }
    json_key(j, "frame_type");
    json_object_start(j);
    json_kv_str(j, "wanted", ipx_frame_name(ipx_frame_wanted(f->o.mech)));
    json_kv_str(j, "configured", ipx_frame_name(f->o.frame_configured));
    if (f->o.mech == IPX_MECH_NETCFG) {
        json_key(j, "adapters");
        json_array_start(j);
        for (i = 0; i < f->nt.nadapters; i++) {
            json_object_start(j);
            json_kv_str(j, "adapter", f->nt.ad[i].name);
            json_kv_str(j, "frame", ipx_frame_name(f->nt.ad[i].frame));
            json_object_end(j);
        }
        json_array_end(j);
    }
    json_object_end(j);
    if (f->o.mech == IPX_MECH_NETCFG) {
        json_key(j, "nt");
        json_object_start(j);
        json_kv_str(j, "component_key", f->nt.comp_key);
        json_kv_bool(j, "service_present", f->nt.service_present);
        json_kv_int(j, "service_running", f->nt.service_running);
        json_object_end(j);
    } else if (f->o.mech == IPX_MECH_9X) {
        char k[48];
        json_key(j, "win98");
        json_object_start(j);
        json_kv_bool(j, "template_ok", f->template_ok);
        json_kv_int(j, "nic_candidates", f->w9.nic_count);
        json_kv_str(j, "nic", f->w9.nic);
        json_kv_str(j, "nic_desc", f->w9.nic_desc);
        _snprintf(k, sizeof(k) - 1, "%s%s", f->w9.cls[0] ? "NetTrans\\" : "", f->w9.cls);
        k[sizeof(k) - 1] = 0;
        json_kv_str(j, "class_key", k);
        _snprintf(k, sizeof(k) - 1, "%s%s", f->w9.inst[0] ? "Network\\NWLINK\\" : "", f->w9.inst);
        k[sizeof(k) - 1] = 0;
        json_kv_str(j, "devnode", f->w9.devnode_present ? k : "");
        json_kv_bool(j, "bound", f->w9.bound);
        json_object_end(j);
    }
    json_kv_bool(j, "reboot_required", f->state == IPX_ST_PENDING_REBOOT);
    json_kv_bool(j, "auto_reboot_allowed", f->o.mech == IPX_MECH_9X && f->reboot_allowed);
    json_kv_int(j, "attempts", f->o.attempts);
    json_kv_uint(j, "installed_at", (DWORD)f->o.installed_at);
    ipx_load(last, sizeof(last));
    json_kv_str(j, "last_pass", last);
}

/* ---- the reboot hand-off (Win9x only) ----------------------------------- */

/* After an install of ours is in place and pending, reboot ONCE to make it
 * live - only when the operator armed it. Order matters and is pinned by
 * tests/python/test_ipxsetup_agent.py: read IpxSetupReboot, write
 * IpxRebootLast, RegFlushKey, THEN the shell's reboot. */
static void ipx_reboot_if_armed_9x(void)
{
    ipx_full_t f;
    DWORD allow = ipx_reg_dword(IPX_REG_REBOOT, 0, NULL);
    DWORD waited = 0, now, last;
    long since;
    HKEY h;

    if (allow != 1) {
        log_msg(LOG_IPX, "not rebooting: IpxSetupReboot is not 1 - IPX/SPX becomes active at the "
                         "next restart of Windows");
        return;
    }
    /* never mid-copy: wait (bounded) for a library sync to finish */
    while (gamesync_busy() && waited < IPX_SYNC_WAIT_MS) {
        if (!waited)
            log_msg(LOG_IPX, "reboot deferred: GAMESYNC is copying - waiting for it to finish");
        Sleep(60000);
        waited += 60000;
    }
    ipx_observe(&f);
    now = (DWORD)time(NULL);
    last = ipx_reg_dword(IPX_REG_REBOOTLAST, 0, NULL);
    since = last ? (long)(now - last) : -1;
    if (!ipx_should_reboot(f.o.mech, 1, f.state, f.o.installed_at, last, since,
                           IPX_REBOOT_GAP_S, gamesync_busy())) {
        log_msg(LOG_IPX, "not rebooting: state %s, installed_at %lu, last IPXSETUP reboot %lu "
                         "(%ld s ago), GAMESYNC %s", ipx_state_name(f.state),
                (unsigned long)f.o.installed_at, (unsigned long)last, since,
                gamesync_busy() ? "copying" : "idle");
        return;
    }
    if (RegCreateKeyExA(HKEY_LOCAL_MACHINE, "Software\\RetroAgent", 0, NULL, 0, KEY_WRITE, NULL,
                        &h, NULL) != ERROR_SUCCESS) {
        log_msg(LOG_IPX, "not rebooting: cannot record IpxRebootLast (a reboot that cannot be "
                         "recorded could repeat)");
        return;
    }
    RegSetValueExA(h, IPX_REG_REBOOTLAST, 0, REG_DWORD, (const BYTE *)&now, sizeof(now));
    RegFlushKey(h);                         /* Win9x writes its registry lazily */
    RegCloseKey(h);
    RegFlushKey(HKEY_LOCAL_MACHINE);
    ipx_store("REBOOTING once so the IPX/SPX protocol loads (IpxSetupReboot=1)");
    log_msg(LOG_IPX, "REBOOTING once so the IPX/SPX protocol loads (IpxSetupReboot=1)");
    log_flush();
    agent_self_reboot_9x("IPXSETUP");
}

/* ---- the pass ------------------------------------------------------------- */

static void ipx_set_last(int outcome, int changed, int reboot_required, const char *msg)
{
    EnterCriticalSection(&g_ipx_cs);
    g_ipx_last.outcome = outcome;
    g_ipx_last.changed = changed;
    g_ipx_last.reboot_required = reboot_required;
    lstrcpynA(g_ipx_last.msg, msg, sizeof(g_ipx_last.msg));
    g_ipx_last.finished = GetTickCount();
    LeaveCriticalSection(&g_ipx_cs);
}

/* Observe, decide, act, observe again, record. manual = a command asked
 * (does not wait for the install lock). Runs on a background thread. */
static void ipx_run(int force, int retry, int manual)
{
    ipx_full_t f;
    char msg[480], desc[400];
    int plan, outcome = IPX_OUT_NOTHING, changed = 0, reboot_required = 0;
    DWORD attempts;

    ipx_observe(&f);
    if (f.state == IPX_ST_ACTIVE && f.o.attempts)
        ipx_reg_set_dword(IPX_REG_ATTEMPTS, 0);     /* reset once it is live */
    plan = ipx_plan(f.state, &f.o, force, retry);
    ipx_describe(&f, desc, sizeof(desc));

    if (f.o.mech == IPX_MECH_NONE || !f.o.managed) {
        _snprintf(msg, sizeof(msg) - 1, "%s", desc);
        outcome = IPX_OUT_REFUSED;
        goto record;
    }
    if (plan == IPX_DO_NOTHING) {
        _snprintf(msg, sizeof(msg) - 1, "%s - nothing changed", desc);
        goto record;
    }
    if (f.o.mech == IPX_MECH_9X && !f.template_ok) {
        _snprintf(msg, sizeof(msg) - 1, "SKIPPED: %s - the Win98 registry template has not been "
                  "validated against a golden install yet (%s is not 1), so nothing is written",
                  desc, IPX_REG_TEMPLATE_OK);
        outcome = IPX_OUT_REFUSED;
        goto record;
    }
    if (manual ? !agent_install_lock_enter() : !agent_install_lock_wait()) {
        _snprintf(msg, sizeof(msg) - 1, "BUSY: another driver install holds the install lock - "
                  "not installed this time");
        outcome = IPX_OUT_BUSY;
        goto record;
    }
    attempts = (DWORD)f.o.attempts + 1;
    ipx_reg_set_dword(IPX_REG_ATTEMPTS, attempts);  /* before: a crash mid-install still counts */

    if (f.o.mech == IPX_MECH_NETCFG) {
        ipxnt_result_t r;
        int rc = ipxnt_install(&r, IPX_NT_WATCHDOG_MS);
        if (rc == IPXNT_HUNG) {
            /* The lock stays held (as gs_force_install's does): nothing else may
             * start an install under a thread still inside INetCfg. */
            InterlockedExchange(&g_ipx_hung, 1);
            _snprintf(msg, sizeof(msg) - 1, "*** IPX/SPX INSTALL HUNG *** INetCfg did not return "
                      "in %lu s - look at the console for a dialog; restart the agent to retry",
                      (unsigned long)(IPX_NT_WATCHDOG_MS / 1000));
            outcome = IPX_OUT_HUNG;
            goto record;
        }
        agent_install_lock_leave();
        switch (rc) {
        case IPXNT_INSTALLED:
        case IPXNT_REBOOT:
            ipx_reg_set_dword(IPX_REG_INSTALLED, (DWORD)time(NULL));
            changed = 1;
            outcome = IPX_OUT_INSTALLED;
            reboot_required = rc == IPXNT_REBOOT;
            _snprintf(msg, sizeof(msg) - 1, "%s", rc == IPXNT_REBOOT
                      ? "*** IPX/SPX INSTALLED - WINDOWS ASKS FOR A REBOOT *** (NETCFG_S_REBOOT: "
                        "the agent never reboots an NT box - use scripts/fleet/safe-reboot.py)"
                      : "installed NWLink IPX/SPX (MS_NWIPX) through INetCfg - no reboot needed");
            break;
        case IPXNT_ALREADY:
            outcome = IPX_OUT_ALREADY;
            _snprintf(msg, sizeof(msg) - 1, "MS_NWIPX is already installed (INetCfg FindComponent) "
                      "- nothing changed");
            break;
        case IPXNT_BUSY:
            ipx_reg_set_dword(IPX_REG_ATTEMPTS, attempts - 1);  /* nothing was tried */
            outcome = IPX_OUT_BUSY;
            _snprintf(msg, sizeof(msg) - 1, "BUSY: the network-configuration write lock is held "
                      "by \"%s\" - retried at the next start", r.holder[0] ? r.holder : "?");
            break;
        default:
            outcome = IPX_OUT_FAILED;
            _snprintf(msg, sizeof(msg) - 1, "*** IPX/SPX NOT INSTALLED *** %s failed, HRESULT "
                      "0x%08lX", r.step, (unsigned long)r.hr);
            break;
        }
    } else {
        ipx9x_result_t r;
        int ok = ipx9x_install(&r, f.probe.catalog_ipx == 1);
        agent_install_lock_leave();
        if (ok && (r.values_written || r.files_copied)) {
            ipx_reg_set_dword(IPX_REG_INSTALLED, (DWORD)time(NULL));
            changed = r.values_written + r.files_copied;
            outcome = IPX_OUT_INSTALLED;
            reboot_required = 1;
            _snprintf(msg, sizeof(msg) - 1, "*** IPX/SPX INSTALLED - NOT ACTIVE UNTIL THE NEXT "
                      "REBOOT *** %s", r.msg);
        } else if (ok) {
            outcome = IPX_OUT_ALREADY;
            _snprintf(msg, sizeof(msg) - 1, "%s", r.msg);
        } else {
            outcome = strncmp(r.msg, "REFUSED", 7) == 0 ? IPX_OUT_REFUSED : IPX_OUT_FAILED;
            _snprintf(msg, sizeof(msg) - 1, "*** IPX/SPX NOT INSTALLED *** %s", r.msg);
            /* Every refusal comes before the first write, so it costs no
             * attempt: a share that was unreachable at two boots must not
             * stop the automatic install for good. It still says so, loudly,
             * at every start. Only a FAILED write counts toward the cap. */
            if (outcome == IPX_OUT_REFUSED)
                ipx_reg_set_dword(IPX_REG_ATTEMPTS, attempts - 1);
        }
    }

record:
    msg[sizeof(msg) - 1] = 0;
    if (outcome == IPX_OUT_INSTALLED && f.o.mech == IPX_MECH_NETCFG) {
        /* NWLink is live at once on XP; give the adapter binding a moment */
        int i;
        for (i = 0; i < 10; i++) {
            ipx_probe_t p;
            ipx_probe(&p);
            if (p.winsock_ipx)
                break;
            Sleep(1000);
        }
    }
    if (outcome != IPX_OUT_NOTHING || manual)
        log_msg(LOG_IPX, "%s", msg);
    else
        log_msg(LOG_IPX, "ipxsetup: %s", msg);
    ipx_store(msg);
    ipx_set_last(outcome, changed, reboot_required, msg);

    /* The post-condition, observed again: live now (XP) resets the attempts;
     * pending (Win9x) is where the one armed reboot happens - never on NT. */
    {
        ipx_full_t g;
        ipx_observe(&g);
        if (g.state == IPX_ST_ACTIVE && g.o.attempts)
            ipx_reg_set_dword(IPX_REG_ATTEMPTS, 0);
        if (g.o.mech == IPX_MECH_9X && outcome != IPX_OUT_HUNG && g.state == IPX_ST_PENDING_REBOOT)
            ipx_reboot_if_armed_9x();
    }
}

/* ---- the startup pass ----------------------------------------------------- */

DWORD WINAPI ipxsetup_thread(LPVOID param)
{
    DWORD waited = 0;
    (void)param;
    thread_background();
    /* A modern host gets NOTHING from this thread - checked FIRST. */
    if (host_policy_skip("ipxsetup (IPX/SPX protocol install)"))
        return 0;
    ipxsetup_init();
    {
        ipx_full_t f;
        ipx_observe(&f);
        if (f.o.mech == IPX_MECH_NONE) {
            ipx_store(f.why);               /* Win7: say so once, do nothing */
            return 0;
        }
    }
    /* Until this start's pass finishes, the previous boot's answer must not be
     * read as this boot's (the PostSkipBoot convention). */
    ipx_store("PENDING: this start's pass has not finished yet");
    if (!agent_nap(IPX_FIRST_DELAY_MS))
        return 0;
    /* clockfix sets a 1980 clock from the NAS on .243: IpxInstalledAt and the
     * boot time it is compared with must be real dates. */
    while (!clockfix_finished() && waited < IPX_CLOCK_WAIT_MS) {
        Sleep(1000);
        waited += 1000;
    }
    if (InterlockedCompareExchange(&g_ipx_job_running, 1, 0) != 0) {
        ipx_store("SKIPPED at startup: an IPXSETUP command's job was already running");
        return 0;
    }
    ResetEvent(g_ipx_done);
    ipx_run(0, 0, 0);
    InterlockedExchange(&g_ipx_job_running, 0);
    SetEvent(g_ipx_done);
    return 0;
}

/* ---- the command ----------------------------------------------------------- */

typedef struct { int force, retry; } ipx_job_args_t;

static DWORD WINAPI ipx_job_thread(LPVOID arg)
{
    ipx_job_args_t a = *(ipx_job_args_t *)arg;
    HeapFree(GetProcessHeap(), 0, arg);
    thread_background();
    ipx_run(a.force, a.retry, 1);
    InterlockedExchange(&g_ipx_job_running, 0);
    SetEvent(g_ipx_done);
    return 0;
}

void handle_ipxsetup(SOCKET sock, const char *args)
{
    int a = ipx_parse_args(args ? args : ""), started = 0, finished = 0;
    ipx_full_t f;
    ipx_last_t last;
    json_t j;
    char *out;

    if (a & IPX_ARG_BAD) {
        send_error_response(sock, "IPXSETUP: usage IPXSETUP [status] | IPXSETUP apply [force] [retry]");
        return;
    }
    ipxsetup_init();
    if (a & IPX_ARG_APPLY) {
        ipx_job_args_t *ja;
        HANDLE th;
        DWORD tid = 0;
        if (!host_manages_this_box()) {
            send_error_response(sock, "IPXSETUP apply: this is a modern Windows host - the agent "
                                      "does not manage it");
            return;
        }
        ipx_observe(&f);
        if (f.o.mech == IPX_MECH_NONE) {
            char e[400];
            _snprintf(e, sizeof(e) - 1, "IPXSETUP apply: %s", f.why);
            e[sizeof(e) - 1] = 0;
            send_error_response(sock, e);
            return;
        }
        if (InterlockedCompareExchange(&g_ipx_job_running, 1, 0) == 0) {
            ja = (ipx_job_args_t *)HeapAlloc(GetProcessHeap(), 0, sizeof(*ja));
            if (ja) {
                ja->force = (a & IPX_ARG_FORCE) != 0;
                ja->retry = (a & IPX_ARG_RETRY) != 0;
                ResetEvent(g_ipx_done);
                th = CreateThread(NULL, 0, ipx_job_thread, ja, 0, &tid);
                if (th) {
                    CloseHandle(th);
                    started = 1;
                } else {
                    HeapFree(GetProcessHeap(), 0, ja);
                }
            }
            if (!started) {
                InterlockedExchange(&g_ipx_job_running, 0);
                SetEvent(g_ipx_done);
                send_error_response(sock, "IPXSETUP apply: could not start the install thread");
                return;
            }
        }
        /* A Win9x agent serves every client from this one thread: never block it. */
        if (!agent_multiplex_mode())
            finished = WaitForSingleObject(g_ipx_done, IPX_CMD_WAIT_MS) == WAIT_OBJECT_0;
    }

    ipx_observe(&f);
    EnterCriticalSection(&g_ipx_cs);
    last = g_ipx_last;
    LeaveCriticalSection(&g_ipx_cs);

    json_init(&j);
    json_object_start(&j);
    ipx_emit(&j, &f);
    json_key(&j, "job");
    json_object_start(&j);
    json_kv_bool(&j, "running", g_ipx_job_running != 0);
    if (a & IPX_ARG_APPLY)
        json_kv_bool(&j, "started_now", started);
    json_kv_str(&j, "last_outcome", ipx_outcome_name(last.outcome));
    json_kv_int(&j, "last_changed", last.changed);
    json_kv_str(&j, "last_message", last.msg);
    if (last.finished)
        json_kv_uint(&j, "last_finished_s_ago", (GetTickCount() - last.finished) / 1000);
    json_object_end(&j);
    if (a & IPX_ARG_APPLY) {
        json_kv_str(&j, "outcome", finished ? ipx_outcome_name(last.outcome) : "running");
        json_kv_int(&j, "changed", finished ? last.changed : 0);
    }
    json_object_end(&j);
    out = json_finish(&j);
    if (!out) {
        send_error_response(sock, "IPXSETUP: out of memory");
        return;
    }
    send_text_response(sock, out);
    HeapFree(GetProcessHeap(), 0, out);
}

/* ---- HWPROFILE ----------------------------------------------------------- */

/* "ipx":{state, mechanism, winsock, installed, net, node} - kept OUT of
 * profile_hash (it is state, not hardware), so inventory.py can render the
 * fleet's IPX table from what each box measures instead of a stale note. */
void ipxsetup_emit_hwprofile(json_t *j)
{
    ipx_full_t f;
    ipx_observe(&f);
    json_key(j, "ipx");
    json_object_start(j);
    json_kv_str(j, "state", ipx_state_name(f.state));
    json_kv_str(j, "mechanism", ipx_mech_name(f.o.mech));
    json_kv_bool(j, "winsock", f.probe.winsock_ipx);
    json_kv_bool(j, "installed", f.o.component_present);
    json_kv_str(j, "net", f.probe.net);
    json_kv_str(j, "node", f.probe.node);
    json_kv_str(j, "frame", ipx_frame_name(f.o.frame_configured));
    json_object_end(j);
}
