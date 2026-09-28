/*
 * 3dfxctl - the 3dfx Control Panel (Win32, Windows XP; also 2000).
 *
 * A settings panel for a 3dfx card that knows WHICH driver stack drives it:
 *
 *   OUR stack ("clean-room lane", the V5 6000 in .124): the vcr-kmd kernel pair
 *     (vcrmp.sys + vcrdd.dll), our h5 Glide fork and our MesaFX OpenGL ICD.
 *     Detected by the display driver answering VCR_ESC_INFO.
 *   the VINTAGE 3dfxvs driver (retro-3dfx H5 source, AmigaMerlin): the keys the
 *     first 3dfxctl wrote, unchanged.
 *
 * Every control is a row of ctl_logic.h's table, which names the value, where
 * the stack reads it and when. Nothing here writes a value the stack does not
 * read, and every write is read back and reported (the summary after Apply and
 * 3dfxctl.log beside the exe), because a tool that says "applied" without
 * looking is how this project keeps losing time.
 *
 * NO REBOOT. Glide and OpenGL settings are read when a game starts, so they
 * apply at the next game launch (the environment ones: games started from the
 * desktop or Start menu after Apply - Explorer re-reads the environment on the
 * WM_SETTINGCHANGE the panel broadcasts). The kernel switches the panel offers
 * are the ones vcr-kmd reads without a reboot; the 2D ones are read at every
 * new display surface, so the panel re-sets the current display mode once, and
 * every display-mode change it makes goes through tools/vcr_pace.h (the
 * monitor is a 1998-era CRT: at least 3 s between switches, box-wide).
 *
 * ANTI-ALIASING is experimental on the V5 6000 (a hard freeze, a ghost image,
 * modes never run). It is off unless a person ticks "allow experimental AA",
 * reads the warning and says yes; only then are AA modes listed, and choosing
 * one arms the kernel's AA switch (Diag\SliAA). Presets never select AA.
 *
 *   3dfxctl.exe                 the panel
 *   3dfxctl.exe /report FILE    write what the panel sees (stack, every value,
 *                               overrides) to FILE and exit - for the agent
 *   3dfxctl.exe /vintage | /vcr force a lane (testing only)
 *
 * Build: make (i686-w64-mingw32-gcc, static, GUI subsystem; see Makefile).
 */
#define WIN32_LEAN_AND_MEAN
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0501
#endif
#ifndef _WIN32_IE
#define _WIN32_IE 0x0600
#endif
#include <windows.h>
#include <commctrl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>

#include "vcr_types.h"
#include "vcr_ioctl.h"

static void ctl_log(const char *fmt, ...);
/* the gate reports into the panel's log - a GUI program has no stderr */
#define VCR_PACE_REPORT(what, err) ctl_log("vcr_pace: %s (error %lu)", (what), (unsigned long)(err))
#include "vcr_pace.h"
#include "ctl_logic.h"

#define CTL_VERSION     "2.0.0"
#define APP_TITLE       "3dfx Control Panel"

/* ---- control ids ------------------------------------------------------------------ */
#define IDC_HEADER      100
#define IDC_TAB         101
#define IDC_OK          102
#define IDC_CANCEL      103
#define IDC_APPLY       104
#define IDC_DEFAULTS    105
#define IDC_STATUS      106
#define IDC_P_QUALITY   110
#define IDC_P_SPEED     111
#define IDC_P_DEFAULTS  112
#define IDC_AA_ALLOW    120
#define IDC_AA_LIST     121
#define IDC_AA_DISARM   122
#define IDC_AA_SCAN     123
#define IDC_AA_LIVE     124
#define IDC_DISP_PROPS  130
#define IDC_2D_STATE    131
#define IDC_RM_OVERRIDE 140
#define IDC_ROW_BASE    1000            /* + row index * 4 (+0 control, +1 value label) */

#define WM_APP_DISPDONE (WM_APP + 1)
#define WM_APP_REBUILD  (WM_APP + 2)        /* wp 1 = re-read the machine, 0 = redraw pages */
#define TIMER_LIVE      1
#define TIMER_COUNTDOWN 2

#define MAXROWS         48
#define MAXITEMS        40
#define MAXMODES        600

/* ---- colours: readable on XP's Luna AND on the fleet's dark classic scheme -------- */
static int g_dark;                          /* COLOR_BTNFACE is dark (the fleet theme) */
static COLORREF c_red(void)    { return g_dark ? RGB(255, 110, 100) : RGB(192, 0, 0); }
static COLORREF c_orange(void) { return g_dark ? RGB(255, 180, 70) : RGB(176, 88, 0); }
static COLORREF c_green(void)  { return g_dark ? RGB(110, 230, 110) : RGB(16, 124, 16); }
static COLORREF c_gray(void)   { return g_dark ? RGB(170, 170, 170) : RGB(96, 96, 96); }
static COLORREF c_blue(void)   { return g_dark ? RGB(120, 190, 255) : RGB(0, 84, 166); }

/* ---- the log: the Advanced tab, and 3dfxctl.log beside the exe -------------------- */
static char g_logbuf[32768];
static char g_logpath[MAX_PATH];
static HWND g_log_edit;

static void ctl_log(const char *fmt, ...)
{
    char line[1024];
    SYSTEMTIME t;
    va_list ap;
    size_t have, n;
    HANDLE h;
    DWORD put;
    GetLocalTime(&t);
    n = (size_t)_snprintf(line, sizeof line, "%04u-%02u-%02u %02u:%02u:%02u  ", t.wYear, t.wMonth,
                          t.wDay, t.wHour, t.wMinute, t.wSecond);
    va_start(ap, fmt);
    _vsnprintf(line + n, sizeof line - n - 3, fmt, ap);
    va_end(ap);
    line[sizeof line - 3] = 0;
    strcat(line, "\r\n");
    n = strlen(line);
    have = strlen(g_logbuf);
    if (have + n + 1 > sizeof g_logbuf) {           /* drop the oldest half */
        memmove(g_logbuf, g_logbuf + have / 2, have - have / 2 + 1);
        have = strlen(g_logbuf);
    }
    memcpy(g_logbuf + have, line, n + 1);
    if (g_log_edit) {
        SetWindowTextA(g_log_edit, g_logbuf);
        SendMessageA(g_log_edit, EM_SETSEL, (WPARAM)-1, (LPARAM)-1);
        SendMessageA(g_log_edit, EM_SCROLLCARET, 0, 0);
    }
    if (g_logpath[0]) {
        h = CreateFileA(g_logpath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                        OPEN_ALWAYS, 0, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            WriteFile(h, line, (DWORD)n, &put, NULL);
            CloseHandle(h);
        }
    }
}

/* ---- small string helpers ----------------------------------------------------------- */
static void scpy(char *d, const char *s, size_t n)
{
    if (!n)
        return;
    _snprintf(d, n, "%s", s ? s : "");
    d[n - 1] = 0;
}

static void scat(char *d, size_t n, const char *fmt, ...)
{
    size_t have = strlen(d);
    va_list ap;
    if (have + 1 >= n)
        return;
    va_start(ap, fmt);
    _vsnprintf(d + have, n - have - 1, fmt, ap);
    va_end(ap);
    d[n - 1] = 0;
}

static int streq(const char *a, const char *b)
{
    if (!a || !b)
        return a == b;
    return strcmp(a, b) == 0;
}

static const char *ci_strstr(const char *hay, const char *needle)
{
    size_t n = strlen(needle);
    for (; *hay; hay++)
        if (_strnicmp(hay, needle, n) == 0)
            return hay;
    return NULL;
}

/* ---- registry ------------------------------------------------------------------------ */
#define REG_WRONG_TYPE ((LONG)0x7fff0001)

/* ERROR_SUCCESS (text in buf), ERROR_FILE_NOT_FOUND (no key or no value),
 * REG_WRONG_TYPE (a value, but not text - Glide ignores it), or another error */
static LONG reg_get_sz(HKEY root, const char *path, const char *name, char *buf, DWORD n)
{
    HKEY h;
    DWORD type = 0, cb = n - 1;
    LONG r;
    buf[0] = 0;
    r = RegOpenKeyExA(root, path, 0, KEY_READ, &h);
    if (r != ERROR_SUCCESS)
        return r == ERROR_PATH_NOT_FOUND ? ERROR_FILE_NOT_FOUND : r;
    r = RegQueryValueExA(h, name, NULL, &type, (BYTE *)buf, &cb);
    RegCloseKey(h);
    if (r != ERROR_SUCCESS)
        return r;
    if (type != REG_SZ && type != REG_EXPAND_SZ) {
        buf[0] = 0;
        return REG_WRONG_TYPE;
    }
    buf[cb < n ? cb : n - 1] = 0;
    return ERROR_SUCCESS;
}

static LONG reg_get_dword(HKEY root, const char *path, const char *name, DWORD *v)
{
    HKEY h;
    DWORD type = 0, cb = sizeof *v;
    LONG r = RegOpenKeyExA(root, path, 0, KEY_READ, &h);
    if (r != ERROR_SUCCESS)
        return r == ERROR_PATH_NOT_FOUND ? ERROR_FILE_NOT_FOUND : r;
    r = RegQueryValueExA(h, name, NULL, &type, (BYTE *)v, &cb);
    RegCloseKey(h);
    if (r == ERROR_SUCCESS && cb < 4)
        return REG_WRONG_TYPE;
    return r;
}

static LONG reg_set_sz(HKEY root, const char *path, const char *name, const char *val)
{
    HKEY h;
    LONG r = RegCreateKeyExA(root, path, 0, NULL, 0, KEY_SET_VALUE, NULL, &h, NULL);
    if (r != ERROR_SUCCESS)
        return r;
    r = RegSetValueExA(h, name, 0, REG_SZ, (const BYTE *)val, (DWORD)strlen(val) + 1);
    RegCloseKey(h);
    return r;
}

static LONG reg_set_dword(HKEY root, const char *path, const char *name, DWORD v)
{
    HKEY h;
    LONG r = RegCreateKeyExA(root, path, 0, NULL, 0, KEY_SET_VALUE, NULL, &h, NULL);
    if (r != ERROR_SUCCESS)
        return r;
    r = RegSetValueExA(h, name, 0, REG_DWORD, (const BYTE *)&v, sizeof v);
    RegCloseKey(h);
    return r;
}

/* Deletes a VALUE - never a key. Absent already = success. */
static LONG reg_del_value(HKEY root, const char *path, const char *name)
{
    HKEY h;
    LONG r = RegOpenKeyExA(root, path, 0, KEY_SET_VALUE, &h);
    if (r == ERROR_FILE_NOT_FOUND || r == ERROR_PATH_NOT_FOUND)
        return ERROR_SUCCESS;
    if (r != ERROR_SUCCESS)
        return r;
    r = RegDeleteValueA(h, name);
    RegCloseKey(h);
    return r == ERROR_FILE_NOT_FOUND ? ERROR_SUCCESS : r;
}

static int reg_key_exists(HKEY root, const char *path)
{
    HKEY h;
    if (RegOpenKeyExA(root, path, 0, KEY_READ, &h) != ERROR_SUCCESS)
        return 0;
    RegCloseKey(h);
    return 1;
}

static const char *reg_err(LONG r)
{
    static char b[64];
    if (r == ERROR_ACCESS_DENIED)
        return "access denied - run the panel from an administrator account";
    if (r == REG_WRONG_TYPE)
        return "the value has the wrong type";
    _snprintf(b, sizeof b, "registry error %ld", (long)r);
    b[sizeof b - 1] = 0;
    return b;
}

/* ---- the stack -------------------------------------------------------------------- */
typedef struct {
    int         lane;               /* CTL_LANE_VCR / CTL_LANE_VINTAGE / 0 */
    int         forced;
    int         have_info;
    vcr_info    info;
    int         have_dd;
    ULONG       dd[4];              /* dd_mode, exclusive_pid, hwc_requests, screen */
    int         have_2d;
    int         size_2d;
    vcr_2d_stats st2d;
    unsigned    nchips;             /* what Glide is told (vcr_info.glide_chips) */
    char        adapter[128];
    int         has_3dfxvs_dev0;
    const char *glide_key;
    char        glide_dll[MAX_PATH];
    int         glide_present, glide_guard, glide_trace, glide_noplugin;
    char        icd_name[64], icd_dll[MAX_PATH], icd_ver[64];
    int         icd_present;
    int         plugin_present;
    char        plugin_path[MAX_PATH];
    DEVMODEA    cur, reg;
    int         have_cur, have_reg;
    ctl_mode    modes[MAXMODES];
    int         nmodes;
    int         sliaa_present;
    DWORD       sliaa;
    int         themed;
} STACK;
static STACK G;

static int esc_call(ULONG code, void *out, int cout)
{
    HDC dc = GetDC(NULL);
    int n;
    if (!dc)
        return 0;
    n = ExtEscape(dc, (int)code, 0, NULL, cout, (LPSTR)out);
    ReleaseDC(NULL, dc);
    return n;
}

/* 1 = our driver's mode list is NOT limited to what the monitor accepts
 * (Diag\EdidFilter = 0): then no fixed refresh rate is offered or set */
static int mon_unfiltered(void)
{
    return G.lane == CTL_LANE_VCR && G.have_info && G.info.backend == VCR_HW_VOODOO &&
           !G.info.mon_filter;
}

static void stack_live(void)
{
    int n;
    memset(&G.info, 0, sizeof G.info);
    n = esc_call(VCR_ESC_INFO, &G.info, sizeof G.info);
    G.have_info = n > 0 && G.info.size >= FIELD_OFFSET(vcr_info, glide_chips);
    memset(G.dd, 0, sizeof G.dd);
    G.have_dd = G.have_info && esc_call(VCR_ESC_DD_STATS, G.dd, sizeof G.dd) >= (int)sizeof G.dd;
    memset(&G.st2d, 0, sizeof G.st2d);
    G.size_2d = G.have_info ? esc_call(VCR_ESC_2D_STATS, &G.st2d, sizeof G.st2d) : 0;
    G.have_2d = G.size_2d >= (int)VCR_2DS_SIZE_V1;
    if (G.have_info)
        G.nchips = G.info.glide_chips ? G.info.glide_chips : G.info.nchips;
    memset(&G.cur, 0, sizeof G.cur);
    G.cur.dmSize = sizeof G.cur;
    G.have_cur = EnumDisplaySettingsA(NULL, ENUM_CURRENT_SETTINGS, &G.cur);
    memset(&G.reg, 0, sizeof G.reg);
    G.reg.dmSize = sizeof G.reg;
    G.have_reg = EnumDisplaySettingsA(NULL, ENUM_REGISTRY_SETTINGS, &G.reg);
    G.sliaa = 0;
    G.sliaa_present = reg_get_dword(HKEY_LOCAL_MACHINE, CTL_KEY_DIAG, "SliAA", &G.sliaa) ==
                      ERROR_SUCCESS;
}

/* the whole file, for a marker search (a Glide or ICD DLL: a few MB) */
static char *read_file(const char *path, DWORD *len)
{
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                           OPEN_EXISTING, 0, NULL);
    DWORD sz, got = 0;
    char *b;
    if (h == INVALID_HANDLE_VALUE)
        return NULL;
    sz = GetFileSize(h, NULL);
    if (sz == INVALID_FILE_SIZE || sz > 16u << 20) {
        CloseHandle(h);
        return NULL;
    }
    b = (char *)malloc(sz + 1);
    if (b && (!ReadFile(h, b, sz, &got, NULL) || got != sz)) {
        free(b);
        b = NULL;
    }
    CloseHandle(h);
    if (b) {
        b[sz] = 0;
        *len = sz;
    }
    return b;
}

static const char *mem_find(const char *b, DWORD len, const char *s)
{
    size_t n = strlen(s);
    DWORD i;
    if (len < n)
        return NULL;
    for (i = 0; i + n <= len; i++)
        if (b[i] == s[0] && memcmp(b + i, s, n) == 0)
            return b + i;
    return NULL;
}

/* 1 = a glide3x.dll carrying our fork's SLI/AA guard (GR_EXTENSION token) */
static int glide_dll_check(const char *path, int *guard, int *trace, int *noplugin)
{
    DWORD len = 0;
    char *b = read_file(path, &len);
    if (!b)
        return 0;
    *guard = mem_find(b, len, "RETRO3DFX_SLIAA_GUARD") != NULL;
    if (trace)
        *trace = mem_find(b, len, "RETRO3DFX_AA_TRACE") != NULL;
    if (noplugin)
        *noplugin = mem_find(b, len, "FX_GLIDE_NO_PLUGIN") != NULL;
    free(b);
    return 1;
}

static void stack_detect(int force_lane)
{
    DISPLAY_DEVICEA dd;
    DWORD i;
    char sys[MAX_PATH], tmp[MAX_PATH], val[MAX_PATH];
    DEVMODEA dm;

    memset(&G, 0, sizeof G);
    stack_live();
    for (i = 0; i < 16; i++) {
        memset(&dd, 0, sizeof dd);
        dd.cb = sizeof dd;
        if (!EnumDisplayDevicesA(NULL, i, &dd, 0))
            break;
        if (dd.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) {
            scpy(G.adapter, dd.DeviceString, sizeof G.adapter);
            break;
        }
    }
    if (force_lane) {
        G.lane = force_lane;
        G.forced = 1;
    } else if (G.have_info) {
        G.lane = CTL_LANE_VCR;
    } else if (reg_key_exists(HKEY_LOCAL_MACHINE, "SYSTEM\\CurrentControlSet\\Services\\3dfxvs") &&
               (ci_strstr(G.adapter, "3dfx") || ci_strstr(G.adapter, "voodoo"))) {
        G.lane = CTL_LANE_VINTAGE;
    }
    G.has_3dfxvs_dev0 = reg_key_exists(HKEY_LOCAL_MACHINE, CTL_KEY_3DFXVS_DEV0);
    G.glide_key = ctl_glide_regpath(G.has_3dfxvs_dev0);

    GetSystemDirectoryA(sys, sizeof sys);
    _snprintf(G.glide_dll, sizeof G.glide_dll, "%s\\glide3x.dll", sys);
    G.glide_dll[sizeof G.glide_dll - 1] = 0;
    G.glide_present = glide_dll_check(G.glide_dll, &G.glide_guard, &G.glide_trace,
                                      &G.glide_noplugin);
    _snprintf(G.plugin_path, sizeof G.plugin_path, "%s\\3dfxspl3.dll", sys);
    G.plugin_path[sizeof G.plugin_path - 1] = 0;
    G.plugin_present = GetFileAttributesA(G.plugin_path) != INVALID_FILE_ATTRIBUTES;
    if (!G.plugin_present) {
        GetWindowsDirectoryA(tmp, sizeof tmp);
        _snprintf(G.plugin_path, sizeof G.plugin_path, "%s\\3dfxspl3.dll", tmp);
        G.plugin_path[sizeof G.plugin_path - 1] = 0;
        G.plugin_present = GetFileAttributesA(G.plugin_path) != INVALID_FILE_ATTRIBUTES;
    }

    /* the system OpenGL ICD, by the name the display driver reports */
    scpy(G.icd_name, "3dfx", sizeof G.icd_name);
    if (G.have_info && G.info.ogl_name[0]) {
        for (i = 0; i < 31 && G.info.ogl_name[i]; i++)
            G.icd_name[i] = (char)(G.info.ogl_name[i] < 0x80 ? G.info.ogl_name[i] : '?');
        G.icd_name[i] = 0;
    }
    _snprintf(tmp, sizeof tmp, "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\OpenGLDrivers\\%s",
              G.icd_name);
    tmp[sizeof tmp - 1] = 0;
    if (reg_get_sz(HKEY_LOCAL_MACHINE, tmp, "Dll", val, sizeof val) == ERROR_SUCCESS ||
        reg_get_sz(HKEY_LOCAL_MACHINE,
                   "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\OpenGLDrivers", G.icd_name,
                   val, sizeof val) == ERROR_SUCCESS) {
        _snprintf(G.icd_dll, sizeof G.icd_dll, "%s\\%s%s", sys, val,
                  ci_strstr(val, ".dll") ? "" : ".dll");
        G.icd_dll[sizeof G.icd_dll - 1] = 0;
        {
            DWORD len = 0;
            char *b = read_file(G.icd_dll, &len);
            if (b) {
                const char *v = mem_find(b, len, "voodoo-cleanroom ");
                G.icd_present = 1;
                if (v) {
                    size_t k;
                    for (k = 0; k + 1 < sizeof G.icd_ver && v + k < b + len && v[k] != ']' &&
                                v[k] != 0; k++)
                        G.icd_ver[k] = v[k];
                    G.icd_ver[k] = 0;
                }
                free(b);
            }
        }
    }

    /* every mode the driver enumerates */
    for (i = 0; G.nmodes < MAXMODES; i++) {
        memset(&dm, 0, sizeof dm);
        dm.dmSize = sizeof dm;
        if (!EnumDisplaySettingsA(NULL, i, &dm))
            break;
        G.modes[G.nmodes].w = dm.dmPelsWidth;
        G.modes[G.nmodes].h = dm.dmPelsHeight;
        G.modes[G.nmodes].bpp = dm.dmBitsPerPel;
        G.modes[G.nmodes].hz = dm.dmDisplayFrequency;
        G.nmodes++;
    }
}

/* the uxtheme calls, resolved at run time (a classic-only box has no theme) */
typedef HRESULT(WINAPI *PFN_ETDT)(HWND, DWORD);
typedef BOOL(WINAPI *PFN_ISTHEME)(void);
static PFN_ETDT p_EnableThemeDialogTexture;

static void theme_init(void)
{
    HMODULE ux = LoadLibraryA("uxtheme.dll");
    PFN_ISTHEME active, appthemed;
    COLORREF f = GetSysColor(COLOR_BTNFACE);
    g_dark = (GetRValue(f) * 30 + GetGValue(f) * 59 + GetBValue(f) * 11) < 128 * 100;
    if (!ux)
        return;
    active = (PFN_ISTHEME)GetProcAddress(ux, "IsThemeActive");
    appthemed = (PFN_ISTHEME)GetProcAddress(ux, "IsAppThemed");
    p_EnableThemeDialogTexture = (PFN_ETDT)GetProcAddress(ux, "EnableThemeDialogTexture");
    G.themed = active && appthemed && active() && appthemed() && p_EnableThemeDialogTexture;
}

/* ---- the rows: current (from the machine) and pending (from the controls) ---------- */
typedef struct {
    const ctl_row *row;
    int     present;                /* the value exists where the stack reads it */
    char    cur[64];
    int     pend_present;
    char    pend[64];
    char    note[600];              /* overrides, oddities, why a control is disabled */
    int     note_level;             /* 0 info, 1 warning, 2 an override that WINS */
    int     disabled;
    /* combo items: value per item index */
    int     nitems;
    char    ival[MAXITEMS][24];
    int     iabsent[MAXITEMS];
    HWND    hctl, hval, hwhen;
} ROWSTATE;
static ROWSTATE R[MAXROWS];
static int NR;

/* overrides of our settings that the panel found elsewhere (a copy that WINS) */
typedef struct {
    HKEY    root;
    char    path[160];
    char    name[48];
    char    value[64];
    char    what[200];
} OVERRIDE;
static OVERRIDE OV[32];
static int NOV;
static int g_rm_overrides;          /* "remove them on Apply" */

static int g_allow_exp;             /* the experimental-AA checkbox */
static int g_aa_confirmed;          /* the person said yes to the warning, this session */

static ROWSTATE *rs_by_id(int id)
{
    int i;
    for (i = 0; i < NR; i++)
        if (R[i].row->id == id)
            return &R[i];
    return NULL;
}

/* vintage gamma LUT (the first 3dfxctl's): gamma from the midpoint of the curve */
static double v_gamma_get(const char *name, double dflt, int *present)
{
    HKEY h;
    DWORD type, cb = 256 * sizeof(ULONG);
    ULONG tab[256];
    double g = dflt;
    *present = 0;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, CTL_KEY_V_DEV0, 0, KEY_READ, &h) != ERROR_SUCCESS)
        return dflt;
    if (RegQueryValueExA(h, name, 0, &type, (BYTE *)tab, &cb) == ERROR_SUCCESS &&
        type == REG_BINARY && cb >= 129 * sizeof(ULONG)) {
        int mid = (int)(tab[128] & 0xFF);
        *present = 1;
        if (mid > 1 && mid < 255)
            g = log(128.0 / 255.0) / log((double)mid / 255.0);
    }
    RegCloseKey(h);
    return g;
}

/* 1 written, 0 failed, -1 refused (a desktop gamma that would wash out the 2D desktop) */
static int v_gamma_set(const char *name, double g, int clamp_safe)
{
    HKEY h;
    ULONG tab[256];
    int i;
    LONG r;
    if (g < 0.30)
        g = 0.30;
    if (g > 3.00)
        g = 3.00;
    for (i = 0; i < 256; i++) {
        double o = (i == 0) ? 0.0 : pow((double)i / 255.0, 1.0 / g) * 255.0 + 0.5;
        int v = (o < 0) ? 0 : (o > 255) ? 255 : (int)o;
        tab[i] = ((ULONG)v << 16) | ((ULONG)v << 8) | (ULONG)v;
    }
    if (clamp_safe && (int)(tab[128] & 0xFF) >= 0xE0)
        return -1;
    if (RegCreateKeyExA(HKEY_LOCAL_MACHINE, CTL_KEY_V_DEV0, 0, 0, 0, KEY_SET_VALUE, 0, &h, 0) !=
        ERROR_SUCCESS)
        return 0;
    r = RegSetValueExA(h, name, 0, REG_BINARY, (const BYTE *)tab, sizeof tab);
    RegCloseKey(h);
    return r == ERROR_SUCCESS;
}

static HKEY store_root(int store)
{
    return store == CTL_ST_ENV ? HKEY_CURRENT_USER : HKEY_LOCAL_MACHINE;
}

static const char *store_path(int store)
{
    switch (store) {
    case CTL_ST_GLIDE:  return G.glide_key;
    case CTL_ST_ENV:    return CTL_KEY_USER_ENV;
    case CTL_ST_DIAG:   return CTL_KEY_DIAG;
    case CTL_ST_V_DEV0: return CTL_KEY_V_DEV0;
    case CTL_ST_V_D3D:  return CTL_KEY_V_D3D;
    case CTL_ST_V_GLIDE: return CTL_KEY_V_GLIDE;
    }
    return "";
}

static const char *store_where(int store)
{
    static char b[200];
    switch (store) {
    case CTL_ST_ENV:     return "HKCU\\Environment";
    case CTL_ST_DISPLAY: return "the display mode";
    }
    _snprintf(b, sizeof b, "HKLM\\%s", store_path(store));
    b[sizeof b - 1] = 0;
    return b;
}

/* the first name of a "A|B|C" row */
static void first_name(const ctl_row *r, char *out, size_t n)
{
    const char *bar = strchr(r->name, '|');
    size_t k = bar ? (size_t)(bar - r->name) : strlen(r->name);
    if (k >= n)
        k = n - 1;
    memcpy(out, r->name, k);
    out[k] = 0;
}

static void add_override(HKEY root, const char *path, const char *name, const char *value,
                         const char *what)
{
    OVERRIDE *o;
    if (NOV >= (int)(sizeof OV / sizeof OV[0]))
        return;
    o = &OV[NOV++];
    o->root = root;
    scpy(o->path, path, sizeof o->path);
    scpy(o->name, name, sizeof o->name);
    scpy(o->value, value, sizeof o->value);
    scpy(o->what, what, sizeof o->what);
}

/* Where else our stack reads this name - the copies that WIN over the panel's
 * store (an override) or that apply when the panel's value is absent (info). */
static void row_scan_elsewhere(ROWSTATE *s)
{
    const ctl_row *r = s->row;
    char name[48], v[128], line[300];
    const char *bar;
    const char *p = r->name;
    if (!(r->lanes & CTL_LANE_VCR))
        return;
    while (p && *p) {
        bar = strchr(p, '|');
        _snprintf(name, sizeof name, "%.*s", (int)(bar ? bar - p : (int)strlen(p)), p);
        name[sizeof name - 1] = 0;
        p = bar ? bar + 1 : NULL;
        if (r->store == CTL_ST_GLIDE) {
            /* hwcGetenv: process environment, then HKCU\<key>, then HKLM\<key> */
            if (reg_get_sz(HKEY_CURRENT_USER, CTL_KEY_USER_ENV, name, v, sizeof v) == ERROR_SUCCESS) {
                _snprintf(line, sizeof line, "OVERRIDDEN: %s=%s in your environment "
                          "(HKCU\\Environment) wins over this setting. ", name, v);
                scat(s->note, sizeof s->note, "%s", line);
                s->note_level = 2;
                add_override(HKEY_CURRENT_USER, CTL_KEY_USER_ENV, name, v,
                             "user environment - wins over the Glide registry key");
            } else if (reg_get_sz(HKEY_LOCAL_MACHINE, CTL_KEY_SYSTEM_ENV, name, v, sizeof v) ==
                       ERROR_SUCCESS) {
                scat(s->note, sizeof s->note, "OVERRIDDEN: %s=%s in the system environment "
                     "wins over this setting. ", name, v);
                s->note_level = 2;
                add_override(HKEY_LOCAL_MACHINE, CTL_KEY_SYSTEM_ENV, name, v,
                             "system environment - wins over the Glide registry key");
            } else if (GetEnvironmentVariableA(name, v, sizeof v) > 0) {
                scat(s->note, sizeof s->note, "This session's environment has %s=%s (set by "
                     "whatever started the panel); a game started the same way would use it. ",
                     name, v);
                if (s->note_level < 1)
                    s->note_level = 1;
            }
            if (reg_get_sz(HKEY_CURRENT_USER, G.glide_key, name, v, sizeof v) == ERROR_SUCCESS) {
                scat(s->note, sizeof s->note, "OVERRIDDEN: HKCU\\%s has %s=%s, which Glide reads "
                     "before HKLM. ", G.glide_key, name, v);
                s->note_level = 2;
                add_override(HKEY_CURRENT_USER, G.glide_key, name, v,
                             "per-user Glide key - Glide reads it before HKLM");
            }
        } else if (r->store == CTL_ST_ENV) {
            if (reg_get_sz(HKEY_LOCAL_MACHINE, CTL_KEY_SYSTEM_ENV, name, v, sizeof v) ==
                ERROR_SUCCESS)
                scat(s->note, sizeof s->note, "The system environment also has %s=%s: it applies "
                     "whenever this is Default. ", name, v);
            if ((r->flags & CTL_F_R_GLIDE) &&
                (reg_get_sz(HKEY_LOCAL_MACHINE, G.glide_key, name, v, sizeof v) == ERROR_SUCCESS ||
                 reg_get_sz(HKEY_CURRENT_USER, G.glide_key, name, v, sizeof v) == ERROR_SUCCESS))
                scat(s->note, sizeof s->note, "The Glide key also has %s=%s: Glide-only games use "
                     "it when this is Default (OpenGL games never do). ", name, v);
        }
    }
}

static void rows_load(void)
{
    const ctl_row *r;
    char name[48], v[64];
    LONG e;
    NR = 0;
    NOV = 0;
    for (r = ctl_rows; r->name && NR < MAXROWS; r++) {
        ROWSTATE *s;
        if (!(r->lanes & G.lane))
            continue;
        s = &R[NR++];
        memset(s, 0, sizeof *s);
        s->row = r;
        switch (r->store) {
        case CTL_ST_DISPLAY:
            s->present = G.have_cur;
            _snprintf(s->cur, sizeof s->cur, "%lu", (unsigned long)G.cur.dmDisplayFrequency);
            break;
        case CTL_ST_DIAG: {
            DWORD d = 0;
            e = reg_get_dword(HKEY_LOCAL_MACHINE, CTL_KEY_DIAG, r->name, &d);
            if (e == ERROR_SUCCESS && d) {
                s->present = 1;
                strcpy(s->cur, "1");
            }
            break;
        }
        default:
            if (r->kind == CTL_K_V_GAMMA) {
                double g = v_gamma_get(r->name, r->fdef, &s->present);
                _snprintf(s->cur, sizeof s->cur, "%.2f", g);
                break;
            }
            first_name(r, name, sizeof name);
            if (r->flags & CTL_F_DWORD) {
                DWORD d = 0;
                e = reg_get_dword(store_root(r->store), store_path(r->store), name, &d);
                if (e == ERROR_SUCCESS) {
                    s->present = 1;
                    _snprintf(s->cur, sizeof s->cur, "%lu", (unsigned long)d);
                }
            } else {
                e = reg_get_sz(store_root(r->store), store_path(r->store), name, v, sizeof v);
                if (e == ERROR_SUCCESS) {
                    s->present = 1;
                    scpy(s->cur, v, sizeof s->cur);
                } else if (e == REG_WRONG_TYPE) {
                    scat(s->note, sizeof s->note, "%s holds %s as a number; the stack reads text "
                         "only, so it is ignored - Apply rewrites it. ", store_where(r->store),
                         name);
                    s->note_level = 1;
                }
            }
            if (r->flags & CTL_F_TRIPLE) {          /* R, G, B written as one */
                char g2[64], b2[64];
                const char *p1 = strchr(r->name, '|'), *p2 = p1 ? strchr(p1 + 1, '|') : NULL;
                char gn[48], bn[48];
                if (p1 && p2) {
                    _snprintf(gn, sizeof gn, "%.*s", (int)(p2 - p1 - 1), p1 + 1);
                    gn[sizeof gn - 1] = 0;
                    scpy(bn, p2 + 1, sizeof bn);
                    if (reg_get_sz(HKEY_LOCAL_MACHINE, G.glide_key, gn, g2, sizeof g2) !=
                            ERROR_SUCCESS)
                        g2[0] = 0;
                    if (reg_get_sz(HKEY_LOCAL_MACHINE, G.glide_key, bn, b2, sizeof b2) !=
                            ERROR_SUCCESS)
                        b2[0] = 0;
                    if (!streq(g2, s->present ? s->cur : "") ||
                        !streq(b2, s->present ? s->cur : "")) {
                        scat(s->note, sizeof s->note, "Red, green and blue are set separately "
                             "(R=%s G=%s B=%s); Apply writes one value to all three. ",
                             s->present ? s->cur : "-", g2[0] ? g2 : "-", b2[0] ? b2 : "-");
                        if (s->note_level < 1)
                            s->note_level = 1;
                    }
                }
            }
            break;
        }
        if (!s->present && r->kind == CTL_K_V_INT) {
            /* vintage number: absent is its driver's default, shown as that number */
            s->present = 1;
            _snprintf(s->cur, sizeof s->cur, "%ld", (long)r->fdef);
        }
        if (!s->present && r->vdflt && r->kind == CTL_K_CHOICE) {
            /* vintage: an absent value means its driver's default - shown as that value,
             * and written only if the user picks another */
            s->present = 1;
            scpy(s->cur, r->vdflt, sizeof s->cur);
        }
        s->pend_present = s->present;
        memcpy(s->pend, s->cur, sizeof s->pend);
        if ((r->flags & CTL_F_NEEDS_PLUGIN) && !G.plugin_present) {
            s->disabled = 1;
            scat(s->note, sizeof s->note, "No splash plugin (3dfxspl3.dll) is installed, so "
                 "there is no splash to skip. ");
        }
        if (r->store == CTL_ST_DIAG && G.lane == CTL_LANE_VCR && r->id != CTL_ID_AA) {
            /* a driver older than the switch never reads it: say so (a driver with no
             * 2D report at all is said once, under the three rows) */
            if (G.have_2d && r->id != CTL_ID_2D_TEXT && G.size_2d < (int)sizeof(vcr_2d_stats))
                scat(s->note, sizeof s->note, "The installed display driver predates this switch "
                     "(its 2D report has no pattern/line counters) - it would be ignored. ");
        }
        row_scan_elsewhere(s);
    }
    /* settings the table does not offer that silently change the AA mode */
    if (G.lane == CTL_LANE_VCR) {
        static const char *const hidden[] = { "FX_GLIDE_AA_SAMPLE", "FX_GLIDE_NUM_CHIPS" };
        unsigned k;
        for (k = 0; k < 2; k++) {
            if (reg_get_sz(HKEY_CURRENT_USER, CTL_KEY_USER_ENV, hidden[k], v, sizeof v) ==
                ERROR_SUCCESS)
                add_override(HKEY_CURRENT_USER, CTL_KEY_USER_ENV, hidden[k], v,
                             "overrides the SLI/AA mode (user environment)");
            if (reg_get_sz(HKEY_LOCAL_MACHINE, CTL_KEY_SYSTEM_ENV, hidden[k], v, sizeof v) ==
                ERROR_SUCCESS)
                add_override(HKEY_LOCAL_MACHINE, CTL_KEY_SYSTEM_ENV, hidden[k], v,
                             "overrides the SLI/AA mode (system environment)");
            if (reg_get_sz(HKEY_CURRENT_USER, G.glide_key, hidden[k], v, sizeof v) ==
                ERROR_SUCCESS)
                add_override(HKEY_CURRENT_USER, G.glide_key, hidden[k], v,
                             "overrides the SLI/AA mode (per-user Glide key)");
            if (reg_get_sz(HKEY_LOCAL_MACHINE, G.glide_key, hidden[k], v, sizeof v) ==
                ERROR_SUCCESS)
                add_override(HKEY_LOCAL_MACHINE, G.glide_key, hidden[k], v,
                             "overrides the SLI/AA mode (Glide key)");
        }
    }
    {
        ROWSTATE *aa = rs_by_id(CTL_ID_AA);
        g_allow_exp = (aa && aa->present && ctl_aa_is_aa(atol(aa->cur))) ||
                      (G.sliaa_present && G.sliaa);
    }
    g_aa_confirmed = 0;
    g_rm_overrides = 0;
}

static int row_changed(const ROWSTATE *s)
{
    if (s->pend_present != s->present)
        return 1;
    return s->present && strcmp(s->pend, s->cur) != 0;
}

static int any_changed(void)
{
    int i;
    for (i = 0; i < NR; i++)
        if (row_changed(&R[i]))
            return 1;
    return g_rm_overrides && NOV > 0;
}

static void set_pend(ROWSTATE *s, const char *v)
{
    s->pend_present = v != NULL;
    scpy(s->pend, v ? v : "", sizeof s->pend);
}

/* ---- UI ------------------------------------------------------------------------------ */
static HINSTANCE g_hi;
static HWND g_main, g_tab, g_status, g_header, g_tip;
static HWND g_page[CTL_NTABS];
static int g_tabmap[CTL_NTABS];             /* tab index -> CTL_TAB_* */
static int g_ntabs;
static HFONT g_font, g_bold, g_big, g_small;
static HICON g_icon, g_icon_small;
static int g_busy;                          /* a display change is in flight */
static int g_close_after;
static char g_summary[16384];
static RECT g_pagerc;
static HWND g_aa_live, g_2d_state;

static void ui_set_status(const char *fmt, ...)
{
    char b[400];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    b[sizeof b - 1] = 0;
    if (g_status)
        SetWindowTextA(g_status, b);
}

static void ui_update_buttons(void)
{
    int ch = any_changed();
    EnableWindow(GetDlgItem(g_main, IDC_APPLY), ch && !g_busy);
    EnableWindow(GetDlgItem(g_main, IDC_OK), !g_busy);
    EnableWindow(GetDlgItem(g_main, IDC_CANCEL), !g_busy);
    EnableWindow(GetDlgItem(g_main, IDC_DEFAULTS), !g_busy && G.lane == CTL_LANE_VCR);
    if (!g_busy)
        ui_set_status(ch ? "Changes pending - Apply writes them."
                         : "No pending changes.");
}

/* an in-memory dialog template: no controls, "MS Shell Dlg 2" 8 pt (Tahoma on XP) */
static LPCDLGTEMPLATEA dlg_template(WORD *buf, size_t words, DWORD style, DWORD exstyle,
                                    short cx, short cy, const char *title)
{
    WORD *p = buf;
    DLGTEMPLATE *t = (DLGTEMPLATE *)p;
    memset(buf, 0, words * sizeof(WORD));
    t->style = style | DS_SETFONT;
    t->dwExtendedStyle = exstyle;
    t->cdit = 0;
    t->cx = cx;
    t->cy = cy;
    p = (WORD *)(t + 1);
    *p++ = 0;                                           /* no menu */
    *p++ = 0;                                           /* the dialog class */
    p += MultiByteToWideChar(CP_ACP, 0, title, -1, (LPWSTR)p, 64);
    *p++ = 8;
    MultiByteToWideChar(CP_ACP, 0, "MS Shell Dlg 2", -1, (LPWSTR)p, 32);
    return (LPCDLGTEMPLATEA)t;
}

static HWND mk(HWND parent, const char *cls, const char *text, DWORD style, int x, int y, int w,
               int h, int id, HFONT f)
{
    HWND c = CreateWindowExA(0, cls, text, WS_CHILD | WS_VISIBLE | style, x, y, w, h, parent,
                             (HMENU)(INT_PTR)id, g_hi, NULL);
    SendMessageA(c, WM_SETFONT, (WPARAM)(f ? f : g_font), 0);
    return c;
}

/* coloured statics: remembered with their colour */
typedef struct { HWND h; int color; } COLSTATIC;
static COLSTATIC g_cs[256];
static int g_ncs;
enum { COL_NONE, COL_RED, COL_ORANGE, COL_GREEN, COL_GRAY, COL_BLUE };

static HWND mk_col(HWND parent, const char *text, int x, int y, int w, int h, int color, HFONT f)
{
    HWND c = mk(parent, "STATIC", text, SS_LEFT | SS_NOPREFIX, x, y, w, h, 0, f);
    if (g_ncs < (int)(sizeof g_cs / sizeof g_cs[0])) {
        g_cs[g_ncs].h = c;
        g_cs[g_ncs].color = color;
        g_ncs++;
    }
    return c;
}

static void set_col(HWND h, int color)
{
    int i;
    for (i = 0; i < g_ncs; i++)
        if (g_cs[i].h == h)
            g_cs[i].color = color;
}

static COLORREF col_ref(int c)
{
    switch (c) {
    case COL_RED:    return c_red();
    case COL_ORANGE: return c_orange();
    case COL_GREEN:  return c_green();
    case COL_GRAY:   return c_gray();
    case COL_BLUE:   return c_blue();
    }
    return GetSysColor(COLOR_BTNTEXT);
}

static void tip_add(HWND owner, HWND ctl, const char *text)
{
    TOOLINFOA ti;
    if (!g_tip || !ctl || !text)
        return;
    memset(&ti, 0, sizeof ti);
    ti.cbSize = TTTOOLINFOA_V2_SIZE;
    ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
    ti.hwnd = owner;
    ti.uId = (UINT_PTR)ctl;
    ti.lpszText = (LPSTR)text;
    SendMessageA(g_tip, TTM_ADDTOOLA, 0, (LPARAM)&ti);
}

/* text rows needed for a string at width w (a rough measure: Tahoma 8 ~ 5.6 px/char) */
static int text_lines(const char *s, int w)
{
    int per = w / 6, n = 1, col = 0;
    if (per < 10)
        per = 10;
    for (; *s; s++) {
        if (*s == '\n' || ++col >= per) {
            n++;
            col = 0;
        }
    }
    return n;
}

static const char *when_text(int when)
{
    switch (when) {
    case CTL_WHEN_LAUNCH:   return "next game launch";
    case CTL_WHEN_EXPLORER: return "next game launch *";
    case CTL_WHEN_NOW:      return "applied now";
    case CTL_WHEN_REBOOT:   return "reboot / next game";
    }
    return "";
}

static void section(HWND page, const char *title, int *y, int w)
{
    mk(page, "STATIC", title, SS_LEFT | SS_NOPREFIX, 12, *y, w - 24, 16, 0, g_bold);
    mk(page, "STATIC", "", SS_ETCHEDHORZ, 12, *y + 17, w - 24, 2, 0, NULL);
    *y += 24;
}

/* ---- one row's control -------------------------------------------------------------- */
static void float_label(ROWSTATE *s, int pos)
{
    char b[64];
    const ctl_row *r = s->row;
    float v = pos / 100.0f;
    _snprintf(b, sizeof b, "%.2f%s", v, (v - r->fdef < 0.005f && v - r->fdef > -0.005f)
              ? "  (default)" : "");
    b[sizeof b - 1] = 0;
    if (s->hval)
        SetWindowTextA(s->hval, b);
}

static int combo_add(ROWSTATE *s, const char *label, const char *value)
{
    int i;
    if (s->nitems >= MAXITEMS)
        return -1;
    i = (int)SendMessageA(s->hctl, CB_ADDSTRING, 0, (LPARAM)label);
    SendMessageA(s->hctl, CB_SETITEMDATA, i, s->nitems);
    s->iabsent[s->nitems] = value == NULL;
    scpy(s->ival[s->nitems], value ? value : "", sizeof s->ival[0]);
    return s->nitems++;
}

static void combo_select_pending(ROWSTATE *s)
{
    int n = (int)SendMessageA(s->hctl, CB_GETCOUNT, 0, 0), i, sel = -1;
    for (i = 0; i < n; i++) {
        int k = (int)SendMessageA(s->hctl, CB_GETITEMDATA, i, 0);
        if (k < 0 || k >= s->nitems)
            continue;
        if (s->pend_present ? (!s->iabsent[k] && strcmp(s->ival[k], s->pend) == 0)
                            : s->iabsent[k]) {
            sel = i;
            break;
        }
    }
    SendMessageA(s->hctl, CB_SETCURSEL, sel, 0);
}

static void aa_fill_combo(ROWSTATE *s)
{
    long cfgs[12];
    int n, i, cur_listed = 0;
    ctl_aa_info a;
    char lab[200];
    long curv = s->pend_present ? atol(s->pend) : -1;
    SendMessageA(s->hctl, CB_RESETCONTENT, 0, 0);
    s->nitems = 0;
    if (!s->present || !s->pend_present)
        combo_add(s, G.nchips >= 2 ? "Driver default - Glide's own (all chips in SLI)"
                                   : "Driver default - Glide's own (single chip)", NULL);
    n = ctl_aa_choices(G.nchips, g_allow_exp, cfgs, 12);
    for (i = 0; i < n; i++) {
        char v[8];
        ctl_aa_describe(cfgs[i], G.nchips, &a);
        _snprintf(lab, sizeof lab, "%s  [value %ld]", a.label, cfgs[i]);
        lab[sizeof lab - 1] = 0;
        _snprintf(v, sizeof v, "%ld", cfgs[i]);
        combo_add(s, lab, v);
        if (s->pend_present && cfgs[i] == curv)
            cur_listed = 1;
    }
    /* the value in force but not offered: show it, as itself */
    if (s->pend_present && !cur_listed) {
        /* SLI written as the other SLI value (2 on a 4-chip board, 5 on a 2-chip one) */
        long sli = ctl_aa_sli_value(G.nchips);
        if (G.nchips >= 2 && (curv == 2 || curv == 5) && curv != sli) {
            ctl_aa_describe(curv, G.nchips, &a);
            _snprintf(lab, sizeof lab, "%s  [value %ld, as set now]", a.label, curv);
        } else {
            ctl_aa_describe(curv, G.nchips, &a);
            _snprintf(lab, sizeof lab, "Current: value %s - %s (not offered here)", s->pend,
                      a.label);
        }
        lab[sizeof lab - 1] = 0;
        combo_add(s, lab, s->pend);
    }
    combo_select_pending(s);
}

static void build_row(HWND page, ROWSTATE *s, int idx, int *y, int w)
{
    const ctl_row *r = s->row;
    int id = IDC_ROW_BASE + idx * 4;
    int cx = 212, cw = w - cx - 118, lh = 22;
    HWND lab = NULL;
    s->hctl = s->hval = s->hwhen = NULL;
    if (r->kind == CTL_K_CHECK) {
        s->hctl = mk(page, "BUTTON", r->label, BS_AUTOCHECKBOX | WS_TABSTOP, 14, *y + 2,
                     cx + cw - 14, 18, id, NULL);
        SendMessageA(s->hctl, BM_SETCHECK,
                     s->pend_present && streq(s->pend, r->choices[1].value) ? BST_CHECKED
                                                                            : BST_UNCHECKED, 0);
    } else {
        lab = mk(page, "STATIC", r->label, SS_LEFT | SS_NOPREFIX, 14, *y + 4, cx - 18, 16, 0, NULL);
        switch (r->kind) {
        case CTL_K_FLOAT:
        case CTL_K_V_GAMMA: {
            int pos;
            float v = r->fdef;
            if (s->pend_present)
                ctl_parse_float(s->pend, &v);
            pos = (int)(v * 100.0f + 0.5f);
            s->hctl = mk(page, TRACKBAR_CLASSA, "", TBS_HORZ | TBS_NOTICKS | WS_TABSTOP, cx - 4,
                         *y, cw - 96, 24, id, NULL);
            SendMessageA(s->hctl, TBM_SETRANGE, FALSE,
                         MAKELPARAM((int)(r->fmin * 100 + 0.5f), (int)(r->fmax * 100 + 0.5f)));
            SendMessageA(s->hctl, TBM_SETPAGESIZE, 0, 10);
            SendMessageA(s->hctl, TBM_SETPOS, TRUE, pos);
            s->hval = mk(page, "STATIC", "", SS_LEFT | SS_NOPREFIX, cx + cw - 96, *y + 4, 96, 16,
                         id + 1, NULL);
            float_label(s, pos);
            lh = 26;
            break;
        }
        case CTL_K_V_INT:
            s->hctl = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", s->pend_present ? s->pend : "0",
                                      WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, cx,
                                      *y, 90, 21, page, (HMENU)(INT_PTR)id, g_hi, NULL);
            SendMessageA(s->hctl, WM_SETFONT, (WPARAM)g_font, 0);
            break;
        default: {
            const ctl_choice *c;
            int i;
            s->hctl = mk(page, "COMBOBOX", "", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, cx, *y,
                         cw, 300, id, NULL);
            /* the open list may be wider than the box: every label readable in full */
            SendMessageA(s->hctl, CB_SETDROPPEDWIDTH, 470, 0);
            s->nitems = 0;
            if (r->kind == CTL_K_AA) {
                aa_fill_combo(s);
            } else if (r->kind == CTL_K_GLIDE_REFRESH) {
                ctl_rate_span sp[32];
                int n = ctl_glide_rates(G.modes, G.nmodes, sp, 32), found = 0;
                char lab2[160], v[16];
                combo_add(s, "Auto - OpenGL the highest listed, Glide the game's", NULL);
                if (mon_unfiltered())
                    n = 0;      /* the driver's list is NOT filtered by the monitor: no fixed rate */
                for (i = 0; i < n; i++) {
                    _snprintf(lab2, sizeof lab2, "%u Hz  (listed from %ux%u to %ux%u)", sp[i].hz,
                              sp[i].minw, sp[i].minh, sp[i].maxw, sp[i].maxh);
                    lab2[sizeof lab2 - 1] = 0;
                    _snprintf(v, sizeof v, "%u", sp[i].hz);
                    combo_add(s, lab2, v);
                    if (s->pend_present && strcmp(s->pend, v) == 0)
                        found = 1;
                }
                if (s->pend_present && !found) {
                    _snprintf(lab2, sizeof lab2, "Current: %s Hz - NOT a rate the driver lists "
                              "for this monitor", s->pend);
                    lab2[sizeof lab2 - 1] = 0;
                    combo_add(s, lab2, s->pend);
                }
            } else if (r->kind == CTL_K_DESK_REFRESH) {
                unsigned hz[64];
                int n = 0, found = 0;
                char lab2[80], v[16];
                if (G.have_cur && !mon_unfiltered())
                    n = ctl_desk_rates(G.modes, G.nmodes, G.cur.dmPelsWidth, G.cur.dmPelsHeight,
                                       G.cur.dmBitsPerPel, hz, 64);
                for (i = 0; i < n; i++) {
                    _snprintf(v, sizeof v, "%u", hz[i]);
                    _snprintf(lab2, sizeof lab2, "%u Hz%s", hz[i],
                              streq(v, s->cur) ? "  (now)" : "");
                    lab2[sizeof lab2 - 1] = 0;
                    combo_add(s, lab2, v);
                    if (streq(v, s->pend))
                        found = 1;
                }
                if (!found && s->pend_present) {
                    _snprintf(lab2, sizeof lab2, "%s Hz (now - not in the driver's list)",
                              s->pend);
                    lab2[sizeof lab2 - 1] = 0;
                    combo_add(s, lab2, s->pend);
                }
            } else {
                int matched = 0;
                for (c = r->choices; c && c->label; c++) {
                    combo_add(s, c->label, c->value);
                    if (s->pend_present ? (c->value && streq(c->value, s->pend)) : !c->value)
                        matched = 1;
                }
                if (s->pend_present && !matched) {
                    char lab2[120];
                    _snprintf(lab2, sizeof lab2, "Current: \"%s\" (set outside this panel)",
                              s->pend);
                    lab2[sizeof lab2 - 1] = 0;
                    combo_add(s, lab2, s->pend);
                }
            }
            combo_select_pending(s);
            break;
        }
        }
    }
    if (s->disabled && s->hctl)
        EnableWindow(s->hctl, FALSE);
    s->hwhen = mk_col(page, when_text(r->when), w - 112, *y + 4, 100, 16,
                      r->when == CTL_WHEN_NOW ? COL_BLUE : COL_GRAY, g_small);
    tip_add(page, s->hctl, r->help);
    if (lab)
        tip_add(page, lab, r->help);
    tip_add(page, s->hwhen,
            r->when == CTL_WHEN_EXPLORER
                ? "* Stored in your user environment: games started from the desktop or the "
                  "Start menu after Apply see it. A program started by something that was "
                  "already running (the retro agent, a command prompt opened earlier) keeps "
                  "that program's old environment."
            : r->when == CTL_WHEN_LAUNCH
                ? "Read when a Glide or OpenGL program starts or opens the board: the next game "
                  "you start uses it. No reboot."
            : r->when == CTL_WHEN_NOW
                ? "The panel applies it when you press Apply (one paced display re-set) and "
                  "checks that the driver took it."
                : "The vintage driver reads it at boot, at a mode set or when a game starts.");
    *y += lh + 4;
    if (s->note[0]) {
        int lines = text_lines(s->note, w - 40);
        mk_col(page, s->note, 30, *y - 2, w - 44, lines * 13 + 2,
               s->note_level >= 2 ? COL_RED : s->note_level == 1 ? COL_ORANGE : COL_GRAY,
               g_small);
        *y += lines * 13 + 4;
    }
    *y += 2;
}

/* ---- the pages ------------------------------------------------------------------------ */
static INT_PTR CALLBACK PageProc(HWND, UINT, WPARAM, LPARAM);

static void list_aa_modes(HWND lv)
{
    LVCOLUMNA col;
    LVITEMA it;
    long cfg;
    int row = 0;
    ctl_aa_info a;
    memset(&col, 0, sizeof col);
    col.mask = LVCF_TEXT | LVCF_WIDTH;
    col.pszText = "Value";
    col.cx = 44;
    SendMessageA(lv, LVM_INSERTCOLUMNA, 0, (LPARAM)&col);
    col.pszText = "Mode";
    col.cx = 190;
    SendMessageA(lv, LVM_INSERTCOLUMNA, 1, (LPARAM)&col);
    col.pszText = "On this board";
    col.cx = 560;
    SendMessageA(lv, LVM_INSERTCOLUMNA, 2, (LPARAM)&col);
    for (cfg = 0; cfg <= 8; cfg++) {
        char v[8];
        ctl_aa_describe(cfg, G.nchips, &a);
        _snprintf(v, sizeof v, "%ld", cfg);
        memset(&it, 0, sizeof it);
        it.mask = LVIF_TEXT | LVIF_PARAM;
        it.iItem = row;
        it.pszText = v;
        it.lParam = a.status;
        SendMessageA(lv, LVM_INSERTITEMA, 0, (LPARAM)&it);
        it.mask = LVIF_TEXT;
        it.iSubItem = 1;
        it.pszText = (LPSTR)a.label;
        SendMessageA(lv, LVM_SETITEMTEXTA, row, (LPARAM)&it);
        it.iSubItem = 2;
        it.pszText = (LPSTR)(a.status == CTL_AA_VALIDATED ? a.evidence : a.evidence);
        SendMessageA(lv, LVM_SETITEMTEXTA, row, (LPARAM)&it);
        row++;
    }
}

static void aa_live_text(char *b, size_t n)
{
    b[0] = 0;
    if (!G.have_info) {
        scpy(b, "Live state: the driver does not answer (not our kernel driver).", n);
        return;
    }
    scat(b, n, "Right now: ");
    if (G.info.sli_chips)
        scat(b, n, "%u chips in SLI/AA", G.info.sli_chips);
    else
        scat(b, n, "no SLI session (the desktop runs on one chip)");
    if (G.have_dd && G.dd[1])
        scat(b, n, " - a Glide program (pid %lu) holds the display", G.dd[1]);
    scat(b, n, ".   Kernel AA switch (Diag\\SliAA): %s",
         G.sliaa_present && G.sliaa ? "ARMED - AA requests are programmed"
                                    : "off - AA requests are refused (safe)");
}

static void twod_state_text(char *b, size_t n)
{
    b[0] = 0;
    if (!G.have_2d) {
        scpy(b, "This display driver does not report its 2D state (it is older than the "
             "engine-text switch): the switches can be stored, but the panel cannot confirm "
             "they took effect.", n);
        return;
    }
    scat(b, n, "The driver reports, for this display mode: 2D engine %s; text %s",
         (G.st2d.flags & VCR_2DS_F_ENGINE) ? "on" : "OFF (software drawing)",
         (G.st2d.flags & VCR_2DS_F_TEXT) ? "ON" : "off");
    if (G.size_2d >= (int)sizeof(vcr_2d_stats))
        scat(b, n, ", patterns %s, lines %s", (G.st2d.flags & VCR_2DS_F_PAT) ? "ON" : "off",
             (G.st2d.flags & VCR_2DS_F_LINE) ? "ON" : "off");
    scat(b, n, ".");
}

static int page_build(int tab, HWND page, int w, int h)
{
    int y = 10, i;
    char b[1200];
    switch (tab) {
    case CTL_TAB_OVERVIEW: {
        section(page, "Driver stack", &y, w);
        if (G.lane == CTL_LANE_VCR) {
            _snprintf(b, sizeof b, "Kernel driver:  vcr-kmd - vcrmp.sys + vcrdd.dll (our own), "
                      "version %u.%u build %u, %s", G.info.version >> 16,
                      G.info.version & 0xffff, G.info.build,
                      G.info.boot_good ? "boot confirmed stable" : "boot not yet confirmed");
        } else if (G.lane == CTL_LANE_VINTAGE) {
            _snprintf(b, sizeof b, "Display driver:  the vintage 3dfxvs driver (%s)%s",
                      G.adapter[0] ? G.adapter : "3dfx", G.forced ? " - lane forced" : "");
        } else {
            _snprintf(b, sizeof b, "No 3dfx display driver is active on this PC (display: %s). "
                      "Nothing here can change it.", G.adapter[0] ? G.adapter : "unknown");
        }
        b[sizeof b - 1] = 0;
        mk(page, "STATIC", b, SS_LEFT | SS_NOPREFIX, 20, y, w - 40, 16, 0, NULL);
        y += 20;
        if (!G.glide_present)
            _snprintf(b, sizeof b, "Glide:  no glide3x.dll in the system folder - each game "
                      "brings its own (the Anti-aliasing tab checks the game folders)");
        else
            _snprintf(b, sizeof b, "Glide:  %s - %s", G.glide_dll,
                      G.glide_guard ? "our h5 fork, with the SLI/AA guard"
                                    : "WITHOUT the SLI/AA guard (not our fork, or an old build)");
        b[sizeof b - 1] = 0;
        mk_col(page, b, 20, y, w - 40, 16,
               G.glide_present && !G.glide_guard && G.lane == CTL_LANE_VCR ? COL_ORANGE : COL_NONE,
               NULL);
        y += 20;
        _snprintf(b, sizeof b, "OpenGL:  %s%s%s", G.icd_present ? G.icd_dll : "no system ICD "
                  "registered for this driver", G.icd_ver[0] ? " - " : "",
                  G.icd_ver[0] ? G.icd_ver : "");
        b[sizeof b - 1] = 0;
        mk(page, "STATIC", b, SS_LEFT | SS_NOPREFIX, 20, y, w - 40, 16, 0, NULL);
        y += 20;
        if (G.lane == CTL_LANE_VCR) {
            G.info.mon_name[15] = 0;
            G.info.mon_pnp[3] = 0;
            _snprintf(b, sizeof b, "Monitor:  %s  (%s%04x) - H %u-%u kHz, V %u-%u Hz, %u MHz - %s",
                      G.info.mon_name[0] ? G.info.mon_name : "unknown", G.info.mon_pnp,
                      G.info.mon_product, G.info.mon_hmin_khz, G.info.mon_hmax_khz,
                      G.info.mon_vmin_hz, G.info.mon_vmax_hz, G.info.mon_max_pixclk_khz / 1000,
                      G.info.mon_src == 1 ? "limits read from the monitor (EDID)"
                      : G.info.mon_src == 2 ? "this monitor's saved limits"
                      : G.info.mon_src == 3 ? "no EDID: the safe envelope"
                      : G.info.mon_src == 4 ? "no EDID: the conservative default"
                                            : "the mode list is NOT filtered by the monitor");
            b[sizeof b - 1] = 0;
            mk_col(page, b, 20, y, w - 40, 16, G.info.mon_src == 0 ? COL_RED : COL_NONE, NULL);
            y += 20;
            _snprintf(b, sizeof b, "Glide settings key:  HKLM\\%s%s", G.glide_key,
                      G.has_3dfxvs_dev0 ? "   (3dfxvs\\Device0 exists, so Glide reads here)" : "");
            b[sizeof b - 1] = 0;
            mk_col(page, b, 20, y, w - 40, 16, COL_GRAY, g_small);
            y += 24;
        }
        section(page, "Presets", &y, w);
        mk(page, "BUTTON", "Maximum quality\nvsync, smoother dithering", BS_PUSHBUTTON |
           BS_MULTILINE | WS_TABSTOP, 20, y, 190, 42, IDC_P_QUALITY, NULL);
        mk(page, "BUTTON", "Maximum speed\nno vsync, deeper frame queue", BS_PUSHBUTTON |
           BS_MULTILINE | WS_TABSTOP, 222, y, 190, 42, IDC_P_SPEED, NULL);
        mk(page, "BUTTON", "Driver defaults\nremove every override", BS_PUSHBUTTON | BS_MULTILINE |
           WS_TABSTOP, 424, y, 190, 42, IDC_P_DEFAULTS, NULL);
        if (G.lane != CTL_LANE_VCR) {
            EnableWindow(GetDlgItem(page, IDC_P_QUALITY), FALSE);
            EnableWindow(GetDlgItem(page, IDC_P_SPEED), FALSE);
            EnableWindow(GetDlgItem(page, IDC_P_DEFAULTS), FALSE);
        }
        y += 48;
        mk_col(page, "A preset only fills in the tabs - nothing changes until you press Apply or "
               "OK. No preset turns anti-aliasing on: it is experimental on this board.", 20, y,
               w - 40, 28, COL_GRAY, g_small);
        y += 34;
        section(page, "Attention", &y, w);
        b[0] = 0;
        if (G.lane == CTL_LANE_VCR && G.sliaa_present && G.sliaa)
            scat(b, sizeof b, "- EXPERIMENTAL ANTI-ALIASING IS ARMED (Diag\\SliAA = 1): the next "
                 "game that asks for AA gets it. Disarm it on the Anti-aliasing tab when you are "
                 "done.\r\n");
        if (G.lane == CTL_LANE_VCR && G.have_dd && G.dd[1])
            scat(b, sizeof b, "- A Glide program (pid %lu) holds the display: display changes wait "
                 "until it exits.\r\n", G.dd[1]);
        {
            ROWSTATE *aa = rs_by_id(CTL_ID_AA);
            if (G.lane == CTL_LANE_VCR && aa && aa->present && ctl_aa_is_aa(atol(aa->cur)) &&
                !(G.sliaa_present && G.sliaa))
                scat(b, sizeof b, "- The chip mode is an anti-aliasing mode (value %s) but the "
                     "kernel's AA switch is off: Glide and OpenGL games will fail to open until "
                     "you choose SLI or single chip on the Anti-aliasing tab.\r\n", aa->cur);
        }
        if (NOV)
            scat(b, sizeof b, "- %d setting(s) are overridden from somewhere else - see "
                 "Advanced.\r\n", NOV);
        for (i = 0; i < NR; i++)
            if (R[i].note_level >= 2) {
                scat(b, sizeof b, "- %s: overridden (see its tab).\r\n", R[i].row->label);
            }
        if (G.lane == CTL_LANE_VCR && G.glide_present && !G.glide_guard)
            scat(b, sizeof b, "- The system Glide lacks the SLI/AA guard: keep anti-aliasing "
                 "off.\r\n");
        if (!b[0])
            scpy(b, "Nothing needs attention.", sizeof b);
        {
            int lines = text_lines(b, w - 40) + 1;
            mk_col(page, b, 20, y, w - 40, lines * 14, b[0] == 'N' ? COL_GREEN : COL_ORANGE, NULL);
            y += lines * 14 + 4;
        }
        break;
    }
    case CTL_TAB_AA: {
        ROWSTATE *aa = NULL;
        if (G.lane == CTL_LANE_VCR) {
            aa_live_text(b, sizeof b);
            g_aa_live = mk_col(page, b, 14, y, w - 28, 30,
                               G.sliaa_present && G.sliaa ? COL_RED : COL_GRAY, g_small);
            y += 34;
        }
        {
            const char *grp = NULL;
            for (i = 0; i < NR; i++) {
                if (R[i].row->tab != tab)
                    continue;
                if (!grp || strcmp(grp, R[i].row->group)) {
                    section(page, R[i].row->group, &y, w);
                    grp = R[i].row->group;
                }
                if (R[i].row->id == CTL_ID_AA)
                    aa = &R[i];
                build_row(page, &R[i], i, &y, w);
            }
        }
        if (G.lane != CTL_LANE_VCR)
            break;
        mk(page, "BUTTON", "Allow EXPERIMENTAL anti-aliasing modes (can freeze this PC)",
           BS_AUTOCHECKBOX | WS_TABSTOP, 14, y, w - 28, 18, IDC_AA_ALLOW, NULL);
        CheckDlgButton(page, IDC_AA_ALLOW, g_allow_exp ? BST_CHECKED : BST_UNCHECKED);
        tip_add(page, GetDlgItem(page, IDC_AA_ALLOW),
                "Lists the anti-aliasing modes. Choosing one needs your confirmation and arms "
                "the kernel's AA switch (Diag\\SliAA); choosing a non-AA mode disarms it.");
        if (!aa || G.nchips == 0)
            EnableWindow(GetDlgItem(page, IDC_AA_ALLOW), FALSE);
        y += 24;
        {
            HWND lv = CreateWindowExA(WS_EX_CLIENTEDGE, WC_LISTVIEWA, "", WS_CHILD | WS_VISIBLE |
                                      LVS_REPORT | LVS_NOSORTHEADER | LVS_SINGLESEL | WS_TABSTOP,
                                      14, y, w - 28, 176, page, (HMENU)IDC_AA_LIST, g_hi, NULL);
            SendMessageA(lv, WM_SETFONT, (WPARAM)g_font, 0);
            SendMessageA(lv, LVM_SETEXTENDEDLISTVIEWSTYLE, 0, LVS_EX_FULLROWSELECT |
                         LVS_EX_LABELTIP);
            list_aa_modes(lv);
            y += 182;
        }
        mk(page, "BUTTON", "Disarm AA now", BS_PUSHBUTTON | WS_TABSTOP, 14, y, 120, 24,
           IDC_AA_DISARM, NULL);
        EnableWindow(GetDlgItem(page, IDC_AA_DISARM), G.sliaa_present);
        tip_add(page, GetDlgItem(page, IDC_AA_DISARM),
                "Deletes Diag\\SliAA at once (no Apply needed): the kernel then refuses every "
                "AA request before it writes anything. A game set to AA fails to open instead.");
        mk(page, "BUTTON", "Check game folders for unguarded Glide DLLs", BS_PUSHBUTTON |
           WS_TABSTOP, 142, y, 280, 24, IDC_AA_SCAN, NULL);
        y += 30;
        {
            int k, n = 0;
            b[0] = 0;
            for (k = 0; k < NOV; k++)
                if (strstr(OV[k].name, "AA_SAMPLE") || strstr(OV[k].name, "NUM_CHIPS")) {
                    scat(b, sizeof b, "%s=%s (%s). ", OV[k].name, OV[k].value, OV[k].what);
                    n++;
                }
            if (n) {
                char m[1400];
                _snprintf(m, sizeof m, "Hidden overrides of the chip mode: %sRemove them on the "
                          "Advanced tab.", b);
                m[sizeof m - 1] = 0;
                mk_col(page, m, 14, y, w - 28, 30, COL_RED, g_small);
                y += 34;
            }
        }
        break;
    }
    case CTL_TAB_DISPLAY: {
        const char *grp = NULL;
        if (G.lane == CTL_LANE_VCR && G.have_cur) {
            _snprintf(b, sizeof b, "Desktop: %lu x %lu, %lu-bit, %lu Hz", G.cur.dmPelsWidth,
                      G.cur.dmPelsHeight, G.cur.dmBitsPerPel, G.cur.dmDisplayFrequency);
            if (G.have_reg && (G.reg.dmPelsWidth != G.cur.dmPelsWidth ||
                               G.reg.dmPelsHeight != G.cur.dmPelsHeight ||
                               G.reg.dmBitsPerPel != G.cur.dmBitsPerPel ||
                               G.reg.dmDisplayFrequency != G.cur.dmDisplayFrequency))
                scat(b, sizeof b, "   (the saved mode is %lu x %lu, %lu-bit, %lu Hz)",
                     G.reg.dmPelsWidth, G.reg.dmPelsHeight, G.reg.dmBitsPerPel,
                     G.reg.dmDisplayFrequency);
            mk(page, "STATIC", b, SS_LEFT | SS_NOPREFIX, 14, y, w - 200, 16, 0, g_bold);
            mk(page, "BUTTON", "Windows display settings...", BS_PUSHBUTTON | WS_TABSTOP, w - 186,
               y - 4, 172, 24, IDC_DISP_PROPS, NULL);
            y += 26;
        }
        for (i = 0; i < NR; i++) {
            if (R[i].row->tab != tab)
                continue;
            if (!grp || strcmp(grp, R[i].row->group)) {
                section(page, R[i].row->group, &y, w);
                grp = R[i].row->group;
            }
            build_row(page, &R[i], i, &y, w);
        }
        if (G.lane == CTL_LANE_VCR) {
            twod_state_text(b, sizeof b);
            g_2d_state = mk_col(page, b, 14, y, w - 28, 30, COL_GRAY, g_small);
            y += 32;
            mk_col(page, "The driver reads the 2D switches when it builds a new display surface, "
                   "so on Apply the panel passes through another mode the driver lists (same "
                   "resolution) and back - two paced switches. The refresh rate is put back "
                   "after 15 seconds unless you keep it.", 14, y, w - 28, 30, COL_GRAY, g_small);
            y += 34;
        }
        break;
    }
    case CTL_TAB_ADV: {
        const char *grp = NULL;
        int k;
        for (i = 0; i < NR; i++) {
            if (R[i].row->tab != tab)
                continue;
            if (!grp || strcmp(grp, R[i].row->group)) {
                section(page, R[i].row->group, &y, w);
                grp = R[i].row->group;
            }
            build_row(page, &R[i], i, &y, w);
        }
        section(page, "Where the settings are stored", &y, w);
        b[0] = 0;
        if (G.lane == CTL_LANE_VCR) {
            scat(b, sizeof b, "Glide and SLI/AA:  HKLM\\%s  (text values)\r\n", G.glide_key);
            scat(b, sizeof b, "OpenGL, vsync, frame queue:  HKCU\\Environment  (your user "
                 "environment; Explorer is told on Apply)\r\n");
            scat(b, sizeof b, "Kernel switches:  HKLM\\%s  (numbers; only SliAA and the three "
                 "Accel2D switches are ever written)\r\n", CTL_KEY_DIAG);
            scat(b, sizeof b, "Log of every change:  %s", g_logpath);
        } else {
            scat(b, sizeof b, "HKLM\\%s  (and its D3D and glide subkeys)\r\nLog:  %s",
                 CTL_KEY_V_DEV0, g_logpath);
        }
        mk_col(page, b, 14, y, w - 28, 70, COL_NONE, g_small);
        y += 74;
        section(page, "Set somewhere else (these win over the panel)", &y, w);
        b[0] = 0;
        for (k = 0; k < NOV; k++)
            scat(b, sizeof b, "%s\\%s  %s = %s   - %s\r\n",
                 OV[k].root == HKEY_CURRENT_USER ? "HKCU" : "HKLM", OV[k].path, OV[k].name,
                 OV[k].value, OV[k].what);
        if (!NOV)
            scpy(b, "None found.", sizeof b);
        mk_col(page, b, 14, y, w - 28, 42, NOV ? COL_RED : COL_GREEN, g_small);
        y += 44;
        mk(page, "BUTTON", "Remove these when I press Apply", BS_AUTOCHECKBOX | WS_TABSTOP, 14, y,
           300, 18, IDC_RM_OVERRIDE, NULL);
        CheckDlgButton(page, IDC_RM_OVERRIDE, g_rm_overrides ? BST_CHECKED : BST_UNCHECKED);
        EnableWindow(GetDlgItem(page, IDC_RM_OVERRIDE), NOV > 0);
        y += 24;
        section(page, "Driver details and the panel's log", &y, w);
        if (G.have_info) {
            b[0] = 0;
            scat(b, sizeof b, "Driver: device %04x, %u chip(s) found, Glide told %u, %u MB a "
                 "chip, desktop %ux%ux%u@%u, flags 0x%x, boot attempts %u (stable %u), SLI %u "
                 "chips (result %d, 6000 clock %u Hz), monitor source %u, %u modes, 2D report %d "
                 "bytes", G.info.device, G.info.nchips, G.info.glide_chips,
                 G.info.fb_per_chip >> 20, G.info.cur_w, G.info.cur_h, G.info.cur_bpp,
                 G.info.cur_hz, G.info.flags, G.info.boot_attempts, G.info.boot_good,
                 G.info.sli_chips, (int)G.info.sli_result, G.info.clock_6k_hz, G.info.mon_src,
                 G.info.nmodes, G.size_2d);
            mk_col(page, b, 14, y, w - 28, 28, COL_GRAY, g_small);
            y += 32;
        }
        g_log_edit = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", g_logbuf, WS_CHILD | WS_VISIBLE |
                                     ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL |
                                     WS_TABSTOP, 14, y, w - 28, 150, page, NULL, g_hi, NULL);
        SendMessageA(g_log_edit, WM_SETFONT, (WPARAM)g_small, 0);
        y += 158;
        break;
    }
    default: {
        const char *grp = NULL;
        for (i = 0; i < NR; i++) {
            if (R[i].row->tab != tab)
                continue;
            if (!grp || strcmp(grp, R[i].row->group)) {
                section(page, R[i].row->group, &y, w);
                grp = R[i].row->group;
            }
            build_row(page, &R[i], i, &y, w);
        }
        if (tab == CTL_TAB_GL)
            mk_col(page, "Vertical sync, the frame queue and the fullscreen refresh rate on the "
                   "3D & Glide tab apply to OpenGL games too. * = stored in your user "
                   "environment: games started from the desktop or Start menu after Apply use "
                   "it.", 14, y + 6, w - 28, 44, COL_GRAY, g_small);
        if (tab == CTL_TAB_GL)
            y += 54;
        if (tab == CTL_TAB_3D && G.lane == CTL_LANE_VCR)
            mk_col(page, "* = stored in your user environment (our OpenGL driver reads it "
                   "nowhere else): games started from the desktop or Start menu after Apply use "
                   "it.", 14, y + 6, w - 28, 30, COL_GRAY, g_small);
        if (tab == CTL_TAB_3D && G.lane == CTL_LANE_VCR)
            y += 40;
        break;
    }
    }
    (void)h;
    return y + 8;
}

/* per page: its content height and scroll position (a page taller than the tab
 * scrolls - notes vary, and an 800x600 desktop must still show every control) */
static int g_page_h[CTL_NTABS], g_page_y[CTL_NTABS];

static int page_index(HWND page)
{
    int i;
    for (i = 0; i < g_ntabs; i++)
        if (g_page[i] == page)
            return i;
    return -1;
}

static void page_scroll_to(HWND page, int pos)
{
    int i = page_index(page), view, max;
    SCROLLINFO si;
    if (i < 0)
        return;
    view = g_pagerc.bottom - g_pagerc.top;
    max = g_page_h[i] - view;
    if (max < 0)
        max = 0;
    if (pos > max)
        pos = max;
    if (pos < 0)
        pos = 0;
    if (pos == g_page_y[i])
        return;
    ScrollWindowEx(page, 0, g_page_y[i] - pos, NULL, NULL, NULL, NULL,
                   SW_SCROLLCHILDREN | SW_INVALIDATE | SW_ERASE);
    g_page_y[i] = pos;
    memset(&si, 0, sizeof si);
    si.cbSize = sizeof si;
    si.fMask = SIF_POS;
    si.nPos = pos;
    SetScrollInfo(page, SB_VERT, &si, TRUE);
}

static void pages_destroy(void)
{
    int i;
    for (i = 0; i < CTL_NTABS; i++)
        if (g_page[i]) {
            DestroyWindow(g_page[i]);
            g_page[i] = NULL;
        }
    g_ncs = 0;
    g_log_edit = NULL;
    g_aa_live = g_2d_state = NULL;
}

static void pages_build(void)
{
    static WORD tmpl[256] __attribute__((aligned(4)));
    int i, sel = TabCtrl_GetCurSel(g_tab);
    pages_destroy();
    for (i = 0; i < g_ntabs; i++) {
        HWND p = CreateDialogIndirectParamA(
            g_hi, dlg_template(tmpl, 256, WS_CHILD | DS_CONTROL, WS_EX_CONTROLPARENT, 10, 10, ""),
            g_main, PageProc, (LPARAM)g_tabmap[i]);
        g_page[i] = p;
        SetWindowPos(p, HWND_TOP, g_pagerc.left, g_pagerc.top, g_pagerc.right - g_pagerc.left,
                     g_pagerc.bottom - g_pagerc.top, SWP_NOACTIVATE);
        if (G.themed && p_EnableThemeDialogTexture)
            p_EnableThemeDialogTexture(p, 6 /* ETDT_ENABLETAB */);
        /* built one scroll bar narrower, so a page that needs one never covers a control */
        g_page_h[i] = page_build(g_tabmap[i], p, g_pagerc.right - g_pagerc.left -
                                 GetSystemMetrics(SM_CXVSCROLL), g_pagerc.bottom - g_pagerc.top);
        g_page_y[i] = 0;
        {
            SCROLLINFO si;
            memset(&si, 0, sizeof si);
            si.cbSize = sizeof si;
            si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS | SIF_DISABLENOSCROLL;
            si.nMin = 0;
            si.nMax = g_page_h[i];
            si.nPage = (UINT)(g_pagerc.bottom - g_pagerc.top);
            si.nPos = 0;
            if (g_page_h[i] > (int)si.nPage) {
                ShowScrollBar(p, SB_VERT, TRUE);
                SetScrollInfo(p, SB_VERT, &si, TRUE);
            } else {
                ShowScrollBar(p, SB_VERT, FALSE);
            }
        }
    }
    if (sel < 0)
        sel = 0;
    for (i = 0; i < g_ntabs; i++)
        ShowWindow(g_page[i], i == sel ? SW_SHOW : SW_HIDE);
    ui_update_buttons();
}

static void reload(void)
{
    int force = G.forced ? G.lane : 0;
    stack_detect(force);
    rows_load();
    pages_build();
    InvalidateRect(g_header, NULL, TRUE);
}

/* ---- experimental AA: the warning, and the unguarded-Glide scan ---------------------- */
static void scan_glide_dirs(char *out, size_t n, int *unguarded)
{
    static const char *const roots[] = { "C:\\Games", "D:\\Games" };
    unsigned k;
    int g = 0, found = 0;
    char path[MAX_PATH];
    WIN32_FIND_DATAA fd;
    HANDLE f;
    *unguarded = 0;
    out[0] = 0;
    if (G.glide_present && !G.glide_guard) {
        scat(out, n, "  %s\r\n", G.glide_dll);
        (*unguarded)++;
    }
    for (k = 0; k < 2; k++) {
        _snprintf(path, sizeof path, "%s\\*", roots[k]);
        path[sizeof path - 1] = 0;
        f = FindFirstFileA(path, &fd);
        if (f == INVALID_HANDLE_VALUE)
            continue;
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == '.')
                continue;
            _snprintf(path, sizeof path, "%s\\%s\\glide3x.dll", roots[k], fd.cFileName);
            path[sizeof path - 1] = 0;
            if (glide_dll_check(path, &g, NULL, NULL)) {
                found++;
                if (!g) {
                    scat(out, n, "  %s\r\n", path);
                    (*unguarded)++;
                }
            }
        } while (FindNextFileA(f, &fd) && found < 200);
        FindClose(f);
    }
    if (!*unguarded)
        scat(out, n, "  none (%d game-local glide3x.dll checked)\r\n", found);
}

static int aa_confirm(HWND owner)
{
    char list[4096], msg[6144];
    int ung = 0, r;
    HCURSOR old = SetCursor(LoadCursor(NULL, IDC_WAIT));
    scan_glide_dirs(list, sizeof list, &ung);
    SetCursor(old);
    _snprintf(msg, sizeof msg,
              "Anti-aliasing on this %s is EXPERIMENTAL on our driver stack.\r\n\r\n"
              "What has happened so far on the Voodoo5 6000:\r\n"
              "  - 4x AA FROZE THE WHOLE PC (only the power switch helped), 2026-09-26\r\n"
              "  - 2x AA runs but draws a ghost / double image, 2026-09-27\r\n"
              "  - 8x AA has never been run\r\n\r\n"
              "Saying yes lists the AA modes. Choosing one and pressing Apply arms the "
              "driver's AA switch (Diag\\SliAA) and makes the next Glide or OpenGL game open "
              "in AA. Do this only with someone at the PC who can power-cycle it, and set the "
              "mode back to SLI afterwards (that disarms the switch).\r\n\r\n"
              "Glide DLLs WITHOUT the SLI/AA guard (a game using one can freeze the board even "
              "with the kernel switch):\r\n%s\r\n"
              "Allow experimental anti-aliasing?",
              ctl_card_name(G.info.device, G.nchips), list);
    msg[sizeof msg - 1] = 0;
    r = MessageBoxA(owner, msg, APP_TITLE " - experimental anti-aliasing",
                    MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2);
    ctl_log("experimental AA warning shown (%d unguarded Glide DLL(s)): %s", ung,
            r == IDYES ? "the user said YES" : "the user said no");
    return r == IDYES;
}

/* ---- presets --------------------------------------------------------------------------- */
static void preset_apply(int p)
{
    int i, n = 0;
    char buf[8];
    const char *v;
    for (i = 0; i < NR; i++) {
        if (ctl_preset_value(p, R[i].row->id, G.nchips, &v, buf, sizeof buf) != CTL_PRESET_SET)
            continue;
        if (R[i].disabled)
            continue;
        /* a chip mode that already means "all chips, no AA" is left as it is */
        if (R[i].row->kind == CTL_K_AA && v &&
            ctl_aa_same_mode(R[i].present, atol(R[i].cur), 1, atol(v), G.nchips)) {
            set_pend(&R[i], R[i].present ? R[i].cur : NULL);
            continue;
        }
        set_pend(&R[i], v);
        n++;
    }
    if (p == CTL_PRESET_DEFAULTS || p == CTL_PRESET_QUALITY || p == CTL_PRESET_SPEED) {
        ROWSTATE *aa = rs_by_id(CTL_ID_AA);
        if (aa && aa->pend_present && ctl_aa_is_aa(atol(aa->pend)))
            set_pend(aa, NULL);             /* never leave AA selected behind a preset */
    }
    /* the button that asked is on a page about to be rebuilt: not from inside its click */
    PostMessageA(g_main, WM_APP_REBUILD, 0, 0);
    ui_set_status("Preset \"%s\" filled in %d setting(s) - press Apply or OK to write them.",
                  p == CTL_PRESET_QUALITY ? "Maximum quality" : p == CTL_PRESET_SPEED
                  ? "Maximum speed" : "Driver defaults", n);
}

/* ---- Apply ----------------------------------------------------------------------------- */
typedef struct {
    int     want_refresh;           /* a new desktop refresh (CDS_UPDATEREGISTRY): one switch */
    int     want_bounce;            /* the 2D switches alone: through `alt` and back (two) */
    int     revert;
    DEVMODEA dm, old, alt;
    unsigned want_2d_mask, want_2d_flags;
    /* results */
    int     paced, paced2;
    char    why[160];
    LONG    cds, cds2;
    DWORD   cur_w, cur_h, cur_bpp, cur_hz, reg_hz;
    unsigned info_hz;
    int     have2d;
    unsigned flags2d;
    int     size2d;
} DISPJOB;
static DISPJOB g_job;

static DEVMODEA devmode_of(const DEVMODEA *base, unsigned w, unsigned h, unsigned bpp,
                           unsigned hz)
{
    DEVMODEA d = *base;
    d.dmSize = sizeof d;
    d.dmDriverExtra = 0;
    d.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_BITSPERPEL | DM_DISPLAYFREQUENCY;
    d.dmPelsWidth = w;
    d.dmPelsHeight = h;
    d.dmBitsPerPel = bpp;
    d.dmDisplayFrequency = hz;
    return d;
}

static void summary(const char *fmt, ...)
{
    size_t have = strlen(g_summary);
    va_list ap;
    if (have + 4 >= sizeof g_summary)
        return;
    va_start(ap, fmt);
    _vsnprintf(g_summary + have, sizeof g_summary - have - 3, fmt, ap);
    va_end(ap);
    g_summary[sizeof g_summary - 3] = 0;
    ctl_log("%s", g_summary + have);            /* ctl_log ends the line itself */
    strcat(g_summary, "\r\n");
}

/* ONE display-mode switch, through the gate (tools/vcr_pace.h): at least the
 * floor since the last switch anyone on the box made, under the box's switch
 * lock. 0 = the gate refused and NOTHING was switched - the caller then makes
 * no switch by any other road. A CDS_FULLSCREEN mode is temporary (XP reverts
 * it when the panel exits or dies): recorded as held, so the exit hold / the
 * crash filter pace that revert. Any other change lasts: recorded, not held.
 * Recorded whether or not the driver accepted it. */
static int paced_switch(const DEVMODEA *mode, DWORD flags, LONG *cds)
{
    DEVMODEA d = *mode;
    if (!vcr_pace_before_switch())
        return 0;
    *cds = ChangeDisplaySettingsExA(NULL, &d, NULL, flags, NULL);
    if (flags & CDS_FULLSCREEN)
        vcr_pace_after_switch();
    else
        vcr_pace_after_restore();
    return 1;
}

static DWORD WINAPI disp_worker(LPVOID arg)
{
    DISPJOB *j = (DISPJOB *)arg;
    DEVMODEA dm;
    vcr_info v;
    vcr_2d_stats st;
    j->cds = j->cds2 = DISP_CHANGE_FAILED;
    if (j->want_refresh) {
        /* the new refresh rate, saved as the desktop's mode */
        j->paced = paced_switch(&j->dm, CDS_UPDATEREGISTRY, &j->cds);
    } else {
        /* the 2D switches: out to `alt` as a TEMPORARY mode, then back to the
         * mode we came from - a new display surface at the same mode, which is
         * what reads them (a same-mode CDS_RESET is not one) */
        j->paced = paced_switch(&j->alt, CDS_FULLSCREEN, &j->cds);
        if (j->paced && j->cds == DISP_CHANGE_SUCCESSFUL) {
            j->paced2 = paced_switch(&j->dm, 0, &j->cds2);  /* waits out the floor */
            if (!j->paced2)
                /* refused on the way out: no other road back - the exit hold
                 * paces XP's revert when the panel closes */
                scpy(j->why, g_vcr_pace_why ? g_vcr_pace_why : "the pace gate refused",
                     sizeof j->why);
        }
    }
    if (!j->paced) {
        scpy(j->why, g_vcr_pace_why ? g_vcr_pace_why : "the pace gate refused", sizeof j->why);
        PostMessageA(g_main, WM_APP_DISPDONE, 0, (LPARAM)j);
        return 0;
    }
    Sleep(400);
    /* the post-condition, read back - not the return value */
    memset(&dm, 0, sizeof dm);
    dm.dmSize = sizeof dm;
    if (EnumDisplaySettingsA(NULL, ENUM_CURRENT_SETTINGS, &dm)) {
        j->cur_w = dm.dmPelsWidth;
        j->cur_h = dm.dmPelsHeight;
        j->cur_bpp = dm.dmBitsPerPel;
        j->cur_hz = dm.dmDisplayFrequency;
    }
    memset(&dm, 0, sizeof dm);
    dm.dmSize = sizeof dm;
    if (EnumDisplaySettingsA(NULL, ENUM_REGISTRY_SETTINGS, &dm))
        j->reg_hz = dm.dmDisplayFrequency;
    memset(&v, 0, sizeof v);
    if (esc_call(VCR_ESC_INFO, &v, sizeof v) > 0)
        j->info_hz = v.cur_hz;
    memset(&st, 0, sizeof st);
    j->size2d = esc_call(VCR_ESC_2D_STATS, &st, sizeof st);
    j->have2d = j->size2d >= (int)VCR_2DS_SIZE_V1;
    j->flags2d = st.flags;
    PostMessageA(g_main, WM_APP_DISPDONE, 0, (LPARAM)j);
    return 0;
}

static void disp_start(void)
{
    HANDLE t;
    g_busy = 1;
    ui_update_buttons();
    ui_set_status("Changing the display - waiting for the monitor's pace gate (3 s between "
                  "mode switches), then one re-sync...");
    t = CreateThread(NULL, 0, disp_worker, &g_job, 0, NULL);
    if (t)
        CloseHandle(t);
    else {
        g_busy = 0;
        summary("  [FAILED] display change: could not start the worker (error %lu)",
                GetLastError());
    }
}

/* write + read back one value; returns 1 when the read-back matches */
static int write_value(HKEY root, const char *path, const char *name, const char *val, int dword,
                       char *why, size_t whyn)
{
    LONG e;
    char back[80];
    DWORD d = 0;
    why[0] = 0;
    if (root == HKEY_LOCAL_MACHINE && strcmp(path, CTL_KEY_DIAG) == 0 && !ctl_diag_name_ok(name)) {
        scpy(why, "refused: not a switch this panel may write", whyn);
        return 0;
    }
    if (!val) {
        e = reg_del_value(root, path, name);
        if (e != ERROR_SUCCESS) {
            scpy(why, reg_err(e), whyn);
            return 0;
        }
        e = dword ? reg_get_dword(root, path, name, &d) : reg_get_sz(root, path, name, back,
                                                                     sizeof back);
        if (e != ERROR_FILE_NOT_FOUND) {
            scpy(why, "still there after the delete", whyn);
            return 0;
        }
        return 1;
    }
    e = dword ? reg_set_dword(root, path, name, (DWORD)strtoul(val, NULL, 10))
              : reg_set_sz(root, path, name, val);
    if (e != ERROR_SUCCESS) {
        scpy(why, reg_err(e), whyn);
        return 0;
    }
    if (dword) {
        if (reg_get_dword(root, path, name, &d) != ERROR_SUCCESS || d != strtoul(val, NULL, 10)) {
            scpy(why, "the read-back differs", whyn);
            return 0;
        }
    } else if (reg_get_sz(root, path, name, back, sizeof back) != ERROR_SUCCESS ||
               strcmp(back, val) != 0) {
        scpy(why, "the read-back differs", whyn);
        return 0;
    }
    return 1;
}

static const char *pend_label(ROWSTATE *s, char *b, size_t n)
{
    int k;
    for (k = 0; k < s->nitems; k++)
        if (s->pend_present ? (!s->iabsent[k] && strcmp(s->ival[k], s->pend) == 0)
                            : s->iabsent[k]) {
            if (s->hctl && (s->row->kind == CTL_K_CHOICE || s->row->kind == CTL_K_AA ||
                            s->row->kind == CTL_K_GLIDE_REFRESH ||
                            s->row->kind == CTL_K_DESK_REFRESH)) {
                int i, cnt = (int)SendMessageA(s->hctl, CB_GETCOUNT, 0, 0);
                for (i = 0; i < cnt; i++)
                    if ((int)SendMessageA(s->hctl, CB_GETITEMDATA, i, 0) == k &&
                        SendMessageA(s->hctl, CB_GETLBTEXTLEN, i, 0) < (LRESULT)n) {
                        SendMessageA(s->hctl, CB_GETLBTEXT, i, (LPARAM)b);
                        return b;
                    }
            }
        }
    if (s->row->kind == CTL_K_CHECK)
        return s->pend_present && streq(s->pend, s->row->choices[1].value) ? "on" : "off";
    _snprintf(b, n, "%s", s->pend_present ? s->pend : "default");
    b[n - 1] = 0;
    return b;
}

static void apply_all(void)
{
    int i, env_changed = 0, fails = 0, oks = 0;
    char why[200], lab[240];
    const char *lb;
    unsigned want2d_mask = 0, want2d_flags = 0;
    ROWSTATE *desk = NULL;
    g_summary[0] = 0;
    summary("3dfx Control Panel %s - Apply (%s lane)", CTL_VERSION,
            G.lane == CTL_LANE_VCR ? "our stack: vcr-kmd" : "vintage 3dfxvs");
    for (i = 0; i < NR; i++) {
        ROWSTATE *s = &R[i];
        const ctl_row *r = s->row;
        if (!row_changed(s) || s->disabled)
            continue;
        lb = pend_label(s, lab, sizeof lab);
        if (r->store == CTL_ST_DISPLAY) {
            desk = s;
            continue;
        }
        if (r->kind == CTL_K_AA) {
            ctl_aa_plan plan;
            int ok = 1;
            ctl_aa_make_plan(G.nchips, s->pend_present ? atol(s->pend) : -1, g_aa_confirmed,
                             G.sliaa_present && G.sliaa, &plan);
            /* a value set elsewhere and kept as it was (not offered) is never rewritten */
            if (plan.refused) {
                summary("  [NOT APPLIED] %s = %s: %s", r->label, lb, plan.refused);
                fails++;
                continue;
            }
            if (!write_value(HKEY_LOCAL_MACHINE, G.glide_key, r->name,
                             plan.cfg_absent ? NULL : plan.cfg_value, 0, why, sizeof why)) {
                summary("  [FAILED] %s = %s: HKLM\\%s\\%s - %s", r->label, lb, G.glide_key,
                        r->name, why);
                fails++;
                continue;
            }
            if (plan.sliaa) {
                ok = write_value(HKEY_LOCAL_MACHINE, CTL_KEY_DIAG, "SliAA",
                                 plan.sliaa > 0 ? "1" : NULL, 1, why, sizeof why);
                if (!ok) {
                    summary("  [FAILED] the kernel AA switch Diag\\SliAA: %s", why);
                    fails++;
                }
            }
            summary("  [OK] %s = %s  ->  HKLM\\...\\%s %s%s%s (read back) - the next Glide or "
                    "OpenGL game", r->label, lb, r->name,
                    plan.cfg_absent ? "deleted (Glide's default)" : "= ",
                    plan.cfg_absent ? "" : plan.cfg_value,
                    plan.sliaa > 0 ? "; Diag\\SliAA = 1 (EXPERIMENTAL AA ARMED)"
                    : plan.sliaa < 0 ? "; Diag\\SliAA deleted (AA disarmed)" : "");
            ctl_log("AA: %s -> %s, SliAA %d", s->present ? s->cur : "(absent)",
                    plan.cfg_absent ? "(absent)" : plan.cfg_value, plan.sliaa);
            oks++;
            continue;
        }
        if (r->kind == CTL_K_V_GAMMA) {
            float g = r->fdef;
            int rc;
            ctl_parse_float(s->pend, &g);
            rc = v_gamma_set(r->name, g, (r->flags & CTL_F_DESKTOP_GAMMA) != 0);
            if (rc == 1) {
                summary("  [OK] %s = %.2f  ->  HKLM\\%s\\%s (a 256-entry table) - %s", r->label,
                        g, CTL_KEY_V_DEV0, r->name, when_text(r->when));
                oks++;
            } else {
                summary("  [%s] %s = %.2f: %s", rc < 0 ? "REFUSED" : "FAILED", r->label, g,
                        rc < 0 ? "it would wash out the 2D desktop" : "registry write failed");
                fails++;
            }
            continue;
        }
        if (r->kind == CTL_K_V_INT) {
            char *end = NULL;
            long v = s->pend_present ? strtol(s->pend, &end, 10) : 0;
            if (!s->pend_present || !end || *end || end == s->pend || v < (long)r->fmin ||
                v > (long)r->fmax) {
                summary("  [NOT APPLIED] %s = \"%s\": a whole number from %ld to %ld, please",
                        r->label, s->pend, (long)r->fmin, (long)r->fmax);
                fails++;
                continue;
            }
        }
        {
            /* a plain value, or three (R|G|B) written alike */
            const char *p = r->name;
            int ok = 1, dword = (r->flags & CTL_F_DWORD) || r->store == CTL_ST_DIAG;
            const char *val = s->pend_present ? s->pend : NULL;
            char name[48];
            while (p && *p) {
                const char *bar = strchr(p, '|');
                _snprintf(name, sizeof name, "%.*s", (int)(bar ? bar - p : (int)strlen(p)), p);
                name[sizeof name - 1] = 0;
                p = bar ? bar + 1 : NULL;
                if (!write_value(store_root(r->store), store_path(r->store), name, val, dword,
                                 why, sizeof why)) {
                    summary("  [FAILED] %s = %s: %s\\%s - %s", r->label, lb,
                            store_where(r->store), name, why);
                    ok = 0;
                    fails++;
                    break;
                }
            }
            if (!ok)
                continue;
            summary("  [OK] %s = %s  ->  %s\\%s %s%s (read back) - %s", r->label, lb,
                    store_where(r->store), r->name, val ? "= " : "deleted", val ? val : "",
                    r->when == CTL_WHEN_EXPLORER ? "games started from the desktop/Start menu "
                                                   "from now on"
                    : r->when == CTL_WHEN_NOW ? "taking effect now (below)"
                                              : when_text(r->when));
            oks++;
            if (r->store == CTL_ST_ENV)
                env_changed = 1;
            if (r->store == CTL_ST_DIAG) {
                unsigned bit = r->id == CTL_ID_2D_TEXT ? VCR_2DS_F_TEXT
                             : r->id == CTL_ID_2D_PAT ? VCR_2DS_F_PAT : VCR_2DS_F_LINE;
                want2d_mask |= bit;
                if (val)
                    want2d_flags |= bit;
            }
        }
    }
    if (g_rm_overrides) {
        int k;
        for (k = 0; k < NOV; k++) {
            if (write_value(OV[k].root, OV[k].path, OV[k].name, NULL, 0, why, sizeof why)) {
                summary("  [OK] removed the override %s\\%s  %s (was %s)",
                        OV[k].root == HKEY_CURRENT_USER ? "HKCU" : "HKLM", OV[k].path,
                        OV[k].name, OV[k].value);
                if (strcmp(OV[k].path, CTL_KEY_USER_ENV) == 0 ||
                    strcmp(OV[k].path, CTL_KEY_SYSTEM_ENV) == 0)
                    env_changed = 1;
                oks++;
            } else {
                summary("  [FAILED] could not remove %s (%s)", OV[k].name, why);
                fails++;
            }
        }
    }
    if (env_changed) {
        DWORD_PTR res = 0;
        LRESULT ok = SendMessageTimeoutA(HWND_BROADCAST, WM_SETTINGCHANGE, 0,
                                         (LPARAM)"Environment", SMTO_ABORTIFHUNG, 5000, &res);
        summary("  %s Explorer told about the environment change (WM_SETTINGCHANGE%s): "
                "games you start from the desktop or Start menu now see it; a program that was "
                "already running (the retro agent included) keeps its old environment.",
                ok ? "[OK]" : "[WARN]", ok ? "" : " timed out");
    }
    /* the display: one paced change covers a new refresh AND the 2D switches */
    memset(&g_job, 0, sizeof g_job);
    g_job.want_2d_mask = want2d_mask;
    g_job.want_2d_flags = want2d_flags;
    if (desk && G.have_cur) {
        unsigned hz = (unsigned)strtoul(desk->pend, NULL, 10);
        if (mon_unfiltered()) {
            summary("  [NOT APPLIED] desktop refresh: the driver's mode list is not filtered by "
                    "the monitor (no EDID limits in force) - not changing a CRT's rate blind");
            fails++;
        } else if (!ctl_mode_listed(G.modes, G.nmodes, G.cur.dmPelsWidth, G.cur.dmPelsHeight,
                                    G.cur.dmBitsPerPel, hz)) {
            summary("  [NOT APPLIED] desktop refresh %u Hz: the driver does not list it for "
                    "%lux%lu %lu-bit", hz, G.cur.dmPelsWidth, G.cur.dmPelsHeight,
                    G.cur.dmBitsPerPel);
            fails++;
        } else {
            /* a real mode change: the new display surface also reads the 2D switches */
            g_job.want_refresh = 1;
            g_job.old = devmode_of(&G.cur, G.cur.dmPelsWidth, G.cur.dmPelsHeight,
                                   G.cur.dmBitsPerPel, G.cur.dmDisplayFrequency);
            g_job.dm = devmode_of(&G.cur, G.cur.dmPelsWidth, G.cur.dmPelsHeight,
                                  G.cur.dmBitsPerPel, hz);
        }
    }
    if (!g_job.want_refresh && want2d_mask && G.have_cur) {
        ctl_mode cur, alt;
        cur.w = G.cur.dmPelsWidth;
        cur.h = G.cur.dmPelsHeight;
        cur.bpp = G.cur.dmBitsPerPel;
        cur.hz = G.cur.dmDisplayFrequency;
        if (mon_unfiltered() || !ctl_bounce_mode(G.modes, G.nmodes, &cur, &alt)) {
            summary("  [NOT APPLIED NOW] the 2D switches are stored, but there is no other mode "
                    "at %ux%u the driver lists for this monitor to pass through - they take "
                    "effect at the next display mode change (a fullscreen game starting or "
                    "ending) or the next boot", cur.w, cur.h);
        } else {
            g_job.want_bounce = 1;
            g_job.dm = devmode_of(&G.cur, cur.w, cur.h, cur.bpp, cur.hz);
            g_job.alt = devmode_of(&G.cur, alt.w, alt.h, alt.bpp, alt.hz);
            summary("  The 2D switches are read when the display driver builds a new surface: "
                    "the panel switches to %ux%u %u-bit %u Hz and back to %u-bit %u Hz (two "
                    "paced switches, at least 3 s apart; the monitor re-syncs twice).", alt.w,
                    alt.h, alt.bpp, alt.hz, cur.bpp, cur.hz);
        }
    }
    if ((g_job.want_refresh || g_job.want_bounce) && G.have_dd && G.dd[1]) {
        summary("  [NOT APPLIED NOW] a Glide program (pid %lu) holds the display: the display "
                "is not touched while it runs. The 2D switches are stored and take effect at "
                "the next mode change.", G.dd[1]);
        g_job.want_refresh = g_job.want_bounce = 0;
    }
    summary("%d setting(s) written and read back, %d not applied%s.", oks, fails,
            g_job.want_refresh || g_job.want_bounce ? " - the display change follows" : "");
    if (g_job.want_refresh || g_job.want_bounce) {
        disp_start();
        return;
    }
    SendMessageA(g_main, WM_APP_DISPDONE, 1, 0);        /* nothing to wait for: finish */
}

/* a dialog resized after creation: centre it over the panel (DS_CENTER used its
 * template size), inside the work area */
static void center_on_main(HWND h)
{
    RECT me, mw, wa;
    int x, y;
    GetWindowRect(h, &me);
    SystemParametersInfoA(SPI_GETWORKAREA, 0, &wa, 0);
    if (g_main && IsWindow(g_main))
        GetWindowRect(g_main, &mw);
    else
        mw = wa;
    x = mw.left + ((mw.right - mw.left) - (me.right - me.left)) / 2;
    y = mw.top + ((mw.bottom - mw.top) - (me.bottom - me.top)) / 2;
    if (x + (me.right - me.left) > wa.right)
        x = wa.right - (me.right - me.left);
    if (y + (me.bottom - me.top) > wa.bottom)
        y = wa.bottom - (me.bottom - me.top);
    if (x < wa.left)
        x = wa.left;
    if (y < wa.top)
        y = wa.top;
    SetWindowPos(h, NULL, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
}

/* ---- the keep-or-revert dialog ------------------------------------------------------- */
static int g_countdown;

static INT_PTR CALLBACK KeepProc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
    char b[300];
    (void)lp;
    switch (m) {
    case WM_INITDIALOG: {
        RECT rc = { 0, 0, 420, 130 };
        AdjustWindowRectEx(&rc, GetWindowLongA(h, GWL_STYLE), FALSE, GetWindowLongA(h, GWL_EXSTYLE));
        SetWindowPos(h, HWND_TOPMOST, 0, 0, rc.right - rc.left, rc.bottom - rc.top, SWP_NOMOVE);
        center_on_main(h);
        _snprintf(b, sizeof b, "The desktop now runs at %lu Hz.\n\nKeep this refresh rate? It "
                  "goes back to %lu Hz in 15 seconds if you do nothing.",
                  g_job.dm.dmDisplayFrequency, g_job.old.dmDisplayFrequency);
        b[sizeof b - 1] = 0;
        mk(h, "STATIC", b, SS_LEFT | SS_NOPREFIX, 16, 14, 390, 60, 200, NULL);
        mk(h, "BUTTON", "&Keep it", BS_PUSHBUTTON | WS_TABSTOP, 196, 90, 100, 26, IDOK, NULL);
        mk(h, "BUTTON", "&Revert (15)", BS_DEFPUSHBUTTON | WS_TABSTOP, 306, 90, 100, 26, IDCANCEL,
           NULL);
        g_countdown = 15;
        SetTimer(h, TIMER_COUNTDOWN, 1000, NULL);
        SetFocus(GetDlgItem(h, IDCANCEL));
        return FALSE;
    }
    case WM_TIMER:
        if (--g_countdown <= 0) {
            KillTimer(h, TIMER_COUNTDOWN);
            EndDialog(h, IDCANCEL);
        } else {
            _snprintf(b, sizeof b, "&Revert (%d)", g_countdown);
            SetDlgItemTextA(h, IDCANCEL, b);
        }
        return TRUE;
    case WM_COMMAND:
        if (LOWORD(wp) == IDOK || LOWORD(wp) == IDCANCEL) {
            KillTimer(h, TIMER_COUNTDOWN);
            EndDialog(h, LOWORD(wp));
        }
        return TRUE;
    case WM_CLOSE:
        EndDialog(h, IDCANCEL);
        return TRUE;
    }
    return FALSE;
}

/* ---- the summary dialog ---------------------------------------------------------------- */
static INT_PTR CALLBACK SummaryProc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
    (void)lp;
    switch (m) {
    case WM_INITDIALOG: {
        RECT rc = { 0, 0, 640, 360 };
        HWND e;
        AdjustWindowRectEx(&rc, GetWindowLongA(h, GWL_STYLE), FALSE, GetWindowLongA(h, GWL_EXSTYLE));
        SetWindowPos(h, NULL, 0, 0, rc.right - rc.left, rc.bottom - rc.top, SWP_NOMOVE |
                     SWP_NOZORDER);
        center_on_main(h);
        e = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", g_summary, WS_CHILD | WS_VISIBLE |
                            ES_MULTILINE | ES_READONLY | WS_VSCROLL | ES_AUTOVSCROLL, 10, 10, 620,
                            300, h, NULL, g_hi, NULL);
        SendMessageA(e, WM_SETFONT, (WPARAM)g_font, 0);
        mk(h, "BUTTON", "OK", BS_DEFPUSHBUTTON | WS_TABSTOP, 530, 322, 100, 26, IDOK, NULL);
        SetFocus(GetDlgItem(h, IDOK));
        return FALSE;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == IDOK || LOWORD(wp) == IDCANCEL)
            EndDialog(h, IDOK);
        return TRUE;
    case WM_CLOSE:
        EndDialog(h, IDOK);
        return TRUE;
    }
    return FALSE;
}

static void show_summary(void)
{
    static WORD tmpl[256] __attribute__((aligned(4)));
    DialogBoxIndirectParamA(g_hi, dlg_template(tmpl, 256, WS_POPUP | WS_CAPTION | WS_SYSMENU |
                                               DS_MODALFRAME | DS_CENTER, 0, 300, 180,
                                               APP_TITLE " - what was applied"),
                            g_main, SummaryProc, 0);
}

/* the display change finished (or there was none: wp = 1) */
static void on_disp_done(WPARAM wp, DISPJOB *j)
{
    static WORD tmpl[256] __attribute__((aligned(4)));
    if (!wp && j) {
        g_busy = 0;
        if (!j->paced) {
            summary("  [NOT APPLIED] display change: the pace gate said \"%s\" - nothing was "
                    "switched. Try again in a moment.", j->why);
        } else if (j->want_refresh) {
            unsigned want = j->dm.dmDisplayFrequency;
            if (j->cds == DISP_CHANGE_SUCCESSFUL && j->cur_hz == want && j->reg_hz == want &&
                (!G.have_info || j->info_hz == want)) {
                summary("  [OK] desktop %s %u Hz (paced; read back: current %lu Hz, saved %lu Hz%s)",
                        j->revert ? "put back to" : "refresh rate set to", want, j->cur_hz,
                        j->reg_hz, G.have_info ? ", driver agrees" : "");
                if (!j->revert) {
                    int keep = (int)DialogBoxIndirectParamA(
                        g_hi, dlg_template(tmpl, 256, WS_POPUP | WS_CAPTION | DS_MODALFRAME |
                                           DS_CENTER, WS_EX_TOPMOST, 280, 100,
                                           APP_TITLE " - keep this refresh rate?"),
                        g_main, KeepProc, 0);
                    if (keep != IDOK) {
                        DEVMODEA back = j->old;
                        DISPJOB nj;
                        memset(&nj, 0, sizeof nj);
                        nj.want_refresh = 1;
                        nj.revert = 1;
                        nj.dm = back;
                        nj.old = j->dm;
                        nj.want_2d_mask = j->want_2d_mask;
                        nj.want_2d_flags = j->want_2d_flags;
                        g_job = nj;
                        summary("  The refresh rate was not kept - putting %lu Hz back.",
                                back.dmDisplayFrequency);
                        disp_start();
                        return;
                    }
                    summary("  Kept %u Hz.", want);
                }
            } else {
                summary("  [FAILED] desktop refresh %u Hz: ChangeDisplaySettingsEx said %ld; "
                        "read back current %lu Hz, saved %lu Hz, driver %u Hz", want,
                        (long)j->cds, j->cur_hz, j->reg_hz, j->info_hz);
            }
        } else if (j->want_bounce) {
            int back = j->cur_w == j->dm.dmPelsWidth && j->cur_h == j->dm.dmPelsHeight &&
                       j->cur_bpp == j->dm.dmBitsPerPel && j->cur_hz == j->dm.dmDisplayFrequency;
            if (j->cds != DISP_CHANGE_SUCCESSFUL)
                summary("  [NOT APPLIED] the switch to %lux%lu %lu-bit %lu Hz failed "
                        "(ChangeDisplaySettingsEx %ld) - the display was not changed",
                        j->alt.dmPelsWidth, j->alt.dmPelsHeight, j->alt.dmBitsPerPel,
                        j->alt.dmDisplayFrequency, (long)j->cds);
            else if (!j->paced2)
                summary("  [WARN] the display is still at %lux%lu %lu-bit %lu Hz: the pace gate "
                        "said \"%s\" on the way back. Close the panel and Windows puts the "
                        "desktop back (paced).", j->alt.dmPelsWidth, j->alt.dmPelsHeight,
                        j->alt.dmBitsPerPel, j->alt.dmDisplayFrequency, j->why);
            else if (j->cds2 != DISP_CHANGE_SUCCESSFUL || !back)
                summary("  [FAILED] the switch back to %lux%lu %lu-bit %lu Hz said %ld; the "
                        "desktop reads %lux%lu %lu-bit %lu Hz now", j->dm.dmPelsWidth,
                        j->dm.dmPelsHeight, j->dm.dmBitsPerPel, j->dm.dmDisplayFrequency,
                        (long)j->cds2, j->cur_w, j->cur_h, j->cur_bpp, j->cur_hz);
            else
                summary("  [OK] display passed through %lu-bit %lu Hz and is back at %lux%lu "
                        "%lu-bit %lu Hz (paced, read back)", j->alt.dmBitsPerPel,
                        j->alt.dmDisplayFrequency, j->cur_w, j->cur_h, j->cur_bpp, j->cur_hz);
        }
        if (j->paced && j->want_2d_mask) {
            if (!j->have2d) {
                summary("  [UNCONFIRMED] the display driver does not report its 2D state - the "
                        "switches are stored but the panel cannot see them take effect");
            } else if (!(j->flags2d & VCR_2DS_F_ENGINE)) {
                summary("  [UNCONFIRMED] the 2D engine is not running on this display mode "
                        "(software drawing): the switches are stored and apply when it runs");
            } else if ((j->flags2d & j->want_2d_mask) == (j->want_2d_flags & j->want_2d_mask)) {
                summary("  [OK] the driver now reports text %s%s%s", (j->flags2d & VCR_2DS_F_TEXT)
                        ? "ON" : "off", j->size2d >= (int)sizeof(vcr_2d_stats)
                        ? ((j->flags2d & VCR_2DS_F_PAT) ? ", patterns ON" : ", patterns off") : "",
                        j->size2d >= (int)sizeof(vcr_2d_stats)
                        ? ((j->flags2d & VCR_2DS_F_LINE) ? ", lines ON" : ", lines off") : "");
            } else {
                summary("  [NOT CONFIRMED] the driver still reports 2D flags 0x%x (wanted 0x%x "
                        "of 0x%x): the new display surface did not pick them up - they take "
                        "effect at the next mode change (a game's) or boot", j->flags2d,
                        j->want_2d_flags, j->want_2d_mask);
            }
        }
    }
    g_busy = 0;
    reload();
    show_summary();
    if (g_close_after)
        EndDialog(g_main, IDOK);
}

/* ---- the page procedure ------------------------------------------------------------ */
static ROWSTATE *row_by_ctl(int id)
{
    int idx;
    if (id < IDC_ROW_BASE)
        return NULL;
    idx = (id - IDC_ROW_BASE) / 4;
    return idx < NR ? &R[idx] : NULL;
}

static INT_PTR CALLBACK PageProc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
    switch (m) {
    case WM_INITDIALOG:
        return FALSE;
    case WM_VSCROLL: {
        int i = page_index(h), pos, view = g_pagerc.bottom - g_pagerc.top;
        if (i < 0 || lp)                        /* lp: a child control's own scroll */
            return FALSE;
        pos = g_page_y[i];
        switch (LOWORD(wp)) {
        case SB_LINEUP:        pos -= 24; break;
        case SB_LINEDOWN:      pos += 24; break;
        case SB_PAGEUP:        pos -= view - 24; break;
        case SB_PAGEDOWN:      pos += view - 24; break;
        case SB_THUMBTRACK:
        case SB_THUMBPOSITION: pos = HIWORD(wp); break;
        case SB_TOP:           pos = 0; break;
        case SB_BOTTOM:        pos = g_page_h[i]; break;
        }
        page_scroll_to(h, pos);
        return TRUE;
    }
    case WM_MOUSEWHEEL: {
        int i = page_index(h);
        if (i >= 0)
            page_scroll_to(h, g_page_y[i] - GET_WHEEL_DELTA_WPARAM(wp) * 48 / WHEEL_DELTA);
        return TRUE;
    }
    case WM_CTLCOLORSTATIC: {
        int i;
        for (i = 0; i < g_ncs; i++)
            if (g_cs[i].h == (HWND)lp && g_cs[i].color != COL_NONE) {
                SetTextColor((HDC)wp, col_ref(g_cs[i].color));
                SetBkMode((HDC)wp, TRANSPARENT);
                if (G.themed)
                    return (INT_PTR)GetStockObject(NULL_BRUSH);
                SetBkColor((HDC)wp, GetSysColor(COLOR_BTNFACE));
                return (INT_PTR)GetSysColorBrush(COLOR_BTNFACE);
            }
        return FALSE;
    }
    case WM_NOTIFY: {
        NMHDR *n = (NMHDR *)lp;
        if (n->idFrom == IDC_AA_LIST && n->code == NM_CUSTOMDRAW) {
            NMLVCUSTOMDRAW *cd = (NMLVCUSTOMDRAW *)lp;
            if (cd->nmcd.dwDrawStage == CDDS_PREPAINT) {
                SetWindowLongPtrA(h, DWLP_MSGRESULT, CDRF_NOTIFYITEMDRAW);
                return TRUE;
            }
            if (cd->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) {
                switch ((int)cd->nmcd.lItemlParam) {
                case CTL_AA_VALIDATED:    cd->clrText = g_dark ? RGB(110, 230, 110) : RGB(16, 124, 16); break;
                case CTL_AA_UNTESTED:     cd->clrText = GetSysColor(COLOR_WINDOWTEXT); break;
                case CTL_AA_EXPERIMENTAL: cd->clrText = g_dark ? RGB(255, 180, 70) : RGB(176, 88, 0); break;
                default:                  cd->clrText = GetSysColor(COLOR_GRAYTEXT); break;
                }
                SetWindowLongPtrA(h, DWLP_MSGRESULT, CDRF_DODEFAULT);
                return TRUE;
            }
        }
        return FALSE;
    }
    case WM_HSCROLL: {
        ROWSTATE *s = row_by_ctl(GetDlgCtrlID((HWND)lp));
        if (s && (s->row->kind == CTL_K_FLOAT || s->row->kind == CTL_K_V_GAMMA)) {
            int pos = (int)SendMessageA(s->hctl, TBM_GETPOS, 0, 0);
            char b[16];
            const char *v;
            float f = pos / 100.0f;
            float_label(s, pos);
            if (s->row->kind == CTL_K_V_GAMMA) {
                _snprintf(b, sizeof b, "%.2f", f);
                b[sizeof b - 1] = 0;
                set_pend(s, b);
            } else {
                v = ctl_float_value(f, s->row->fdef, b);
                set_pend(s, v);
            }
            ui_update_buttons();
        }
        return FALSE;
    }
    case WM_COMMAND: {
        int id = LOWORD(wp), code = HIWORD(wp);
        ROWSTATE *s = row_by_ctl(id);
        if (s && code == CBN_SELCHANGE) {
            int sel = (int)SendMessageA(s->hctl, CB_GETCURSEL, 0, 0);
            int k = sel >= 0 ? (int)SendMessageA(s->hctl, CB_GETITEMDATA, sel, 0) : -1;
            if (k >= 0 && k < s->nitems) {
                const char *v = s->iabsent[k] ? NULL : s->ival[k];
                int exp = 0;
                if (s->row->kind == CTL_K_AA && v && ctl_aa_is_aa(atol(v)) &&
                    !(s->present && streq(v, s->cur)))
                    exp = 1;
                if (s->row->id == CTL_ID_V_SLIAA && v && atol(v) >= 6)
                    exp = 1;
                if (exp && !g_aa_confirmed) {
                    if (!aa_confirm(GetParent(h))) {
                        combo_select_pending(s);
                        return TRUE;
                    }
                    g_aa_confirmed = 1;
                }
                set_pend(s, v);
                ui_update_buttons();
            }
            return TRUE;
        }
        if (s && code == BN_CLICKED && s->row->kind == CTL_K_CHECK) {
            int on = IsDlgButtonChecked(h, id) == BST_CHECKED;
            set_pend(s, s->row->choices[on ? 1 : 0].value);
            ui_update_buttons();
            return TRUE;
        }
        if (s && code == EN_CHANGE && s->row->kind == CTL_K_V_INT) {
            char b[32];
            GetWindowTextA(s->hctl, b, sizeof b);
            if (!(s->pend_present && strcmp(b, s->pend) == 0)) {     /* a real edit only */
                set_pend(s, b);
                ui_update_buttons();
            }
            return TRUE;
        }
        switch (id) {
        case IDC_P_QUALITY:  preset_apply(CTL_PRESET_QUALITY); return TRUE;
        case IDC_P_SPEED:    preset_apply(CTL_PRESET_SPEED); return TRUE;
        case IDC_P_DEFAULTS: preset_apply(CTL_PRESET_DEFAULTS); return TRUE;
        case IDC_AA_ALLOW: {
            ROWSTATE *aa = rs_by_id(CTL_ID_AA);
            int on = IsDlgButtonChecked(h, IDC_AA_ALLOW) == BST_CHECKED;
            if (on && !g_allow_exp) {
                if (!g_aa_confirmed && !aa_confirm(GetParent(h))) {
                    CheckDlgButton(h, IDC_AA_ALLOW, BST_UNCHECKED);
                    return TRUE;
                }
                g_aa_confirmed = 1;
                g_allow_exp = 1;
            } else if (!on && g_allow_exp) {
                g_allow_exp = 0;
                /* AA no longer allowed: any AA choice goes back to SLI (disarms on Apply) */
                if (aa && aa->pend_present && ctl_aa_is_aa(atol(aa->pend))) {
                    char v[8];
                    _snprintf(v, sizeof v, "%ld", ctl_aa_sli_value(G.nchips));
                    set_pend(aa, v);
                }
            }
            if (aa && aa->hctl)
                aa_fill_combo(aa);
            ui_update_buttons();
            return TRUE;
        }
        case IDC_AA_DISARM: {
            char why[160];
            if (write_value(HKEY_LOCAL_MACHINE, CTL_KEY_DIAG, "SliAA", NULL, 1, why, sizeof why)) {
                ctl_log("Diag\\SliAA deleted (Disarm AA now) - read back absent");
                MessageBoxA(GetParent(h), "The kernel's AA switch is off (Diag\\SliAA deleted and "
                            "read back absent). Every anti-aliasing request is refused before "
                            "the driver writes anything; a game set to AA fails to open instead.",
                            APP_TITLE, MB_OK | MB_ICONINFORMATION);
            } else {
                ctl_log("Disarm AA FAILED: %s", why);
                MessageBoxA(GetParent(h), why, APP_TITLE " - could not disarm",
                            MB_OK | MB_ICONERROR);
            }
            PostMessageA(g_main, WM_APP_REBUILD, 1, 0);
            return TRUE;
        }
        case IDC_AA_SCAN: {
            char list[4096], msg[4400];
            int ung = 0;
            HCURSOR old = SetCursor(LoadCursor(NULL, IDC_WAIT));
            scan_glide_dirs(list, sizeof list, &ung);
            SetCursor(old);
            _snprintf(msg, sizeof msg, "Glide DLLs without our SLI/AA guard (system folder, "
                      "C:\\Games\\*, D:\\Games\\*):\r\n\r\n%s", list);
            msg[sizeof msg - 1] = 0;
            MessageBoxA(GetParent(h), msg, APP_TITLE, MB_OK |
                        (ung ? MB_ICONWARNING : MB_ICONINFORMATION));
            return TRUE;
        }
        case IDC_DISP_PROPS:
            WinExec("control.exe desk.cpl,,3", SW_SHOWNORMAL);
            return TRUE;
        case IDC_RM_OVERRIDE:
            g_rm_overrides = IsDlgButtonChecked(h, IDC_RM_OVERRIDE) == BST_CHECKED;
            ui_update_buttons();
            return TRUE;
        }
        return FALSE;
    }
    }
    return FALSE;
}

/* ---- the header ------------------------------------------------------------------------- */
static void header_draw(DRAWITEMSTRUCT *d)
{
    RECT rc = d->rcItem, band;
    HDC dc = d->hDC;
    int i, w = rc.right - rc.left, hgt = rc.bottom - rc.top;
    char title[160], sub[300], stat[400];
    int armed = G.lane == CTL_LANE_VCR && G.sliaa_present && G.sliaa;
    for (i = 0; i < 64; i++) {                          /* navy -> purple */
        int r = 16 + (58 - 16) * i / 63, g = 22 + (26 - 22) * i / 63, b = 48 + (92 - 48) * i / 63;
        HBRUSH br = CreateSolidBrush(RGB(r, g, b));
        band.left = rc.left + w * i / 64;
        band.right = rc.left + w * (i + 1) / 64;
        band.top = rc.top;
        band.bottom = rc.bottom - 3;
        FillRect(dc, &band, br);
        DeleteObject(br);
    }
    band = rc;
    band.top = rc.bottom - 3;
    {
        HBRUSH br = CreateSolidBrush(armed ? RGB(220, 40, 40) : RGB(242, 140, 40));
        FillRect(dc, &band, br);
        DeleteObject(br);
    }
    if (g_icon)
        DrawIconEx(dc, rc.left + 14, rc.top + (hgt - 48) / 2, g_icon, 48, 48, 0, NULL, DI_NORMAL);
    SetBkMode(dc, TRANSPARENT);
    if (G.lane == CTL_LANE_VCR) {
        unsigned mb = G.info.fb_per_chip >> 20, n = G.info.nchips;
        _snprintf(title, sizeof title, "%s", ctl_card_name(G.info.device, n));
        _snprintf(sub, sizeof sub, "%u x %s, %u MB each = %u MB%s  -  our stack: vcr-kmd, h5 "
                  "Glide, MesaFX OpenGL", n, G.info.device == 9 ? "VSA-100" : "chip", mb,
                  mb * n, (G.info.device == 9 && n == 4) ? (mb >= 64 ? " (the 256 MB VBIOS mode)"
                                                                     : " (the 128 MB VBIOS mode)")
                                                         : "");
        _snprintf(stat, sizeof stat, "Desktop %lux%lu %lu-bit @ %lu Hz    SLI: %s    AA: %s",
                  G.cur.dmPelsWidth, G.cur.dmPelsHeight, G.cur.dmBitsPerPel,
                  G.cur.dmDisplayFrequency,
                  G.info.sli_chips ? "active" : (G.have_dd && G.dd[1]) ? "Glide running" : "idle",
                  armed ? "EXPERIMENTAL AA ARMED" : "off (kernel switch safe)");
    } else if (G.lane == CTL_LANE_VINTAGE) {
        _snprintf(title, sizeof title, "%s", G.adapter[0] ? G.adapter : "3dfx");
        _snprintf(sub, sizeof sub, "vintage 3dfxvs driver%s", G.forced ? " (lane forced)" : "");
        _snprintf(stat, sizeof stat, "Desktop %lux%lu %lu-bit @ %lu Hz", G.cur.dmPelsWidth,
                  G.cur.dmPelsHeight, G.cur.dmBitsPerPel, G.cur.dmDisplayFrequency);
    } else {
        scpy(title, "No 3dfx driver active", sizeof title);
        _snprintf(sub, sizeof sub, "display: %s", G.adapter[0] ? G.adapter : "unknown");
        stat[0] = 0;
    }
    title[sizeof title - 1] = sub[sizeof sub - 1] = stat[sizeof stat - 1] = 0;
    band = rc;
    band.left += 76;
    band.top += 7;
    SelectObject(dc, g_big);
    SetTextColor(dc, RGB(255, 255, 255));
    DrawTextA(dc, title, -1, &band, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);
    band.top += 27;
    SelectObject(dc, g_font);
    SetTextColor(dc, RGB(200, 205, 230));
    DrawTextA(dc, sub, -1, &band, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    band.top += 17;
    SelectObject(dc, g_bold);
    SetTextColor(dc, armed ? RGB(255, 120, 110) : RGB(255, 200, 120));
    DrawTextA(dc, stat, -1, &band, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    {
        RECT vr = rc;
        vr.right -= 10;
        vr.top += 10;
        SelectObject(dc, g_small);
        SetTextColor(dc, RGB(160, 165, 200));
        DrawTextA(dc, "v" CTL_VERSION, -1, &vr, DT_RIGHT | DT_TOP | DT_SINGLELINE);
    }
}

/* ---- the main dialog --------------------------------------------------------------------- */
#define MAIN_W 660
#define MAIN_H 540
#define HEAD_H 76

static void tab_show(int sel)
{
    int i;
    for (i = 0; i < g_ntabs; i++)
        ShowWindow(g_page[i], i == sel ? SW_SHOW : SW_HIDE);
}

static INT_PTR CALLBACK MainProc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
    switch (m) {
    case WM_INITDIALOG: {
        RECT rc = { 0, 0, MAIN_W, MAIN_H };
        static const char *const names[CTL_NTABS] = { "Overview", "3D && Glide",
                                                      "Anti-aliasing && SLI", "OpenGL",
                                                      "Display && 2D", "Advanced" };
        TCITEMA ti;
        int t, i;
        g_main = h;
        AdjustWindowRectEx(&rc, GetWindowLongA(h, GWL_STYLE), FALSE, GetWindowLongA(h, GWL_EXSTYLE));
        SetWindowPos(h, NULL, 0, 0, rc.right - rc.left, rc.bottom - rc.top, SWP_NOMOVE |
                     SWP_NOZORDER);
        {
            RECT wr, wa;
            GetWindowRect(h, &wr);
            int x, y;
            SystemParametersInfoA(SPI_GETWORKAREA, 0, &wa, 0);
            /* centred; on a desktop smaller than the panel, its caption stays on screen */
            x = wa.left + ((wa.right - wa.left) - (wr.right - wr.left)) / 2;
            y = wa.top + ((wa.bottom - wa.top) - (wr.bottom - wr.top)) / 2;
            SetWindowPos(h, NULL, x > wa.left ? x : wa.left, y > wa.top ? y : wa.top, 0, 0,
                         SWP_NOSIZE | SWP_NOZORDER);
        }
        SendMessageA(h, WM_SETICON, ICON_BIG, (LPARAM)g_icon);
        SendMessageA(h, WM_SETICON, ICON_SMALL, (LPARAM)g_icon_small);
        g_header = mk(h, "STATIC", "", SS_OWNERDRAW, 0, 0, MAIN_W, HEAD_H, IDC_HEADER, NULL);
        g_tab = mk(h, WC_TABCONTROLA, "", WS_TABSTOP | WS_CLIPSIBLINGS, 10, HEAD_H + 8,
                   MAIN_W - 20, MAIN_H - HEAD_H - 56, IDC_TAB, NULL);
        g_tip = CreateWindowExA(WS_EX_TOPMOST, TOOLTIPS_CLASSA, NULL, WS_POPUP | TTS_ALWAYSTIP |
                                TTS_NOPREFIX, 0, 0, 0, 0, h, NULL, g_hi, NULL);
        SendMessageA(g_tip, TTM_SETMAXTIPWIDTH, 0, 380);
        SendMessageA(g_tip, TTM_SETDELAYTIME, TTDT_AUTOPOP, 30000);
        g_ntabs = 0;
        for (t = 0; t < CTL_NTABS; t++) {
            int has = t == CTL_TAB_OVERVIEW || t == CTL_TAB_ADV;
            for (i = 0; i < NR && !has; i++)
                if (R[i].row->tab == t)
                    has = 1;
            if (!has)
                continue;
            memset(&ti, 0, sizeof ti);
            ti.mask = TCIF_TEXT;
            ti.pszText = (LPSTR)names[t];
            SendMessageA(g_tab, TCM_INSERTITEMA, g_ntabs, (LPARAM)&ti);
            g_tabmap[g_ntabs++] = t;
        }
        GetWindowRect(g_tab, &g_pagerc);
        MapWindowPoints(NULL, h, (POINT *)&g_pagerc, 2);
        TabCtrl_AdjustRect(g_tab, FALSE, &g_pagerc);
        mk(h, "BUTTON", "Restore defaults", BS_PUSHBUTTON | WS_TABSTOP, 10, MAIN_H - 38, 130, 26,
           IDC_DEFAULTS, NULL);
        g_status = mk(h, "STATIC", "", SS_LEFT | SS_NOPREFIX | SS_ENDELLIPSIS, 150, MAIN_H - 32,
                      MAIN_W - 150 - 330, 16, IDC_STATUS, g_small);
        mk(h, "BUTTON", "OK", BS_DEFPUSHBUTTON | WS_TABSTOP, MAIN_W - 320, MAIN_H - 38, 96, 26,
           IDC_OK, NULL);
        mk(h, "BUTTON", "Cancel", BS_PUSHBUTTON | WS_TABSTOP, MAIN_W - 216, MAIN_H - 38, 96, 26,
           IDC_CANCEL, NULL);
        mk(h, "BUTTON", "&Apply", BS_PUSHBUTTON | WS_TABSTOP, MAIN_W - 112, MAIN_H - 38, 96, 26,
           IDC_APPLY, NULL);
        tip_add(h, GetDlgItem(h, IDC_DEFAULTS), "Fill every tab with the driver's own defaults "
                "(every value the panel manages removed). Nothing is written until Apply.");
        TabCtrl_SetCurSel(g_tab, 0);
        pages_build();
        SetTimer(h, TIMER_LIVE, 2000, NULL);
        ctl_log("panel %s started: lane %s, %s, Glide key HKLM\\%s", CTL_VERSION,
                G.lane == CTL_LANE_VCR ? "vcr-kmd" : G.lane == CTL_LANE_VINTAGE ? "vintage" : "none",
                G.adapter, G.glide_key);
        return TRUE;
    }
    case WM_DRAWITEM:
        if (wp == IDC_HEADER) {
            header_draw((DRAWITEMSTRUCT *)lp);
            return TRUE;
        }
        return FALSE;
    case WM_NOTIFY: {
        NMHDR *n = (NMHDR *)lp;
        if (n->idFrom == IDC_TAB && n->code == TCN_SELCHANGE)
            tab_show(TabCtrl_GetCurSel(g_tab));
        return FALSE;
    }
    case WM_TIMER:
        /* live status only while the panel is in front: no driver escapes at all
         * while a fullscreen game (or anything else) has the foreground */
        if (wp == TIMER_LIVE && !g_busy && G.lane == CTL_LANE_VCR &&
            GetForegroundWindow() == h) {
            char b[600];
            unsigned sli = G.info.sli_chips;
            ULONG ex = G.dd[1];
            DWORD hz = G.cur.dmDisplayFrequency;
            stack_live();
            if (sli != G.info.sli_chips || ex != G.dd[1] || hz != G.cur.dmDisplayFrequency)
                InvalidateRect(g_header, NULL, FALSE);
            if (g_aa_live) {
                char old[600];
                aa_live_text(b, sizeof b);
                GetWindowTextA(g_aa_live, old, sizeof old);
                if (strcmp(old, b) != 0) {
                    RECT rc;
                    SetWindowTextA(g_aa_live, b);
                    set_col(g_aa_live, G.sliaa_present && G.sliaa ? COL_RED : COL_GRAY);
                    GetWindowRect(g_aa_live, &rc);
                    MapWindowPoints(NULL, GetParent(g_aa_live), (POINT *)&rc, 2);
                    InvalidateRect(GetParent(g_aa_live), &rc, TRUE);
                }
            }
        }
        return TRUE;
    case WM_APP_DISPDONE:
        on_disp_done(wp, (DISPJOB *)lp);
        return TRUE;
    case WM_APP_REBUILD:
        if (wp) {
            reload();
        } else {
            pages_build();
        }
        return TRUE;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_APPLY:
            if (!g_busy && any_changed())
                apply_all();
            return TRUE;
        case IDC_OK:
            if (g_busy)
                return TRUE;
            if (any_changed()) {
                g_close_after = 1;
                apply_all();
            } else {
                EndDialog(h, IDOK);
            }
            return TRUE;
        case IDC_CANCEL:
        case IDCANCEL:
            if (!g_busy)
                EndDialog(h, IDCANCEL);
            return TRUE;
        case IDC_DEFAULTS:
            if (MessageBoxA(h, "Fill every tab with the driver's defaults? Every value this "
                            "panel manages will be removed when you press Apply (the desktop "
                            "refresh rate is left alone).", APP_TITLE,
                            MB_YESNO | MB_ICONQUESTION) == IDYES)
                preset_apply(CTL_PRESET_DEFAULTS);
            return TRUE;
        }
        return FALSE;
    case WM_CLOSE:
        if (!g_busy)
            EndDialog(h, IDCANCEL);
        return TRUE;
    case WM_DESTROY:
        KillTimer(h, TIMER_LIVE);
        return FALSE;
    }
    return FALSE;
}

/* ---- /report --------------------------------------------------------------------------- */
static int write_report(const char *path)
{
    FILE *f = fopen(path, "w");
    int i;
    if (!f)
        return 2;
    fprintf(f, "3dfx Control Panel %s report\n", CTL_VERSION);
    fprintf(f, "lane: %s%s\n", G.lane == CTL_LANE_VCR ? "vcr-kmd (our stack)"
            : G.lane == CTL_LANE_VINTAGE ? "vintage 3dfxvs" : "none", G.forced ? " (forced)" : "");
    fprintf(f, "adapter: %s\n", G.adapter);
    if (G.have_info)
        fprintf(f, "card: %s device %04x, %u chips (Glide told %u), %u MB per chip; desktop "
                "%ux%ux%u@%u; sli_chips %u; boot_good %u; mon_src %u; nmodes %u\n",
                ctl_card_name(G.info.device, G.info.nchips), G.info.device, G.info.nchips,
                G.info.glide_chips, G.info.fb_per_chip >> 20, G.info.cur_w, G.info.cur_h,
                G.info.cur_bpp, G.info.cur_hz, G.info.sli_chips, G.info.boot_good,
                G.info.mon_src, G.info.nmodes);
    if (G.have_dd)
        fprintf(f, "glide exclusive owner pid: %lu\n", G.dd[1]);
    if (G.have_2d)
        fprintf(f, "2d flags: 0x%x (engine %d text %d pattern %d line %d), report %d bytes\n",
                G.st2d.flags, !!(G.st2d.flags & VCR_2DS_F_ENGINE),
                !!(G.st2d.flags & VCR_2DS_F_TEXT), !!(G.st2d.flags & VCR_2DS_F_PAT),
                !!(G.st2d.flags & VCR_2DS_F_LINE), G.size_2d);
    fprintf(f, "desktop: current %lux%lux%lu@%lu, saved %lux%lux%lu@%lu\n", G.cur.dmPelsWidth,
            G.cur.dmPelsHeight, G.cur.dmBitsPerPel, G.cur.dmDisplayFrequency, G.reg.dmPelsWidth,
            G.reg.dmPelsHeight, G.reg.dmBitsPerPel, G.reg.dmDisplayFrequency);
    fprintf(f, "glide key: HKLM\\%s (3dfxvs\\Device0 %s)\n", G.glide_key,
            G.has_3dfxvs_dev0 ? "exists" : "absent");
    fprintf(f, "system glide3x.dll: %s, guard %d, trace %d\n",
            G.glide_present ? G.glide_dll : "(absent)", G.glide_guard, G.glide_trace);
    fprintf(f, "system ICD: %s %s\n", G.icd_present ? G.icd_dll : "(none)", G.icd_ver);
    fprintf(f, "splash plugin: %s\n", G.plugin_present ? G.plugin_path : "(absent)");
    fprintf(f, "Diag\\SliAA: %s\n", G.sliaa_present ? (G.sliaa ? "1 (ARMED)" : "0") : "absent");
    fprintf(f, "settings:\n");
    for (i = 0; i < NR; i++)
        fprintf(f, "  %-28s %-34s %s = %s%s%s\n", R[i].row->label, store_where(R[i].row->store),
                R[i].row->name[0] ? R[i].row->name : "(mode)",
                R[i].present ? R[i].cur : "(absent: the stack default)",
                R[i].note[0] ? "  NOTE: " : "", R[i].note);
    fprintf(f, "overrides found: %d\n", NOV);
    for (i = 0; i < NOV; i++)
        fprintf(f, "  %s\\%s %s = %s (%s)\n", OV[i].root == HKEY_CURRENT_USER ? "HKCU" : "HKLM",
                OV[i].path, OV[i].name, OV[i].value, OV[i].what);
    fclose(f);
    return 0;
}

/* ---- entry --------------------------------------------------------------------------------- */
int WINAPI WinMain(HINSTANCE hi, HINSTANCE hp, LPSTR cmd, int show)
{
    static WORD tmpl[256] __attribute__((aligned(4)));
    INITCOMMONCONTROLSEX ic;
    int force = 0;
    char *rep = NULL, *p;
    (void)hp;
    (void)show;
    g_hi = hi;
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    GetModuleFileNameA(NULL, g_logpath, sizeof g_logpath);
    if ((p = strrchr(g_logpath, '\\')) != NULL)
        strcpy(p + 1, "3dfxctl.log");
    else
        g_logpath[0] = 0;
    if (ci_strstr(cmd, "/vintage"))
        force = CTL_LANE_VINTAGE;
    else if (ci_strstr(cmd, "/vcr"))
        force = CTL_LANE_VCR;
    if ((p = (char *)ci_strstr(cmd, "/report")) != NULL) {
        p += 7;
        while (*p == ' ')
            p++;
        rep = p;
        if (*rep == '"') {
            char *q = strchr(++rep, '"');
            if (q)
                *q = 0;
        } else if ((p = strchr(rep, ' ')) != NULL) {
            *p = 0;
        }
        if (!*rep)
            rep = "3dfxctl-report.txt";
    }
    ic.dwSize = sizeof ic;
    ic.dwICC = ICC_STANDARD_CLASSES | ICC_TAB_CLASSES | ICC_BAR_CLASSES | ICC_LISTVIEW_CLASSES;
    InitCommonControlsEx(&ic);
    theme_init();
    stack_detect(force);
    rows_load();
    if (rep)
        return write_report(rep);
    g_icon = (HICON)LoadImageA(hi, MAKEINTRESOURCEA(1), IMAGE_ICON, 48, 48, 0);
    g_icon_small = (HICON)LoadImageA(hi, MAKEINTRESOURCEA(1), IMAGE_ICON, 16, 16, 0);
    g_font = CreateFontA(-11, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY,
                         0, "Tahoma");
    g_small = CreateFontA(-10, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
                          CLEARTYPE_QUALITY, 0, "Tahoma");
    g_bold = CreateFontA(-11, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY,
                         0, "Tahoma");
    g_big = CreateFontA(-20, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0,
                        "Tahoma");
    DialogBoxIndirectParamA(hi, dlg_template(tmpl, 256, WS_POPUP | WS_CAPTION | WS_SYSMENU |
                                             WS_MINIMIZEBOX | DS_MODALFRAME, WS_EX_APPWINDOW,
                                             300, 200, APP_TITLE),
                            NULL, MainProc, 0);
    return 0;
}
