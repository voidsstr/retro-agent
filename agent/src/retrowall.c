/*
 * retrowall.c - Ensure the retro "dossier" wallpaper rotation is applied and the
 * desktop icons are parked in the blank icon well, every time the agent starts.
 *
 * The per-machine wallpaper BMPs and the two helper exes are staged onto the
 * machine ahead of time by the server-side retro-wallpaper skill
 * (deploy_rotation.py) into C:\retro-wall\ :
 *
 *     C:\retro-wall\wall00.bmp .. wallNN.bmp   (the iterations)
 *     C:\retro-wall\rotate_wall.exe            (GUI, single-instance, cycles them)
 *     C:\retro-wall\arrange_icons.exe          (LEGACY bottom-right arranger - staged
 *                                              but deliberately NOT run; see below)
 *
 * The agent cannot render those BMPs itself (they encode per-machine specs), so
 * this module only *applies* what has been staged. If nothing is staged yet it
 * quietly does nothing. When the rotation is present it, on every startup:
 *
 *   1. makes sure the wallpaper style is set (stretch, no tile),
 *   2. applies a retro wallpaper immediately if the current one isn't ours
 *      ("set them if they are not already set"),
 *   3. (re)installs the HKCU Run key so the rotator survives logon,
 *   4. launches the rotator (its named mutex dedupes, so this is a no-op if it's
 *      already running), and
 *   5. does NOT arrange icons - gs_arrange_icons() owns that, and the legacy
 *      exe would fight it by parking them bottom-right.
 *
 * Runs in a background thread after a short delay so the shell/desktop is up
 * (the icon listview lives in explorer, and SPI_SETDESKWALLPAPER needs the
 * interactive session). The agent normally runs from an HKLM Run key under
 * autologon, i.e. in the user's session, so these calls reach the visible
 * desktop. In NT service mode (session 0) they are harmless no-ops.
 */

#include <windows.h>
#include <stdio.h>
#include <string.h>

#include "handlers.h"
#include "util.h"
#include "log.h"

#define WALLDIR        "C:\\retro-wall"
#define ROTATE_EXE     WALLDIR "\\rotate_wall.exe"
#define ARRANGE_EXE    WALLDIR "\\arrange_icons.exe"
#define THEME_REG      WALLDIR "\\retro_theme.reg"
#define SETCOLORS_EXE  WALLDIR "\\setsyscolors.exe"
#define WALL0_BMP      WALLDIR "\\wall00.bmp"
#define RUN_KEY        "Software\\Microsoft\\Windows\\CurrentVersion\\Run"
#define RUN_VALUE      "RetroWallRotate"
#define DESKTOP_KEY    "Control Panel\\Desktop"

/* Let the shell/desktop finish coming up before we touch it. */
#define RETROWALL_DELAY_SEC  20
/* How often to check that the fleet wallpaper is still the wallpaper. */
#define RETROWALL_KEEP_SEC   300

#ifndef SPI_SETDESKWALLPAPER
#define SPI_SETDESKWALLPAPER 0x0014
#endif

static int file_exists(const char *path)
{
    return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
}

/* Set a REG_SZ under HKCU\<subkey>. */
static void hkcu_set_sz(const char *subkey, const char *name, const char *value)
{
    HKEY h;
    if (RegCreateKeyExA(HKEY_CURRENT_USER, subkey, 0, NULL, 0,
                        KEY_SET_VALUE, NULL, &h, NULL) == ERROR_SUCCESS) {
        RegSetValueExA(h, name, 0, REG_SZ,
                       (const BYTE *)value, (DWORD)(strlen(value) + 1));
        RegCloseKey(h);
    }
}

/* Read a REG_SZ from HKCU\<subkey>. Returns 0 on success. */
static int hkcu_get_sz(const char *subkey, const char *name, char *buf, DWORD bufsize)
{
    HKEY h;
    DWORD type, size = bufsize;
    LONG rc;

    if (RegOpenKeyExA(HKEY_CURRENT_USER, subkey, 0, KEY_QUERY_VALUE, &h)
            != ERROR_SUCCESS)
        return -1;
    rc = RegQueryValueExA(h, name, NULL, &type, (BYTE *)buf, &size);
    RegCloseKey(h);
    if (rc != ERROR_SUCCESS || type != REG_SZ)
        return -1;
    buf[bufsize - 1] = '\0';
    return 0;
}

/* Launch a process. If wait_ms > 0, wait up to that long for it to exit. */
static void run_process(const char *cmdline, DWORD wait_ms)
{
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    char cmd[512];

    safe_strncpy(cmd, cmdline, sizeof(cmd));  /* CreateProcess may modify the buffer */

    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    memset(&pi, 0, sizeof(pi));

    if (CreateProcessA(NULL, cmd, NULL, NULL, FALSE,
                       CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        if (wait_ms)
            WaitForSingleObject(pi.hProcess, wait_ms);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    } else {
        log_msg(LOG_MAIN, "retrowall: failed to launch \"%s\" (%lu)",
                cmdline, (unsigned long)GetLastError());
    }
}

/*
 * Stop the Themes service and set it to Disabled.
 *
 * XP's visual style ("Luna") is applied by that service. Setting ThemeActive=0
 * in the registry is necessary but not sufficient - while the service runs it
 * keeps the style alive, and the result is a machine themed in patches: our
 * colours in window bodies, XP blue on title bars and Explorer's task panes.
 *
 * Failure is tolerated at every step. A machine that keeps Luna is cosmetically
 * wrong; a machine where the agent aborted trying to change a service setting
 * is actually broken.
 */
/* True on Windows XP / 2003 and older. Anything from Vista (6.0) up drives its
 * desktop compositor from the same Themes service that XP uses for Luna, so the
 * two need opposite treatment. */
static int os_is_xp_or_older(void)
{
    OSVERSIONINFOA osvi;
    memset(&osvi, 0, sizeof(osvi));
    osvi.dwOSVersionInfoSize = sizeof(osvi);
    if (!GetVersionExA(&osvi))
        return 1;               /* cannot tell - assume the fleet's XP default */
    return osvi.dwMajorVersion < 6;
}

static void stop_and_disable_themes(void)
{
    SC_HANDLE scm, svc;
    SERVICE_STATUS st;

    scm = OpenSCManagerA(NULL, NULL, SC_MANAGER_CONNECT);
    if (!scm)
        return;
    svc = OpenServiceA(scm, "Themes",
                       SERVICE_STOP | SERVICE_QUERY_STATUS | SERVICE_CHANGE_CONFIG);
    if (!svc) {
        /* Not present on 9x, and on some XP builds it is called differently.
         * Nothing to do either way. */
        CloseServiceHandle(scm);
        return;
    }
    if (QueryServiceStatus(svc, &st) && st.dwCurrentState != SERVICE_STOPPED) {
        if (ControlService(svc, SERVICE_CONTROL_STOP, &st))
            log_msg(LOG_MAIN, "retrowall: stopped the Themes service (Luna off)");
        else
            log_msg(LOG_MAIN, "retrowall: could not stop Themes (%lu) - the "
                              "desktop may stay part-themed", GetLastError());
        Sleep(1500);
    }
    /* Disabled, not Manual: on Manual something else can start it again and the
     * box silently reverts to half-themed after a reboot. */
    if (ChangeServiceConfigA(svc, SERVICE_NO_CHANGE, SERVICE_DISABLED,
                             SERVICE_NO_CHANGE, NULL, NULL, NULL, NULL,
                             NULL, NULL, NULL))
        log_msg(LOG_MAIN, "retrowall: Themes service set to Disabled");
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
}


/*
 * Apply the fleet-wide "hacker" theme: switch off the XP "Luna" visual style
 * (-> Windows Classic, so windows honor the system colors, incl. the black
 * folder-view/Window background) and set a green-on-black system color scheme.
 * Done directly with Win32 (no staged assets needed), both live (SetSysColors +
 * a best-effort uxtheme visual-style switch) and persisted (HKCU) so it survives
 * the next logon. This is the DEFACTO retro-fleet desktop theme.
 */
#ifndef WM_THEMECHANGED
#define WM_THEMECHANGED 0x031A
#endif
#ifndef COLOR_HOTLIGHT
#define COLOR_HOTLIGHT 26
#endif
#ifndef COLOR_MENUHILIGHT
#define COLOR_MENUHILIGHT 29
#endif

/* uxtheme.dll (XP+) SetSystemVisualStyle, private ordinal 65: an empty style
 * filename selects the Classic look. Best-effort — if the ordinal differs on
 * a given build the registry ThemeActive=0 below still makes it Classic on
 * the next logon, and the classic colors apply live regardless. */
typedef HRESULT (WINAPI *SetSystemVisualStyle_t)(LPCWSTR, LPCWSTR, LPCWSTR,
                                                 DWORD);

/* Green-on-black "hacker" scheme: {registry name, COLOR_ index, R,G,B}.
 * Matches scripts/retro-wallpaper/retro_theme.reg so the agent-applied theme
 * and the server-side deploy_rotation theme are identical. Window=0,0,0 makes
 * Explorer's folder-view background black in Classic mode. */
static const struct {
    const char *reg_name;
    int idx;
    int r, g, b;
} HACKER_COLORS[] = {
    {"Scrollbar",             COLOR_SCROLLBAR,                 18,  22,  18},
    {"Background",            COLOR_BACKGROUND,                 0,   0,   0},
    {"ActiveTitle",          COLOR_ACTIVECAPTION,              0,  28,   0},
    {"InactiveTitle",        COLOR_INACTIVECAPTION,            8,  12,   8},
    {"Menu",                 COLOR_MENU,                       0,   0,   0},
    {"Window",               COLOR_WINDOW,                     0,   0,   0},
    {"WindowFrame",          COLOR_WINDOWFRAME,                0,  80,   0},
    {"MenuText",             COLOR_MENUTEXT,                   0, 224,   0},
    {"WindowText",           COLOR_WINDOWTEXT,                 0, 230,   0},
    {"TitleText",            COLOR_CAPTIONTEXT,                0, 255,   0},
    {"ActiveBorder",         COLOR_ACTIVEBORDER,               0,  60,   0},
    {"InactiveBorder",       COLOR_INACTIVEBORDER,            10,  14,  10},
    {"AppWorkspace",         COLOR_APPWORKSPACE,               0,   0,   0},
    {"Hilight",              COLOR_HIGHLIGHT,                  0, 112,   0},
    {"HilightText",          COLOR_HIGHLIGHTTEXT,              0, 255,   0},
    {"ButtonFace",           COLOR_BTNFACE,                   18,  22,  18},
    {"ButtonShadow",         COLOR_BTNSHADOW,                  0,  40,   0},
    {"GrayText",             COLOR_GRAYTEXT,                   0, 100,   0},
    {"ButtonText",           COLOR_BTNTEXT,                    0, 224,   0},
    {"InactiveTitleText",    COLOR_INACTIVECAPTIONTEXT,        0, 120,   0},
    {"ButtonHilight",        COLOR_BTNHIGHLIGHT,               0,  90,   0},
    {"ButtonDkShadow",       COLOR_3DDKSHADOW,                 0,  24,   0},
    {"ButtonLight",          COLOR_3DLIGHT,                    0,  52,   0},
    {"InfoText",             COLOR_INFOTEXT,                   0, 224,   0},
    {"InfoWindow",           COLOR_INFOBK,                     0,   0,   0},
    {"GradientActiveTitle",  COLOR_GRADIENTACTIVECAPTION,      0,  52,   0},
    {"GradientInactiveTitle",COLOR_GRADIENTINACTIVECAPTION,   10,  14,  10},
    {"HotTrackingColor",     COLOR_HOTLIGHT,                   0, 200,   0},
    {"MenuHilight",          COLOR_MENUHILIGHT,                0, 112,   0},
};

static void apply_hacker_theme(void)
{
    int n = (int)(sizeof(HACKER_COLORS) / sizeof(HACKER_COLORS[0]));
    int idx[64];
    COLORREF rgb[64];
    int i;

    /* 1. Persist the green scheme to HKCU\Control Panel\Colors and build the
     *    live SetSysColors arrays. */
    for (i = 0; i < n && i < 64; i++) {
        char val[32];
        _snprintf(val, sizeof(val), "%d %d %d",
                  HACKER_COLORS[i].r, HACKER_COLORS[i].g, HACKER_COLORS[i].b);
        hkcu_set_sz("Control Panel\\Colors", HACKER_COLORS[i].reg_name, val);
        idx[i] = HACKER_COLORS[i].idx;
        rgb[i] = RGB(HACKER_COLORS[i].r, HACKER_COLORS[i].g,
                     HACKER_COLORS[i].b);
    }
    /* 2. Apply the colors live (SetSysColors broadcasts WM_SYSCOLORCHANGE). */
    SetSysColors(n < 64 ? n : 64, idx, rgb);

    /* 3. Turn off the Luna visual style -> Classic. Persist for next logon. */
    hkcu_set_sz("Software\\Microsoft\\Windows\\CurrentVersion\\ThemeManager",
                "ThemeActive", "0");
    /* The registry value alone does NOT switch Luna off. The Themes service
     * re-applies the visual style, so a machine ends up half-themed: system
     * colours black-and-green as we asked, but blue XP title bars and blue
     * Explorer task panes. Seen on the Gateway - the ThemeManager key did not
     * even exist there, while the service ran happily.
     *
     * Stopping the service is what actually drops XP to Classic, and disabling
     * it is what makes that survive a reboot. */
    /* XP ONLY. On XP the visual style is "Luna", and switching it off is what
     * gives the fleet its flat Classic look - without it the box ends up themed
     * in patches: our colours in window bodies, XP blue on title bars and
     * Explorer's task panes.
     *
     * On Vista and later the SAME service drives Aero, and stopping it does not
     * produce Classic-with-our-colours - it strips the compositor and leaves a
     * machine looking broken. .246 is a Windows 7 box on this fleet and got
     * exactly that. The fleet look is an XP-era aesthetic; newer machines keep
     * their own chrome and take the wallpaper and icons only. */
    if (os_is_xp_or_older())
        stop_and_disable_themes();
    else
        log_msg(LOG_MAIN, "retrowall: not XP - leaving the visual style alone "
                          "(stopping Themes strips Aero on Vista and later)");
    {
        HMODULE ux = LoadLibraryA("uxtheme.dll");
        if (ux) {
            SetSystemVisualStyle_t fn =
                (SetSystemVisualStyle_t)GetProcAddress(ux, (LPCSTR)65);
            if (fn) {
                HRESULT hr = fn(L"", L"", L"", 0);
                log_msg(LOG_MAIN,
                        "retrowall: SetSystemVisualStyle(classic) hr=0x%lx",
                        (unsigned long)hr);
            }
            FreeLibrary(ux);
        }
    }
    /* 4. Nudge running apps to repaint in the new (classic) style + colors. */
    SendMessageTimeoutA(HWND_BROADCAST, WM_THEMECHANGED, 0, 0,
                        SMTO_ABORTIFHUNG, 2000, NULL);
    SendMessageTimeoutA(HWND_BROADCAST, WM_SYSCOLORCHANGE, 0, 0,
                        SMTO_ABORTIFHUNG, 2000, NULL);
    log_msg(LOG_MAIN, "retrowall: applied green-on-black hacker theme "
                      "(Luna off + green colors)");
}

/*
 * Set the desktop screensaver to Starfield (the fleet look). Prefer a copy
 * staged at C:\retro-wall\ssstars.scr (so it works on Win7, which ships no
 * ssstars.scr, and needs no system32 write), else the in-box XP one. Persisted
 * to HKCU + applied live via SPI. No-op if neither .scr is present.
 */
#ifndef SPI_SETSCREENSAVEACTIVE
#define SPI_SETSCREENSAVEACTIVE 0x0011
#endif
#ifndef SPI_SETSCREENSAVETIMEOUT
#define SPI_SETSCREENSAVETIMEOUT 0x000F
#endif

static void set_starfield_screensaver(void)
{
    char scr[MAX_PATH];
    char sys[MAX_PATH];

    scr[0] = '\0';
    if (file_exists(WALLDIR "\\ssstars.scr")) {
        safe_strncpy(scr, WALLDIR "\\ssstars.scr", sizeof(scr));
    } else {
        UINT len = GetSystemDirectoryA(sys, sizeof(sys));
        if (len > 0 && len < sizeof(sys)) {
            _snprintf(scr, sizeof(scr), "%s\\ssstars.scr", sys);
            if (!file_exists(scr))
                scr[0] = '\0';
        }
    }
    if (!scr[0]) {
        log_msg(LOG_MAIN, "retrowall: no ssstars.scr found, screensaver unset");
        return;
    }
    hkcu_set_sz(DESKTOP_KEY, "SCRNSAVE.EXE", scr);
    hkcu_set_sz(DESKTOP_KEY, "ScreenSaveActive", "1");
    hkcu_set_sz(DESKTOP_KEY, "ScreenSaveTimeOut", "600");     /* 10 minutes */
    SystemParametersInfoA(SPI_SETSCREENSAVEACTIVE, TRUE, NULL,
                          SPIF_UPDATEINIFILE | SPIF_SENDWININICHANGE);
    SystemParametersInfoA(SPI_SETSCREENSAVETIMEOUT, 600, NULL,
                          SPIF_UPDATEINIFILE | SPIF_SENDWININICHANGE);
    log_msg(LOG_MAIN, "retrowall: screensaver set to Starfield (%s)", scr);
}

/*
 * Apply the fleet wallpaper that matches this screen's resolution.
 *
 * SEPARATE from the older "rotation" below, which is a per-machine set of
 * dossier BMPs staged by a server-side skill and named wall00.bmp upward. The
 * fleet wallpaper is generated by scripts/retro-wallpaper/gen_retro_wall.py,
 * ships inside the image, and is named for the resolution it was drawn at -
 * because its icon bay is laid out in pixels and a bay drawn for 1024x768 is
 * simply wrong on an 800x600 screen.
 *
 * I staged these into the image and did NOT wire this up at first: retrowall
 * only ever looked for wall00.bmp, so the files sat on disk and the desktop
 * stayed black. Exactly the same shape of mistake as the 3dfx drivers being
 * staged but left out of the driver path.
 *
 * Falls back to the nearest smaller width if there is no exact match, so an odd
 * resolution gets a bay that fits on screen rather than one running off it.
 */
/* Stop the legacy wallpaper rotation and stop it coming back.
 *
 * Only called once a fleet wallpaper has been applied, so a box that still
 * depends on the rotation is never touched. */
static void stop_wallpaper_rotation(void)
{
    HKEY  k;
    DWORD n = 0;

    /* The running instance. taskkill rather than a handle-based kill: it is a
     * separate GUI process we did not start and may not own. */
    run_process("cmd.exe /c taskkill /f /im rotate_wall.exe", 10000);
    n++;

    /* ...and the Run key that would start a fresh one at the next logon. */
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE,
                      "Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                      0, KEY_SET_VALUE, &k) == ERROR_SUCCESS) {
        if (RegDeleteValueA(k, "RetroWallRotate") == ERROR_SUCCESS) {
            log_msg(LOG_MAIN, "retrowall: removed the RetroWallRotate Run key");
            n++;
        }
        RegCloseKey(k);
    }
    if (n)
        log_msg(LOG_MAIN, "retrowall: legacy wallpaper rotation stopped");
}

/* Returns 1 if a fleet wallpaper was found and applied. The caller uses that
 * to decide whether the older rotation should run at all. */
/* The fleet wallpaper this agent applied, or "" if none. The keeper loop below
 * needs it, and it is also the flag that says the fleet path (rather than the
 * legacy rotation) is in charge of this desktop. */
static char g_fleet_wall[MAX_PATH];

static int apply_fleet_wallpaper(void)
{
    static const struct { int w, h; } SIZES[] = {
        { 1920, 1080 }, { 1600, 1200 }, { 1440, 900 }, { 1280, 1024 },
        { 1280, 800 }, { 1024, 768 }, { 800, 600 },
    };
    char path[MAX_PATH];
    char best[MAX_PATH];
    int  sw = GetSystemMetrics(SM_CXSCREEN);
    int  sh = GetSystemMetrics(SM_CYSCREEN);
    unsigned i;

    best[0] = '\0';

    /* Exact match first. */
    for (i = 0; i < sizeof(SIZES) / sizeof(SIZES[0]); i++) {
        if (SIZES[i].w != sw || SIZES[i].h != sh)
            continue;
        _snprintf(path, sizeof(path), "%s\\retrowall_%dx%d.bmp",
                  WALLDIR, SIZES[i].w, SIZES[i].h);
        if (file_exists(path)) {
            safe_strncpy(best, path, sizeof(best));
            break;
        }
    }
    /* Otherwise the largest that still fits, so the bay stays on screen. */
    if (!best[0]) {
        for (i = 0; i < sizeof(SIZES) / sizeof(SIZES[0]); i++) {
            if (SIZES[i].w > sw || SIZES[i].h > sh)
                continue;
            _snprintf(path, sizeof(path), "%s\\retrowall_%dx%d.bmp",
                      WALLDIR, SIZES[i].w, SIZES[i].h);
            if (file_exists(path)) {
                safe_strncpy(best, path, sizeof(best));
                break;
            }
        }
    }
    if (!best[0]) {
        log_msg(LOG_MAIN, "retrowall: no fleet wallpaper for %dx%d in %s",
                sw, sh, WALLDIR);
        return 0;
    }

    /* Centred, not stretched: the bay is drawn in exact pixels and stretching
     * it would put the icons out of their slots. */
    hkcu_set_sz(DESKTOP_KEY, "WallpaperStyle", "0");
    hkcu_set_sz(DESKTOP_KEY, "TileWallpaper", "0");
    hkcu_set_sz(DESKTOP_KEY, "Wallpaper", best);
    SystemParametersInfoA(SPI_SETDESKWALLPAPER, 0, best,
                          SPIF_UPDATEINIFILE | SPIF_SENDWININICHANGE);
    log_msg(LOG_MAIN, "retrowall: wallpaper set to %s (screen %dx%d)", best, sw, sh);
    safe_strncpy(g_fleet_wall, best, sizeof(g_fleet_wall));
    return 1;
}

/*
 * Put the fleet wallpaper back if something took it away.
 *
 * WHY THIS EXISTS. .246 runs a copy of Windows 7 that is not activated, and
 * Windows' "Notification mode" enforcement BLANKS THE DESKTOP - it clears
 * HKCU\Control Panel\Desktop\Wallpaper to an empty string and paints black,
 * on its own schedule, roughly hourly. The agent applied the wallpaper
 * correctly at startup, logged that it had, and the desktop was black again
 * within the hour with nothing in the log to say why. A game that takes an
 * exclusive fullscreen mode and exits badly can do the same thing.
 *
 * So applying it once is not enough on a box like that: it has to be kept.
 * This is a registry read every five minutes - free even on a Pentium 1 - and
 * it only ever acts when the value no longer names OUR file, so a person who
 * deliberately sets a different wallpaper from C:\retro-wall is not fought.
 *
 * Returns 1 if it had to repair.
 */
static int keep_fleet_wallpaper(void)
{
    char cur[MAX_PATH];

    if (!g_fleet_wall[0])
        return 0;                       /* no fleet wallpaper in charge here */
    if (hkcu_get_sz(DESKTOP_KEY, "Wallpaper", cur, sizeof(cur)) == 0 &&
        lstrcmpiA(cur, g_fleet_wall) == 0)
        return 0;                       /* still ours */

    hkcu_set_sz(DESKTOP_KEY, "WallpaperStyle", "0");
    hkcu_set_sz(DESKTOP_KEY, "TileWallpaper", "0");
    hkcu_set_sz(DESKTOP_KEY, "Wallpaper", g_fleet_wall);
    SystemParametersInfoA(SPI_SETDESKWALLPAPER, 0, g_fleet_wall,
                          SPIF_UPDATEINIFILE | SPIF_SENDWININICHANGE);
    log_msg(LOG_MAIN, "retrowall: wallpaper had been changed to \"%s\" - "
                      "put %s back", cur[0] ? cur : "(none)", g_fleet_wall);
    return 1;
}


/*
 * Apply the staged wallpaper rotation + icon layout. Safe to call every startup;
 * a no-op when nothing is staged.
 */
void retrowall_apply_startup(void)
{
    char runcmd[512];
    char curwall[MAX_PATH];

    /* The THEME and the SCREENSAVER need nothing staged - the theme is registry
     * plus a colour call, and the screensaver falls back to XP's own
     * system32\ssstars.scr. Only the wallpaper ROTATION needs C:\retro-wall.
     *
     * They used to sit behind the same early return, so a freshly imaged
     * machine - which by definition has no rotation staged yet - got no theme
     * and no screensaver either, silently. Apply what does not depend on the
     * staged assets first, then return if the rotation is genuinely absent. */
    apply_hacker_theme();
    set_starfield_screensaver();

    /* The FLEET wallpaper wins over the old rotation.
     *
     * apply_fleet_wallpaper() picks retrowall_<W>x<H>.bmp to match the screen -
     * the image every newly-provisioned machine gets. The older rotation
     * (wall00..NN.bmp driven by rotate_wall.exe) predates it and is still
     * staged on hand-built boxes, where it started up and replaced the fleet
     * wallpaper seconds later: two machines sat on wall02.bmp and wall07.bmp
     * while every imaged box showed the new one, and nothing in the log said
     * why, because both steps had done exactly what they were told.
     *
     * So when a fleet wallpaper is present it is applied and the rotation is
     * left alone. The rotation is still there for a box that has no fleet
     * wallpaper staged. */
    if (apply_fleet_wallpaper()) {
        /* Declining to START the rotation is not enough. An instance left over
         * from a previous boot keeps running and re-sets the wallpaper every
         * interval, and its Run key starts a fresh one at the next logon - so
         * the fleet wallpaper was applied, logged as applied, and then quietly
         * replaced by wall04.bmp seconds later. Stop the process and remove the
         * Run key, or the change does not survive the minute it was made in. */
        stop_wallpaper_rotation();
        log_msg(LOG_MAIN, "retrowall: fleet wallpaper applied; older rotation "
                          "stopped so it cannot replace it");
        return;
    }

    if (!file_exists(ROTATE_EXE)) {
        log_msg(LOG_MAIN, "retrowall: theme + screensaver applied; no wallpaper "
                          "rotation staged (%s missing)", ROTATE_EXE);
        return;
    }

    log_msg(LOG_MAIN, "retrowall: applying staged wallpaper rotation + icon layout");

    /* 1. Wallpaper style: stretch to fill (2), no tiling. */
    hkcu_set_sz(DESKTOP_KEY, "WallpaperStyle", "2");
    hkcu_set_sz(DESKTOP_KEY, "TileWallpaper", "0");

    /* 2. If the current wallpaper isn't one of ours, apply wall00 immediately so
     *    it's set even before the rotator's first tick. The rotator then advances
     *    it on its interval. */
    if (file_exists(WALL0_BMP)) {
        int need = 1;
        if (hkcu_get_sz(DESKTOP_KEY, "Wallpaper", curwall, sizeof(curwall)) == 0) {
            if (_strnicmp(curwall, WALLDIR "\\", (int)strlen(WALLDIR) + 1) == 0)
                need = 0;  /* already a retro-wall wallpaper */
        }
        if (need) {
            SystemParametersInfoA(SPI_SETDESKWALLPAPER, 0, (PVOID)WALL0_BMP,
                                  SPIF_UPDATEINIFILE | SPIF_SENDWININICHANGE);
            log_msg(LOG_MAIN, "retrowall: applied %s (was \"%s\")",
                    WALL0_BMP, curwall[0] ? curwall : "(none)");
        }
    }

    /* 3. Persist the rotator across logon. Preserve an existing command (it
     *    carries the chosen interval); otherwise install a default. */
    if (hkcu_get_sz(RUN_KEY, RUN_VALUE, runcmd, sizeof(runcmd)) != 0 || !runcmd[0]) {
        _snprintf(runcmd, sizeof(runcmd), "%s 60", ROTATE_EXE);
        hkcu_set_sz(RUN_KEY, RUN_VALUE, runcmd);
        log_msg(LOG_MAIN, "retrowall: installed Run key (%s)", runcmd);
    }

    /* 4. Ensure the rotator is running (its named mutex makes this idempotent). */
    run_process(runcmd, 0);

    /* 5. Park the desktop icons in the blank well. */
    /* DO NOT run arrange_icons.exe here.
     *
     * It parks icons in the BOTTOM-RIGHT well, which is where the wallpaper
     * used to reserve space. The wallpaper now draws its icon bay TOP-LEFT and
     * the agent parks them there itself in gs_arrange_icons() - which also
     * clears LVS_EX_SNAPTOGRID (v1.67.0) and widens into extra columns when the
     * library outgrows the bay (v1.68.0), neither of which the old exe does.
     *
     * Running it here therefore UNDID a correct arrangement on every single
     * agent start. That is the worst shape a bug can take: each manual fix
     * appeared to work and was silently reverted at the next boot, so the
     * defect looked like "the icons keep moving" rather than anything
     * attributable. The deployed binary is byte-identical to the repo's
     * bottom-right arranger and its own printf still says "moved %d icons to
     * bottom-right well".
     *
     * The agent has done this natively since it grew gs_arrange_icons(), so
     * there is nothing to replace it with - the call simply goes away. Staging
     * the file remains harmless; only running it was wrong. */
    if (file_exists(ARRANGE_EXE))
        log_msg(LOG_MAIN, "retrowall: %s present but NOT run - the agent "
                          "arranges icons itself (top-left bay)", ARRANGE_EXE);

    /* The theme and screensaver were applied at the top of this function, before
     * the rotation check, so that a machine with nothing staged still gets them.
     * Both are idempotent, but calling them twice per startup is just noise. */
}

DWORD WINAPI retrowall_thread(LPVOID param)
{
    (void)param;
    Sleep(RETROWALL_DELAY_SEC * 1000);
    if (!g_running)
        return 0;
    retrowall_apply_startup();

    /* Then keep it. See keep_fleet_wallpaper() - an unactivated Windows blanks
     * the desktop on its own schedule, so "applied at startup" is not the same
     * as "applied". Sleep in short slices so a QUIT is not held up by this. */
    while (g_running) {
        int i;
        for (i = 0; i < RETROWALL_KEEP_SEC && g_running; i++)
            Sleep(1000);
        if (!g_running)
            break;
        keep_fleet_wallpaper();
    }
    return 0;
}
