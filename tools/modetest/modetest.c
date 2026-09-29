/*
 * modetest.exe - what does a fullscreen mode change that names NO refresh
 * rate actually land on, on THIS box's OS and display driver?
 *
 * Why it exists (the refresh-per-title work, 2026-09-29): the fleet wants each
 * Quake-era title at the highest refresh its monitor supports AT THE TITLE'S
 * OWN RESOLUTION. Most of those engines set their mode naming no rate - WON
 * hl.exe sub_464070 builds dmFields 0x1C0000 (BITSPERPEL|PELSWIDTH|PELSHEIGHT)
 * + CDS_FULLSCREEN, Quake II's ref_gl 0x180000 (no depth either) - so what
 * they get is whatever the OS + driver picks for "no rate". The only
 * measurement this project has (agent/tools/refreshlogic.h: .124, GeForce2 GTS,
 * ForceWare 71.89, 2026-08-25) says XP picked 60 Hz with the desktop already
 * at 100 Hz at that same resolution. Whether a LAUNCHER could pre-switch to
 * the best rate so that the game's no-rate request keeps it is a per-driver
 * fact, and this tool measures it - nothing else here assumes it.
 *
 * TESTS (any combination; each one restores the persisted mode after itself):
 *   -a            ChangeDisplaySettings(W,H,BPP, no rate, CDS_FULLSCREEN)
 *   -b <hz|max>   CDS at <hz> (CDS_FULLSCREEN), then the no-rate request again:
 *                 does the game's request KEEP the pre-switched rate?
 *   -c            DirectDraw: exclusive + SetDisplayMode(W,H,BPP, 0) - the
 *                 D3D5/6/7-era path (UE1 D3DDrv, the Westwood titles, Glide
 *                 on the H5 via minihwc win_mode.c setVideoMode)
 *   -cb <hz|max>  CDS at <hz>, then the DirectDraw rate-0 request
 *   -d            Direct3D 9: fullscreen CreateDevice, FullScreen_RefreshRateInHz 0
 *   -nobpp        make the no-rate CDS request W|H only (Quake II's 0x180000)
 *   -list         change NOTHING: OS, current + persisted mode, EDID, every
 *                 mode the driver lists, and the TIMED refresh of the desktop
 *   -restore      recovery: put the persisted (registry) mode back, paced
 *   -dry          check every request and print the plan; switch nothing
 *   -pace <s>     seconds between switches (5..30, default 6)
 *   -hold <s>     seconds each tested mode is held (2..20, default 4)
 *   -maxhz <n>    operator ceiling for a monitor with no EDID
 *   -novb         skip the vertical-blank timing
 *   -tag <word>   a label for this run in the appended log (the host's runner)
 *
 * WHAT IT READS BACK after every switch, because a driver can report one rate
 * and scan out another (Win9x reports 0 for everything - .243's DISPLAYCFG get
 * reads refresh 0, registry_refresh 75):
 *   eds_hz   EnumDisplaySettings(ENUM_CURRENT_SETTINGS).dmDisplayFrequency
 *   caps_hz  GetDeviceCaps(VREFRESH)
 *   dd_freq  IDirectDraw::GetMonitorFrequency
 *   vblank   the MEASURED scanout: IDirectDraw::WaitForVerticalBlank timed
 *            with QueryPerformanceCounter, median interval (modetest_logic.h)
 *
 * SAFETY (modetest_logic.h has the rules; this file enforces them):
 *   - only modes the driver enumerates, never bigger than the persisted desktop;
 *   - explicit rates must be listed and under the EDID/-maxhz ceiling; "max"
 *     refuses with no ceiling at all;
 *   - refuses to start when the live mode is not the persisted mode (the
 *     screen belongs to something else right now), and on Windows 8+;
 *   - every switch goes through voodoo-cleanroom/vcr-kmd/tools/vcr_pace.h,
 *     the fleet's one mode-switch gate (cross-process stamp C:\vcr\
 *     lastswitch.dat + named mutex "vcr-pace"), with this tool's own floor of
 *     >= 5 s on top; at most MT_MAX_SWITCHES per run;
 *   - ALWAYS restores the persisted mode and READS IT BACK; a failed restore
 *     is a banner in the log and exit code 3, never a quiet return;
 *   - a watchdog thread ends the run if it overruns its own time budget.
 *
 * OUTPUT: modetest.log beside the exe (appended), mirrored to stdout when the
 * process has one (the agent's EXECW captures it on XP). One RESULT line per
 * observation, machine-readable.
 *
 * EXIT: 0 done and restored; 2 refused (nothing switched); 3 RESTORE FAILED;
 *       4 a test's own mode change failed (still restored); 5 watchdog.
 *
 * BUILD: tools/modetest/build.sh - CRT-free, -march=pentium, imports only
 * KERNEL32/USER32/GDI32, and only the functions on its Win95/98-era allowlist;
 * ADVAPI32, ddraw.dll and d3d9.dll are resolved at run time. The build script
 * checks the import table and the CMOV count.
 *
 * THREADS (0.2.0, review of 2026-09-29). Three things run beside the main one:
 *   - the WATCHDOG. It takes the run over rather than racing it: it raises
 *     g_takeover, then waits for g_mt_lock - the lock every switch holds from
 *     the pace gate to the switch's own stamp - so the main thread is never in
 *     the middle of a switch when the watchdog makes one, and parks at its
 *     next attempt. vcr_pace.h's bookkeeping (depth / armed) is not
 *     thread-safe; before this a watchdog restore could interleave with a main
 *     thread's pace wait, read a stale depth + armed pair, and let a switch
 *     through with no floor at all. A main thread stuck INSIDE a switch (a
 *     hung driver call) keeps the lock: the watchdog then ends the run without
 *     a give-back of its own (XP reverts the mode as the process goes, paced by
 *     vcr_pace's exit hold; Win9x does NOT revert - the log says so).
 *   - the VBLANK WORKER. The scanout is timed on a thread the main one waits
 *     for with a timeout, so a WaitForVerticalBlank that never returns cannot
 *     hang the run (or -list, which has no watchdog); an abandoned worker ends
 *     every later measurement in that run. It runs at HIGHEST priority on NT
 *     only: on Win9x the agent serves every client from ONE normal-priority
 *     thread, and 2.5 s of HIGHEST-priority polling starves it.
 *   - CreateThread gets a real lpThreadId: Win95/98 FAIL the call with NULL
 *     (error 87), which is how the agent's own helper threads once silently
 *     never started on .243 - and how this tool's watchdog would have.
 *
 * NOT RUN ON ANY BOX YET (2026-09-29). The per-box plan is in
 * tools/modetest/run_modetest.py, which refuses to run without --go.
 */
#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#define INITGUID
#include <windows.h>
#include <stdarg.h>
#include <ddraw.h>
#include <d3d9.h>

#include "modetest_logic.h"

#define MODETEST_VERSION "0.2.0"

#ifndef ENUM_CURRENT_SETTINGS
#define ENUM_CURRENT_SETTINGS  ((DWORD)-1)
#endif
#ifndef ENUM_REGISTRY_SETTINGS
#define ENUM_REGISTRY_SETTINGS ((DWORD)-2)
#endif
#ifndef VREFRESH
#define VREFRESH 116
#endif

/* ------------------------------------------------------------------ */
/* CRT-free shims                                                       */
/* ------------------------------------------------------------------ */
/* The fleet's Win9x helpers are built -nostdlib: a normal msvcrt build left an
 * orphaned #32770 dialog on .243 that later blocked ExitWindowsEx (memory
 * note win98-mingw-msvcrt-hangs). gcc may still emit memset/memcpy for struct
 * initialisation, so they are provided here (built with
 * -fno-tree-loop-distribute-patterns so these loops are not turned back into
 * calls to themselves). */
void *memset(void *d, int c, unsigned int n)
{
    unsigned char *p = (unsigned char *)d;
    while (n--) *p++ = (unsigned char)c;
    return d;
}

void *memcpy(void *d, const void *s, unsigned int n)
{
    unsigned char *p = (unsigned char *)d;
    const unsigned char *q = (const unsigned char *)s;
    while (n--) *p++ = *q++;
    return d;
}

/* vcr_pace.h arms its exit hold with atexit(); this exe runs the handlers
 * itself (mt_exit) because there is no CRT to do it. */
static void (__cdecl *g_atexit_fn[8])(void);
static int g_atexit_n;
int __cdecl atexit(void (__cdecl *fn)(void))
{
    if (g_atexit_n >= 8)
        return -1;
    g_atexit_fn[g_atexit_n++] = fn;
    return 0;
}

/* ------------------------------------------------------------------ */
/* log                                                                  */
/* ------------------------------------------------------------------ */
static HANDLE g_log = INVALID_HANDLE_VALUE;
static HANDLE g_out;
static DWORD  g_t0;
static char   g_logbuf[1100], g_fmtbuf[1030];
/* the watchdog logs too: one line at a time, or two threads garble each
 * other's lines - including the "exit N" line the host's runner looks for */
static CRITICAL_SECTION g_logcs;
static int    g_logcs_ok;

static void mt_write(const char *s, int n)
{
    DWORD w;
    if (g_log != INVALID_HANDLE_VALUE)
        WriteFile(g_log, s, (DWORD)n, &w, NULL);
    if (g_out && g_out != INVALID_HANDLE_VALUE)
        WriteFile(g_out, s, (DWORD)n, &w, NULL);
}

static void mt_logf(const char *fmt, ...)
{
    va_list ap;
    int n;
    if (g_logcs_ok)
        EnterCriticalSection(&g_logcs);
    va_start(ap, fmt);
    wvsprintfA(g_fmtbuf, fmt, ap);                  /* user32: caps at 1024 */
    va_end(ap);
    g_fmtbuf[1000] = 0;                             /* room for the prefix + CRLF */
    n = wsprintfA(g_logbuf, "[%7lu ms] %s\r\n", (unsigned long)(GetTickCount() - g_t0),
                  g_fmtbuf);
    mt_write(g_logbuf, n);
    if (g_logcs_ok)
        LeaveCriticalSection(&g_logcs);
}

/* vcr_pace.h reports through this instead of fprintf(stderr) */
#define VCR_PACE_REPORT(what, err) \
    mt_logf("vcr_pace: %s (error %lu)", (what), (unsigned long)(err))
#include "../../voodoo-cleanroom/vcr-kmd/tools/vcr_pace.h"

static void mt_open_log(void)
{
    char path[MAX_PATH + 32];
    int i, cut = -1;
    DWORD n = GetModuleFileNameA(NULL, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH)
        lstrcpyA(path, "modetest.log");
    else {
        for (i = 0; path[i]; i++)
            if (path[i] == '\\' || path[i] == '/')
                cut = i;
        lstrcpyA(path + cut + 1, "modetest.log");
    }
    /* FILE_SHARE_READ | FILE_SHARE_WRITE: the agent's DOWNLOAD reads it while
     * the run is still going (the host polls it for the "exit" line) */
    g_log = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                        OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (g_log != INVALID_HANDLE_VALUE)
        SetFilePointer(g_log, 0, NULL, FILE_END);
    g_out = GetStdHandle(STD_OUTPUT_HANDLE);
}

/* ------------------------------------------------------------------ */
/* run-time resolved imports (never a static import Win98 might lack)   */
/* ------------------------------------------------------------------ */
typedef LONG (WINAPI *PFN_RegOpenKeyExA)(HKEY, LPCSTR, DWORD, REGSAM, PHKEY);
typedef LONG (WINAPI *PFN_RegEnumKeyExA)(HKEY, DWORD, LPSTR, LPDWORD, LPDWORD, LPSTR,
                                         LPDWORD, PFILETIME);
typedef LONG (WINAPI *PFN_RegQueryValueExA)(HKEY, LPCSTR, LPDWORD, LPDWORD, LPBYTE, LPDWORD);
typedef LONG (WINAPI *PFN_RegCloseKey)(HKEY);
typedef BOOL (WINAPI *PFN_EnumDisplayDevicesA)(LPCSTR, DWORD, PDISPLAY_DEVICEA, DWORD);
typedef HRESULT (WINAPI *PFN_DirectDrawCreate)(GUID *, LPDIRECTDRAW *, IUnknown *);
typedef IDirect3D9 *(WINAPI *PFN_Direct3DCreate9)(UINT);

static PFN_RegOpenKeyExA       p_RegOpenKeyExA;
static PFN_RegEnumKeyExA       p_RegEnumKeyExA;
static PFN_RegQueryValueExA    p_RegQueryValueExA;
static PFN_RegCloseKey         p_RegCloseKey;
static PFN_EnumDisplayDevicesA p_EnumDisplayDevicesA;
static PFN_DirectDrawCreate    p_DirectDrawCreate;
static PFN_Direct3DCreate9     p_Direct3DCreate9;

static void mt_resolve(void)
{
    HMODULE adv = LoadLibraryA("advapi32.dll");
    HMODULE usr = GetModuleHandleA("user32.dll");
    HMODULE dd  = LoadLibraryA("ddraw.dll");
    if (adv) {
        p_RegOpenKeyExA    = (PFN_RegOpenKeyExA)GetProcAddress(adv, "RegOpenKeyExA");
        p_RegEnumKeyExA    = (PFN_RegEnumKeyExA)GetProcAddress(adv, "RegEnumKeyExA");
        p_RegQueryValueExA = (PFN_RegQueryValueExA)GetProcAddress(adv, "RegQueryValueExA");
        p_RegCloseKey      = (PFN_RegCloseKey)GetProcAddress(adv, "RegCloseKey");
    }
    if (usr)
        p_EnumDisplayDevicesA = (PFN_EnumDisplayDevicesA)GetProcAddress(usr,
                                                                     "EnumDisplayDevicesA");
    if (dd)
        p_DirectDrawCreate = (PFN_DirectDrawCreate)GetProcAddress(dd, "DirectDrawCreate");
}

/* ------------------------------------------------------------------ */
/* state                                                                */
/* ------------------------------------------------------------------ */
static mt_mode_t g_modes[MT_MAX_MODES];
static int       g_nmodes;
static mt_mode_t g_reg, g_live;         /* persisted + live at start */
static int       g_have_reg;
static int       g_edid_ok, g_edid_vmax, g_edid_hmax, g_edid_w, g_edid_h;
static char      g_edid_name[16], g_edid_pnp[32];

static int  g_W, g_H, g_BPP;
static unsigned g_tests;
static int  g_b_want, g_cb_want;        /* -1 max, >0 explicit */
static int  g_b_hz, g_cb_hz;            /* resolved */
static int  g_nobpp, g_novb, g_dry, g_list, g_restore_only;
static int  g_pace_s = MT_PACE_DEF_S, g_hold_s = MT_HOLD_DEF_S, g_op_maxhz;
static int  g_switches, g_restore_failed, g_test_failed;
static int  g_test_switches;            /* switches that were NOT give-backs */
static volatile LONG g_done;
static DWORD g_cap_ms;
static int  g_is9x;                     /* Windows 95/98/Me */
/* THE SWITCH LOCK (process-local): held from the pace gate to the switch's
 * own stamp, by whichever thread switches. vcr_pace.h's bookkeeping is not
 * thread-safe; this is what lets the watchdog take a run over without racing
 * it (see THREADS at the top). */
static HANDLE g_mt_lock;
static volatile LONG g_takeover;        /* the watchdog owns the run now */
static DWORD g_wd_tid;                  /* the watchdog's thread id      */
static volatile LONG g_exiting;
#define MT_TAKEOVER_MS 45000u           /* longest the watchdog waits for a
                                         * switch in flight to finish      */

/* ------------------------------------------------------------------ */
/* modes                                                                */
/* ------------------------------------------------------------------ */
static int mt_get_mode(DWORD which, mt_mode_t *m)
{
    DEVMODEA dm;
    memset(&dm, 0, sizeof dm);
    dm.dmSize = sizeof dm;
    if (!EnumDisplaySettingsA(NULL, which, &dm))
        return 0;
    m->w = (int)dm.dmPelsWidth;
    m->h = (int)dm.dmPelsHeight;
    m->bpp = (int)dm.dmBitsPerPel;
    m->hz = (int)dm.dmDisplayFrequency;
    return 1;
}

static void mt_add_mode(const DEVMODEA *dm)
{
    int i, w = (int)dm->dmPelsWidth, h = (int)dm->dmPelsHeight;
    int b = (int)dm->dmBitsPerPel, z = (int)dm->dmDisplayFrequency;
    for (i = 0; i < g_nmodes; i++)
        if (g_modes[i].w == w && g_modes[i].h == h && g_modes[i].bpp == b &&
            g_modes[i].hz == z)
            return;
    if (g_nmodes < MT_MAX_MODES) {
        g_modes[g_nmodes].w = w;
        g_modes[g_nmodes].h = h;
        g_modes[g_nmodes].bpp = b;
        g_modes[g_nmodes].hz = z;
        g_nmodes++;
    }
}

static void mt_enum_modes(void)
{
    DEVMODEA dm;
    DWORD i;
    for (i = 0;; i++) {
        memset(&dm, 0, sizeof dm);
        dm.dmSize = sizeof dm;
        if (!EnumDisplaySettingsA(NULL, i, &dm))
            break;
        mt_add_mode(&dm);
    }
    /* .143's GeForce 6800 answered ENUM_CURRENT_SETTINGS and then FALSE at
     * index 0 for the NULL device: retry per attached adapter, by name. */
    if (g_nmodes < 4 && p_EnumDisplayDevicesA) {
        DISPLAY_DEVICEA ad;
        DWORD a;
        for (a = 0; a < 8; a++) {
            memset(&ad, 0, sizeof ad);
            ad.cb = sizeof ad;
            if (!p_EnumDisplayDevicesA(NULL, a, &ad, 0))
                break;
            if (!(ad.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP))
                continue;
            for (i = 0;; i++) {
                memset(&dm, 0, sizeof dm);
                dm.dmSize = sizeof dm;
                if (!EnumDisplaySettingsA(ad.DeviceName, i, &dm))
                    break;
                mt_add_mode(&dm);
            }
        }
    }
}

static void mt_log_modes(void)
{
    /* grouped: one line per W x H x BPP with every rate listed for it */
    static char line[1000];
    static unsigned char done[MT_MAX_MODES];
    int i, j, n;
    mt_logf("driver lists %d mode entries (EnumDisplaySettings, sentinels included):",
            g_nmodes);
    memset(done, 0, sizeof done);
    for (i = 0; i < g_nmodes; i++) {
        if (done[i])
            continue;
        n = wsprintfA(line, "  %dx%dx%d @", g_modes[i].w, g_modes[i].h, g_modes[i].bpp);
        for (j = i; j < g_nmodes && n < 900; j++)
            if (!done[j] && g_modes[j].w == g_modes[i].w && g_modes[j].h == g_modes[i].h &&
                g_modes[j].bpp == g_modes[i].bpp) {
                done[j] = 1;
                n += wsprintfA(line + n, " %d", g_modes[j].hz);
            }
        mt_logf("%s", line);
    }
}

/* ------------------------------------------------------------------ */
/* EDID - a port of provisioning/fleetres/fleetres.c panel_probe()       */
/* ------------------------------------------------------------------ */
static int mt_edid_parse(const BYTE *b, DWORD n)
{
    static const BYTE hdr[8] = { 0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0 };
    int off, i, ok = 0;
    if (n < 128)
        return 0;
    for (i = 0; i < 8; i++)
        if (b[i] != hdr[i])
            return 0;
    g_edid_vmax = g_edid_hmax = g_edid_w = g_edid_h = 0;
    g_edid_name[0] = 0;
    for (off = 54; off <= 108; off += 18) {
        const BYTE *d = b + off;
        if ((d[0] | (d[1] << 8)) == 0) {
            if (d[3] == 0xFD) {                     /* display range limits */
                g_edid_vmax = d[6];                 /* max vertical, Hz */
                g_edid_hmax = d[8];                 /* max horizontal, kHz */
            } else if (d[3] == 0xFC && !g_edid_name[0]) {
                for (i = 0; i < 13 && d[5 + i] != '\n' && d[5 + i]; i++)
                    g_edid_name[i] = (char)d[5 + i];
                g_edid_name[i] = 0;
            }
            continue;
        }
        if (!g_edid_w) {
            g_edid_w = d[2] | ((d[4] & 0xF0) << 4);
            g_edid_h = d[5] | ((d[7] & 0xF0) << 4);
            ok = g_edid_w > 0 && g_edid_h > 0;
        }
    }
    return ok || g_edid_vmax;
}

static int mt_edid_from_pnp(const char *pnp, int require_active)
{
    HKEY hk, hi, hdp, hc;
    static char path[300], inst[128];
    static BYTE edid[512];
    DWORD idx, len, elen, type;
    int got = 0;

    wsprintfA(path, "SYSTEM\\CurrentControlSet\\Enum\\DISPLAY\\%s", pnp);
    if (p_RegOpenKeyExA(HKEY_LOCAL_MACHINE, path, 0, KEY_READ, &hk) != ERROR_SUCCESS)
        return 0;
    for (idx = 0; !got; idx++) {
        int active;
        len = sizeof inst;
        if (p_RegEnumKeyExA(hk, idx, inst, &len, NULL, NULL, NULL, NULL) != ERROR_SUCCESS)
            break;
        if (p_RegOpenKeyExA(hk, inst, 0, KEY_READ, &hi) != ERROR_SUCCESS)
            continue;
        active = p_RegOpenKeyExA(hi, "Control", 0, KEY_READ, &hc) == ERROR_SUCCESS;
        if (active)
            p_RegCloseKey(hc);
        if ((active || !require_active) &&
            p_RegOpenKeyExA(hi, "Device Parameters", 0, KEY_READ, &hdp) == ERROR_SUCCESS) {
            elen = sizeof edid;
            if (p_RegQueryValueExA(hdp, "EDID", NULL, &type, edid, &elen) == ERROR_SUCCESS &&
                mt_edid_parse(edid, elen)) {
                lstrcpynA(g_edid_pnp, pnp, sizeof g_edid_pnp);
                got = 1;
            }
            p_RegCloseKey(hdp);
        }
        p_RegCloseKey(hi);
    }
    p_RegCloseKey(hk);
    return got;
}

static void mt_probe_edid(void)
{
    DISPLAY_DEVICEA ad, mo;
    DWORD a, m;
    static char pnp[64], key[256];
    char *s, *e;

    g_edid_ok = 0;
    if (!p_RegOpenKeyExA || !p_RegEnumKeyExA || !p_RegQueryValueExA || !p_RegCloseKey)
        return;
    if (p_EnumDisplayDevicesA)
        for (a = 0; a < 8 && !g_edid_ok; a++) {
            memset(&ad, 0, sizeof ad);
            ad.cb = sizeof ad;
            if (!p_EnumDisplayDevicesA(NULL, a, &ad, 0))
                break;
            if (!(ad.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP))
                continue;
            for (m = 0; m < 8 && !g_edid_ok; m++) {
                memset(&mo, 0, sizeof mo);
                mo.cb = sizeof mo;
                if (!p_EnumDisplayDevicesA(ad.DeviceName, m, &mo, 0))
                    break;
                s = mo.DeviceID;
                while (*s && *s != '\\')
                    s++;
                if (!*s)
                    continue;
                lstrcpynA(pnp, s + 1, sizeof pnp);
                for (e = pnp; *e && *e != '\\'; e++)
                    ;
                *e = 0;
                if (!pnp[0] || !lstrcmpiA(pnp, "Default_Monitor"))
                    continue;
                g_edid_ok = mt_edid_from_pnp(pnp, 0);
            }
        }
    if (!g_edid_ok) {
        /* .171: EnumDisplayDevices alternated between the real panel and
         * Default_Monitor between runs - scan Enum\DISPLAY for an ACTIVE node */
        HKEY hk;
        DWORD i, l;
        if (p_RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SYSTEM\\CurrentControlSet\\Enum\\DISPLAY", 0,
                            KEY_READ, &hk) == ERROR_SUCCESS) {
            for (i = 0; !g_edid_ok; i++) {
                l = sizeof key;
                if (p_RegEnumKeyExA(hk, i, key, &l, NULL, NULL, NULL, NULL) != ERROR_SUCCESS)
                    break;
                if (!lstrcmpiA(key, "Default_Monitor"))
                    continue;
                g_edid_ok = mt_edid_from_pnp(key, 1);
            }
            p_RegCloseKey(hk);
        }
    }
    /* a parse that found no range descriptor carries no ceiling */
    if (g_edid_ok && !g_edid_vmax)
        mt_logf("EDID found (%s) but it states NO vertical range - no ceiling from it",
                g_edid_pnp);
}

/* ------------------------------------------------------------------ */
/* the measured refresh                                                 */
/* ------------------------------------------------------------------ */
#define MT_VB_SAMPLES    73
#define MT_VB_TIMEOUT_MS 8000u  /* DirectDrawCreate + 2.5 s of sampling + margin */
static double g_iv[MT_VB_SAMPLES];
static int    g_vb_dead;        /* a worker was abandoned: no more timing this run */

/* One measurement's inputs and outputs. STATIC on purpose: an abandoned
 * worker may still write it after the main thread has moved on. */
static struct {
    LPDIRECTDRAW  dd;           /* in: the caller's object (exclusive), or NULL */
    int           ddfreq;       /* out: GetMonitorFrequency, -1 = none          */
    const char   *how;          /* out */
    unsigned long mhz;          /* out: 0 = not measurable                      */
} g_vbjob;

/* Time the scanout (the worker's body). dd may be an object the caller holds
 * (exclusive mode); NULL = make a DDSCL_NORMAL one for the measurement.
 *
 * Each sample waits for the END of a blank and then for the start of the
 * next, when the driver supports DDWAITVB_BLOCKEND: a BLOCKBEGIN that answers
 * at once while the beam is still inside the blank (legal for a driver that
 * tests "in vblank" rather than the edge) would otherwise alternate ~0 and
 * one frame, and the median check would refuse the whole run as unmeasured. */
static DWORD WINAPI mt_vb_worker(LPVOID unused)
{
    LARGE_INTEGER qf, t0, t1, start;
    LPDIRECTDRAW own = NULL, dd = g_vbjob.dd;
    int n = 0, use_end;
    double lim;
    (void)unused;

    if (!dd) {
        if (!p_DirectDrawCreate || p_DirectDrawCreate(NULL, &own, NULL) != DD_OK || !own) {
            g_vbjob.how = "no DirectDraw";
            return 0;
        }
        IDirectDraw_SetCooperativeLevel(own, NULL, DDSCL_NORMAL);
        dd = own;
    }
    {
        DWORD f = 0;
        if (IDirectDraw_GetMonitorFrequency(dd, &f) == DD_OK)
            g_vbjob.ddfreq = (int)f;
    }
    if (!QueryPerformanceFrequency(&qf) || qf.QuadPart <= 0) {
        g_vbjob.how = "no QueryPerformanceFrequency";
        goto out;
    }
    lim = (double)qf.QuadPart * 2.5;                /* never more than 2.5 s */
    /* HIGHEST only on NT. On Win9x the agent serves every client from one
     * normal-priority thread: 2.5 s of HIGHEST-priority polling starves it. */
    if (!g_is9x)
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    QueryPerformanceCounter(&start);
    use_end = IDirectDraw_WaitForVerticalBlank(dd, DDWAITVB_BLOCKEND, NULL) == DD_OK;
    if (IDirectDraw_WaitForVerticalBlank(dd, DDWAITVB_BLOCKBEGIN, NULL) == DD_OK) {
        g_vbjob.how = use_end ? "WaitForVerticalBlank END+BEGIN" : "WaitForVerticalBlank BEGIN";
        QueryPerformanceCounter(&t0);
        while (n < MT_VB_SAMPLES) {
            if (use_end && IDirectDraw_WaitForVerticalBlank(dd, DDWAITVB_BLOCKEND, NULL) != DD_OK)
                break;
            if (IDirectDraw_WaitForVerticalBlank(dd, DDWAITVB_BLOCKBEGIN, NULL) != DD_OK)
                break;
            QueryPerformanceCounter(&t1);
            g_iv[n++] = (double)(t1.QuadPart - t0.QuadPart);
            t0 = t1;
            if ((double)(t1.QuadPart - start.QuadPart) > lim)
                break;
        }
    } else {
        /* no blocking wait: poll GetVerticalBlankStatus for 0 -> 1 edges */
        BOOL in = FALSE, was = TRUE;
        int have = 0;
        g_vbjob.how = "GetVerticalBlankStatus poll";
        for (;;) {
            QueryPerformanceCounter(&t1);
            if ((double)(t1.QuadPart - start.QuadPart) > lim || n >= MT_VB_SAMPLES)
                break;
            if (IDirectDraw_GetVerticalBlankStatus(dd, &in) != DD_OK) {
                g_vbjob.how = "no vertical-blank support";
                break;
            }
            if (in && !was) {
                if (have)
                    g_iv[n++] = (double)(t1.QuadPart - t0.QuadPart);
                t0 = t1;
                have = 1;
            }
            was = in;
        }
    }
    g_vbjob.mhz = mt_vblank_mhz(g_iv, n, (double)qf.QuadPart);
out:
    if (own)
        IDirectDraw_Release(own);
    return 0;
}

static void mt_pump(void);

/* Time the scanout on the worker, waiting at most MT_VB_TIMEOUT_MS. *ddfreq
 * gets GetMonitorFrequency (or -1). Returns milli-Hz, 0 = not measurable. */
static unsigned long mt_measure_vblank(LPDIRECTDRAW dd, int *ddfreq, const char **how)
{
    HANDLE th;
    DWORD tid = 0, w;

    *ddfreq = -1;
    *how = "none";
    if (g_novb)
        return 0;
    if (g_vb_dead) {
        *how = "skipped - an earlier measurement never returned";
        return 0;
    }
    g_vbjob.dd = dd;
    g_vbjob.ddfreq = -1;
    g_vbjob.how = "none";
    g_vbjob.mhz = 0;
    th = CreateThread(NULL, 0, mt_vb_worker, NULL, 0, &tid);   /* &tid: Win9x */
    if (!th) {
        *how = "no worker thread";
        return 0;
    }
    /* wait PUMPING: in tests C/D this thread owns a fullscreen window, and one
     * that stops answering messages for 5 s is ghosted by XP */
    {
        DWORD t0 = GetTickCount(), el;
        for (;;) {
            el = GetTickCount() - t0;
            if (el >= MT_VB_TIMEOUT_MS) {
                w = WAIT_TIMEOUT;
                break;
            }
            w = MsgWaitForMultipleObjects(1, &th, FALSE, MT_VB_TIMEOUT_MS - el, QS_ALLINPUT);
            if (w == WAIT_OBJECT_0 + 1) {
                mt_pump();
                continue;
            }
            break;                          /* WAIT_OBJECT_0, WAIT_TIMEOUT or failed */
        }
    }
    CloseHandle(th);
    if (w != WAIT_OBJECT_0) {
        g_vb_dead = 1;
        mt_logf("!!! the vblank measurement did not return within %lu ms - abandoned; no "
                "more timing this run (a WaitForVerticalBlank that never returns)",
                (unsigned long)MT_VB_TIMEOUT_MS);
        *how = "TIMED OUT - abandoned";
        return 0;
    }
    *ddfreq = g_vbjob.ddfreq;
    *how = g_vbjob.how;
    return g_vbjob.mhz;
}

typedef struct {
    mt_mode_t eds;
    int eds_ok, caps_hz, dd_freq, agree;
    unsigned long vb_mhz;
    const char *how;
} mt_obs_t;

static void mt_observe(LPDIRECTDRAW dd, mt_obs_t *o)
{
    HDC dc;
    memset(o, 0, sizeof *o);
    o->eds_ok = mt_get_mode(ENUM_CURRENT_SETTINGS, &o->eds);
    dc = GetDC(NULL);
    o->caps_hz = dc ? GetDeviceCaps(dc, VREFRESH) : -1;
    if (dc)
        ReleaseDC(NULL, dc);
    o->vb_mhz = mt_measure_vblank(dd, &o->dd_freq, &o->how);
    o->agree = mt_rate_agreement(o->eds.hz, o->vb_mhz);
}

/* RESULT lines are what the host parses; everything else is for a person */
static void mt_result(const char *test, const char *step, int req_hz, const mt_obs_t *o,
                      const char *rc)
{
    mt_logf("RESULT test=%s step=%s req=%dx%dx%d@%d now=%dx%dx%d eds_hz=%d caps_hz=%d "
            "dd_freq=%d vblank_mhz=%lu vblank_hz=%d agree=%d via=\"%s\" rc=%s",
            test, step, g_W, g_H, g_BPP, req_hz, o->eds.w, o->eds.h, o->eds.bpp, o->eds.hz,
            o->caps_hz, o->dd_freq, o->vb_mhz, mt_mhz_to_hz(o->vb_mhz), o->agree, o->how, rc);
    if (o->agree < 0)
        mt_logf("!!! the driver REPORTS %d Hz and the scanout MEASURES %d Hz - trust the "
                "measurement", o->eds.hz, mt_mhz_to_hz(o->vb_mhz));
}

static const char *mt_cds_name(LONG r)
{
    switch (r) {
    case DISP_CHANGE_SUCCESSFUL:  return "SUCCESSFUL";
    case DISP_CHANGE_RESTART:     return "RESTART";
    case DISP_CHANGE_BADFLAGS:    return "BADFLAGS";
    case DISP_CHANGE_BADPARAM:    return "BADPARAM";
    case DISP_CHANGE_FAILED:      return "FAILED";
    case DISP_CHANGE_BADMODE:     return "BADMODE";
    case DISP_CHANGE_NOTUPDATED:  return "NOTUPDATED";
    default:                      return "other";
    }
}

/* ------------------------------------------------------------------ */
/* switching - every one through vcr_pace.h                             */
/* ------------------------------------------------------------------ */
/* A thread that must not switch any more: the watchdog owns the run. It ends
 * the process; nothing this thread could do next would be paced or ours. */
static void mt_park(void)
{
    for (;;)
        Sleep(1000);
}

/*
 * Before any switch. Takes g_mt_lock and keeps it until mt_switch_end(), so
 * the pace gate's wait, the switch and its stamp are one unit no other thread
 * of this process can interleave with. giveback: a restore / RestoreDisplay-
 * Mode / device release - never refused by the per-run count
 * (mt_switch_allowed), still paced. 1 = switch now, then mt_switch_end().
 */
static int mt_switch_begin(const char *what, int giveback)
{
    if (g_mt_lock)
        WaitForSingleObject(g_mt_lock, INFINITE);
    if (g_takeover && GetCurrentThreadId() != g_wd_tid) {
        if (g_mt_lock)
            ReleaseMutex(g_mt_lock);
        mt_park();                          /* the watchdog ends the process */
    }
    if (!mt_switch_allowed(g_test_switches, giveback)) {
        mt_logf("SWITCH REFUSED (%s): per-run switch limit reached", what);
        if (g_mt_lock)
            ReleaseMutex(g_mt_lock);
        return 0;
    }
    if (!vcr_pace_before_switch()) {
        mt_logf("SWITCH REFUSED (%s): vcr_pace: %s", what,
                g_vcr_pace_why ? g_vcr_pace_why : "?");
        if (g_mt_lock)
            ReleaseMutex(g_mt_lock);
        return 0;
    }
    /* the pace wait can take a whole floor: a watchdog that fired meanwhile
     * gets the lock BEFORE this test switch, not after it. vcr_pace_cancel
     * keeps the pre-stamp, so the watchdog's give-back waits a floor from it. */
    if (g_takeover && GetCurrentThreadId() != g_wd_tid) {
        vcr_pace_cancel();
        if (g_mt_lock)
            ReleaseMutex(g_mt_lock);
        mt_park();
    }
    g_switches++;
    if (!giveback)
        g_test_switches++;
    mt_logf("SWITCH %d: %s%s", g_switches, what, giveback ? " (give-back)" : "");
    return 1;
}

/* After the switch: stamp it (temp = 1 when a temporary mode is now held)
 * and hand the switch lock back. */
static void mt_switch_end(int temp)
{
    vcr_pace_after_switch_ex(temp);
    if (g_mt_lock)
        ReleaseMutex(g_mt_lock);
}

static void mt_pump(void)
{
    MSG msg;
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
}

/* hold the mode just set for g_hold_s from `since`, pumping messages */
static void mt_hold(DWORD since)
{
    DWORD want = (DWORD)g_hold_s * 1000u;
    while (GetTickCount() - since < want) {
        mt_pump();
        Sleep(50);
    }
}

static LONG mt_cds(int w, int h, int bpp, int hz, int with_bpp, DWORD flags)
{
    DEVMODEA dm;
    memset(&dm, 0, sizeof dm);
    dm.dmSize = sizeof dm;
    dm.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT;
    dm.dmPelsWidth = (DWORD)w;
    dm.dmPelsHeight = (DWORD)h;
    if (with_bpp) {
        dm.dmFields |= DM_BITSPERPEL;
        dm.dmBitsPerPel = (DWORD)bpp;
    }
    if (hz > 0) {
        dm.dmFields |= DM_DISPLAYFREQUENCY;
        dm.dmDisplayFrequency = (DWORD)hz;
    }
    return ChangeDisplaySettingsA(&dm, flags);
}

static void mt_exit(int code);

/* Put the persisted mode back and READ IT BACK. 1 = restored. */
static int mt_restore(const char *why)
{
    mt_mode_t cur;
    LONG r;
    int tries;

    memset(&cur, 0, sizeof cur);
    for (tries = 0; tries < 3; tries++) {
        if (mt_switch_begin(why, 1))
            break;
        Sleep(5000);
    }
    if (tries == 3) {
        /* vcr_pace.h's contract: a tool refused on the way OUT of a temporary
         * mode gives it back by NO other route - it ends, and the exit hold
         * paces the revert XP makes as the process goes */
        mt_logf("########################################################");
        mt_logf("## RESTORE NOT ATTEMPTED - the pace gate refused 3 times");
        if (g_is9x) {
            mt_logf("## WINDOWS 9x DOES NOT REVERT A CDS_FULLSCREEN MODE WHEN");
            mt_logf("## THE PROCESS ENDS: the screen STAYS in the test mode.");
            mt_logf("## LAUNCH modetest -restore (or a person at the box).");
        } else {
            mt_logf("## ending now: XP reverts a CDS_FULLSCREEN / exclusive");
            mt_logf("## mode at exit (vcr_pace holds it for the floor first).");
            mt_logf("## CHECK THE SCREEN (DISPLAYCFG get); modetest -restore");
        }
        mt_logf("########################################################");
        g_restore_failed = 1;
        mt_exit(3);
    }
    r = ChangeDisplaySettingsA(NULL, 0);
    mt_switch_end(0);
    mt_logf("restore: ChangeDisplaySettings(NULL, 0) -> %ld (%s)", (long)r, mt_cds_name(r));
    if (mt_get_mode(ENUM_CURRENT_SETTINGS, &cur) && mt_live_is_persisted(&cur, &g_reg)) {
        mt_logf("restore: VERIFIED %dx%dx%d@%d (persisted %dx%dx%d@%d)", cur.w, cur.h,
                cur.bpp, cur.hz, g_reg.w, g_reg.h, g_reg.bpp, g_reg.hz);
        return 1;
    }
    mt_logf("restore: live %dx%dx%d@%d is NOT the persisted %dx%dx%d@%d - explicit retry",
            cur.w, cur.h, cur.bpp, cur.hz, g_reg.w, g_reg.h, g_reg.bpp, g_reg.hz);
    if (mt_switch_begin("restore (explicit persisted DEVMODE)", 1)) {
        r = mt_cds(g_reg.w, g_reg.h, g_reg.bpp, mt_hz_is_real(g_reg.hz) ? g_reg.hz : 0, 1, 0);
        mt_switch_end(0);
        mt_logf("restore: explicit -> %ld (%s)", (long)r, mt_cds_name(r));
        if (mt_get_mode(ENUM_CURRENT_SETTINGS, &cur) && mt_live_is_persisted(&cur, &g_reg)) {
            mt_logf("restore: VERIFIED on the second attempt");
            return 1;
        }
    }
    mt_logf("########################################################");
    mt_logf("## RESTORE FAILED: the screen is %dx%dx%d@%d, the box is", cur.w, cur.h,
            cur.bpp, cur.hz);
    mt_logf("## persisted at %dx%dx%d@%d. The desktop may be STRANDED.", g_reg.w, g_reg.h,
            g_reg.bpp, g_reg.hz);
    mt_logf("## No further test runs on a screen that is not its own.");
    mt_logf("########################################################");
    g_restore_failed = 1;
    mt_exit(3);
    return 0;
}

/* After a request that FAILED nothing landed: when the screen still reads its
 * persisted mode, a restore would be one more monitor re-sync for nothing. */
static void mt_restore_after(LONG r, const char *why)
{
    mt_mode_t cur;
    if (r != DISP_CHANGE_SUCCESSFUL && mt_get_mode(ENUM_CURRENT_SETTINGS, &cur) &&
        mt_live_is_persisted(&cur, &g_reg)) {
        mt_logf("%s: the request failed and the screen is still at its persisted mode - "
                "no restore needed", why);
        return;
    }
    mt_restore(why);
}

/* The API gave the persisted mode back itself (DirectDraw's RestoreDisplayMode,
 * a Direct3D device release) and the read-back proves it: tell vcr_pace.h no
 * temporary mode is held any more, or its exit hold would wait a floor for a
 * revert that is not coming. */
static void mt_mark_given_back(void)
{
    g_vcr_pace_temp_mode = 0;
}

/* ------------------------------------------------------------------ */
/* the window a DirectDraw / Direct3D test needs                        */
/* ------------------------------------------------------------------ */
static HWND g_wnd;

static LRESULT CALLBACK mt_wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_SETCURSOR) {
        SetCursor(NULL);
        return TRUE;
    }
    return DefWindowProcA(h, m, w, l);
}

static int mt_make_window(void)
{
    WNDCLASSA wc;
    DWORD fg_tid, me = GetCurrentThreadId();
    HWND fg;
    int i;

    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = mt_wndproc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = "modetest";
    RegisterClassA(&wc);
    g_wnd = CreateWindowExA(WS_EX_TOPMOST, "modetest", "modetest", WS_POPUP | WS_VISIBLE, 0,
                            0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN),
                            NULL, NULL, wc.hInstance, NULL);
    if (!g_wnd)
        return 0;
    /* twice: the first ShowWindow takes the launcher's STARTUPINFO (the
     * agent's EXEC starts hidden), the second takes ours */
    ShowWindow(g_wnd, SW_SHOWNORMAL);
    ShowWindow(g_wnd, SW_SHOWNORMAL);
    UpdateWindow(g_wnd);
    for (i = 0; i < 5 && GetForegroundWindow() != g_wnd; i++) {
        fg = GetForegroundWindow();
        fg_tid = fg ? GetWindowThreadProcessId(fg, NULL) : 0;
        if (fg_tid && fg_tid != me)
            AttachThreadInput(me, fg_tid, TRUE);
        SetForegroundWindow(g_wnd);
        BringWindowToTop(g_wnd);
        SetFocus(g_wnd);
        if (fg_tid && fg_tid != me)
            AttachThreadInput(me, fg_tid, FALSE);
        mt_pump();
        Sleep(200);
    }
    mt_pump();
    return GetForegroundWindow() == g_wnd;
}

static void mt_kill_window(void)
{
    if (g_wnd) {
        DestroyWindow(g_wnd);
        g_wnd = NULL;
        mt_pump();
    }
}

/* ------------------------------------------------------------------ */
/* the tests                                                            */
/* ------------------------------------------------------------------ */
static void mt_test_a(void)
{
    mt_obs_t o;
    LONG r;
    DWORD t;
    mt_logf("=== TEST A: ChangeDisplaySettings %dx%dx%d, NO rate (dmFields %s), "
            "CDS_FULLSCREEN - what WON hl.exe / GLQuake ask for", g_W, g_H, g_BPP,
            g_nobpp ? "W|H" : "W|H|BPP");
    if (!mt_switch_begin("A: no-rate request", 0))
        return;
    r = mt_cds(g_W, g_H, g_BPP, 0, !g_nobpp, CDS_FULLSCREEN);
    mt_switch_end(1);
    t = GetTickCount();
    mt_logf("A: ChangeDisplaySettings -> %ld (%s)", (long)r, mt_cds_name(r));
    mt_observe(NULL, &o);
    mt_result("A", "norate", 0, &o, mt_cds_name(r));
    if (r != DISP_CHANGE_SUCCESSFUL)
        g_test_failed = 1;
    mt_hold(t);
    mt_restore_after(r, "A: restore");
}

static void mt_test_b(void)
{
    mt_obs_t o;
    LONG r;
    DWORD t;
    int pre_hz;
    mt_logf("=== TEST B: pre-switch %dx%dx%d@%d (CDS_FULLSCREEN), then the NO-rate "
            "request again - does a game's request keep the pre-switched rate?",
            g_W, g_H, g_BPP, g_b_hz);
    if (!mt_switch_begin("B1: at-rate pre-switch", 0))
        return;
    r = mt_cds(g_W, g_H, g_BPP, g_b_hz, 1, CDS_FULLSCREEN);
    mt_switch_end(1);
    t = GetTickCount();
    mt_logf("B1: ChangeDisplaySettings(@%d) -> %ld (%s)", g_b_hz, (long)r, mt_cds_name(r));
    mt_observe(NULL, &o);
    mt_result("B", "prerate", g_b_hz, &o, mt_cds_name(r));
    pre_hz = mt_observed_hz(o.eds.hz, o.vb_mhz);
    if (r != DISP_CHANGE_SUCCESSFUL) {
        g_test_failed = 1;
        mt_hold(t);
        mt_restore_after(r, "B: restore after a failed pre-switch");
        return;
    }
    mt_hold(t);
    if (mt_switch_begin("B2: the game's no-rate request", 0)) {
        r = mt_cds(g_W, g_H, g_BPP, 0, !g_nobpp, CDS_FULLSCREEN);
        mt_switch_end(1);
        t = GetTickCount();
        mt_logf("B2: ChangeDisplaySettings(no rate) -> %ld (%s)", (long)r, mt_cds_name(r));
        mt_observe(NULL, &o);
        mt_result("B", "norate_after_prerate", 0, &o, mt_cds_name(r));
        mt_logf("B VERDICT: %s - pre-switch asked %d Hz and landed on %d Hz, the no-rate "
                "request then landed on %d Hz (0 = unmeasured)",
                mt_keep_verdict_name(mt_keep_verdict(g_b_hz, pre_hz,
                                                     mt_observed_hz(o.eds.hz, o.vb_mhz))),
                g_b_hz, pre_hz, mt_observed_hz(o.eds.hz, o.vb_mhz));
        if (r != DISP_CHANGE_SUCCESSFUL)
            g_test_failed = 1;
        mt_hold(t);
    }
    mt_restore("B: restore");
}

/* DirectDraw: exclusive + SetDisplayMode(W,H,BPP,0). With prehz > 0 the mode
 * is first set to W x H x BPP @prehz through CDS (test CB). */
static void mt_test_c(int prehz)
{
    const char *tag = prehz ? "CB" : "C";
    LPDIRECTDRAW dd1 = NULL;
    LPDIRECTDRAW2 dd2 = NULL;
    HRESULT hr;
    mt_obs_t o;
    DWORD t;
    int fg, pre_hz = 0, switched = 0;

    mt_logf("=== TEST %s: DirectDraw exclusive SetDisplayMode(%d, %d, %d, refresh 0)%s",
            tag, g_W, g_H, g_BPP, prehz ? " after a CDS pre-switch" : "");
    if (!p_DirectDrawCreate) {
        mt_logf("%s: SKIPPED - ddraw.dll / DirectDrawCreate not available", tag);
        return;
    }
    fg = mt_make_window();
    if (!fg) {
        mt_logf("%s: SKIPPED - this process could not take the FOREGROUND, and an "
                "exclusive-mode result from a background window is not a measurement "
                "(launch through LAUNCH on the console session)", tag);
        mt_kill_window();
        return;
    }
    if (prehz) {
        LONG r;
        if (!mt_switch_begin("CB1: at-rate pre-switch", 0)) {
            mt_kill_window();
            return;
        }
        r = mt_cds(g_W, g_H, g_BPP, prehz, 1, CDS_FULLSCREEN);
        mt_switch_end(1);
        t = GetTickCount();
        mt_logf("CB1: ChangeDisplaySettings(@%d) -> %ld (%s)", prehz, (long)r, mt_cds_name(r));
        mt_observe(NULL, &o);
        mt_result("CB", "prerate", prehz, &o, mt_cds_name(r));
        pre_hz = mt_observed_hz(o.eds.hz, o.vb_mhz);
        mt_hold(t);
        if (r != DISP_CHANGE_SUCCESSFUL) {
            g_test_failed = 1;
            mt_kill_window();
            mt_restore_after(r, "CB: restore after a failed pre-switch");
            return;
        }
    }
    hr = p_DirectDrawCreate(NULL, &dd1, NULL);
    if (hr != DD_OK || !dd1) {
        mt_logf("%s: DirectDrawCreate failed 0x%08lx", tag, (unsigned long)hr);
        g_test_failed = 1;
        goto done;
    }
    hr = IDirectDraw_QueryInterface(dd1, &IID_IDirectDraw2, (void **)&dd2);
    if (hr != DD_OK || !dd2) {
        mt_logf("%s: no IDirectDraw2 (0x%08lx) - DirectX 5+ needed", tag, (unsigned long)hr);
        g_test_failed = 1;
        goto done;
    }
    hr = IDirectDraw2_SetCooperativeLevel(dd2, g_wnd, DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN);
    mt_logf("%s: SetCooperativeLevel(EXCLUSIVE|FULLSCREEN) -> 0x%08lx", tag, (unsigned long)hr);
    if (hr != DD_OK) {
        g_test_failed = 1;
        goto done;
    }
    if (mt_switch_begin(prehz ? "CB2: DirectDraw SetDisplayMode rate 0"
                              : "C1: DirectDraw SetDisplayMode rate 0", 0)) {
        switched = 1;
        hr = IDirectDraw2_SetDisplayMode(dd2, (DWORD)g_W, (DWORD)g_H, (DWORD)g_BPP, 0, 0);
        mt_switch_end(1);
        t = GetTickCount();
        mt_logf("%s: SetDisplayMode(..., 0) -> 0x%08lx", tag, (unsigned long)hr);
        mt_pump();
        mt_observe(dd1, &o);
        mt_result(tag, prehz ? "ddraw_rate0_after_prerate" : "ddraw_rate0", 0, &o,
                  hr == DD_OK ? "DD_OK" : "DDERR");
        if (prehz)
            mt_logf("CB VERDICT: %s - pre-switch asked %d Hz and landed on %d Hz, "
                    "DirectDraw's rate-0 request then landed on %d Hz (0 = unmeasured)",
                    mt_keep_verdict_name(mt_keep_verdict(prehz, pre_hz,
                                                         mt_observed_hz(o.eds.hz, o.vb_mhz))),
                    prehz, pre_hz, mt_observed_hz(o.eds.hz, o.vb_mhz));
        if (hr != DD_OK)
            g_test_failed = 1;
        mt_hold(t);
        if (!mt_switch_begin("C: DirectDraw RestoreDisplayMode", 1)) {
            /* refused on the way OUT: no other give-back route (a Release or
             * DDSCL_NORMAL would revert the mode unpaced) - end the run */
            g_restore_failed = 1;
            mt_exit(3);
        }
        hr = IDirectDraw2_RestoreDisplayMode(dd2);
        mt_switch_end(1);                   /* still maybe a temporary mode (CB) */
        mt_logf("%s: RestoreDisplayMode -> 0x%08lx", tag, (unsigned long)hr);
    }
    IDirectDraw2_SetCooperativeLevel(dd2, g_wnd, DDSCL_NORMAL);
done:
    if (dd2)
        IDirectDraw2_Release(dd2);
    if (dd1)
        IDirectDraw_Release(dd1);
    mt_kill_window();
    {
        mt_mode_t cur;
        /* CB always gives its pre-switch back; C only what its own
         * SetDisplayMode may have left (a mode it did not make is not ours) */
        if (prehz || (switched && (!mt_get_mode(ENUM_CURRENT_SETTINGS, &cur) ||
                                   !mt_live_is_persisted(&cur, &g_reg))))
            mt_restore(prehz ? "CB: restore" : "C: corrective restore");
        else if (switched) {
            mt_mark_given_back();
            mt_logf("%s: DirectDraw gave the persisted mode back - verified, no restore "
                    "needed", tag);
        } else {
            mt_logf("%s: no DirectDraw mode was set - the screen is still at its persisted "
                    "mode", tag);
        }
    }
}

static void mt_test_d(void)
{
    HMODULE lib;
    IDirect3D9 *d3d = NULL;
    IDirect3DDevice9 *dev = NULL;
    D3DPRESENT_PARAMETERS pp;
    D3DDISPLAYMODE dm;
    D3DFORMAT fmt = g_BPP == 32 ? D3DFMT_X8R8G8B8 : D3DFMT_R5G6B5;
    HRESULT hr;
    mt_obs_t o;
    DWORD t;
    UINT i, n;
    static char rates[400];
    int k = 0, listed = 0, switched = 0, created = 0;

    mt_logf("=== TEST D: Direct3D 9 fullscreen CreateDevice %dx%d, "
            "FullScreen_RefreshRateInHz 0 (D3DPRESENT_RATE_DEFAULT)", g_W, g_H);
    if (g_BPP != 32 && g_BPP != 16) {
        mt_logf("D: SKIPPED - Direct3D 9 fullscreen needs 16 or 32 bpp");
        return;
    }
    lib = LoadLibraryA("d3d9.dll");
    p_Direct3DCreate9 = lib ? (PFN_Direct3DCreate9)GetProcAddress(lib, "Direct3DCreate9") : NULL;
    if (!p_Direct3DCreate9 || !(d3d = p_Direct3DCreate9(D3D_SDK_VERSION))) {
        mt_logf("D: SKIPPED - no Direct3D 9 runtime");
        return;
    }
    n = IDirect3D9_GetAdapterModeCount(d3d, D3DADAPTER_DEFAULT, fmt);
    rates[0] = 0;
    for (i = 0; i < n && k < 360; i++) {
        D3DDISPLAYMODE m;
        if (IDirect3D9_EnumAdapterModes(d3d, D3DADAPTER_DEFAULT, fmt, i, &m) == D3D_OK &&
            (int)m.Width == g_W && (int)m.Height == g_H) {
            k += wsprintfA(rates + k, " %u", m.RefreshRate);
            listed = 1;
        }
    }
    mt_logf("D: Direct3D 9 lists %dx%d at:%s", g_W, g_H, listed ? rates : " NOTHING");
    if (!listed) {
        mt_logf("D: SKIPPED - Direct3D 9 does not list the mode");
        IDirect3D9_Release(d3d);
        return;
    }
    if (!mt_make_window()) {
        mt_logf("D: SKIPPED - no foreground (see test C)");
        mt_kill_window();
        IDirect3D9_Release(d3d);
        return;
    }
    memset(&pp, 0, sizeof pp);
    pp.BackBufferWidth = (UINT)g_W;
    pp.BackBufferHeight = (UINT)g_H;
    pp.BackBufferFormat = fmt;
    pp.BackBufferCount = 1;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.hDeviceWindow = g_wnd;
    pp.Windowed = FALSE;
    pp.FullScreen_RefreshRateInHz = 0;
    pp.PresentationInterval = D3DPRESENT_INTERVAL_DEFAULT;
    if (mt_switch_begin("D1: Direct3D 9 CreateDevice rate 0", 0)) {
        switched = 1;
        hr = IDirect3D9_CreateDevice(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, g_wnd,
                                     D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &dev);
        mt_switch_end(1);
        t = GetTickCount();
        mt_logf("D: CreateDevice -> 0x%08lx", (unsigned long)hr);
        mt_pump();
        if (hr == D3D_OK && dev) {
            created = 1;
            memset(&dm, 0, sizeof dm);
            IDirect3DDevice9_GetDisplayMode(dev, 0, &dm);
            mt_logf("D: device reports %ux%u @%u Hz", dm.Width, dm.Height, dm.RefreshRate);
        } else {
            g_test_failed = 1;
        }
        /* no DirectDraw vblank timing under a D3D9 exclusive device */
        {
            int keep = g_novb;
            g_novb = 1;
            mt_observe(NULL, &o);
            g_novb = keep;
        }
        mt_result("D", "d3d9_rate0", 0, &o, hr == D3D_OK ? "D3D_OK" : "D3DERR");
        mt_hold(t);
        if (dev) {
            if (!mt_switch_begin("D2: Direct3D 9 device released", 1)) {
                g_restore_failed = 1;               /* no unpaced Release: end, */
                mt_exit(3);                         /* the exit hold paces XP's revert */
            }
            IDirect3DDevice9_Release(dev);
            mt_switch_end(1);
            dev = NULL;
        }
    }
    IDirect3D9_Release(d3d);
    mt_kill_window();
    {
        mt_mode_t cur;
        if (!switched)
            mt_logf("D: the pace gate refused the device - nothing to give back");
        else if (!mt_get_mode(ENUM_CURRENT_SETTINGS, &cur) || !mt_live_is_persisted(&cur, &g_reg))
            mt_restore("D: corrective restore");
        else {
            mt_mark_given_back();
            mt_logf(created ? "D: Direct3D gave the persisted mode back - verified"
                            : "D: CreateDevice failed and the screen is still at its "
                              "persisted mode - verified");
        }
    }
}

/* ------------------------------------------------------------------ */
/* arguments                                                            */
/* ------------------------------------------------------------------ */
static char *g_argv[32];
static int   g_argc;
static char  g_cmd[1024];

static void mt_split_args(void)
{
    char *p;
    lstrcpynA(g_cmd, GetCommandLineA(), sizeof g_cmd);
    p = g_cmd;
    while (*p && g_argc < 32) {
        while (*p == ' ' || *p == '\t')
            p++;
        if (!*p)
            break;
        if (*p == '"') {
            g_argv[g_argc++] = ++p;
            while (*p && *p != '"')
                p++;
        } else {
            g_argv[g_argc++] = p;
            while (*p && *p != ' ' && *p != '\t')
                p++;
        }
        if (*p)
            *p++ = 0;
    }
}

static int mt_num(const char *s, int *out)
{
    int v = 0;
    if (!s || *s < '0' || *s > '9')
        return 0;
    for (; *s >= '0' && *s <= '9'; s++) {
        v = v * 10 + (*s - '0');
        if (v > 100000)
            return 0;
    }
    if (*s)
        return 0;
    *out = v;
    return 1;
}

static int mt_rate_arg(const char *s, int *out)
{
    if (s && !lstrcmpiA(s, "max")) {
        *out = -1;
        return 1;
    }
    return mt_num(s, out) && *out > 0;
}

static void mt_usage(void)
{
    mt_logf("usage: modetest -list | -restore | <W> <H> <BPP> [-a] [-b <hz|max>] [-c] "
            "[-cb <hz|max>] [-d] [-nobpp] [-pace <s>] [-hold <s>] [-maxhz <n>] [-novb] [-dry] "
            "[-tag <word>]");
}

/* 1 = parsed; 0 = bad arguments (logged) */
static int mt_parse(void)
{
    int i, pos = 0, v;
    for (i = 1; i < g_argc; i++) {
        const char *a = g_argv[i];
        if (a[0] != '-') {
            if (!mt_num(a, &v)) {
                mt_logf("bad number '%s'", a);
                return 0;
            }
            if (pos == 0) g_W = v;
            else if (pos == 1) g_H = v;
            else if (pos == 2) g_BPP = v;
            else {
                mt_logf("too many numbers");
                return 0;
            }
            pos++;
        } else if (!lstrcmpiA(a, "-list")) g_list = 1;
        else if (!lstrcmpiA(a, "-restore")) g_restore_only = 1;
        else if (!lstrcmpiA(a, "-a")) g_tests |= MT_T_A;
        else if (!lstrcmpiA(a, "-c")) g_tests |= MT_T_C;
        else if (!lstrcmpiA(a, "-d")) g_tests |= MT_T_D;
        else if (!lstrcmpiA(a, "-nobpp")) g_nobpp = 1;
        else if (!lstrcmpiA(a, "-novb")) g_novb = 1;
        else if (!lstrcmpiA(a, "-dry")) g_dry = 1;
        else if (!lstrcmpiA(a, "-b") || !lstrcmpiA(a, "-cb")) {
            int *dst = !lstrcmpiA(a, "-b") ? &g_b_want : &g_cb_want;
            if (i + 1 >= g_argc || !mt_rate_arg(g_argv[i + 1], dst)) {
                mt_logf("%s needs <hz|max>", a);
                return 0;
            }
            g_tests |= !lstrcmpiA(a, "-b") ? MT_T_B : MT_T_CB;
            i++;
        } else if (!lstrcmpiA(a, "-tag")) {
            /* a label the host puts on this run so it can find the run in an
             * appended log; logged with the command line, otherwise ignored */
            if (i + 1 >= g_argc) {
                mt_logf("-tag needs a word");
                return 0;
            }
            i++;
        } else if (!lstrcmpiA(a, "-pace") || !lstrcmpiA(a, "-hold") ||
                   !lstrcmpiA(a, "-maxhz")) {
            if (i + 1 >= g_argc || !mt_num(g_argv[i + 1], &v)) {
                mt_logf("%s needs a number", a);
                return 0;
            }
            if (!lstrcmpiA(a, "-pace")) {
                if (v < MT_PACE_MIN_S || v > MT_PACE_MAX_S) {
                    mt_logf("-pace must be %d..%d s", MT_PACE_MIN_S, MT_PACE_MAX_S);
                    return 0;
                }
                g_pace_s = v;
            } else if (!lstrcmpiA(a, "-hold")) {
                if (v < MT_HOLD_MIN_S || v > MT_HOLD_MAX_S) {
                    mt_logf("-hold must be %d..%d s", MT_HOLD_MIN_S, MT_HOLD_MAX_S);
                    return 0;
                }
                g_hold_s = v;
            } else {
                if (!mt_hz_is_real(v)) {
                    mt_logf("-maxhz must be a real refresh (%d..%d)", MT_HZ_MIN, MT_HZ_MAX - 1);
                    return 0;
                }
                g_op_maxhz = v;
            }
            i++;
        } else {
            mt_logf("unknown option '%s'", a);
            return 0;
        }
    }
    if (g_list || g_restore_only)
        return 1;
    if (pos != 3 || !g_tests) {
        mt_usage();
        return 0;
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* main                                                                 */
/* ------------------------------------------------------------------ */
/* ONE thread runs the exit sequence (the atexit handlers are vcr_pace's exit
 * hold); a second caller parks and ExitProcess takes it. Once the watchdog
 * owns the run the main thread never exits by itself either: the watchdog's
 * give-back decides the exit code and ends the process. */
static void mt_exit(int code)
{
    if (g_takeover && GetCurrentThreadId() != g_wd_tid)
        mt_park();
    if (InterlockedExchange(&g_exiting, 1))
        mt_park();
    InterlockedExchange(&g_done, 1);
    mt_logf("exit %d (switches made: %d)", code, g_switches);
    while (g_atexit_n > 0)                          /* vcr_pace's exit hold, if armed */
        g_atexit_fn[--g_atexit_n]();
    if (g_log != INVALID_HANDLE_VALUE)
        CloseHandle(g_log);
    ExitProcess((UINT)code);
}

/*
 * The run overran its budget. TAKE IT OVER, do not race it: raise g_takeover
 * (the main thread parks at its next switch or exit), then wait for g_mt_lock,
 * which the main thread holds only from a pace gate to that switch's stamp.
 *   - got it: no switch is in flight and none can start; give back what THIS
 *     RUN changed, if the screen is off its persisted mode (mt_should_giveback),
 *     through the same paced path as every other restore, and end;
 *   - did not get it within MT_TAKEOVER_MS: the main thread is stuck INSIDE a
 *     switch (a driver call that never returned). A second switch now would be
 *     unpaced and may collide with that one: end instead - XP reverts a
 *     CDS_FULLSCREEN / exclusive mode as the process goes, and vcr_pace's exit
 *     hold paces it; Windows 9x does not revert, and the log says so.
 */
static DWORD WINAPI mt_watchdog(LPVOID unused)
{
    DWORD w;
    mt_mode_t cur;
    (void)unused;
    Sleep(g_cap_ms);
    if (g_done)
        return 0;
    InterlockedExchange(&g_takeover, 1);
    mt_logf("WATCHDOG: the run exceeded its %lu s budget - taking it over",
            (unsigned long)(g_cap_ms / 1000u));
    w = g_mt_lock ? WaitForSingleObject(g_mt_lock, MT_TAKEOVER_MS) : WAIT_OBJECT_0;
    if (w == WAIT_TIMEOUT || w == WAIT_FAILED) {
        mt_logf("########################################################");
        mt_logf("## WATCHDOG: the main thread is stuck INSIDE a mode switch");
        mt_logf("## (%lu s) - ending WITHOUT a give-back of its own.",
                (unsigned long)(MT_TAKEOVER_MS / 1000u));
        if (g_is9x)
            mt_logf("## WINDOWS 9x DOES NOT REVERT the mode at exit: CHECK THE SCREEN.");
        else
            mt_logf("## XP reverts the mode as the process ends (exit hold paces it).");
        mt_logf("########################################################");
        g_restore_failed = 1;
        mt_exit(5);
    }
    memset(&cur, 0, sizeof cur);                    /* an unreadable mode reads "off" */
    mt_get_mode(ENUM_CURRENT_SETTINGS, &cur);
    if (mt_should_giveback(g_switches, &cur, &g_reg))
        mt_restore("watchdog restore");
    else
        mt_logf("WATCHDOG: %s - no give-back needed",
                g_switches ? "the screen is at its persisted mode"
                           : "this run switched nothing");
    mt_exit(5);
    return 0;
}

/* Start the watchdog. 0 = it could not be started, and a run that switches
 * modes must not start without it. lpThreadId is REQUIRED on Win95/98 - a
 * NULL there fails the call with error 87. */
static int mt_start_watchdog(void)
{
    HANDLE th = CreateThread(NULL, 0, mt_watchdog, NULL, 0, &g_wd_tid);
    if (!th) {
        mt_logf("REFUSED: the watchdog thread could not start (error %lu) - no switch is "
                "made without one", (unsigned long)GetLastError());
        return 0;
    }
    CloseHandle(th);
    return 1;
}

static int mt_main(void)
{
    OSVERSIONINFOA vi;
    int rc;

    g_t0 = GetTickCount();
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    InitializeCriticalSection(&g_logcs);
    g_logcs_ok = 1;
    mt_open_log();
    mt_split_args();
    mt_logf("==== modetest %s  (%s)", MODETEST_VERSION, GetCommandLineA());
    mt_resolve();

    memset(&vi, 0, sizeof vi);
    vi.dwOSVersionInfoSize = sizeof vi;
    GetVersionExA(&vi);
    g_is9x = vi.dwPlatformId == VER_PLATFORM_WIN32_WINDOWS;
    mt_logf("OS: platform %lu, %lu.%lu build %lu %s", vi.dwPlatformId, vi.dwMajorVersion,
            vi.dwMinorVersion, vi.dwBuildNumber & 0xFFFF, vi.szCSDVersion);
    /* process-local, unnamed: the pace gate's own mutex is the cross-process one */
    g_mt_lock = CreateMutexA(NULL, FALSE, NULL);
    if (!g_mt_lock) {
        mt_logf("REFUSED: no switch lock (CreateMutex error %lu)", (unsigned long)GetLastError());
        return 2;
    }

    if (!mt_parse())
        return 2;

    /* GetVersionEx says 6.2 on every Windows 8/10/11 without a manifest - so
     * ">= 6.2" is exactly the modern family, and this exe has no manifest */
    if (vi.dwPlatformId == VER_PLATFORM_WIN32_NT &&
        (vi.dwMajorVersion > 6 || (vi.dwMajorVersion == 6 && vi.dwMinorVersion >= 2))) {
        mt_logf("REFUSED: Windows 8 or later - not a managed retro box");
        return 2;
    }

    /*
     * The persisted mode, read the way the two writers read it (live, then
     * registry: agent/src/gameres.c gameres_probe, fleetres.c main). An XP box
     * whose registry holds NO DefaultSettings.* - the build VM, measured
     * 2026-09-29 - answers ENUM_REGISTRY_SETTINGS with TRUE and an all-zero
     * DEVMODE. The writers then take the live SIZE and NO rate (reg_hz 0:
     * FR_HZ falls back to 60, the no-EDID per-target rate is 0). modetest
     * falls back to the live mode for its own checks, and says so - 0.1.0
     * printed that zero answer as a real persisted "@60" (the live rate).
     */
    memset(&g_reg, 0, sizeof g_reg);
    if (!mt_get_mode(ENUM_CURRENT_SETTINGS, &g_live)) {
        mt_logf("REFUSED: cannot read the live mode");
        return 2;
    }
    g_have_reg = mt_get_mode(ENUM_REGISTRY_SETTINGS, &g_reg) && g_reg.w >= 320;
    mt_logf("registry  : ENUM_REGISTRY_SETTINGS -> %dx%dx%d@%d%s", g_reg.w, g_reg.h, g_reg.bpp,
            g_reg.hz, g_have_reg ? "" : "  (empty: the writers see NO persisted rate here)");
    mt_enum_modes();
    if (!g_have_reg)
        g_reg = g_live;                             /* the live mode is all we have */
    mt_probe_edid();
    mt_logf("live      : %dx%dx%d @%d Hz", g_live.w, g_live.h, g_live.bpp, g_live.hz);
    mt_logf("persisted : %dx%dx%d @%d Hz%s", g_reg.w, g_reg.h, g_reg.bpp, g_reg.hz,
            g_have_reg ? "" : " (ENUM_REGISTRY_SETTINGS failed - using live)");
    if (g_edid_ok)
        mt_logf("EDID      : %s [%s] native %dx%d, vmax %d Hz, hmax %d kHz", g_edid_name,
                g_edid_pnp, g_edid_w, g_edid_h, g_edid_vmax, g_edid_hmax);
    else
        mt_logf("EDID      : NONE READ - no measured ceiling (only -maxhz can give one)");
    mt_logf("ceiling   : %d Hz (EDID %d, -maxhz %d; 0 = none)",
            mt_ceiling(g_edid_ok ? g_edid_vmax : 0, g_op_maxhz), g_edid_ok ? g_edid_vmax : 0,
            g_op_maxhz);
    mt_log_modes();

    if (g_list) {
        mt_obs_t o;
        mt_logf("=== LIST: no switch. Timing the desktop's own scanout");
        mt_observe(NULL, &o);
        g_W = g_live.w;
        g_H = g_live.h;
        g_BPP = g_live.bpp;
        mt_result("LIST", "desktop", g_live.hz, &o, "none");
        return 0;
    }

    {
        DWORD ms;
        vcr_pace_set_min((DWORD)g_pace_s * 1000u);
        ms = 60000u + (DWORD)(mt_plan_switches(g_tests) + 2) *
                          ((DWORD)g_pace_s * 1000u + (DWORD)g_hold_s * 1000u + 4000u);
        g_cap_ms = ms > 600000u ? 600000u : ms;
#ifdef MT_TEST_CAP_MS
        /* TEST BUILDS ONLY (never shipped): a short budget, so the watchdog's
         * take-over can be exercised in the build VM */
        g_cap_ms = MT_TEST_CAP_MS;
        mt_logf("TEST BUILD: watchdog budget forced to %lu ms", (unsigned long)g_cap_ms);
#endif
    }

    if (g_restore_only) {
        if (mt_live_is_persisted(&g_live, &g_reg)) {
            mt_logf("-restore: the screen is already at its persisted mode - no switch made");
            return 0;
        }
        if (!mt_start_watchdog())
            return 2;
        return mt_restore("operator -restore") ? 0 : 3;
    }

    /* every refusal happens here, before the first switch */
    rc = MT_OK;
    if (!mt_live_is_persisted(&g_live, &g_reg))
        rc = MT_E_LIVE_NOT_PERSISTED;
    if (rc == MT_OK && mt_plan_switches(g_tests) > MT_MAX_SWITCHES)
        rc = MT_E_TOO_MANY_SWITCHES;
    if (rc == MT_OK)
        rc = mt_check_nobpp(g_nobpp, g_BPP, &g_reg);
    if (rc == MT_OK) {
        int hz;
        rc = mt_check_request(g_modes, g_nmodes, &g_reg, g_edid_ok ? g_edid_vmax : 0,
                              g_edid_ok ? g_edid_hmax : 0, g_op_maxhz, g_W, g_H, g_BPP, 0, &hz);
    }
    if (rc == MT_OK && (g_tests & MT_T_B))
        rc = mt_check_request(g_modes, g_nmodes, &g_reg, g_edid_ok ? g_edid_vmax : 0,
                              g_edid_ok ? g_edid_hmax : 0, g_op_maxhz, g_W, g_H, g_BPP, g_b_want, &g_b_hz);
    if (rc == MT_OK && (g_tests & MT_T_CB))
        rc = mt_check_request(g_modes, g_nmodes, &g_reg, g_edid_ok ? g_edid_vmax : 0,
                              g_edid_ok ? g_edid_hmax : 0, g_op_maxhz, g_W, g_H, g_BPP, g_cb_want, &g_cb_hz);
    mt_logf("PLAN: %dx%dx%d tests%s%s%s%s%s  pre-rate B=%d CB=%d  switches<=%d  pace %d s  "
            "hold %d s  budget %lu s", g_W, g_H, g_BPP, (g_tests & MT_T_A) ? " A" : "",
            (g_tests & MT_T_B) ? " B" : "", (g_tests & MT_T_C) ? " C" : "",
            (g_tests & MT_T_CB) ? " CB" : "", (g_tests & MT_T_D) ? " D" : "", g_b_hz, g_cb_hz,
            mt_plan_switches(g_tests), g_pace_s, g_hold_s, (unsigned long)(g_cap_ms / 1000u));
    if (rc != MT_OK) {
        mt_logf("REFUSED: %s - nothing was switched", mt_reason(rc));
        return 2;
    }
    if (g_dry) {
        mt_logf("DRY RUN: every request passed its checks; nothing was switched");
        return 0;
    }

    if (!mt_start_watchdog())
        return 2;
    if (g_tests & MT_T_A)  mt_test_a();
    if (g_tests & MT_T_B)  mt_test_b();
    if (g_tests & MT_T_C)  mt_test_c(0);
    if (g_tests & MT_T_CB) mt_test_c(g_cb_hz);
    if (g_tests & MT_T_D)  mt_test_d();

    {
        mt_mode_t cur;
        memset(&cur, 0, sizeof cur);
        mt_get_mode(ENUM_CURRENT_SETTINGS, &cur);
        if (mt_should_giveback(g_switches, &cur, &g_reg))
            mt_restore("final check found the screen off its persisted mode");
        else if (!mt_live_is_persisted(&cur, &g_reg))
            mt_logf("final: the screen is %dx%dx%d@%d, NOT its persisted mode - and this run "
                    "switched nothing, so it is someone else's; left alone", cur.w, cur.h,
                    cur.bpp, cur.hz);
        else
            mt_logf("final: the screen is at its persisted mode %dx%dx%d@%d", cur.w, cur.h,
                    cur.bpp, cur.hz);
    }
    return g_restore_failed ? 3 : (g_test_failed ? 4 : 0);
}

void WINAPI start(void)
{
    mt_exit(mt_main());
}
