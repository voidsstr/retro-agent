/*
 * monpower.c - keep the monitor awake; the screensaver is what runs when the
 * box is idle (agent 1.96.0). Decision logic and the story: agent/shared/monpower.h.
 *
 * Runs from retrowall_apply_startup() (after the screensaver is set - retrowall
 * stays the one owner of the screensaver) on every agent start of a MANAGED box,
 * and on demand as `MONPOWER apply`. `MONPOWER` alone reports and changes nothing.
 *
 * powrprof.dll is LoadLibrary'd and every entry point GetProcAddress'd: the
 * agent must import nothing a Win98 box cannot resolve (ntdyn.h), and the
 * Vista GUID functions do not exist on XP at all. Nothing here is linked.
 *
 * Registry (HKLM\Software\RetroAgent):
 *   MonitorNeverSleep  REG_DWORD  0 = leave power management alone
 *   MonitorPowerBoot   REG_SZ     what the last pass did (read it with REGREAD)
 */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stddef.h>
#include <powrprof.h>

#include "handlers.h"
#include "protocol.h"
#include "util.h"
#include "log.h"
#include "hostpolicy.h"
#include "../shared/monpower.h"

#define LOG_MP "MONPOWER"

/* GetProcAddress returns FARPROC; the casts below are the point (ntdyn.c). */
#pragma GCC diagnostic ignored "-Wcast-function-type"

/* The raw-blob offsets in monpower.h are what the native test decodes .243's
 * real scheme with; the compiler's struct must agree or that test proves
 * nothing about this code. */
typedef char mp_chk_idle_ac [offsetof(USER_POWER_POLICY, IdleTimeoutAc)  == MP_UPP_IDLE_AC  ? 1 : -1];
typedef char mp_chk_idle_dc [offsetof(USER_POWER_POLICY, IdleTimeoutDc)  == MP_UPP_IDLE_DC  ? 1 : -1];
typedef char mp_chk_video_ac[offsetof(USER_POWER_POLICY, VideoTimeoutAc) == MP_UPP_VIDEO_AC ? 1 : -1];
typedef char mp_chk_video_dc[offsetof(USER_POWER_POLICY, VideoTimeoutDc) == MP_UPP_VIDEO_DC ? 1 : -1];
typedef char mp_chk_guid[sizeof(mp_guid_t) == sizeof(GUID) ? 1 : -1];

#ifndef SPI_GETLOWPOWERACTIVE
#define SPI_GETLOWPOWERACTIVE 0x0053
#endif
#ifndef SPI_GETPOWEROFFACTIVE
#define SPI_GETPOWEROFFACTIVE 0x0054
#endif
#ifndef SPI_SETLOWPOWERACTIVE
#define SPI_SETLOWPOWERACTIVE 0x0055
#endif
#ifndef SPI_SETPOWEROFFACTIVE
#define SPI_SETPOWEROFFACTIVE 0x0056
#endif

/* ---- powrprof, resolved at run time ---------------------------------- */
typedef BOOLEAN (WINAPI *pGetActivePwrScheme)(PUINT);
typedef BOOLEAN (WINAPI *pReadPwrScheme)(UINT, PPOWER_POLICY);
/* LPWSTR on NT, LPSTR on 9x: the name is handed back exactly as
 * EnumPwrSchemes gave it, so the character width never matters here. */
typedef BOOLEAN (WINAPI *pWritePwrScheme)(PUINT, void *, void *, PPOWER_POLICY);
typedef BOOLEAN (WINAPI *pSetActivePwrScheme)(UINT, PGLOBAL_POWER_POLICY, PPOWER_POLICY);
typedef BOOLEAN (WINAPI *pGetCurrentPowerPolicies)(PGLOBAL_POWER_POLICY, PPOWER_POLICY);
typedef BOOLEAN (CALLBACK *pEnumProc)(UINT, DWORD, void *, DWORD, void *, PPOWER_POLICY, LPARAM);
typedef BOOLEAN (WINAPI *pEnumPwrSchemes)(pEnumProc, LPARAM);

typedef DWORD (WINAPI *pPowerGetActiveScheme)(HKEY, GUID **);
typedef DWORD (WINAPI *pPowerReadValueIndex)(HKEY, const GUID *, const GUID *, const GUID *, LPDWORD);
typedef DWORD (WINAPI *pPowerWriteValueIndex)(HKEY, const GUID *, const GUID *, const GUID *, DWORD);
typedef DWORD (WINAPI *pPowerSetActiveScheme)(HKEY, const GUID *);

#define MP_NFIELDS 6
static const char *const mp_field_names[MP_NFIELDS] = {
    "monitor_ac", "monitor_dc", "standby_ac", "standby_dc",
    "hibernate_ac", "hibernate_dc"
};

typedef struct {
    const char *api;            /* "old" | "new" | "none" */
    int  enabled;               /* MonitorNeverSleep */
    int  known;                 /* the scheme was read */
    unsigned long before[MP_NFIELDS];
    unsigned long after[MP_NFIELDS];
    int  changed;               /* scheme fields written */
    int  spi_poweroff_before, spi_poweroff_after;   /* -1 = n/a */
    int  spi_lowpower_before, spi_lowpower_after;
    int  spi_changed;
    unsigned long live_ac, live_dc;  /* GetCurrentPowerPolicies (old API) */
    int  ok;                    /* every enforced value reads back 0 */
    char msg[256];
} mp_result_t;

static volatile LONG g_mp_busy;

static int mp_os(unsigned long *major, int *is_nt)
{
    DWORD maj = 0;
    DWORD v = GetVersion();
    *is_nt = (v & 0x80000000UL) == 0;
    if (!host_os_version(&maj, NULL, NULL))
        maj = (DWORD)(LOBYTE(LOWORD(v)));
    *major = maj;
    return 1;
}

static int mp_switch_on(void)
{
    HKEY h;
    DWORD v = 1, sz = sizeof(v), type = 0;
    int present = 0;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "Software\\RetroAgent", 0, KEY_READ, &h) == ERROR_SUCCESS) {
        if (RegQueryValueExA(h, MP_REG_SWITCH, NULL, &type, (LPBYTE)&v, &sz) == ERROR_SUCCESS
                && type == REG_DWORD)
            present = 1;
        RegCloseKey(h);
    }
    return mp_enabled(present, v);
}

static void mp_store(const char *text)
{
    HKEY h;
    if (RegCreateKeyExA(HKEY_LOCAL_MACHINE, "Software\\RetroAgent", 0, NULL, 0,
                        KEY_SET_VALUE, NULL, &h, NULL) != ERROR_SUCCESS)
        return;
    RegSetValueExA(h, MP_REG_RESULT, 0, REG_SZ, (const BYTE *)text, (DWORD)strlen(text) + 1);
    RegCloseKey(h);
}

/* ---- old API (98 / XP) ------------------------------------------------ */

static void mp_old_fields(POWER_POLICY *pp, unsigned long **f)
{
    f[0] = &pp->user.VideoTimeoutAc;  f[1] = &pp->user.VideoTimeoutDc;
    f[2] = &pp->user.IdleTimeoutAc;   f[3] = &pp->user.IdleTimeoutDc;
    f[4] = &pp->mach.DozeS4TimeoutAc; f[5] = &pp->mach.DozeS4TimeoutDc;
}

typedef struct {
    UINT  want;
    int   found;
    DWORD name_len, desc_len;
    BYTE  name[512], desc[1024];
} mp_enum_ctx_t;

static BOOLEAN CALLBACK mp_enum_cb(UINT idx, DWORD nlen, void *name, DWORD dlen,
                                   void *desc, PPOWER_POLICY pp, LPARAM lp)
{
    mp_enum_ctx_t *c = (mp_enum_ctx_t *)lp;
    (void)pp;
    if (idx != c->want)
        return TRUE;
    /* Sizes are bytes including the terminator; keep two zero bytes after
     * whatever fits, which terminates an ANSI and a UTF-16 string alike. */
    memset(c->name, 0, sizeof(c->name));
    memset(c->desc, 0, sizeof(c->desc));
    if (name && nlen)
        memcpy(c->name, name, nlen < sizeof(c->name) - 2 ? nlen : sizeof(c->name) - 2);
    if (desc && dlen)
        memcpy(c->desc, desc, dlen < sizeof(c->desc) - 2 ? dlen : sizeof(c->desc) - 2);
    c->found = 1;
    return FALSE;       /* stop */
}

static void mp_run_old(HMODULE pp_dll, int do_write, mp_result_t *r)
{
    pGetActivePwrScheme gas = (pGetActivePwrScheme)GetProcAddress(pp_dll, "GetActivePwrScheme");
    pReadPwrScheme rps = (pReadPwrScheme)GetProcAddress(pp_dll, "ReadPwrScheme");
    pWritePwrScheme wps = (pWritePwrScheme)GetProcAddress(pp_dll, "WritePwrScheme");
    pSetActivePwrScheme sas = (pSetActivePwrScheme)GetProcAddress(pp_dll, "SetActivePwrScheme");
    pGetCurrentPowerPolicies gcp = (pGetCurrentPowerPolicies)GetProcAddress(pp_dll, "GetCurrentPowerPolicies");
    pEnumPwrSchemes eps = (pEnumPwrSchemes)GetProcAddress(pp_dll, "EnumPwrSchemes");
    POWER_POLICY pp, back;
    unsigned long *f[MP_NFIELDS];
    UINT id = 0;
    int i, n;

    r->api = "old";
    if (!gas || !rps) {
        _snprintf(r->msg, sizeof(r->msg), "FAILED: powrprof has no GetActivePwrScheme/ReadPwrScheme");
        return;
    }
    memset(&pp, 0, sizeof(pp));
    if (!gas(&id) || !rps(id, &pp)) {
        _snprintf(r->msg, sizeof(r->msg), "FAILED: could not read the active power scheme (error %lu)",
                  (unsigned long)GetLastError());
        return;
    }
    r->known = 1;
    mp_old_fields(&pp, f);
    for (i = 0; i < MP_NFIELDS; i++)
        r->before[i] = r->after[i] = *f[i];

    n = mp_zero_fields(f, MP_NFIELDS);          /* pp now holds the target */
    if (n && do_write) {
        mp_enum_ctx_t ctx;
        memset(&ctx, 0, sizeof(ctx));
        ctx.want = id;
        if (!wps || !sas || !eps) {
            _snprintf(r->msg, sizeof(r->msg), "FAILED: powrprof lacks WritePwrScheme/SetActivePwrScheme/EnumPwrSchemes");
            return;
        }
        eps((pEnumProc)mp_enum_cb, (LPARAM)&ctx);
        if (!ctx.found) {
            _snprintf(r->msg, sizeof(r->msg), "FAILED: active scheme %u not found by EnumPwrSchemes - not written", id);
            return;
        }
        if (!wps(&id, ctx.name, ctx.desc, &pp)) {
            _snprintf(r->msg, sizeof(r->msg), "FAILED: WritePwrScheme(%u) error %lu", id,
                      (unsigned long)GetLastError());
            return;
        }
        if (!sas(id, NULL, NULL))
            log_msg(LOG_MP, "SetActivePwrScheme(%u) failed (error %lu) - scheme written, not applied live",
                    id, (unsigned long)GetLastError());
        r->changed = n;
    }
    /* Read back what Windows now holds - the scheme, and the live policy. */
    memset(&back, 0, sizeof(back));
    if (rps(id, &back)) {
        mp_old_fields(&back, f);
        for (i = 0; i < MP_NFIELDS; i++)
            r->after[i] = *f[i];
    } else {
        _snprintf(r->msg, sizeof(r->msg), "FAILED: read-back of scheme %u failed", id);
        return;
    }
    if (gcp) {
        GLOBAL_POWER_POLICY g;
        POWER_POLICY live;
        memset(&live, 0, sizeof(live));
        if (gcp(&g, &live)) {
            r->live_ac = live.user.VideoTimeoutAc;
            r->live_dc = live.user.VideoTimeoutDc;
        }
    }
    r->ok = mp_all_never(r->after, MP_NFIELDS) && r->live_ac == 0 && r->live_dc == 0;
    if (do_write && (r->live_ac || r->live_dc))
        log_msg(LOG_MP, "WARNING: the LIVE policy still turns the monitor off (AC %lu s, DC %lu s)",
                r->live_ac, r->live_dc);
}

/* ---- new API (Vista+) ------------------------------------------------- */

static void mp_run_new(HMODULE pp_dll, int do_write, mp_result_t *r)
{
    static const mp_guid_t g_video = MP_GUID_VIDEO_SUBGROUP, g_videoidle = MP_GUID_VIDEOIDLE;
    static const mp_guid_t g_sleep = MP_GUID_SLEEP_SUBGROUP, g_standby = MP_GUID_STANDBYIDLE;
    static const mp_guid_t g_hib = MP_GUID_HIBERNATEIDLE;
    const GUID *sub[3], *set[3];
    pPowerGetActiveScheme gas = (pPowerGetActiveScheme)GetProcAddress(pp_dll, "PowerGetActiveScheme");
    pPowerReadValueIndex rac = (pPowerReadValueIndex)GetProcAddress(pp_dll, "PowerReadACValueIndex");
    pPowerReadValueIndex rdc = (pPowerReadValueIndex)GetProcAddress(pp_dll, "PowerReadDCValueIndex");
    pPowerWriteValueIndex wac = (pPowerWriteValueIndex)GetProcAddress(pp_dll, "PowerWriteACValueIndex");
    pPowerWriteValueIndex wdc = (pPowerWriteValueIndex)GetProcAddress(pp_dll, "PowerWriteDCValueIndex");
    pPowerSetActiveScheme sas = (pPowerSetActiveScheme)GetProcAddress(pp_dll, "PowerSetActiveScheme");
    GUID *scheme = NULL;
    int known[MP_NFIELDS];
    int i, n = 0, allknown = 1;
    DWORD rc, err = 0;

    sub[0] = (const GUID *)&g_video; set[0] = (const GUID *)&g_videoidle;
    sub[1] = (const GUID *)&g_sleep; set[1] = (const GUID *)&g_standby;
    sub[2] = (const GUID *)&g_sleep; set[2] = (const GUID *)&g_hib;

    r->api = "new";
    if (!gas || !rac || !rdc || !wac || !wdc || !sas) {
        _snprintf(r->msg, sizeof(r->msg), "FAILED: powrprof lacks the Power*ValueIndex API");
        return;
    }
    if ((rc = gas(NULL, &scheme)) != ERROR_SUCCESS || !scheme) {
        _snprintf(r->msg, sizeof(r->msg), "FAILED: PowerGetActiveScheme error %lu", (unsigned long)rc);
        return;
    }
    for (i = 0; i < MP_NFIELDS; i++) {
        DWORD v = 0;
        pPowerReadValueIndex rd = (i & 1) ? rdc : rac;
        known[i] = rd(NULL, scheme, sub[i / 2], set[i / 2], &v) == ERROR_SUCCESS;
        r->before[i] = r->after[i] = known[i] ? v : 0xFFFFFFFFUL;
        if (!known[i]) allknown = 0;
    }
    r->known = allknown;
    if (do_write) {
        for (i = 0; i < MP_NFIELDS; i++) {
            pPowerWriteValueIndex wr = (i & 1) ? wdc : wac;
            if (!mp_needs_write(known[i], r->before[i]))
                continue;
            rc = wr(NULL, scheme, sub[i / 2], set[i / 2], MP_NEVER);
            if (rc != ERROR_SUCCESS) {
                err = rc;
                log_msg(LOG_MP, "FAILED: write %s = 0 error %lu", mp_field_names[i], (unsigned long)rc);
            } else {
                n++;
            }
        }
        if (n && (rc = sas(NULL, scheme)) != ERROR_SUCCESS)
            log_msg(LOG_MP, "PowerSetActiveScheme error %lu - written, not applied live", (unsigned long)rc);
        r->changed = n;
    }
    for (i = 0; i < MP_NFIELDS; i++) {
        DWORD v = 0;
        pPowerReadValueIndex rd = (i & 1) ? rdc : rac;
        r->after[i] = rd(NULL, scheme, sub[i / 2], set[i / 2], &v) == ERROR_SUCCESS ? v : 0xFFFFFFFFUL;
    }
    LocalFree(scheme);
    r->ok = mp_all_never(r->after, MP_NFIELDS);
    if (err)
        _snprintf(r->msg, sizeof(r->msg), "FAILED: a Power*ValueIndex write returned %lu", (unsigned long)err);
}

/* ---- SPI (98 / XP) ---------------------------------------------------- */

static int mp_spi_one(UINT get, UINT setc, unsigned long major, int is_nt, int do_write,
                      int *before, int *after)
{
    BOOL on = FALSE;
    int get_ok;
    *before = *after = -1;
    if (is_nt && major >= 6)
        return 0;
    get_ok = SystemParametersInfoA(get, 0, &on, 0) != 0;
    if (!get_ok)
        return 0;
    *before = *after = on ? 1 : 0;
    if (!do_write || !mp_spi_should_clear(major, is_nt, get_ok, on))
        return 0;
    SystemParametersInfoA(setc, FALSE, NULL, SPIF_UPDATEINIFILE | SPIF_SENDWININICHANGE);
    on = TRUE;
    if (SystemParametersInfoA(get, 0, &on, 0))
        *after = on ? 1 : 0;
    return 1;
}

/* ---- the pass --------------------------------------------------------- */

static void mp_run(int do_write, mp_result_t *r)
{
    unsigned long major = 0;
    int is_nt = 0, api, i;
    HMODULE dll;

    memset(r, 0, sizeof(*r));
    r->api = "none";
    r->spi_poweroff_before = r->spi_poweroff_after = -1;
    r->spi_lowpower_before = r->spi_lowpower_after = -1;
    r->enabled = mp_switch_on();
    if (!r->enabled)
        do_write = 0;
    mp_os(&major, &is_nt);

    dll = LoadLibraryA("powrprof.dll");
    api = mp_pick_api(major, is_nt,
                      dll && GetProcAddress(dll, "PowerGetActiveScheme") != NULL,
                      dll && GetProcAddress(dll, "ReadPwrScheme") != NULL);
    if (api == MP_API_NEW)
        mp_run_new(dll, do_write, r);
    else if (api == MP_API_OLD)
        mp_run_old(dll, do_write, r);
    else
        _snprintf(r->msg, sizeof(r->msg), "FAILED: no powrprof.dll power-scheme API on this Windows");
    if (dll)
        FreeLibrary(dll);

    /* After the scheme: on XP the SPI flags mirror it. */
    r->spi_changed += mp_spi_one(SPI_GETPOWEROFFACTIVE, SPI_SETPOWEROFFACTIVE, major, is_nt,
                                 do_write, &r->spi_poweroff_before, &r->spi_poweroff_after);
    r->spi_changed += mp_spi_one(SPI_GETLOWPOWERACTIVE, SPI_SETLOWPOWERACTIVE, major, is_nt,
                                 do_write, &r->spi_lowpower_before, &r->spi_lowpower_after);
    if (r->spi_poweroff_after == 1 || r->spi_lowpower_after == 1)
        r->ok = 0;

    if (!r->msg[0]) {
        char vals[128];
        int len = 0;
        vals[0] = '\0';
        for (i = 0; i < MP_NFIELDS; i++)
            len += _snprintf(vals + len, sizeof(vals) - len, "%s%lu", i ? "/" : "", r->after[i]);
        if (!r->enabled)
            _snprintf(r->msg, sizeof(r->msg), "OFF: MonitorNeverSleep=0 - nothing changed (%s api, monitor/standby/hibernate AC/DC %s)",
                      r->api, vals);
        else if (!r->ok)
            _snprintf(r->msg, sizeof(r->msg), "%s: monitor/standby/hibernate AC/DC read back %s, poweroff=%d lowpower=%d (%s api)",
                      do_write ? "FAILED" : "NOT SET", vals, r->spi_poweroff_after, r->spi_lowpower_after, r->api);
        else if (r->changed || r->spi_changed)
            _snprintf(r->msg, sizeof(r->msg), "ok: set %d timeout(s) + %d SPI flag(s) to never (%s api); read back %s",
                      r->changed, r->spi_changed, r->api, vals);
        else
            _snprintf(r->msg, sizeof(r->msg), "ok: already never - nothing written (%s api)", r->api);
    }
}

void monpower_apply_startup(void)
{
    mp_result_t r;
    SYSTEMTIME st;
    char line[320];

    if (host_policy_skip("monpower (monitor power-off / standby timeouts)"))
        return;
    if (InterlockedExchange((LONG *)&g_mp_busy, 1))
        return;
    mp_run(1, &r);
    InterlockedExchange((LONG *)&g_mp_busy, 0);

    /* A settled box logs one line; a change or a failure logs what it was. */
    if (r.changed || r.spi_changed || !r.ok) {
        int i;
        for (i = 0; i < MP_NFIELDS; i++)
            if (r.before[i] != r.after[i])
                log_msg(LOG_MP, "%s %lu -> %lu s", mp_field_names[i], r.before[i], r.after[i]);
    }
    if (r.enabled && !r.ok)
        log_msg(LOG_MP, "*** MONITOR POWER-OFF NOT DISABLED *** %s", r.msg);
    else
        log_msg(LOG_MP, "%s", r.msg);
    GetLocalTime(&st);
    _snprintf(line, sizeof(line), "%04u-%02u-%02u %02u:%02u %s", st.wYear, st.wMonth, st.wDay,
              st.wHour, st.wMinute, r.msg);
    line[sizeof(line) - 1] = '\0';
    mp_store(line);
}

/* MONPOWER [apply] - the current values as JSON; `apply` enforces them now. */
void handle_monpower(SOCKET sock, const char *args)
{
    mp_result_t r;
    json_t j;
    char *out;
    char last[320];
    int i, apply = args && _stricmp(args, "apply") == 0;

    if (apply) {
        if (!host_manages_this_box()) {
            send_error_response(sock, "MONPOWER apply: this is a modern Windows host - the agent does not manage it");
            return;
        }
        monpower_apply_startup();       /* same pass, same log + record */
    }
    if (InterlockedExchange((LONG *)&g_mp_busy, 1)) {
        send_error_response(sock, "MONPOWER already running");
        return;
    }
    mp_run(0, &r);
    InterlockedExchange((LONG *)&g_mp_busy, 0);

    last[0] = '\0';
    {
        HKEY h;
        DWORD sz = sizeof(last) - 1, type = 0;
        if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "Software\\RetroAgent", 0, KEY_READ, &h) == ERROR_SUCCESS) {
            if (RegQueryValueExA(h, MP_REG_RESULT, NULL, &type, (LPBYTE)last, &sz) != ERROR_SUCCESS
                    || type != REG_SZ)
                last[0] = '\0';
            last[sizeof(last) - 1] = '\0';
            RegCloseKey(h);
        }
    }
    json_init(&j);
    json_object_start(&j);
    json_kv_str(&j, "api", r.api);
    json_kv_bool(&j, "enabled", r.enabled);
    json_kv_bool(&j, "scheme_read", r.known);
    for (i = 0; i < MP_NFIELDS; i++)
        json_kv_uint(&j, mp_field_names[i], (DWORD)r.after[i]);
    json_kv_uint(&j, "live_monitor_ac", (DWORD)r.live_ac);
    json_kv_uint(&j, "live_monitor_dc", (DWORD)r.live_dc);
    json_kv_int(&j, "spi_poweroff_active", r.spi_poweroff_after);
    json_kv_int(&j, "spi_lowpower_active", r.spi_lowpower_after);
    json_kv_bool(&j, "monitor_never_sleeps", r.ok);
    json_kv_str(&j, "state", r.msg);
    json_kv_str(&j, "last_pass", last);
    json_object_end(&j);
    out = json_finish(&j);
    if (!out) { send_error_response(sock, "MONPOWER: out of memory"); return; }
    send_text_response(sock, out);
    HeapFree(GetProcessHeap(), 0, out);
}
