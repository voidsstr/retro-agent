/* joy9x - joystick / gameport probe and configurator for Windows 9x.
 *
 *   joy9x [outfile]                report (default C:\RETRO_AGENT\JOY9X.TXT):
 *                                  what winmm (VJOYD) says about joysticks 1-2
 *                                  and a RAW read of the gameport at 201h
 *   joy9x watch <secs> [outfile]   log every change while someone moves the
 *                                  stick (default C:\RETRO_AGENT\JOYWATCH.TXT)
 *   joy9x types [outfile]          DirectInput's joystick types (JOYTYPES.TXT)
 *   joy9x set <type> [outfile]     make joystick 1 that type: "#3" is the
 *                                  predefined 2-axis 4-button stick, "#8"
 *                                  3-axis 4-button, an OEM key such as "CH_1"
 *                                  (CH Flightstick Pro) - see `types`
 *                                  (default C:\RETRO_AGENT\JOYSET.TXT)
 *   joy9x rest [outfile]           provisional X/Y calibration from where the
 *                                  stick rests now (nobody needs to touch it)
 *   joy9x cal <secs> [outfile]     calibrate joystick 1 from what the stick
 *                                  does for <secs>: the FIRST sample is the
 *                                  centre (hands off at the start), then the
 *                                  extremes of every axis that moved
 *                                  (default C:\RETRO_AGENT\JOYCAL.TXT)
 *
 * Written 2026-09-28 for .243 (Compaq Deskpro 2000, Win98 SE) when a Sound
 * Blaster 16 PnP was refitted to drive a gameport joystick. Windows installed
 * the card's gameport (ISAPNP\CTL0024_DEV0003, *CTL7001 -> "Gameport
 * Joystick", vjoyd.vxd) but no joystick TYPE was configured, and a type is
 * what winmm/DirectInput games need before they see joystick 1 at all. Nobody
 * at the keyboard needs to open Game Controllers for that, and a person at the
 * box can still recalibrate there any time.
 *
 * The raw read is how every DOS game reads the stick: write 201h to fire the
 * four one-shots, then time how long each axis bit stays high. An axis with
 * nothing connected never drops, so the timeout says which axes are wired -
 * the question "what kind of stick is this" without anyone having to answer
 * it. Buttons are bits 4-7, low = pressed. Only port 201h is ever written.
 *
 * No C runtime (see pci9x.c / usb9x.c). DINPUT.DLL is loaded at run time, not
 * imported, so the report still runs where DirectInput is missing. Build:
 *   i686-w64-mingw32-gcc -O1 -march=i586 -mwindows -nostdlib -e _start@0 \
 *       -o joy9x.exe joy9x.c -lkernel32 -luser32 -lwinmm -ladvapi32 -lgcc -s
 * (-lgcc: the static 64-bit divide the TSC arithmetic needs - still no CRT)
 */
#define DIRECTINPUT_VERSION 0x0500
#define COBJMACROS
#include <windows.h>
#include <mmsystem.h>
#include <dinput.h>
#include <dinputd.h>

static const GUID IID_JoyCfg =
    { 0x1de12ab1, 0xc9f5, 0x11cf, { 0xbf, 0xc7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 } };

#define GAMEPORT 0x201

static HANDLE g_out = INVALID_HANDLE_VALUE;
static char   g_line[768];

static void emit(const char *s)
{
    DWORD n;
    if (g_out == INVALID_HANDLE_VALUE) return;
    WriteFile(g_out, s, lstrlenA(s), &n, NULL);
    WriteFile(g_out, "\r\n", 2, &n, NULL);
    FlushFileBuffers(g_out);
}

static __inline__ unsigned char io_inb(unsigned short port)
{
    unsigned char v;
    __asm__ __volatile__("inb %w1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

/* The ONLY port this program ever writes: it fires the gameport one-shots. */
static __inline__ void gameport_fire(void)
{
    __asm__ __volatile__("outb %0, %w1" : : "a"((unsigned char)0xFF), "Nd"((unsigned short)GAMEPORT));
}

static __inline__ unsigned long long rdtsc(void)
{
    unsigned long lo, hi;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return ((unsigned long long)hi << 32) | lo;
}

static unsigned long g_mhz;     /* TSC ticks per microsecond */

static void calibrate_tsc(void)
{
    LARGE_INTEGER f, a, b;
    unsigned long long t0, t1;
    if (!QueryPerformanceFrequency(&f) || !f.QuadPart) { g_mhz = 166; return; }
    QueryPerformanceCounter(&a);
    t0 = rdtsc();
    Sleep(200);
    QueryPerformanceCounter(&b);
    t1 = rdtsc();
    {
        unsigned long long dq = (unsigned long long)(b.QuadPart - a.QuadPart);
        unsigned long long us = dq * 1000000ULL / (unsigned long long)f.QuadPart;
        g_mhz = us ? (unsigned long)((t1 - t0) / us) : 166;
        if (!g_mhz) g_mhz = 1;
    }
}

/* One raw read: per-axis one-shot time in microseconds (-1 = never dropped
 * within the limit, i.e. nothing connected), and the button nibble. */
typedef struct { long us[4]; unsigned char buttons; unsigned char idle; } rawjoy;

#define AXIS_LIMIT_US 8000

static void raw_read(rawjoy *r)
{
    unsigned long long t0, lim, now;
    unsigned char v, pending = 0x0F;
    int i;
    r->idle = io_inb(GAMEPORT);
    r->buttons = (unsigned char)(~r->idle >> 4) & 0x0F;   /* 1 = pressed */
    for (i = 0; i < 4; i++) r->us[i] = -1;
    lim = (unsigned long long)AXIS_LIMIT_US * g_mhz;
    gameport_fire();
    t0 = rdtsc();
    do {
        v = io_inb(GAMEPORT);
        now = rdtsc() - t0;
        for (i = 0; i < 4; i++)
            if ((pending & (1 << i)) && !(v & (1 << i))) {
                pending &= (unsigned char)~(1 << i);
                r->us[i] = (long)(now / g_mhz);
            }
    } while (pending && now < lim);
}

/* Several reads, median per axis - a ring-3 timing loop gets interrupted. */
static void raw_median(rawjoy *out, int n)
{
    rawjoy s[9];
    int i, a, j, k;
    if (n > 9) n = 9;
    for (i = 0; i < n; i++) { raw_read(&s[i]); Sleep(2); }
    *out = s[n - 1];
    for (a = 0; a < 4; a++) {
        long v[9];
        for (i = 0; i < n; i++) v[i] = s[i].us[a];
        for (j = 1; j < n; j++)
            for (k = j; k > 0 && v[k - 1] > v[k]; k--) { long t = v[k]; v[k] = v[k - 1]; v[k - 1] = t; }
        out->us[a] = v[n / 2];
    }
}

static const char *axisname[4] = { "joyA-X", "joyA-Y", "joyB-X", "joyB-Y" };

static void raw_report(void)
{
    rawjoy r;
    int a, wired = 0;
    raw_median(&r, 7);
    wsprintfA(g_line, "gameport 201h raw: idle byte %02X (bits 0-3 axes, 4-7 buttons low=pressed), "
              "TSC %lu MHz", r.idle, g_mhz);
    emit(g_line);
    if (r.idle == 0xFF && r.us[0] == -1 && r.us[1] == -1 && r.us[2] == -1 && r.us[3] == -1)
        emit("  NOTHING answers at 201h - no stick connected, or the port is not enabled");
    for (a = 0; a < 4; a++) {
        if (r.us[a] < 0)
            wsprintfA(g_line, "  axis %d %-7s NOT CONNECTED (bit stayed high %d us)", a, axisname[a], AXIS_LIMIT_US);
        else {
            wsprintfA(g_line, "  axis %d %-7s %ld us", a, axisname[a], r.us[a]);
            wired++;
        }
        emit(g_line);
    }
    wsprintfA(g_line, "  buttons pressed now: A1=%u A2=%u B1=%u B2=%u", r.buttons & 1,
              (r.buttons >> 1) & 1, (r.buttons >> 2) & 1, (r.buttons >> 3) & 1);
    emit(g_line);
    wsprintfA(g_line, "  => %d axis(es) wired%s", wired,
              wired == 0 ? "" : wired == 2 ? " - a 2-axis stick (X/Y)"
            : wired == 3 ? " - 3 axes: a stick with a throttle (or rudder) on joyB"
            : wired == 4 ? " - 4 axes: stick + throttle + rudder/hat" : "");
    emit(g_line);
}

static const char *mmerr(MMRESULT r)
{
    switch (r) {
    case JOYERR_NOERROR:   return "OK";
    case JOYERR_PARMS:     return "JOYERR_PARMS (bad id - no such joystick configured)";
    case JOYERR_NOCANDO:   return "JOYERR_NOCANDO (driver cannot poll)";
    case JOYERR_UNPLUGGED: return "JOYERR_UNPLUGGED (configured but not answering)";
    case MMSYSERR_NODRIVER:return "MMSYSERR_NODRIVER (no joystick driver)";
    case MMSYSERR_BADDEVICEID: return "MMSYSERR_BADDEVICEID";
    default:               return "error";
    }
}

static void winmm_report(void)
{
    UINT n = joyGetNumDevs(), id;
    wsprintfA(g_line, "winmm: joyGetNumDevs=%u", n);
    emit(g_line);
    for (id = 0; id < 2 && id < n; id++) {
        JOYCAPSA c;
        JOYINFOEX ji;
        MMRESULT r = joyGetDevCapsA(id, &c, sizeof(c));
        if (r != JOYERR_NOERROR) {
            wsprintfA(g_line, "  joystick %u: caps %u %s", id + 1, r, mmerr(r));
            emit(g_line);
        } else {
            wsprintfA(g_line, "  joystick %u: \"%s\" axes %u/%u buttons %u/%u caps %04X "
                      "(Z=%u R=%u U=%u V=%u POV=%u) X %u-%u Y %u-%u", id + 1, c.szPname,
                      c.wNumAxes, c.wMaxAxes, c.wNumButtons, c.wMaxButtons, c.wCaps,
                      (c.wCaps & JOYCAPS_HASZ) != 0, (c.wCaps & JOYCAPS_HASR) != 0,
                      (c.wCaps & JOYCAPS_HASU) != 0, (c.wCaps & JOYCAPS_HASV) != 0,
                      (c.wCaps & JOYCAPS_HASPOV) != 0, c.wXmin, c.wXmax, c.wYmin, c.wYmax);
            emit(g_line);
        }
        ZeroMemory(&ji, sizeof(ji));
        ji.dwSize = sizeof(ji);
        ji.dwFlags = JOY_RETURNALL;
        r = joyGetPosEx(id, &ji);
        if (r == JOYERR_NOERROR)
            wsprintfA(g_line, "    position: X=%lu Y=%lu Z=%lu R=%lu U=%lu V=%lu buttons=%08lX pov=%lu",
                      ji.dwXpos, ji.dwYpos, ji.dwZpos, ji.dwRpos, ji.dwUpos, ji.dwVpos,
                      ji.dwButtons, ji.dwPOV);
        else
            wsprintfA(g_line, "    position: %u %s", r, mmerr(r));
        emit(g_line);
    }
}

/* ---- DirectInput joystick configuration ---------------------------------- */

typedef HRESULT (WINAPI *DICREATE)(HINSTANCE, DWORD, LPDIRECTINPUTA *, LPUNKNOWN);

static LPDIRECTINPUTA         g_di;
static IDirectInputJoyConfig *g_jc;
static HWND                   g_hwnd;

static int jc_open(void)
{
    HMODULE h = LoadLibraryA("DINPUT.DLL");
    DICREATE create;
    HRESULT hr;
    if (!h) { emit("DINPUT.DLL not present - DirectInput is not installed"); return 0; }
    create = (DICREATE)GetProcAddress(h, "DirectInputCreateA");
    if (!create) { emit("DirectInputCreateA not exported"); return 0; }
    hr = create(GetModuleHandleA(NULL), DIRECTINPUT_VERSION, &g_di, NULL);
    if (FAILED(hr)) { wsprintfA(g_line, "DirectInputCreateA failed %08lX", (unsigned long)hr); emit(g_line); return 0; }
    hr = g_di->lpVtbl->QueryInterface(g_di, &IID_JoyCfg, (void **)&g_jc);
    if (FAILED(hr)) { wsprintfA(g_line, "no IDirectInputJoyConfig: %08lX", (unsigned long)hr); emit(g_line); return 0; }
    g_hwnd = CreateWindowExA(0, "STATIC", "joy9x", WS_POPUP, 0, 0, 1, 1, NULL, NULL,
                             GetModuleHandleA(NULL), NULL);
    hr = IDirectInputJoyConfig_SetCooperativeLevel(g_jc, g_hwnd, DISCL_EXCLUSIVE | DISCL_BACKGROUND);
    if (FAILED(hr)) { wsprintfA(g_line, "SetCooperativeLevel failed %08lX", (unsigned long)hr); emit(g_line); return 0; }
    hr = IDirectInputJoyConfig_Acquire(g_jc);
    if (FAILED(hr)) { wsprintfA(g_line, "Acquire failed %08lX (another program holds the joystick config?)", (unsigned long)hr); emit(g_line); return 0; }
    return 1;
}

static void jc_close(void)
{
    if (g_jc) {
        IDirectInputJoyConfig_Unacquire(g_jc);
        IDirectInputJoyConfig_Release(g_jc);
    }
    if (g_di) g_di->lpVtbl->Release(g_di);
    if (g_hwnd) DestroyWindow(g_hwnd);
}

static void w2a(const WCHAR *w, char *a, int cap)
{
    int i;
    for (i = 0; i < cap - 1 && w[i]; i++) a[i] = (char)(w[i] < 128 ? w[i] : '?');
    a[i] = 0;
}

/* Win9x's kernel32 has almost no W functions, and ONE missing import stops
 * the whole program loading - so no lstrcpynW, a loop instead. */
static void wcopy(WCHAR *d, const WCHAR *s, int cap)
{
    int i;
    for (i = 0; i < cap - 1 && s[i]; i++) d[i] = s[i];
    d[i] = 0;
}

/* joyConfigChanged is resolved at run time for the same reason. */
static void notify_winmm(void)
{
    typedef MMRESULT (WINAPI *JCC)(DWORD);
    HMODULE h = GetModuleHandleA("WINMM.DLL");
    JCC f = h ? (JCC)GetProcAddress(h, "joyConfigChanged") : NULL;
    if (f) {
        wsprintfA(g_line, "joyConfigChanged = %u", (unsigned)f(0));
        emit(g_line);
    } else
        emit("joyConfigChanged not exported here - VJOYD rereads the config on its next poll");
}

static void a2w(const char *a, WCHAR *w, int cap)
{
    int i;
    for (i = 0; i < cap - 1 && a[i]; i++) w[i] = (WCHAR)(unsigned char)a[i];
    w[i] = 0;
}

static BOOL CALLBACK type_cb(LPCWSTR name, LPVOID ctx)
{
    DIJOYTYPEINFO_DX5 ti;
    char n[128], d[260], c[260];
    (void)ctx;
    ZeroMemory(&ti, sizeof(ti));
    ti.dwSize = sizeof(ti);
    w2a(name, n, sizeof(n));
    if (SUCCEEDED(IDirectInputJoyConfig_GetTypeInfo(g_jc, name, (LPDIJOYTYPEINFO)&ti,
                                                   DITC_REGHWSETTINGS | DITC_DISPLAYNAME | DITC_CALLOUT))) {
        w2a(ti.wszDisplayName, d, sizeof(d));
        w2a(ti.wszCallout, c, sizeof(c));
        wsprintfA(g_line, "  %-34s \"%s\" flags %08lX buttons %lu%s%s", n, d,
                  (unsigned long)ti.hws.dwFlags, (unsigned long)ti.hws.dwNumButtons,
                  c[0] ? " callout " : "", c);
    } else
        wsprintfA(g_line, "  %-34s (no type info)", n);
    emit(g_line);
    return DIENUM_CONTINUE;
}

static void show_config(const char *when)
{
    DIJOYCONFIG_DX5 jc;
    HRESULT hr;
    char t[260], c[260];
    ZeroMemory(&jc, sizeof(jc));
    jc.dwSize = sizeof(jc);
    hr = IDirectInputJoyConfig_GetConfig(g_jc, 0, (LPDIJOYCONFIG)&jc,
                                         DIJC_REGHWCONFIGTYPE | DIJC_CALLOUT | DIJC_GAIN);
    if (FAILED(hr) || hr == S_FALSE) {
        wsprintfA(g_line, "joystick 1 config %s: none (%08lX)", when, (unsigned long)hr);
        emit(g_line);
        return;
    }
    w2a(jc.wszType, t, sizeof(t));
    w2a(jc.wszCallout, c, sizeof(c));
    wsprintfA(g_line, "joystick 1 config %s: type \"%s\" dwType %lu usage %08lX flags %08lX buttons %lu "
              "callout \"%s\" calflags %08lX", when, t, (unsigned long)jc.hwc.dwType,
              (unsigned long)jc.hwc.dwUsageSettings, (unsigned long)jc.hwc.hws.dwFlags,
              (unsigned long)jc.hwc.hws.dwNumButtons, c, (unsigned long)jc.hwc.hwv.dwCalFlags);
    emit(g_line);
    wsprintfA(g_line, "  range X %lu..%lu c%lu  Y %lu..%lu c%lu  Z %lu..%lu  R %lu..%lu",
              (unsigned long)jc.hwc.hwv.jrvHardware.jpMin.dwX, (unsigned long)jc.hwc.hwv.jrvHardware.jpMax.dwX,
              (unsigned long)jc.hwc.hwv.jrvHardware.jpCenter.dwX,
              (unsigned long)jc.hwc.hwv.jrvHardware.jpMin.dwY, (unsigned long)jc.hwc.hwv.jrvHardware.jpMax.dwY,
              (unsigned long)jc.hwc.hwv.jrvHardware.jpCenter.dwY,
              (unsigned long)jc.hwc.hwv.jrvHardware.jpMin.dwZ, (unsigned long)jc.hwc.hwv.jrvHardware.jpMax.dwZ,
              (unsigned long)jc.hwc.hwv.jrvHardware.jpMin.dwR, (unsigned long)jc.hwc.hwv.jrvHardware.jpMax.dwR);
    emit(g_line);
}

static int do_set(const char *type)
{
    DIJOYTYPEINFO_DX5 ti;
    DIJOYCONFIG_DX5 jc;
    WCHAR wt[128];
    HRESULT hr;
    int predefined = type[0] == '#';

    a2w(type, wt, 128);
    ZeroMemory(&ti, sizeof(ti));
    ti.dwSize = sizeof(ti);
    hr = IDirectInputJoyConfig_GetTypeInfo(g_jc, wt, (LPDIJOYTYPEINFO)&ti,
                                           DITC_REGHWSETTINGS | DITC_DISPLAYNAME | DITC_CALLOUT);
    if (FAILED(hr)) {
        wsprintfA(g_line, "unknown type \"%s\" (%08lX) - run `joy9x types`", type, (unsigned long)hr);
        emit(g_line);
        return 3;
    }
    show_config("before");
    ZeroMemory(&jc, sizeof(jc));
    jc.dwSize = sizeof(jc);
    jc.hwc.hws = ti.hws;
    jc.hwc.dwUsageSettings = JOY_US_PRESENT;
    /* A predefined type carries its JOY_HW_* number in its name ("#3" = 3);
     * an OEM type is JOY_HW_CUSTOM and is found by its key name. */
    jc.hwc.dwType = predefined ? (DWORD)(type[1] - '0') * (type[2] ? 10 : 1) + (type[2] ? (DWORD)(type[2] - '0') : 0)
                               : JOY_HW_CUSTOM;
    /* Uncalibrated ranges wide enough for any analog stick VJOYD reads; the
     * `cal` mode (or Game Controllers) replaces them with measured ones. */
    jc.hwc.hwv.jrvHardware.jpMin.dwX = jc.hwc.hwv.jrvHardware.jpMin.dwY = 0;
    jc.hwc.hwv.jrvHardware.jpMin.dwZ = jc.hwc.hwv.jrvHardware.jpMin.dwR = 0;
    jc.hwc.hwv.jrvHardware.jpMax.dwX = jc.hwc.hwv.jrvHardware.jpMax.dwY = 1600;
    jc.hwc.hwv.jrvHardware.jpMax.dwZ = jc.hwc.hwv.jrvHardware.jpMax.dwR = 1600;
    jc.hwc.hwv.jrvHardware.jpCenter.dwX = jc.hwc.hwv.jrvHardware.jpCenter.dwY = 800;
    jc.hwc.hwv.jrvHardware.jpCenter.dwZ = jc.hwc.hwv.jrvHardware.jpCenter.dwR = 800;
    jc.dwGain = 10000;
    wcopy(jc.wszType, wt, 256);
    wcopy(jc.wszCallout, ti.wszCallout, 256);
    hr = IDirectInputJoyConfig_SetConfig(g_jc, 0, (LPCDIJOYCONFIG)&jc,
                                         DIJC_REGHWCONFIGTYPE | DIJC_CALLOUT | DIJC_GAIN);
    wsprintfA(g_line, "SetConfig(joystick 1, \"%s\") = %08lX", type, (unsigned long)hr);
    emit(g_line);
    if (FAILED(hr)) return 4;
    hr = IDirectInputJoyConfig_SendNotify(g_jc);
    wsprintfA(g_line, "SendNotify = %08lX", (unsigned long)hr);
    emit(g_line);
    show_config("after");
    return 0;
}

/* ---- watch / calibrate --------------------------------------------------- */

static DWORD axv(const JOYINFOEX *j, int a)
{
    switch (a) {
    case 0: return j->dwXpos; case 1: return j->dwYpos; case 2: return j->dwZpos;
    default: return j->dwRpos;
    }
}

static int watch(int secs, int calibrate)
{
    DWORD end = GetTickCount() + (DWORD)secs * 1000, start = GetTickCount(), lastbeat = 0;
    JOYINFOEX j, first;
    rawjoy r, lastr;
    DWORD lo[4], hi[4], lastw[4], lastbtn = 0xFFFFFFFF, v, btnseen = 0;
    DWORD rlo[2] = { 0xFFFFFFFF, 0xFFFFFFFF }, rhi[2] = { 0, 0 }, rsum[2] = { 0, 0 }, rn = 0;
    int a, have = 0, moved[4] = { 0, 0, 0, 0 };
    ZeroMemory(&lastr, sizeof(lastr));
    for (a = 0; a < 4; a++) { lo[a] = 0xFFFFFFFF; hi[a] = 0; lastw[a] = 0; }
    emit(calibrate ? "calibrating: hands off the stick for the first second, then move every axis "
                     "to its limits (stick in full circles, throttle/rudder end to end)"
                   : "watching: move the stick, press the buttons");
    while ((long)(end - GetTickCount()) > 0) {
        MMRESULT m;
        ZeroMemory(&j, sizeof(j));
        j.dwSize = sizeof(j);
        j.dwFlags = JOY_RETURNALL | (calibrate ? JOY_RETURNRAWDATA : 0);
        m = joyGetPosEx(0, &j);
        raw_read(&r);
        if (m == JOYERR_NOERROR) {
            if (!have) { first = j; have = 1; }
            for (a = 0; a < 4; a++) {
                v = axv(&j, a);
                if (v < lo[a]) lo[a] = v;
                if (v > hi[a]) hi[a] = v;
            }
            btnseen |= j.dwButtons;
            /* The first 1.5 s: if the stick RESTS there, that is its centre. */
            if (GetTickCount() - start < 1500) {
                for (a = 0; a < 2; a++) {
                    v = axv(&j, a);
                    if (v < rlo[a]) rlo[a] = v;
                    if (v > rhi[a]) rhi[a] = v;
                    rsum[a] += v;
                }
                rn++;
            }
        }
        {
            int changed = (m == JOYERR_NOERROR && j.dwButtons != lastbtn) || r.buttons != lastr.buttons;
            for (a = 0; a < 4 && !changed; a++) {
                long d = r.us[a] - lastr.us[a];
                if ((r.us[a] < 0) != (lastr.us[a] < 0) || d > 60 || d < -60) changed = 1;
                if (m == JOYERR_NOERROR) {
                    long dw = (long)axv(&j, a) - (long)lastw[a];
                    if (dw > 3000 || dw < -3000) changed = 1;
                }
            }
            if (changed || GetTickCount() - lastbeat >= 5000) {
                wsprintfA(g_line, "t=%5lu ms raw us %ld %ld %ld %ld btn %X | winmm %s X=%lu Y=%lu Z=%lu R=%lu btn=%lX",
                          GetTickCount() - start, r.us[0], r.us[1], r.us[2], r.us[3], r.buttons,
                          m == JOYERR_NOERROR ? "ok" : mmerr(m), j.dwXpos, j.dwYpos, j.dwZpos, j.dwRpos,
                          j.dwButtons);
                emit(g_line);
                lastr = r;
                if (m == JOYERR_NOERROR) { lastbtn = j.dwButtons; for (a = 0; a < 4; a++) lastw[a] = axv(&j, a); }
                lastbeat = GetTickCount();
            }
        }
        Sleep(40);
    }
    for (a = 0; a < 4; a++)
        if (have && hi[a] > lo[a] && hi[a] - lo[a] > 200) moved[a] = 1;
    wsprintfA(g_line, "winmm range seen: X %lu..%lu  Y %lu..%lu  Z %lu..%lu  R %lu..%lu  buttons pressed: %08lX",
              lo[0], hi[0], lo[1], hi[1], lo[2], hi[2], lo[3], hi[3], btnseen);
    emit(g_line);
    if (!calibrate) return 0;
    if (!have) { emit("no winmm reading at all - set a joystick type first (joy9x set #3)"); return 5; }
    if (!moved[0] || !moved[1]) { emit("X and Y did not both move through a range - NOT calibrated"); return 6; }
    {
        DIJOYCONFIG_DX5 jc;
        HRESULT hr;
        ZeroMemory(&jc, sizeof(jc));
        jc.dwSize = sizeof(jc);
        hr = IDirectInputJoyConfig_GetConfig(g_jc, 0, (LPDIJOYCONFIG)&jc,
                                             DIJC_REGHWCONFIGTYPE | DIJC_CALLOUT | DIJC_GAIN);
        if (FAILED(hr)) { wsprintfA(g_line, "GetConfig failed %08lX", (unsigned long)hr); emit(g_line); return 7; }
        jc.hwc.hwv.jrvHardware.jpMin.dwX = lo[0]; jc.hwc.hwv.jrvHardware.jpMax.dwX = hi[0];
        jc.hwc.hwv.jrvHardware.jpMin.dwY = lo[1]; jc.hwc.hwv.jrvHardware.jpMax.dwY = hi[1];
        /* Centre: the resting value if the stick sat still for the first
         * 1.5 s (a spread under 4% of the travel), else the middle of the
         * travel - a person may already be moving it when this starts. */
        for (a = 0; a < 2; a++) {
            DWORD span = hi[a] - lo[a], c;
            int rested = rn >= 5 && rhi[a] - rlo[a] <= span / 25;
            c = rested ? rsum[a] / rn : lo[a] + span / 2;
            if (a == 0) jc.hwc.hwv.jrvHardware.jpCenter.dwX = c;
            else        jc.hwc.hwv.jrvHardware.jpCenter.dwY = c;
            wsprintfA(g_line, "centre %s = %lu (%s)", a ? "Y" : "X", c,
                      rested ? "resting value, first 1.5 s" : "middle of the travel - it was moving at the start");
            emit(g_line);
        }
        (void)first;
        jc.hwc.hwv.dwCalFlags |= JOY_ISCAL_XY;
        if (moved[2]) {
            jc.hwc.hwv.jrvHardware.jpMin.dwZ = lo[2]; jc.hwc.hwv.jrvHardware.jpMax.dwZ = hi[2];
            jc.hwc.hwv.jrvHardware.jpCenter.dwZ = (lo[2] + hi[2]) / 2;
            jc.hwc.hwv.dwCalFlags |= JOY_ISCAL_Z;
        }
        if (moved[3]) {
            jc.hwc.hwv.jrvHardware.jpMin.dwR = lo[3]; jc.hwc.hwv.jrvHardware.jpMax.dwR = hi[3];
            jc.hwc.hwv.jrvHardware.jpCenter.dwR = (lo[3] + hi[3]) / 2;
            jc.hwc.hwv.dwCalFlags |= JOY_ISCAL_R;
        }
        hr = IDirectInputJoyConfig_SetConfig(g_jc, 0, (LPCDIJOYCONFIG)&jc,
                                             DIJC_REGHWCONFIGTYPE | DIJC_CALLOUT | DIJC_GAIN);
        wsprintfA(g_line, "calibration saved: SetConfig = %08lX, SendNotify = %08lX", (unsigned long)hr,
                  (unsigned long)IDirectInputJoyConfig_SendNotify(g_jc));
        emit(g_line);
        show_config("after calibration");
        return FAILED(hr) ? 8 : 0;
    }
}

/* Provisional calibration with nobody at the stick: the centre is where X/Y
 * rest now, the travel is assumed symmetric about it (an analog pot's timing
 * is roughly proportional to resistance, so full travel is about twice the
 * centre). Without it the uncalibrated ranges put a centred stick at ~13% -
 * "held up and left" in every Windows game. Z (a throttle) is left alone: its
 * resting place says nothing about its ends. Game Controllers -> Calibrate
 * (or `joy9x cal`) replaces all of it with measured values. */
static int rest_cal(void)
{
    JOYINFOEX j;
    DIJOYCONFIG_DX5 jc;
    HRESULT hr;
    DWORD sum[2] = { 0, 0 }, lo[2] = { 0xFFFFFFFF, 0xFFFFFFFF }, hi[2] = { 0, 0 }, n = 0, c, v;
    int i, a;
    for (i = 0; i < 25; i++) {
        ZeroMemory(&j, sizeof(j));
        j.dwSize = sizeof(j);
        j.dwFlags = JOY_RETURNALL | JOY_RETURNRAWDATA;
        if (joyGetPosEx(0, &j) == JOYERR_NOERROR) {
            for (a = 0; a < 2; a++) {
                v = a ? j.dwYpos : j.dwXpos;
                sum[a] += v;
                if (v < lo[a]) lo[a] = v;
                if (v > hi[a]) hi[a] = v;
            }
            n++;
        }
        Sleep(40);
    }
    if (n < 10) { emit("no winmm reading - set a joystick type first (joy9x set #8)"); return 5; }
    for (a = 0; a < 2; a++)
        if (hi[a] - lo[a] > sum[a] / n / 10) {
            wsprintfA(g_line, "the stick is MOVING (axis %d %lu..%lu) - leave it centred and run again", a, lo[a], hi[a]);
            emit(g_line);
            return 6;
        }
    ZeroMemory(&jc, sizeof(jc));
    jc.dwSize = sizeof(jc);
    hr = IDirectInputJoyConfig_GetConfig(g_jc, 0, (LPDIJOYCONFIG)&jc, DIJC_REGHWCONFIGTYPE | DIJC_CALLOUT | DIJC_GAIN);
    if (FAILED(hr)) { wsprintfA(g_line, "GetConfig failed %08lX", (unsigned long)hr); emit(g_line); return 7; }
    c = sum[0] / n;
    jc.hwc.hwv.jrvHardware.jpCenter.dwX = c;
    jc.hwc.hwv.jrvHardware.jpMin.dwX = c / 10;
    jc.hwc.hwv.jrvHardware.jpMax.dwX = 2 * c - c / 10;
    c = sum[1] / n;
    jc.hwc.hwv.jrvHardware.jpCenter.dwY = c;
    jc.hwc.hwv.jrvHardware.jpMin.dwY = c / 10;
    jc.hwc.hwv.jrvHardware.jpMax.dwY = 2 * c - c / 10;
    jc.hwc.hwv.dwCalFlags |= JOY_ISCAL_XY;
    hr = IDirectInputJoyConfig_SetConfig(g_jc, 0, (LPCDIJOYCONFIG)&jc, DIJC_REGHWCONFIGTYPE | DIJC_CALLOUT | DIJC_GAIN);
    wsprintfA(g_line, "provisional X/Y calibration (centre = resting value, symmetric travel): SetConfig = %08lX, SendNotify = %08lX",
              (unsigned long)hr, (unsigned long)IDirectInputJoyConfig_SendNotify(g_jc));
    emit(g_line);
    show_config("after rest calibration");
    return FAILED(hr) ? 8 : 0;
}

/* ---- entry ---------------------------------------------------------------- */

static int next_arg(const char **p, char *out, int cap)
{
    const char *s = *p;
    int n = 0, q = 0;
    while (*s == ' ' || *s == '\t') s++;
    if (!*s) { *p = s; return 0; }
    if (*s == '"') { q = 1; s++; }
    while (*s && (q ? *s != '"' : (*s != ' ' && *s != '\t'))) {
        if (n < cap - 1) out[n++] = *s;
        s++;
    }
    if (q && *s == '"') s++;
    out[n] = 0;
    *p = s;
    return 1;
}

static long to_num(const char *s)
{
    long v = 0;
    while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
    return v;
}

void WINAPI _start(void)
{
    const char *cmd = GetCommandLineA();
    char exe[MAX_PATH], a1[128], a2[128], a3[MAX_PATH];
    const char *out = "C:\\RETRO_AGENT\\JOY9X.TXT";
    int rc = 0, mode = 0, secs = 0;

    next_arg(&cmd, exe, sizeof(exe));
    a1[0] = a2[0] = a3[0] = 0;
    next_arg(&cmd, a1, sizeof(a1));
    next_arg(&cmd, a2, sizeof(a2));
    next_arg(&cmd, a3, sizeof(a3));
    if (!lstrcmpiA(a1, "watch"))      { mode = 1; secs = (int)to_num(a2); out = a3[0] ? a3 : "C:\\RETRO_AGENT\\JOYWATCH.TXT"; }
    else if (!lstrcmpiA(a1, "types")) { mode = 2; out = a2[0] ? a2 : "C:\\RETRO_AGENT\\JOYTYPES.TXT"; }
    else if (!lstrcmpiA(a1, "set"))   { mode = 3; out = a3[0] ? a3 : "C:\\RETRO_AGENT\\JOYSET.TXT"; }
    else if (!lstrcmpiA(a1, "cal"))   { mode = 4; secs = (int)to_num(a2); out = a3[0] ? a3 : "C:\\RETRO_AGENT\\JOYCAL.TXT"; }
    else if (!lstrcmpiA(a1, "rest"))  { mode = 5; out = a2[0] ? a2 : "C:\\RETRO_AGENT\\JOYCAL.TXT"; }
    else if (a1[0])                   out = a1;
    if ((mode == 1 || mode == 4) && secs <= 0) secs = 20;
    if (secs > 300) secs = 300;

    g_out = CreateFileA(out, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    calibrate_tsc();
    emit("joy9x - gameport joystick probe/configurator");
    switch (mode) {
    case 0:
        raw_report();
        winmm_report();
        if (jc_open()) { show_config("now"); jc_close(); }
        break;
    case 1:
        winmm_report();
        rc = watch(secs, 0);
        break;
    case 2:
        if (!jc_open()) { rc = 2; break; }
        emit("DirectInput joystick types (name, display name, hardware flags, buttons):");
        IDirectInputJoyConfig_EnumTypes(g_jc, (LPVOID)type_cb, NULL);
        show_config("now");
        jc_close();
        break;
    case 3:
        if (!a2[0]) { emit("usage: joy9x set <type>  (e.g. #3, #8, CH_1)"); rc = 1; break; }
        if (!jc_open()) { rc = 2; break; }
        rc = do_set(a2);
        jc_close();
        if (!rc) { notify_winmm(); winmm_report(); }
        break;
    case 4:
        if (!jc_open()) { rc = 2; break; }
        rc = watch(secs, 1);
        jc_close();
        if (!rc) { notify_winmm(); winmm_report(); }
        break;
    case 5:
        if (!jc_open()) { rc = 2; break; }
        rc = rest_cal();
        jc_close();
        if (!rc) { notify_winmm(); winmm_report(); }
        break;
    }
    wsprintfA(g_line, "done rc=%d", rc);
    emit(g_line);
    if (g_out != INVALID_HANDLE_VALUE) CloseHandle(g_out);
    ExitProcess((UINT)rc);
}
