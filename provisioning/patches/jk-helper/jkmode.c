/* JKMODE.EXE - give Jedi Knight DF2 / Mysteries of the Sith this box's resolution.
 *
 *   JKMODE df2  <width> <height> [dry] [list]
 *   JKMODE mots <width> <height> [dry] [list]
 *
 * WHY THIS EXISTS. Both games keep their display mode in HKLM as `displayMode`,
 * an INDEX into a DirectDraw mode list they enumerate themselves (at most 64
 * entries, then SORTED by bpp/width/height), next to `b3DAccel` and the GUIDs
 * of the display device and the Direct3D device. There is no width/height
 * anywhere to write, and the index moves whenever the driver's mode list
 * changes - 1920x1080x16 is entry 31 of DF2's sorted list on .123, 33 on .145
 * and 39 on .195, and MotS drops high-resolution 8-bpp modes, so its index is
 * lower again. So a staged constant is wrong by construction, and this tool,
 * run by the launcher before every start, rebuilds the list EXACTLY as the
 * game does - same passes, same cap, MotS's filter, the same qsort with the
 * same comparator - and writes the index of <width>x<height> at 16 bpp,
 * b3DAccel=1 and the two GUIDs the game itself would store when a player ticks
 * Setup -> Display -> 3D Acceleration and picks that mode. Every value is read
 * back. See jklogic.h for the rules and the game addresses they come from.
 *
 * IT REFUSES rather than guess, writing nothing, when:
 *   - the mode is not in the game's list (not enumerated, or past the 64-entry
 *     cap: .240's X800 offers 1920x1080x16 at position 76)          exit 3
 *   - no Direct3D device passes the game's own 3D test               exit 4
 *   - DirectDraw / Direct3D enumeration fails where the game's would  exit 2
 *   - the key's Version is not "0.1" (the game DELETES the whole key
 *     at start-up in that case, JK 0x50f254)                         exit 5
 *   - the chosen device's list holds a mode with neither PAL8 nor RGB
 *     (the game sorts it on a stale bpp, so no index is knowable)   exit 7
 * The one change it makes on a refusal: a stored displayMode that is PAST THE
 * END of the list the game will index is deleted, because the game does not
 * range-check it (JK 0x414ba6) - that is a crash, not a preference.
 *
 * Exit: 0 written (or already right) and read back, or disabled on this box;
 * 1 usage; 2..5 and 7 refused (above); 6 a value did not read back as written.
 *
 * PER-BOX OFF SWITCH: HKLM\Software\RetroAgent `JkMode` (REG_DWORD) = 0 makes it
 * touch nothing and exit 0, the game keeping whatever it has stored - the same
 * `<feature>=0` convention as GameGate/PciRescue/RefreshMax. It exists because
 * this runs before EVERY launch: on a box whose card lists 1920x1080 at 16 bpp
 * but cannot run the game's 3D there, nothing else could stop it re-applying.
 *
 * BUILD: provisioning/patches/jk-helper/build.sh. No C runtime, no manifest,
 * ddraw.dll loaded at run time: imports are KERNEL32/USER32/ADVAPI32 only.
 * NO MANIFEST ON PURPOSE: neither game has one, so under UAC both are
 * registry-VIRTUALIZED; a manifested helper would write the real HKLM while the
 * game reads VirtualStore. 32-bit like the games, so WOW64 redirects both alike.
 */
#define WIN32_LEAN_AND_MEAN
#define DIRECTDRAW_VERSION 0x0500
#define DIRECT3D_VERSION 0x0500
#include <windows.h>
#include <stdarg.h>
#include <ddraw.h>
#include <d3d.h>
#include "jklogic.h"

#define JKMODE_VERSION "1.1"

/* ---- freestanding bits the compiler may call ----------------------------- */
void *memset(void *d, int c, unsigned int n)
{ unsigned char *p = d; while (n--) *p++ = (unsigned char)c; return d; }
void *memcpy(void *d, const void *s, unsigned int n)
{ unsigned char *p = d; const unsigned char *q = s; while (n--) *p++ = *q++; return d; }
int memcmp(const void *a, const void *b, unsigned int n)
{ const unsigned char *p = a, *q = b; for (; n; n--, p++, q++) if (*p != *q) return *p - *q; return 0; }

/* ---- output --------------------------------------------------------------- */
static HANDLE g_out;
static void out(const char *fmt, ...)
{
    char buf[1100];
    DWORD n;
    va_list ap;
    va_start(ap, fmt);
    n = (DWORD)wvsprintfA(buf, fmt, ap);
    va_end(ap);
    if (g_out && g_out != INVALID_HANDLE_VALUE) WriteFile(g_out, buf, n, &n, NULL);
}

static const char *guid_str(const jk_guid *g, char *buf)
{
    const unsigned char *b = g->b;
    wsprintfA(buf, "{%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
              (unsigned long)jk_u32(b, 0), (unsigned)(b[4] | (b[5] << 8)),
              (unsigned)(b[6] | (b[7] << 8)), b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
    return buf;
}

static void copy_str(char *d, const char *s, int cap)
{
    int i = 0;
    if (s) for (; s[i] && i < cap - 1; i++) d[i] = s[i];
    d[i] = 0;
}

/* ---- the two games ---------------------------------------------------------- */
typedef struct {
    const char *arg, *label, *key;
    DWORD pass1_coop;    /* JK 0x425b97 push 0x51 / MotS 0x428cc7 push 0x11 */
    int hires8_filter;   /* MotS 0x428fa1: pass 2 drops width >= 1400 with lPitch == width */
} game_t;

static const game_t GAMES[] = {
    { "df2",  "Jedi Knight: Dark Forces II (JK.EXE 1.01)",
      "Software\\LucasArts Entertainment Company\\JediKnight\\v1.0", 0x51, 0 },
    { "mots", "Jedi Knight: Mysteries of the Sith (JKM.EXE)",
      "Software\\LucasArts Entertainment Company LLC\\Mysteries of the Sith\\v1.0", 0x11, 1 },
};

/* IID_IDirect3D {3BBA0080-2421-11CF-A31A-00AA00B93356}: what both games QI for
 * (JK 0x429616 -> 0x522808, MotS 0x42c7d6 -> 0x57a820). Defined here so nothing
 * links dxguid. */
static const GUID IID_D3D1 = {0x3bba0080, 0x2421, 0x11cf, {0xa3,0x1a,0x00,0xaa,0x00,0xb9,0x33,0x56}};

typedef HRESULT (WINAPI *pDDCreate)(GUID *, LPDIRECTDRAW *, IUnknown *);
typedef HRESULT (WINAPI *pDDEnumA)(LPDDENUMCALLBACKA, LPVOID);

static pDDCreate p_create;
static pDDEnumA p_enum;
static HWND g_hwnd;
static const game_t *g_game;
static int g_tw, g_th;

static jk_dev g_dev[JK_DEV_CAP];   /* DirectDraw devices; game list index = i + 1 */
static int g_ndev;
static int g_enum_stopped;          /* the callback returned FALSE the way the game's does */
static jk_dev *g_cur;
static int g_pass;

/* DirectDrawEnumerateA callback - JK 0x425c40 / MotS 0x428d70. */
static BOOL WINAPI dd_enum_cb(GUID *guid, LPSTR desc, LPSTR name, LPVOID ctx)
{
    LPDIRECTDRAW dd = NULL;
    static unsigned char drv[0x180], hel[0x180];
    jk_dev *d;
    (void)ctx;
    if (g_ndev + 1 >= JK_DEV_CAP) { g_enum_stopped = 1; return FALSE; }
    d = &g_dev[g_ndev];
    memset(d, 0, sizeof(*d));
    d->target_seen_at = -1;
    if (guid) { memcpy(d->guid.b, guid, 16); d->has_guid = 1; }
    else d->primary = 1;
    copy_str(d->desc, desc, sizeof(d->desc));
    copy_str(d->name, name, sizeof(d->name));
    if (p_create(guid, &dd, NULL) != DD_OK || !dd) return TRUE;          /* skipped, go on */
    if (IDirectDraw_SetCooperativeLevel(dd, g_hwnd, 0x11) != DD_OK) {    /* game: stops enumerating */
        IDirectDraw_Release(dd);
        g_enum_stopped = 1;
        return FALSE;
    }
    memset(drv, 0, sizeof(drv)); memset(hel, 0, sizeof(hel));
    *(DWORD *)drv = 0x16c; *(DWORD *)hel = 0x16c;                          /* the DX5 DDCAPS size the game passes */
    if (IDirectDraw_GetCaps(dd, (LPDDCAPS)drv, (LPDDCAPS)hel) != DD_OK) {
        IDirectDraw_Release(dd);
        return TRUE;
    }
    d->is3d = (int)(jk_u32(drv, 4) & 1);                                   /* DDCAPS_3D */
    if (IDirectDraw_SetCooperativeLevel(dd, g_hwnd, 8) != DD_OK) {
        IDirectDraw_Release(dd);
        g_enum_stopped = 1;
        return FALSE;
    }
    IDirectDraw_Release(dd);
    g_ndev++;
    return TRUE;
}

/* EnumDisplayModes callback - both passes (JK 0x426030 / 0x425e30, MotS
 * 0x429160 / 0x428f60). The raw offsets are the ones the game reads: +0x08
 * dwHeight, +0x0c dwWidth, +0x10 lPitch (MotS's filter), +0x4c
 * ddpfPixelFormat.dwFlags, +0x54 dwRGBBitCount. */
static HRESULT WINAPI mode_cb(LPDDSURFACEDESC sd, LPVOID ctx)
{
    const unsigned char *p = (const unsigned char *)sd;
    (void)ctx;
    jk_mode_offer(g_cur, g_pass, (int)jk_u32(p, 0x0c), (int)jk_u32(p, 0x08), (int)jk_u32(p, 0x10),
                  jk_u32(p, 0x4c), jk_u32(p, 0x54), g_tw, g_th, g_game->hires8_filter);
    return DDENUMRET_OK;
}

/* EnumDevices callback - JK 0x42bd50 / MotS 0x42ef10 (identical code). */
static HRESULT WINAPI d3d_cb(GUID *guid, LPSTR desc, LPSTR name,
                             LPD3DDEVICEDESC hw, LPD3DDEVICEDESC hel, LPVOID ctx)
{
    jk_d3d *r;
    (void)desc; (void)ctx;
    r = jk_d3d_add(g_cur);
    if (!r) return D3DENUMRET_CANCEL;
    memset(r, 0, sizeof(*r));
    if (guid) memcpy(r->guid.b, guid, 16);
    copy_str(r->name, name, sizeof(r->name));
    jk_d3d_from_desc(r, (const unsigned char *)hw, (const unsigned char *)hel);
    return D3DENUMRET_OK;
}

/* The per-device work the game does while building its device list: its mode
 * enumeration (JK 0x425b60 / MotS 0x428c90) and then Direct3D on that same
 * DirectDraw object (JK 0x429600 / MotS 0x42c7c0). Any failure here fails the
 * GAME's list build too, so it is reported and nothing is written. */
static int enum_device(jk_dev *d, int gi)
{
    LPDIRECTDRAW dd = NULL;
    LPDIRECT3D d3 = NULL;
    HRESULT hr;
    g_cur = d;
    hr = p_create(d->primary ? NULL : (GUID *)d->guid.b, &dd, NULL);
    if (hr != DD_OK || !dd) { out("FAIL device %d: DirectDrawCreate 0x%08lX\r\n", gi, (unsigned long)hr); return 0; }
    hr = IDirectDraw_SetCooperativeLevel(dd, g_hwnd, g_game->pass1_coop);
    if (hr != DD_OK) { out("FAIL device %d: SetCooperativeLevel(0x%lX) 0x%08lX\r\n", gi, g_game->pass1_coop, (unsigned long)hr); goto bad; }
    g_pass = 1;
    hr = IDirectDraw_EnumDisplayModes(dd, 0, NULL, NULL, mode_cb);
    if (hr != DD_OK) { out("FAIL device %d: EnumDisplayModes pass 1 0x%08lX\r\n", gi, (unsigned long)hr); goto bad; }
    hr = IDirectDraw_SetCooperativeLevel(dd, g_hwnd, 0x11);
    if (hr != DD_OK) { out("FAIL device %d: SetCooperativeLevel(0x11) 0x%08lX\r\n", gi, (unsigned long)hr); goto bad; }
    g_pass = 2;
    hr = IDirectDraw_EnumDisplayModes(dd, 0, NULL, NULL, mode_cb);
    if (hr != DD_OK) { out("FAIL device %d: EnumDisplayModes pass 2 0x%08lX\r\n", gi, (unsigned long)hr); goto bad; }
    hr = IDirectDraw_SetCooperativeLevel(dd, g_hwnd, 8);
    if (hr != DD_OK) { out("FAIL device %d: SetCooperativeLevel(8) 0x%08lX\r\n", gi, (unsigned long)hr); goto bad; }
    /* The game sorts the capped list here, before the device record copies it
     * (JK 0x422797 -> qsort 0x513060 / comparator 0x4249d0). displayMode is an
     * index into THIS order, not the enumeration order. */
    jk_sort_modes(d);
    hr = IDirectDraw_QueryInterface(dd, &IID_D3D1, (void **)&d3);
    if (hr != DD_OK || !d3) { out("FAIL device %d: QueryInterface(IID_IDirect3D) 0x%08lX\r\n", gi, (unsigned long)hr); goto bad; }
    hr = IDirect3D_EnumDevices(d3, d3d_cb, NULL);
    IDirect3D_Release(d3);
    if (hr != DD_OK) { out("FAIL device %d: EnumDevices 0x%08lX\r\n", gi, (unsigned long)hr); goto bad; }
    if (d->n3d == 0) { out("FAIL device %d: no Direct3D device at all (the game's list build fails)\r\n", gi); goto bad; }
    IDirectDraw_Release(dd);
    return 1;
bad:
    IDirectDraw_Release(dd);
    return 0;
}

/* ---- registry ---------------------------------------------------------------- */
typedef struct { int present; DWORD type, size; unsigned char data[20]; } regval;

static void reg_get(HKEY k, const char *name, regval *v)
{
    memset(v, 0, sizeof(*v));
    v->size = sizeof(v->data);
    if (RegQueryValueExA(k, name, NULL, &v->type, v->data, &v->size) == ERROR_SUCCESS) v->present = 1;
}

/* Game-identical REG_BINARY (type 3) - JK 0x50f5a0 (16 bytes), 0x50f300 and
 * 0x50f4c0 (4 bytes). The readers take any type but need the exact size. */
static int reg_same(const regval *v, const void *want, DWORD n)
{
    return v->present && v->type == REG_BINARY && v->size == n && memcmp(v->data, want, n) == 0;
}

static int reg_put(HKEY k, const char *name, const void *data, DWORD n, int *changed)
{
    regval v;
    reg_get(k, name, &v);
    if (reg_same(&v, data, n)) { out("  %-18s unchanged\r\n", name); return 1; }
    if (RegSetValueExA(k, name, 0, REG_BINARY, (const BYTE *)data, n) != ERROR_SUCCESS) {
        out("FAIL  %-18s RegSetValueEx failed\r\n", name);
        return 0;
    }
    *changed += 1;
    reg_get(k, name, &v);
    if (!reg_same(&v, data, n)) { out("FAIL  %-18s did not read back as written\r\n", name); return 0; }
    out("  %-18s written, read back\r\n", name);
    return 1;
}

static void reg_would(HKEY k, const char *name, const void *data, DWORD n)
{
    regval v;
    reg_get(k, name, &v);
    out("dry: %-18s %s\r\n", name, reg_same(&v, data, n) ? "already right" : (v.present ? "would change" : "would be added"));
}

/* JK 0x50f1f1..0x50f254: a Version value that is present and not "0.1" makes
 * the game delete the whole key at start-up. Absent is fine (the game writes it). */
static int version_ok(HKEY k, char *seen)
{
    char buf[128];
    DWORD type, n = sizeof(buf) - 1;
    seen[0] = 0;
    if (RegQueryValueExA(k, "Version", NULL, &type, (BYTE *)buf, &n) != ERROR_SUCCESS) {
        copy_str(seen, "(absent)", 16);
        return 1;
    }
    while (n && buf[n - 1] == 0) n--;
    buf[n] = 0;
    copy_str(seen, buf, 64);
    return lstrcmpA(buf, "0.1") == 0;
}

/* ---- command line -------------------------------------------------------------- */
static int parse_int(const char *s, int *v)
{
    int n = 0, any = 0;
    for (; *s >= '0' && *s <= '9'; s++) { n = n * 10 + (*s - '0'); any = 1; if (n > 100000) return 0; }
    if (*s || !any) return 0;
    *v = n;
    return 1;
}

static int split_args(char *cl, char **argv, int max)
{
    int n = 0;
    while (*cl && n < max) {
        while (*cl == ' ' || *cl == '\t') cl++;
        if (!*cl) break;
        if (*cl == '"') {
            argv[n++] = ++cl;
            while (*cl && *cl != '"') cl++;
        } else {
            argv[n++] = cl;
            while (*cl && *cl != ' ' && *cl != '\t') cl++;
        }
        if (*cl) *cl++ = 0;
    }
    return n;
}

static int usage(void)
{
    out("JKMODE " JKMODE_VERSION " - set Jedi Knight's display mode for this box\r\n"
        "usage: JKMODE df2|mots <width> <height> [dry] [list]\r\n"
        "  df2   Dark Forces II (JK.EXE)    mots  Mysteries of the Sith (JKM.EXE)\r\n"
        "  dry   report what would be written, change nothing\r\n"
        "  list  also print every mode in the game's (sorted) list\r\n"
        "  HKLM\\Software\\RetroAgent JkMode=0 (REG_DWORD) switches it off on this box\r\n");
    return 1;
}

static int run(void)
{
    static char cl[1024];
    char *argv[12];
    char g1[48], g2[48], ver[80];
    int argc, i, j, dry = 0, list = 0, di = -1, ti = -1, idx, rc = 0, changed = 0;
    HMODULE ddraw;
    WNDCLASSA wc;
    HKEY key = NULL;
    const jk_dev *pd;

    copy_str(cl, GetCommandLineA(), sizeof(cl));
    argc = split_args(cl, argv, 12);
    if (argc < 4) return usage();
    for (i = 0; i < 2; i++) if (lstrcmpiA(argv[1], GAMES[i].arg) == 0) g_game = &GAMES[i];
    if (!g_game || !parse_int(argv[2], &g_tw) || !parse_int(argv[3], &g_th) || !g_tw || !g_th) return usage();
    for (i = 4; i < argc; i++) {
        if (!lstrcmpiA(argv[i], "dry") || !lstrcmpiA(argv[i], "--dry-run")) dry = 1;
        else if (!lstrcmpiA(argv[i], "list") || !lstrcmpiA(argv[i], "--list")) list = 1;
        else return usage();
    }

    out("JKMODE %s  %s\r\n", JKMODE_VERSION, g_game->label);
    out("target %dx%d 16bpp with 3D acceleration%s\r\n", g_tw, g_th, dry ? "  [dry run: nothing is written]" : "");

    {   /* the per-box off switch - read only, and before anything else runs */
        HKEY ra;
        DWORD v = 1, t = 0, n = sizeof(v);
        if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "Software\\RetroAgent", 0, KEY_QUERY_VALUE, &ra) == ERROR_SUCCESS) {
            LONG r = RegQueryValueExA(ra, "JkMode", NULL, &t, (BYTE *)&v, &n);
            RegCloseKey(ra);
            if (r == ERROR_SUCCESS && t == REG_DWORD && n == sizeof(v) && v == 0) {
                out("RESULT DISABLED on this box by HKLM\\Software\\RetroAgent JkMode=0 - nothing checked, "
                    "nothing written; the game keeps what it has stored\r\n");
                return 0;
            }
        }
    }

    ddraw = LoadLibraryA("ddraw.dll");
    p_create = ddraw ? (pDDCreate)GetProcAddress(ddraw, "DirectDrawCreate") : NULL;
    p_enum = ddraw ? (pDDEnumA)GetProcAddress(ddraw, "DirectDrawEnumerateA") : NULL;
    if (!p_create || !p_enum) { out("REFUSED: ddraw.dll or its exports are missing - nothing written\r\n"); return 2; }

    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "jkmode_probe";
    RegisterClassA(&wc);
    g_hwnd = CreateWindowExA(0, "jkmode_probe", "jkmode", WS_POPUP, 0, 0, 1, 1, NULL, NULL, wc.hInstance, NULL);
    if (!g_hwnd) { out("REFUSED: could not create the probe window - nothing written\r\n"); return 2; }

    if (p_enum(dd_enum_cb, NULL) != DD_OK) { out("REFUSED: DirectDrawEnumerateA failed - nothing written\r\n"); rc = 2; goto done; }
    if (g_ndev == 0) { out("REFUSED: no DirectDraw device survived the game's checks - nothing written\r\n"); rc = 2; goto done; }
    if (g_enum_stopped) out("note: device enumeration stopped early, exactly where the game's would\r\n");

    for (i = 0; i < g_ndev; i++) {
        if (!enum_device(&g_dev[i], i + 1)) { out("REFUSED: the game's own device-list build would fail here - nothing written\r\n"); rc = 2; goto done; }
    }

    for (i = 0; i < g_ndev; i++) {
        const jk_dev *d = &g_dev[i];
        out("device %d: \"%s\" guid=%s%s 3D=%s modes=%d stored of %d offered\r\n", i + 1, d->desc,
            guid_str(&d->guid, g1), d->primary ? " (primary)" : "", d->is3d ? "yes" : "no", d->nmodes, d->seen);
        for (j = 0; j < d->n3d; j++) {
            const jk_d3d *r = &d->d3d[j];
            out("   3D %d: \"%s\" %s hw=%d persp=%d z=%d rgb=%d 16bpp=%d -> %s\r\n", j, r->name, guid_str(&r->guid, g1),
                r->hw, r->persp, r->zbuf, (r->color & 2) ? 1 : 0, (r->depths & 0x10) ? 1 : 0,
                jk_d3d_usable(r) ? "usable" : "not usable");
        }
        if (list)
            for (j = 0; j < d->nmodes; j++)     /* the SORTED list: j is what displayMode means */
                out("   mode %2d: %4dx%-4d %2dbpp%s\r\n", j, d->modes[j].w, d->modes[j].h, d->modes[j].bpp,
                    d->modes[j].modex ? " [ModeX]" : "");
    }

    if (dry) {
        if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, g_game->key, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) key = NULL;
    } else if (RegCreateKeyExA(HKEY_LOCAL_MACHINE, g_game->key, 0, NULL, 0, KEY_QUERY_VALUE | KEY_SET_VALUE,
                               NULL, &key, NULL) != ERROR_SUCCESS) {
        out("REFUSED: cannot open HKLM\\%s for writing - nothing written\r\n", g_game->key);
        rc = 5; goto done;
    }
    if (key && !version_ok(key, ver)) {
        out("REFUSED: HKLM\\%s has Version=\"%s\"; the game deletes the whole key at start-up unless it is \"0.1\" - nothing written\r\n",
            g_game->key, ver);
        rc = 5; goto done;
    }

    if (!jk_pick_3d(g_dev, g_ndev, &di, &ti)) {
        out("REFUSED: no Direct3D device passes the game's 3D test (HAL, perspective textures, z-buffer, RGB, 16 bpp) - "
            "the game would force b3DAccel off\r\n");
        rc = 4; goto sanitize;
    }
    pd = &g_dev[di];
    if (jk_modes_unknown_bpp(pd)) {
        out("REFUSED: device %d's list holds %d mode(s) with neither a palette nor an RGB format; the game sorts "
            "them on a stale bpp, so the index of %dx%d cannot be known - nothing written\r\n",
            di + 1, jk_modes_unknown_bpp(pd), g_tw, g_th);
        rc = 7; goto sanitize;
    }
    idx = jk_find_mode(pd, g_tw, g_th);
    if (idx < 0) {
        if (pd->target_seen_at >= JK_MODE_CAP)
            out("REFUSED: %dx%dx16 is enumerated at position %d of %d, past the %d modes the game keeps - the game "
                "cannot select it on this box\r\n", g_tw, g_th, pd->target_seen_at, pd->seen, JK_MODE_CAP);
        else
            out("REFUSED: the driver does not offer %dx%d at 16 bpp on device %d (%d modes)\r\n", g_tw, g_th, di + 1, pd->seen);
        rc = 3; goto sanitize;
    }

    out("chosen: device %d %s, 3D device %d \"%s\" %s\r\n", di + 1, guid_str(&pd->guid, g1), ti,
        pd->d3d[ti].name, guid_str(&pd->d3d[ti].guid, g2));
    out("displayMode = %d (%dx%d 16bpp in the game's sorted list; it keeps %d of %d enumerated)\r\n",
        idx, g_tw, g_th, pd->nmodes, pd->seen);

    if (dry) {
        DWORD one = 1, mode = (DWORD)idx;
        if (!key) { out("dry: HKLM\\%s does not exist yet (install.reg not merged?)\r\n", g_game->key); goto done; }
        reg_would(key, "displayDeviceGUID", pd->guid.b, 16);
        reg_would(key, "3DDeviceGUID", pd->d3d[ti].guid.b, 16);
        reg_would(key, "displayMode", &mode, 4);
        reg_would(key, "b3DAccel", &one, 4);
        out("RESULT dry run - nothing written\r\n");
        goto done;
    } else {
        DWORD one = 1, mode = (DWORD)idx;
        out("HKLM\\%s\r\n", g_game->key);
        if (!reg_put(key, "displayDeviceGUID", pd->guid.b, 16, &changed) ||
            !reg_put(key, "3DDeviceGUID", pd->d3d[ti].guid.b, 16, &changed) ||
            !reg_put(key, "displayMode", &mode, 4, &changed) ||
            !reg_put(key, "b3DAccel", &one, 4, &changed)) {
            rc = 6; goto sanitize;     /* a half-written set can leave an index past the new device's list */
        }
        out("RESULT OK %dx%d 16bpp 3D, displayMode=%d, %d value(s) changed\r\n", g_tw, g_th, idx, changed);
        goto done;
    }

sanitize:
    /* Refused. The only thing still worth doing is removing a stored index the
     * game would read past the end of its list with (it does not range-check). */
    if (key) {
        regval gd, g3, m;
        jk_guid dg;
        int both;
        reg_get(key, "displayDeviceGUID", &gd);
        reg_get(key, "3DDeviceGUID", &g3);
        reg_get(key, "displayMode", &m);
        both = gd.present && gd.size == 16 && g3.present && g3.size == 16;
        memcpy(dg.b, gd.data, 16);
        if (m.present && m.size == 4 &&
            jk_stored_mode_is_stale(g_dev, g_ndev, both, &dg, (unsigned)jk_u32(m.data, 0))) {
            /* b3DAccel goes with it: without displayMode the game falls back to its
             * own 640x480 8-bpp default, and 3D on an 8-bpp mode is not a state it
             * ever produces itself. Both absent = the game's first-start defaults. */
            if (dry) out("dry: stored displayMode %lu is past the end of the game's list; it and b3DAccel would be deleted\r\n",
                         (unsigned long)jk_u32(m.data, 0));
            else if (RegDeleteValueA(key, "displayMode") == ERROR_SUCCESS) {
                RegDeleteValueA(key, "b3DAccel");
                reg_get(key, "b3DAccel", &g3);
                out("deleted a stale displayMode %lu (past the end of the game's list; the game would read beyond it)"
                    " and b3DAccel%s\r\n", (unsigned long)jk_u32(m.data, 0), g3.present ? " - FAIL: b3DAccel is still there" : "");
                if (g3.present && rc != 6) rc = 6;
            } else
                out("FAIL could not delete a stale displayMode %lu\r\n", (unsigned long)jk_u32(m.data, 0));
        } else {
            out("stored values left as they are (%s)\r\n", m.present ? "their displayMode is inside the game's list" : "no displayMode stored");
        }
    }
    if (rc == 6) out("RESULT FAILED - a registry write or delete did not hold (read back)\r\n");
    else out("RESULT REFUSED - the game keeps whatever it has stored\r\n");

done:
    if (key) RegCloseKey(key);
    if (g_hwnd) DestroyWindow(g_hwnd);
    return rc;
}

void jkmode_entry(void)
{
    g_out = GetStdHandle(STD_OUTPUT_HANDLE);
    ExitProcess((UINT)run());
}
