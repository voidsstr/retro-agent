/* 3dfxctl - retro3dfx driver Control Panel (Win32, Windows 2000/XP).
 *
 * A user-facing settings panel for the VINTAGE 3dfx Voodoo driver stack.  Every
 * setting is a persistent registry value the driver reads at boot / mode-set /
 * app-launch, so changes survive restarts and can be changed again here.  This
 * is the deliberate, sane path for writing these values (unlike a game that
 * leaves an overbright gamma ramp behind).
 *
 * The driver has THREE reader paths, each a different key + type -- get this
 * right or the driver won't see the write:
 *   SUB_ROOT  = HKLM\SYSTEM\CurrentControlSet\Services\3dfxvs\Device0
 *               plain miniport names, REG_DWORD (GraphicsClocking, CmdfifoSize,
 *               TiledMode...) and the REG_BINARY gamma LUTs.
 *   SUB_D3D   = ...\Device0\D3D
 *               all SSTH3_* names, REG_SZ (ASCII decimal/float); the display
 *               driver's ddgetenv searches D3D first, then Device0.
 *   SUB_GLIDE = ...\Device0\glide
 *               all FX_GLIDE_* names, REG_SZ; the Glide runtime's hwcGetenv
 *               resolves this via HKLM\HARDWARE\DEVICEMAP\VIDEO\\Device\Video0
 *               (which points at 3dfxvs on this box).
 *
 * Data-driven: the UI is generated from gSettings[]; adding a knob is one row.
 * Gamma LUTs are exposed as a single gamma NUMBER and expanded to a curve on
 * Apply; the DESKTOP gamma is clamped so this tool can never write the blown
 * overbright ramp that washes out the desktop.
 *
 * Build: make (i686-w64-mingw32-gcc, -mwindows -static-libgcc).
 */

#include <windows.h>
#include <commctrl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define SVC_ROOT   "SYSTEM\\CurrentControlSet\\Services\\3dfxvs\\Device0"
#define APP_TITLE  "3dfx Control Panel  -  retro3dfx driver settings"

enum { SUB_ROOT = 0, SUB_D3D = 1, SUB_GLIDE = 2 };
enum { T_COMBO, T_INT, T_GAMMA };   /* how it is edited */

typedef struct { const char *label; const char *value; } CHOICE;

typedef struct {
    const char *group;
    const char *label;
    int         sub;        /* SUB_* */
    const char *name;       /* registry value name */
    int         type;       /* T_* */
    int         dword;      /* 1 = REG_DWORD, 0 = REG_SZ (ignored for T_GAMMA) */
    const CHOICE *choices;   /* T_COMBO: NULL-terminated (value==NULL) */
    const char *deflt;
    const char *help;
    int         reboot;
    HWND        hCtl, hLabel, hHelp;
} SETTING;

/* ---- choice lists (values are the exact strings/decimals the driver parses) */
static const CHOICE cClock[] = {
    {"Auto (BIOS / stock 166 MHz)", "0"},
    {"143 MHz (conservative)",      "143"},
    {"150 MHz",                     "150"},
    {"166 MHz (stock)",             "166"},
    {"183 MHz (overclock)",         "183"},
    {"200 MHz (overclock)",         "200"}, {NULL,NULL} };
static const CHOICE cVsyncGlide[] = {
    {"Software controlled (app decides)", "-1"},
    {"Off (no vblank wait)",              "0"},
    {"On - every vblank (recommended)",   "1"},
    {"On - every 2nd vblank (half)",      "2"}, {NULL,NULL} };
static const CHOICE cVsyncD3D[] = {
    {"On - every vblank (recommended)", "1"}, {"Off", "0"},
    {"On - every 2nd vblank", "2"}, {NULL,NULL} };
static const CHOICE cRefresh[] = {
    {"Auto (driver picks highest safe)", "__AUTO__"}, {"60 Hz", "60"}, {"70 Hz", "70"},
    {"72 Hz", "72"}, {"75 Hz", "75"}, {"85 Hz", "85"}, {NULL,NULL} };
static const CHOICE cSliAa[] = {
    {"Single chip (no SLI/AA)",  "0"},
    {"2-way SLI",                "2"},
    {"4-way SLI",                "5"},
    {"4-way SLI + 2x AA",        "6"},
    {"2-way SLI + 4x AA",        "7"},
    {"8x AA",                    "8"}, {NULL,NULL} };
static const CHOICE cOverlay[] = {
    {"Off", "0"}, {"2x2 below 1024 (default)", "1"},
    {"4x1 always", "2"}, {"2x2 always", "3"}, {NULL,NULL} };
static const CHOICE cDither[] = {
    {"Off", "0"}, {"Optimal (default)", "1"},
    {"Sharper", "2"}, {"Smoother", "3"}, {NULL,NULL} };
static const CHOICE cTiled[] = { {"On (tiled - default)","1"}, {"Off (linear)","0"}, {NULL,NULL} };
static const CHOICE cOnOff[] = { {"On","1"}, {"Off","0"}, {NULL,NULL} };

/* The table. */
static SETTING gSettings[] = {
  /* group,          label,                          sub,       name,                          type,    dword, choices,      deflt,  help, reboot */
  {"Clock",       "Core / graphics clock (MHz)",     SUB_ROOT,  "GraphicsClocking",            T_COMBO, 1, cClock,      "0",    "VSA-100 core PLL. 0=leave the BIOS clock. >166 is overclocking (may artifact). An invalid MHz is ignored (BIOS kept).", 1},

  {"Display & Sync","Vertical sync (Glide / OpenGL)", SUB_GLIDE, "FX_GLIDE_SWAPINTERVAL",       T_COMBO, 0, cVsyncGlide, "1",    "Wait for vblank on buffer swap. Fixes tearing and the swap/scanout beat (stutter).", 1},
  {"Display & Sync","Vertical sync (Direct3D)",       SUB_D3D,   "SSTH3_SWAPINTERVAL",          T_COMBO, 0, cVsyncD3D,   "1",    "D3D flip waits for N vblanks.", 1},
  {"Display & Sync","Full-screen refresh override",   SUB_GLIDE, "FX_GLIDE_REFRESH",            T_COMBO, 0, cRefresh,    "__AUTO__", "Auto = the driver picks the highest CRT-safe refresh per resolution (85Hz <=1024x768, 75Hz@1280x1024). Only override if you know the mode is safe.", 1},

  {"Gamma",       "Desktop gamma",                    SUB_ROOT,  "GammaTable",                  T_GAMMA, 0, NULL,        "1.00", "2D desktop brightness (1.00 = neutral). Clamped so it can't wash out the desktop.", 1},
  {"Gamma",       "3D / Glide gamma",                 SUB_ROOT,  "GlideGammaTable",             T_GAMMA, 0, NULL,        "1.30", "In-game brightness for Glide/OpenGL (1.30 = 3dfx default, brighter than 1.0).", 1},

  {"Multi-chip",  "SLI / anti-aliasing",              SUB_D3D,   "SSTH3_SLI_AA_CONFIGURATION",  T_COMBO, 0, cSliAa,      "2",    "How the chips cooperate. 2-way SLI is the 5500 default; AA modes need enough VRAM.", 1},
  {"Multi-chip",  "SLI band height (scanlines)",      SUB_D3D,   "SSTH3_SLI_BAND_HEIGHT",       T_INT,   0, NULL,        "0",    "2..128; 0 = driver-computed.", 1},

  {"Image Quality","Video overlay filter",            SUB_D3D,   "SSTH3_OVERLAYMODE",           T_COMBO, 0, cOverlay,    "1",    "Scanout scaling/filter for the displayed image.", 1},
  {"Image Quality","Alpha dither",                    SUB_D3D,   "SSTH3_ALPHADITHERMODE",       T_COMBO, 0, cDither,     "1",    "16bpp alpha dithering quality.", 1},
  {"Image Quality","Texture LOD bias",                SUB_D3D,   "SSTH3_LOD_BIAS",              T_INT,   0, NULL,        "0",    "Sharper (negative) / blurrier (positive) mip selection. 0 = neutral.", 1},

  {"Performance", "Command FIFO size (bytes)",        SUB_ROOT,  "CmdfifoSize",                 T_INT,   1, NULL,        "0",    "0 = driver default. Rounded to 4KB, clamped 64KB..1MB.", 1},
  {"Performance", "Framebuffer tiling",               SUB_ROOT,  "TiledMode",                   T_COMBO, 1, cTiled,      "1",    "Tiled is faster; Off forces a linear framebuffer (compatibility).", 1},

  {"Diagnostics", "Verbose registry-ring log",        SUB_ROOT,  "Retro3dfxLog",                T_COMBO, 0, cOnOff,      "0",    "Extra lifecycle logging to the RLog flight-recorder ring.", 1},

  {NULL,NULL,0,NULL,0,0,NULL,NULL,NULL,0}
};

/* ---- registry helpers ----------------------------------------------------- */

static void keyPath(int sub, char *out, int n)
{
    if      (sub == SUB_GLIDE) _snprintf(out, n, "%s\\glide", SVC_ROOT);
    else if (sub == SUB_D3D)   _snprintf(out, n, "%s\\D3D",   SVC_ROOT);
    else                       _snprintf(out, n, "%s",        SVC_ROOT);
    out[n-1] = 0;
}

static int regGetSzAt(int sub, const char *name, char *buf, int n)
{
    char path[512]; HKEY h; DWORD type, cb = n; LONG r;
    keyPath(sub, path, sizeof(path));
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, path, 0, KEY_READ, &h) != ERROR_SUCCESS) return 0;
    r = RegQueryValueExA(h, name, 0, &type, (BYTE*)buf, &cb);
    RegCloseKey(h);
    if (r != ERROR_SUCCESS || type != REG_SZ) return 0;
    buf[(cb < (DWORD)n) ? cb : n-1] = 0;
    return 1;
}
/* SSTH3_* live in D3D but Device0 is a fallback the driver also reads: try both */
static int regGetSz(int sub, const char *name, char *buf, int n)
{
    if (regGetSzAt(sub, name, buf, n)) return 1;
    if (sub == SUB_D3D) return regGetSzAt(SUB_ROOT, name, buf, n);
    return 0;
}
static int regSetSz(int sub, const char *name, const char *val)
{
    char path[512]; HKEY h; LONG r;
    keyPath(sub, path, sizeof(path));
    if (RegCreateKeyExA(HKEY_LOCAL_MACHINE, path, 0, 0, 0, KEY_WRITE, 0, &h, 0) != ERROR_SUCCESS) return 0;
    r = RegSetValueExA(h, name, 0, REG_SZ, (const BYTE*)val, (DWORD)strlen(val)+1);
    RegCloseKey(h);
    return r == ERROR_SUCCESS;
}
/* Delete a value (used for "Auto" choices whose absence is what the driver
 * wants). e.g. FX_GLIDE_REFRESH: any present value (even "0") OVERRIDES the
 * ICD's per-resolution safe refresh, so "Auto" must remove it entirely. */
static int regDelete(int sub, const char *name)
{
    char path[512]; HKEY h; LONG r;
    keyPath(sub, path, sizeof(path));
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, path, 0, KEY_SET_VALUE, &h) != ERROR_SUCCESS) return 1;
    r = RegDeleteValueA(h, name);
    RegCloseKey(h);
    return (r == ERROR_SUCCESS || r == ERROR_FILE_NOT_FOUND);
}
static int regGetDword(int sub, const char *name, DWORD *out)
{
    char path[512]; HKEY h; DWORD type, cb = sizeof(DWORD); LONG r;
    keyPath(sub, path, sizeof(path));
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, path, 0, KEY_READ, &h) != ERROR_SUCCESS) return 0;
    r = RegQueryValueExA(h, name, 0, &type, (BYTE*)out, &cb);
    RegCloseKey(h);
    return (r == ERROR_SUCCESS && type == REG_DWORD);
}
static int regSetDword(int sub, const char *name, DWORD val)
{
    char path[512]; HKEY h; LONG r;
    keyPath(sub, path, sizeof(path));
    if (RegCreateKeyExA(HKEY_LOCAL_MACHINE, path, 0, 0, 0, KEY_WRITE, 0, &h, 0) != ERROR_SUCCESS) return 0;
    r = RegSetValueExA(h, name, 0, REG_DWORD, (const BYTE*)&val, sizeof(val));
    RegCloseKey(h);
    return r == ERROR_SUCCESS;
}
/* infer gamma exponent from a REG_BINARY LUT's midpoint (GammaTable[128] blue) */
static double regGetGamma(const char *name, double deflt)
{
    char path[512]; HKEY h; DWORD type, cb; BYTE *data; double g = deflt;
    keyPath(SUB_ROOT, path, sizeof(path));
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, path, 0, KEY_READ, &h) != ERROR_SUCCESS) return deflt;
    cb = 256*sizeof(ULONG); data = (BYTE*)malloc(cb);
    if (data && RegQueryValueExA(h, name, 0, &type, data, &cb) == ERROR_SUCCESS &&
        type == REG_BINARY && cb >= 129*sizeof(ULONG)) {
        int mid = (int)(((ULONG*)data)[128] & 0xFF);
        if (mid > 1 && mid < 255) g = log(128.0/255.0) / log((double)mid/255.0);
    }
    free(data); RegCloseKey(h);
    return g;
}
static int regSetGamma(const char *name, double g, int clampSafe)
{
    char path[512]; HKEY h; ULONG tab[256]; int i; LONG r;
    if (g < 0.30) g = 0.30;
    if (g > 3.00) g = 3.00;
    for (i = 0; i < 256; i++) {
        double o = (i == 0) ? 0.0 : pow((double)i/255.0, 1.0/g) * 255.0 + 0.5;
        int v = (o < 0) ? 0 : (o > 255) ? 255 : (int)o;
        tab[i] = ((ULONG)v << 16) | ((ULONG)v << 8) | (ULONG)v;
    }
    if (clampSafe && (int)(tab[128] & 0xFF) >= 0xE0) return -1;   /* would wash out */
    keyPath(SUB_ROOT, path, sizeof(path));
    if (RegCreateKeyExA(HKEY_LOCAL_MACHINE, path, 0, 0, 0, KEY_WRITE, 0, &h, 0) != ERROR_SUCCESS) return 0;
    r = RegSetValueExA(h, name, 0, REG_BINARY, (const BYTE*)tab, sizeof(tab));
    RegCloseKey(h);
    return r == ERROR_SUCCESS;
}

/* ---- UI ------------------------------------------------------------------- */

static HFONT gFont;
static HWND  gStatus;

static void comboSelectValue(SETTING *s, const char *cur)
{
    int i, sel = 0;
    for (i = 0; s->choices[i].value; i++) {
        SendMessageA(s->hCtl, CB_ADDSTRING, 0, (LPARAM)s->choices[i].label);
        if (strcmp(s->choices[i].value, cur) == 0) sel = i;
    }
    SendMessageA(s->hCtl, CB_SETCURSEL, sel, 0);
}

static void loadCurrent(SETTING *s)
{
    char buf[64];
    if (s->type == T_GAMMA) {
        double g = regGetGamma(s->name, atof(s->deflt));
        _snprintf(buf, sizeof(buf), "%.2f", g); buf[sizeof(buf)-1]=0;
        SetWindowTextA(s->hCtl, buf);
        return;
    }
    if (s->dword) {
        DWORD v;
        if (regGetDword(s->sub, s->name, &v)) _snprintf(buf, sizeof(buf), "%lu", (unsigned long)v);
        else strcpy(buf, s->deflt);
    } else {
        if (!regGetSz(s->sub, s->name, buf, sizeof(buf))) strcpy(buf, s->deflt);
    }
    buf[sizeof(buf)-1]=0;
    if (s->type == T_COMBO) comboSelectValue(s, buf);
    else                    SetWindowTextA(s->hCtl, buf);
}

static int applyAll(int *anyReboot, int *rejected)
{
    int n = 0, i; *anyReboot = 0; *rejected = 0;
    for (i = 0; gSettings[i].label; i++) {
        SETTING *s = &gSettings[i]; char buf[64]; const char *val = NULL; int ok = 0;
        if (s->type == T_GAMMA) {
            double g; GetWindowTextA(s->hCtl, buf, sizeof(buf)); g = atof(buf);
            int clampSafe = (strcmp(s->name, "GammaTable") == 0);
            int rc = regSetGamma(s->name, g, clampSafe);
            if (rc == -1) { (*rejected)++; continue; }
            ok = (rc == 1);
        } else {
            if (s->type == T_COMBO) {
                int sel = (int)SendMessageA(s->hCtl, CB_GETCURSEL, 0, 0);
                if (sel >= 0) val = s->choices[sel].value;
            } else {
                GetWindowTextA(s->hCtl, buf, sizeof(buf)); val = buf;
            }
            if (val && strcmp(val, "__AUTO__") == 0)
                ok = regDelete(s->sub, s->name);   /* "Auto" = remove the override */
            else if (val)
                ok = s->dword ? regSetDword(s->sub, s->name, (DWORD)strtoul(val,0,10))
                              : regSetSz(s->sub, s->name, val);
        }
        if (ok) { n++; if (s->reboot) *anyReboot = 1; }
    }
    return n;
}

#define ID_APPLY 1001
#define ID_CLOSE 1002

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE: {
        int i, y = 6; const char *lastGroup = NULL;
        HINSTANCE hi = ((LPCREATESTRUCT)lp)->hInstance;
        gFont = CreateFontA(-11,0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,0,0,0,0,"MS Shell Dlg");
        for (i = 0; gSettings[i].label; i++) {
            SETTING *s = &gSettings[i];
            if (!lastGroup || strcmp(lastGroup, s->group)) {
                HWND g = CreateWindowA("STATIC", s->group, WS_CHILD|WS_VISIBLE,
                    12, y, 600, 15, hwnd, 0, hi, 0);
                SendMessageA(g, WM_SETFONT, (WPARAM)gFont, 0);
                y += 17; lastGroup = s->group;
            }
            s->hLabel = CreateWindowA("STATIC", s->label, WS_CHILD|WS_VISIBLE,
                24, y+2, 260, 15, hwnd, 0, hi, 0);
            SendMessageA(s->hLabel, WM_SETFONT, (WPARAM)gFont, 0);
            if (s->type == T_COMBO)
                s->hCtl = CreateWindowA("COMBOBOX", 0,
                    WS_CHILD|WS_VISIBLE|WS_TABSTOP|CBS_DROPDOWNLIST|WS_VSCROLL,
                    290, y, 300, 200, hwnd, 0, hi, 0);
            else
                s->hCtl = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", 0,
                    WS_CHILD|WS_VISIBLE|WS_TABSTOP|ES_AUTOHSCROLL,
                    290, y, 90, 20, hwnd, 0, hi, 0);
            SendMessageA(s->hCtl, WM_SETFONT, (WPARAM)gFont, 0);
            y += 20;
            s->hHelp = CreateWindowA("STATIC", s->help, WS_CHILD|WS_VISIBLE,
                24, y, 585, 12, hwnd, 0, hi, 0);
            SendMessageA(s->hHelp, WM_SETFONT, (WPARAM)gFont, 0);
            y += 14;
            loadCurrent(s);
        }
        {
            HWND b1 = CreateWindowA("BUTTON","Apply (writes registry)",
                WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_DEFPUSHBUTTON,
                290, y+8, 190, 26, hwnd, (HMENU)ID_APPLY, hi, 0);
            HWND b2 = CreateWindowA("BUTTON","Close",
                WS_CHILD|WS_VISIBLE|WS_TABSTOP, 490, y+8, 100, 26, hwnd, (HMENU)ID_CLOSE, hi, 0);
            gStatus = CreateWindowA("STATIC","Changes take effect after a reboot or on the next game launch.",
                WS_CHILD|WS_VISIBLE, 24, y+14, 260, 30, hwnd, 0, hi, 0);
            SendMessageA(b1, WM_SETFONT, (WPARAM)gFont, 0);
            SendMessageA(b2, WM_SETFONT, (WPARAM)gFont, 0);
            SendMessageA(gStatus, WM_SETFONT, (WPARAM)gFont, 0);
        }
        /* auto-size the window so the Apply/Close buttons always fit on screen */
        {
            RECT rc; rc.left = 0; rc.top = 0; rc.right = 620; rc.bottom = y + 46;
            AdjustWindowRect(&rc, (DWORD)GetWindowLongA(hwnd, GWL_STYLE), FALSE);
            SetWindowPos(hwnd, 0, 0, 0, rc.right - rc.left, rc.bottom - rc.top,
                         SWP_NOMOVE | SWP_NOZORDER);
        }
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == ID_APPLY) {
            int reboot = 0, rejected = 0, n = applyAll(&reboot, &rejected);
            char msg[320];
            _snprintf(msg, sizeof(msg),
                "Wrote %d setting(s) to the registry.%s\n\n%s", n,
                rejected ? "\n\nOne overbright DESKTOP gamma was rejected (it would wash out the 2D desktop)." : "",
                reboot ? "Reboot the machine (or restart the game) for the changes to take effect." : "");
            msg[sizeof(msg)-1]=0;
            MessageBoxA(hwnd, msg, APP_TITLE, MB_OK|MB_ICONINFORMATION);
        } else if (LOWORD(wp) == ID_CLOSE) {
            DestroyWindow(hwnd);
        }
        return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

int WINAPI WinMain(HINSTANCE hi, HINSTANCE hp, LPSTR cmd, int show)
{
    WNDCLASSA wc; MSG m; HWND hwnd; INITCOMMONCONTROLSEX ic;
    (void)hp; (void)cmd;
    ic.dwSize = sizeof(ic); ic.dwICC = ICC_STANDARD_CLASSES; InitCommonControlsEx(&ic);
    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc = WndProc; wc.hInstance = hi;
    wc.hCursor = LoadCursor(0, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE+1);
    wc.lpszClassName = "Retro3dfxCtl";
    RegisterClassA(&wc);
    hwnd = CreateWindowA("Retro3dfxCtl", APP_TITLE,
        WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX,
        24, 6, 630, 628, 0, 0, hi, 0);
    ShowWindow(hwnd, show); UpdateWindow(hwnd);
    while (GetMessage(&m, 0, 0, 0)) { TranslateMessage(&m); DispatchMessage(&m); }
    return 0;
}
