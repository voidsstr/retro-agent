/*
 * FLEETRES.EXE - retro-fleet staged-game resolution resolver.
 *
 * ONE staged tree deploys to EIGHT machines with different panels, so a
 * resolution baked into a staged config is wrong somewhere by construction.
 * This tiny helper is staged INSIDE the game tree and is run by the title's
 * "Play <Game>.bat" at launch, before the game starts.  It reports the
 * resolution that is right for THIS box's monitor, and can write it straight
 * into an INI-shaped config.
 *
 * It answers three questions the batch cannot:
 *   1. what mode is the desktop in right now      (EnumDisplaySettings)
 *   2. what modes does this adapter actually offer (EnumDisplaySettings loop)
 *   3. what is the PANEL's native mode and is it an LCD  (EDID, from the
 *      registry - the preferred detailed timing + the digital-input bit +
 *      the vertical-refresh range)
 *
 * WHY NOT wmic:  measured on this fleet 2026-08-29, XP's
 * Win32_VideoController.CurrentHorizontalResolution reported 640x480 on
 * 192.168.1.123 while the box was really running 1024x768.  It is not a
 * usable source.  EnumDisplaySettings(ENUM_CURRENT_SETTINGS) is correct.
 *
 * Build:  i686-w64-mingw32-gcc -O2 -s -o FLEETRES.EXE fleetres.c -ladvapi32 -luser32
 *
 * Usage:
 *   FLEETRES.EXE -cmd                       emit "set FR_*=..." for CALL
 *   FLEETRES.EXE -info                      human readable dump
 *   FLEETRES.EXE -ini <file> <sec> <k> <v>  WritePrivateProfileString
 *   FLEETRES.EXE -setline <f> <key> <line>  replace the line starting <key>
 *   FLEETRES.EXE -kv <file> <key> <value>   replace/append  key=value
 *   FLEETRES.EXE -reg <HKLM|HKCU> <subkey> <value> <dword|sz> <data>
 *   FLEETRES.EXE -cap <w> <h>               cap the chosen mode (perf ceiling)
 *
 * WHY -kv AND -reg EXIST.  Three staged titles keep their mode somewhere none
 * of the first three modes can reach, and each was found by reading the game
 * rather than guessing:
 *   * DXX-Rebirth (Descent 2) writes DESCENT.CFG as bare  ResolutionX=1024
 *     with NO [section] header, so WritePrivateProfileString cannot address it
 *     and -setline cannot match it either - "ResolutionX=1024" is ONE
 *     whitespace token.  -kv splits at the '=' instead.
 *   * Max Payne reads its mode through MFC's SetRegistryKey("Remedy
 *     Entertainment"), and Red Faction through HKLM\SOFTWARE\Volition\Red
 *     Faction.  Nothing that ships in the tree can set those, so a box
 *     inherits whatever stale HKCU it happens to have.  -reg writes them.
 * -reg is used rather than reg.exe because reg.exe does not exist on Win9x and
 * this helper is already staged beside every launcher.
 */

#include <windows.h>
#include <stdio.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>

#ifndef ENUM_CURRENT_SETTINGS
#define ENUM_CURRENT_SETTINGS ((DWORD)-1)
#endif
#ifndef ENUM_REGISTRY_SETTINGS
#define ENUM_REGISTRY_SETTINGS ((DWORD)-2)
#endif

typedef struct { int w, h; } MODE;

static MODE g_modes[512];
static int  g_nmodes = 0;

static void add_mode(int w, int h)
{
    int i;
    for (i = 0; i < g_nmodes; i++)
        if (g_modes[i].w == w && g_modes[i].h == h) return;
    if (g_nmodes < 512) { g_modes[g_nmodes].w = w; g_modes[g_nmodes].h = h; g_nmodes++; }
}

static int have_mode(int w, int h)
{
    int i;
    for (i = 0; i < g_nmodes; i++)
        if (g_modes[i].w == w && g_modes[i].h == h) return 1;
    return 0;
}

/* ---------------------------------------------------------------- EDID -- */

typedef struct {
    int  ok;
    int  native_w, native_h, native_hz;
    int  digital;
    int  vmax;              /* max vertical refresh from the range descriptor */
    int  hcm, vcm;          /* physical size */
    char name[16];
    char pnpid[16];
} PANEL;

static int edid_valid(const BYTE *b, DWORD n)
{
    static const BYTE hdr[8] = {0,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0};
    return (n >= 128 && memcmp(b, hdr, 8) == 0);
}

static void edid_parse(const BYTE *b, PANEL *p)
{
    int off, i;
    p->digital = (b[20] & 0x80) ? 1 : 0;
    p->hcm = b[21]; p->vcm = b[22];
    p->vmax = 0;
    p->name[0] = 0;

    for (off = 54; off <= 108; off += 18) {
        const BYTE *d = b + off;
        int px = d[0] | (d[1] << 8);
        if (px == 0) {
            if (d[3] == 0xFD) {                 /* display range limits */
                p->vmax = d[6];
            } else if (d[3] == 0xFC && !p->name[0]) {  /* monitor name */
                for (i = 0; i < 13; i++) {
                    char c = (char)d[5+i];
                    if (c == '\n' || c == 0) break;
                    p->name[i] = c;
                }
                p->name[i] = 0;
                while (i > 0 && p->name[i-1] == ' ') p->name[--i] = 0;
            }
            continue;
        }
        if (!p->native_w) {                     /* first detailed = preferred */
            int hact = d[2] | ((d[4] & 0xF0) << 4);
            int hbl  = d[3] | ((d[4] & 0x0F) << 8);
            int vact = d[5] | ((d[7] & 0xF0) << 4);
            int vbl  = d[6] | ((d[7] & 0x0F) << 8);
            double clk = px * 10000.0;
            int htot = hact + hbl, vtot = vact + vbl;
            p->native_w = hact; p->native_h = vact;
            p->native_hz = (htot && vtot) ? (int)(clk / (htot * (double)vtot) + 0.5) : 0;
        }
    }
    p->ok = (p->native_w > 0 && p->native_h > 0);
}

/* Find the active monitor's PNP id via EnumDisplayDevices, then dig its EDID
 * out of HKLM\SYSTEM\CurrentControlSet\Enum\DISPLAY\<pnpid>\<inst>.
 * The active instance is the one carrying a "Control" subkey. */
/* Read the EDID for ONE monitor PnP id out of the registry. */
static int panel_from_pnp(const char *pnp, PANEL *p, int require_active)
{
    HKEY hk, hi, hdp;
    char path[512], inst[256];
    DWORD idx, len, elen, type;
    BYTE edid[512];
    int got = 0, best = 0;

    sprintf(path, "SYSTEM\\CurrentControlSet\\Enum\\DISPLAY\\%s", pnp);
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, path, 0, KEY_READ, &hk) != ERROR_SUCCESS)
        return 0;

    for (idx = 0; ; idx++) {
        len = sizeof(inst);
        if (RegEnumKeyExA(hk, idx, inst, &len, NULL, NULL, NULL, NULL) != ERROR_SUCCESS)
            break;
        if (RegOpenKeyExA(hk, inst, 0, KEY_READ, &hi) != ERROR_SUCCESS) continue;
        {
            HKEY hc;
            int active = (RegOpenKeyExA(hi, "Control", 0, KEY_READ, &hc) == ERROR_SUCCESS);
            if (active) RegCloseKey(hc);
            if (RegOpenKeyExA(hi, "Device Parameters", 0, KEY_READ, &hdp) == ERROR_SUCCESS) {
                elen = sizeof(edid);
                if (RegQueryValueExA(hdp, "EDID", NULL, &type, edid, &elen) == ERROR_SUCCESS
                    && edid_valid(edid, elen) && (active || !require_active)) {
                    if (!got || (active && !best)) {
                        PANEL tmp; memset(&tmp, 0, sizeof(tmp));
                        edid_parse(edid, &tmp);
                        if (tmp.ok) {
                            strncpy(tmp.pnpid, pnp, sizeof(tmp.pnpid)-1);
                            *p = tmp; got = 1;
                            if (active) best = 1;
                        }
                    }
                }
                RegCloseKey(hdp);
            }
        }
        RegCloseKey(hi);
        if (best) break;
    }
    RegCloseKey(hk);
    return got;
}

/* Walk EVERY attached adapter and EVERY monitor on it, not just adapter 0 /
 * monitor 0.  Measured on .171: two monitor nodes are enumerated (a real
 * Gateway VX1120 and a Default_Monitor with no EDID) and which one comes back
 * first is not stable between runs - so a single-shot probe silently fell back
 * to the desktop mode on some runs and read the panel correctly on others. */
static int panel_probe(PANEL *p)
{
    DISPLAY_DEVICEA ad, mo;
    DWORD a, m;
    char pnp[64];

    memset(p, 0, sizeof(*p));

    for (a = 0; a < 8; a++) {
        memset(&ad, 0, sizeof(ad)); ad.cb = sizeof(ad);
        if (!EnumDisplayDevicesA(NULL, a, &ad, 0)) break;
        if (!(ad.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP)) continue;
        for (m = 0; m < 8; m++) {
            char *s2, *e2;
            memset(&mo, 0, sizeof(mo)); mo.cb = sizeof(mo);
            if (!EnumDisplayDevicesA(ad.DeviceName, m, &mo, 0)) break;
            s2 = strchr(mo.DeviceID, '\\');
            if (!s2) continue;
            s2++;
            strncpy(pnp, s2, sizeof(pnp)-1); pnp[sizeof(pnp)-1] = 0;
            e2 = strchr(pnp, '\\'); if (e2) *e2 = 0;
            if (!pnp[0] || _stricmp(pnp, "Default_Monitor") == 0) continue;
            if (panel_from_pnp(pnp, p, 0)) return 1;
        }
    }

    /* Last resort: EnumDisplayDevices can hand back Default_Monitor for a
     * panel whose EDID the registry still holds - measured on .171, where the
     * Intel 865G alternates between reporting GWY0460 and Default_Monitor from
     * one run to the next.  Scan Enum\DISPLAY directly and take any non-default
     * node with a valid EDID on an ACTIVE instance. */
    {
        HKEY hk; DWORD i2, l2; char key[256];
        if (RegOpenKeyExA(HKEY_LOCAL_MACHINE,
                "SYSTEM\\CurrentControlSet\\Enum\\DISPLAY", 0, KEY_READ, &hk)
            == ERROR_SUCCESS) {
            for (i2 = 0; ; i2++) {
                l2 = sizeof(key);
                if (RegEnumKeyExA(hk, i2, key, &l2, NULL, NULL, NULL, NULL) != ERROR_SUCCESS)
                    break;
                if (_stricmp(key, "Default_Monitor") == 0) continue;
                if (panel_from_pnp(key, p, 1)) { RegCloseKey(hk); return 1; }
            }
            RegCloseKey(hk);
        }
    }
    return 0;
}

/* ------------------------------------------------------------- policy --- */

/* Aspect CLASS from a ratio.  Physical CRT sizes are reported in whole
 * centimetres so the ratio is coarse - a 17" 4:3 tube reads 33x24 = 1.375.
 * The bands are therefore wide on purpose. */
static int aspect_class_phys(double r)   /* 43, 54, 1610, 169, or 0 */
{
    if (r > 1.15 && r < 1.45) return 43;     /* a 5:4 PANEL is physically 4:3-ish */
    if (r >= 1.45 && r < 1.68) return 1610;
    if (r >= 1.68 && r < 2.10) return 169;
    return 0;
}

/* MODE aspects are exact, so their bands must be tight.  Using the wide
 * physical bands here was a real bug: 1280x1024 (1.250) fell inside the 4:3
 * band and .171 kept being handed a 5:4 mode for a 4:3 tube. */
static int aspect_class_mode(double r)
{
    if (r > 1.320 && r < 1.348) return 43;
    if (r > 1.240 && r < 1.260) return 54;
    if (r > 1.580 && r < 1.620) return 1610;
    if (r > 1.760 && r < 1.790) return 169;
    return 0;
}

static int gcd_i(int a, int b) { while (b) { int t = a % b; a = b; b = t; } return a; }

static void aspect_str(int w, int h, char *out)
{
    int g = gcd_i(w, h), aw = w / g, ah = h / g;
    /* 1366x768 and friends reduce to something silly; snap to the classics */
    double r = (double)w / (double)h;
    if      (r > 1.760 && r < 1.790) { strcpy(out, "16:9");  return; }
    else if (r > 1.590 && r < 1.610) { strcpy(out, "16:10"); return; }
    else if (r > 1.320 && r < 1.345) { strcpy(out, "4:3");   return; }
    else if (r > 1.240 && r < 1.260) { strcpy(out, "5:4");   return; }
    if (aw > 64 || ah > 64) sprintf(out, "%.2f", r);
    else sprintf(out, "%d:%d", aw, ah);
}

/* id Tech 3 / GoldSrc horizontal FOV that preserves the 4:3 vertical FOV
 * (hor+).  4:3 @ 90 -> vfov 73.74.  hfov = 2*atan(tan(vfov/2)*aspect). */
static int horplus_fov(int w, int h)
{
    double aspect = (double)w / (double)h;
    double vhalf  = atan(0.75);                 /* tan(vfov/2) at 4:3, fov 90 */
    double hhalf  = atan(tan(vhalf) * aspect);
    int    f      = (int)(hhalf * 2.0 * 180.0 / 3.14159265358979 + 0.5);
    if (f < 90)  f = 90;
    if (f > 130) f = 130;
    return f;
}

/* Quake II / SiN / SoF share id Tech 2's FIXED mode table - no custom mode. */
static const MODE q2tab[] = {
    {320,240},{400,300},{512,384},{640,480},{800,600},
    {960,720},{1024,768},{1152,864},{1280,960},{1600,1200}
};

static int q2_mode_for(int w, int h)
{
    int i, best = 3;                            /* 640x480 floor */
    for (i = 0; i < (int)(sizeof(q2tab)/sizeof(q2tab[0])); i++)
        if (q2tab[i].w <= w && q2tab[i].h <= h) best = i;
    return best;
}

/* id TECH 3 HAS A DIFFERENT TABLE FROM id TECH 2, AND THE DIFFERENCE BITES AT
 * INDEX 8: id Tech 2's mode 8 is 1280x960 (4:3), id Tech 3's is 1280x1024
 * (5:4).  Handing FR_Q2MODE to a Quake III-family engine therefore asks a 16:9
 * panel for a 5:4 image - the exact squashed picture this whole mechanism
 * exists to remove.  Measured: r_mode 8 gives 1280x1024 on SoF2 (.123).
 *
 * Index 8 and index 11 (856x480) are deliberately skipped: one is 5:4 and the
 * other is a 16:9 mode so small that a correctly proportioned 4:3 one beats it
 * on every fleet panel.  Everything else on the list is 4:3.
 *
 * This exists at all because r_mode -1 (the custom-mode branch) IS NOT
 * UNIVERSAL in this engine family - see the launcher notes.  Where the branch
 * is missing the engine does not error, it renders 640x480. */
static const MODE q3tab[] = {
    {320,240},{400,300},{512,384},{640,480},{800,600},{960,720},
    {1024,768},{1152,864},{1280,1024},{1600,1200},{2048,1536},{856,480}
};

static int q3_mode_for(int w, int h)
{
    int i, best = 3;                            /* 640x480 floor */
    for (i = 0; i < (int)(sizeof(q3tab)/sizeof(q3tab[0])); i++) {
        if (i == 8 || i == 11) continue;        /* 5:4, and a tiny 16:9 */
        if (q3tab[i].w <= w && q3tab[i].h <= h) best = i;
    }
    return best;
}


/* ------------------------------------------------------------- GLIDE -- */
/* Is there REAL 3dfx silicon in this box?
 *
 * WHY THIS IS HERE.  Two staged titles (UnrealGold, Carmageddon2) ship a
 * game-local nGlide `glide2x.dll` wrapper.  Game-local wins at load time, so on
 * the only two boxes that still HAVE Glide silicon that wrapper shadows the
 * real system32 glide2x.dll and the game gets neither the card nor a working
 * wrapper - grSstOpen fails and UE1 silently falls back to the software
 * rasterizer at 100% CPU.  Diagnosed the expensive way on .171 (commit
 * 7823586), where it presented as "UnrealGold crashes" and was never a crash.
 * So the wrapper is not deleted - six boxes have no other Glide path - it is
 * moved aside per box, and that decision needs this measurement.
 *
 * A Voodoo 2 is Class=MEDIA and NEVER appears as a display adapter, so
 * EnumDisplayDevices and VIDEODIAG both report it absent.  The PCI enum key is
 * the only place it shows up.  Matching is on the vendor id VEN_121A, which
 * covers Voodoo Banshee/3/4/5 (display class) and Voodoo 1/2 (media class)
 * alike; a match is case-insensitive because the key spelling is not ours. */
static int g_glide_dev[64];

static int glide_probe(char *devout, int devcap)
{
    static const char *ENUM = "SYSTEM\\CurrentControlSet\\Enum\\PCI";
    HKEY hk;
    char name[256];
    DWORD i = 0, n;
    int found = 0;

    if (devout && devcap) devout[0] = 0;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, ENUM, 0, KEY_READ, &hk) != ERROR_SUCCESS)
        return 0;
    for (;; i++) {
        n = sizeof(name);
        if (RegEnumKeyExA(hk, i, name, &n, NULL, NULL, NULL, NULL) != ERROR_SUCCESS)
            break;
        /* strstr is case-sensitive; the key is written by Windows as
         * VEN_121A but a case-insensitive walk is what this repo's own rule
         * about Windows identifiers demands. */
        {
            char up[256]; int k;
            for (k = 0; name[k] && k < 255; k++)
                up[k] = (char)((name[k] >= 'a' && name[k] <= 'z') ? name[k] - 32 : name[k]);
            up[k] = 0;
            if (strstr(up, "VEN_121A")) {
                found++;
                if (devout && devcap && !devout[0]) {
                    strncpy(devout, name, devcap - 1);
                    devout[devcap - 1] = 0;
                }
            }
        }
    }
    RegCloseKey(hk);
    (void)g_glide_dev;
    return found;
}

/* Replace the first line whose first whitespace-delimited token matches KEY
 * (quotes stripped, case-insensitive) with LINE; append LINE if absent.
 * Needed for the configs that are NOT ini-shaped:
 *   Dark engine (Thief/System Shock 2)  CAM.CFG   "game_screen_size 1280 960"
 *   LithTech (Shogo)                    autoexec.cfg  "screenwidth" "1920"
 *   Quake-family .cfg                   seta r_mode "-1"                     */
static int first_token(const char *line, char *out, int cap)
{
    int n = 0;
    while (*line == ' ' || *line == '\t') line++;
    while (*line && *line != ' ' && *line != '\t' && *line != '\r' && *line != '\n') {
        if (*line != '"' && n < cap - 1) out[n++] = *line;
        line++;
    }
    out[n] = 0;
    return n;
}

/* Like first_token, but for the KEY=VALUE configs where the whole
 * "ResolutionX=1024" is a single whitespace-delimited token and first_token
 * therefore never matches.  Returns 0 (no key) for any line that is not
 * key=value, so a comment or a [section] header can never be overwritten. */
static int first_key(const char *line, char *out, int cap)
{
    int n = 0;
    while (*line == ' ' || *line == '\t') line++;
    while (*line && *line != '=' && *line != ' ' && *line != '\t'
           && *line != '\r' && *line != '\n') {
        if (*line != '"' && n < cap - 1) out[n++] = *line;
        line++;
    }
    while (*line == ' ' || *line == '\t') line++;
    if (*line != '=' || n == 0) { out[0] = 0; return 0; }
    out[n] = 0;
    return n;
}

static int do_setline_x(const char *file, const char *key, const char *line,
                        int kv)
{
    FILE *f; char *buf; long sz; long i, ls; int done = 0;
    char tok[128];
    FILE *o;
    char tmp[MAX_PATH];

    f = fopen(file, "rb");
    if (!f) return 1;
    fseek(f, 0, SEEK_END); sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz < 0 || sz > 8*1024*1024) { fclose(f); return 1; }
    buf = (char*)malloc(sz + 2);
    if (!buf) { fclose(f); return 1; }
    if (sz && fread(buf, 1, sz, f) != (size_t)sz) { fclose(f); free(buf); return 1; }
    fclose(f);
    buf[sz] = 0;

    sprintf(tmp, "%s.fr_tmp", file);
    o = fopen(tmp, "wb");
    if (!o) { free(buf); return 1; }

    ls = 0;
    for (i = 0; i <= sz; i++) {
        if (i == sz || buf[i] == '\n') {
            long end = i;                       /* [ls, end) is the line body */
            long body = end;
            if (body > ls && buf[body-1] == '\r') body--;
            {
                char save = buf[body]; buf[body] = 0;
                if (kv) first_key(buf + ls, tok, sizeof(tok));
                else    first_token(buf + ls, tok, sizeof(tok));
                buf[body] = save;
            }
            if (!done && tok[0] && _stricmp(tok, key) == 0) {
                fputs(line, o); fputs("\r\n", o);
                done = 1;
            } else if (end > ls || i < sz) {
                fwrite(buf + ls, 1, body - ls, o);
                if (i < sz) fputs("\r\n", o);
            }
            ls = i + 1;
        }
    }
    if (!done) { fputs(line, o); fputs("\r\n", o); }
    fclose(o);
    free(buf);
    remove(file);
    if (rename(tmp, file) != 0) return 1;
    return 0;
}

static int do_setline(const char *file, const char *key, const char *line)
{
    return do_setline_x(file, key, line, 0);
}

/* HKLM/HKCU only: those are the two roots every staged game uses, and a typo
 * that silently picked the wrong hive would be invisible until a box behaved
 * oddly.  An unknown root is an error, not a default. */
static int do_reg(const char *root, const char *sub, const char *val,
                  const char *type, const char *data)
{
    HKEY h, k;
    DWORD disp;
    LONG r;

    if (_stricmp(root, "HKLM") == 0)      h = HKEY_LOCAL_MACHINE;
    else if (_stricmp(root, "HKCU") == 0) h = HKEY_CURRENT_USER;
    else { fprintf(stderr, "FLEETRES: -reg root must be HKLM or HKCU\n"); return 2; }

    r = RegCreateKeyExA(h, sub, 0, NULL, REG_OPTION_NON_VOLATILE,
                        KEY_SET_VALUE, NULL, &k, &disp);
    if (r != ERROR_SUCCESS) {
        fprintf(stderr, "FLEETRES: -reg cannot open %s\\%s (%ld)\n", root, sub, (long)r);
        return 1;
    }
    if (_stricmp(type, "dword") == 0) {
        DWORD d = (DWORD)strtoul(data, NULL, 0);
        r = RegSetValueExA(k, val, 0, REG_DWORD, (const BYTE *)&d, sizeof(d));
    } else {
        r = RegSetValueExA(k, val, 0, REG_SZ, (const BYTE *)data,
                           (DWORD)strlen(data) + 1);
    }
    RegCloseKey(k);
    if (r != ERROR_SUCCESS) {
        fprintf(stderr, "FLEETRES: -reg cannot write %s (%ld)\n", val, (long)r);
        return 1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    DEVMODEA dm;
    int i;
    PANEL p;
    int desk_w = 1024, desk_h = 768, desk_bpp = 32, desk_hz = 60;
    int reg_w = 0, reg_h = 0, reg_hz = 0;  /* the PERSISTED desktop mode */
    int ov_w = 0, ov_h = 0;            /* per-box cap from the registry */
    int tgt_w, tgt_h;
    int t43_w, t43_h;
    int lcd, native_ok;
    int cap_w = 0, cap_h = 0;
    int glide_n = 0, glide_render = 0;
    char glide_dev[128];
    char asp[16], nasp[16];
    const char *mode = "-cmd";

    for (i = 1; i < argc; i++) {
        if (_stricmp(argv[i], "-cap") == 0 && i + 2 < argc) {
            cap_w = atoi(argv[i+1]); cap_h = atoi(argv[i+2]); i += 2;
        } else if (argv[i][0] == '-') {
            mode = argv[i];
            if (_stricmp(argv[i], "-ini") == 0) break;
            if (_stricmp(argv[i], "-setline") == 0) break;
            if (_stricmp(argv[i], "-kv") == 0) break;
            if (_stricmp(argv[i], "-reg") == 0) break;
        }
    }

    if (_stricmp(mode, "-ini") == 0) {
        /* -ini <file> <section> <key> <value> */
        int a = 0;
        for (i = 1; i < argc; i++) if (_stricmp(argv[i], "-ini") == 0) { a = i; break; }
        if (!a || argc < a + 5) { fprintf(stderr, "usage: -ini <file> <sec> <key> <val>\n"); return 2; }
        if (!WritePrivateProfileStringA(argv[a+2], argv[a+3], argv[a+4], argv[a+1])) {
            fprintf(stderr, "FLEETRES: ini write failed (%lu)\n", GetLastError());
            return 1;
        }
        return 0;
    }

    if (_stricmp(mode, "-setline") == 0) {
        int a = 0; char line[1024]; int k;
        for (i = 1; i < argc; i++) if (_stricmp(argv[i], "-setline") == 0) { a = i; break; }
        if (!a || argc < a + 4) { fprintf(stderr, "usage: -setline <file> <key> <line...>\n"); return 2; }
        line[0] = 0;
        for (k = a + 3; k < argc; k++) {
            if (k > a + 3) strcat(line, " ");
            strncat(line, argv[k], sizeof(line) - strlen(line) - 2);
        }
        /* cmd.exe eats double quotes out of an argument, and two of these
         * config formats REQUIRE them (LithTech writes  "screenwidth" "1920").
         * A backtick in the replacement therefore stands for a double quote -
         * cmd gives ` no special meaning, so a .bat can pass it literally. */
        for (k = 0; line[k]; k++) if (line[k] == '`') line[k] = '"';
        if (do_setline(argv[a+1], argv[a+2], line)) {
            fprintf(stderr, "FLEETRES: setline failed on %s\n", argv[a+1]);
            return 1;
        }
        return 0;
    }

    if (_stricmp(mode, "-kv") == 0) {
        /* -kv <file> <key> <value>  ->  writes "key=value" */
        int a = 0; char line[1024];
        for (i = 1; i < argc; i++) if (_stricmp(argv[i], "-kv") == 0) { a = i; break; }
        if (!a || argc < a + 4) { fprintf(stderr, "usage: -kv <file> <key> <value>\n"); return 2; }
        sprintf(line, "%.400s=%.400s", argv[a+2], argv[a+3]);
        if (do_setline_x(argv[a+1], argv[a+2], line, 1)) {
            fprintf(stderr, "FLEETRES: kv failed on %s\n", argv[a+1]);
            return 1;
        }
        return 0;
    }

    if (_stricmp(mode, "-reg") == 0) {
        int a = 0;
        for (i = 1; i < argc; i++) if (_stricmp(argv[i], "-reg") == 0) { a = i; break; }
        if (!a || argc < a + 6) {
            fprintf(stderr, "usage: -reg <HKLM|HKCU> <subkey> <value> <dword|sz> <data>\n");
            return 2;
        }
        return do_reg(argv[a+1], argv[a+2], argv[a+3], argv[a+4], argv[a+5]);
    }

    memset(&dm, 0, sizeof(dm)); dm.dmSize = sizeof(dm);
    if (EnumDisplaySettingsA(NULL, ENUM_CURRENT_SETTINGS, &dm)) {
        desk_w = dm.dmPelsWidth; desk_h = dm.dmPelsHeight;
        desk_bpp = dm.dmBitsPerPel; desk_hz = dm.dmDisplayFrequency;
    }
    reg_w = desk_w; reg_h = desk_h;
    memset(&dm, 0, sizeof(dm)); dm.dmSize = sizeof(dm);
    if (EnumDisplaySettingsA(NULL, ENUM_REGISTRY_SETTINGS, &dm)
        && dm.dmPelsWidth >= 320) {
        reg_w = dm.dmPelsWidth; reg_h = dm.dmPelsHeight;
        reg_hz = dm.dmDisplayFrequency;
    }
    for (i = 0; ; i++) {
        memset(&dm, 0, sizeof(dm)); dm.dmSize = sizeof(dm);
        if (!EnumDisplaySettingsA(NULL, i, &dm)) break;
        if (dm.dmBitsPerPel >= 16) add_mode(dm.dmPelsWidth, dm.dmPelsHeight);
    }
    /* Some drivers (measured on .143, GeForce 6800) answer ENUM_CURRENT_SETTINGS
     * but return FALSE at index 0 for the NULL device.  Retry against each
     * attached adapter by name, and always seed the list with the two modes we
     * KNOW are usable. */
    if (g_nmodes < 4) {
        DISPLAY_DEVICEA ad2;
        DWORD a;
        for (a = 0; a < 8; a++) {
            memset(&ad2, 0, sizeof(ad2)); ad2.cb = sizeof(ad2);
            if (!EnumDisplayDevicesA(NULL, a, &ad2, 0)) break;
            if (!(ad2.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP)) continue;
            for (i = 0; ; i++) {
                memset(&dm, 0, sizeof(dm)); dm.dmSize = sizeof(dm);
                if (!EnumDisplaySettingsA(ad2.DeviceName, i, &dm)) break;
                if (dm.dmBitsPerPel >= 16) add_mode(dm.dmPelsWidth, dm.dmPelsHeight);
            }
        }
    }
    add_mode(desk_w, desk_h);
    add_mode(reg_w, reg_h);

    native_ok = panel_probe(&p);

    /* Optional PER-BOX ceiling, for a machine whose 3D hardware cannot drive
     * the mode its monitor deserves (a Voodoo 2 stops at 800x600; an Intel
     * 865G will not enjoy 1152x864).  Set once on the box, never in the
     * staged tree:
     *   HKLM\Software\RetroAgent  ResCapW / ResCapH  (REG_DWORD)            */
    {
        HKEY hk; DWORD v, n = sizeof(v), t;
        if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "Software\\RetroAgent", 0, KEY_READ, &hk)
            == ERROR_SUCCESS) {
            n = sizeof(v);
            if (RegQueryValueExA(hk, "ResCapW", NULL, &t, (BYTE*)&v, &n) == ERROR_SUCCESS
                && t == REG_DWORD) ov_w = (int)v;
            n = sizeof(v);
            if (RegQueryValueExA(hk, "ResCapH", NULL, &t, (BYTE*)&v, &n) == ERROR_SUCCESS
                && t == REG_DWORD) ov_h = (int)v;
            RegCloseKey(hk);
        }
    }
    if (ov_w && ov_h && (!cap_w || ov_w < cap_w)) { cap_w = ov_w; cap_h = ov_h; }

    /* Real Glide silicon, and whether this box should RENDER on it.
     *
     * These are two different questions and conflating them would be wrong on
     * .143: it has a Voodoo5 5500 fitted, but the monitor is on a GeForce 6800
     * and the V5 is a second adapter, so rendering through Glide would draw to
     * a port nobody is looking at.  So:
     *   FR_GLIDE  = the silicon is present  -> move the game-local nGlide
     *               wrapper aside so it stops shadowing the real system32
     *               glide2x.dll.  Measured, and right on every box.
     *   FR_UE1DEV = which UE1 render device to write.  Glide only when the
     *               box says its 3dfx card is the one driving the screen:
     *               HKLM\Software\RetroAgent  GlideRender (REG_DWORD) 1.
     *               Default is D3D, which is a working renderer on every
     *               fleet GPU including .171's Intel 865G. */
    glide_n = glide_probe(glide_dev, sizeof(glide_dev));
    {
        HKEY hk; DWORD v, n2, t;
        if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "Software\\RetroAgent", 0, KEY_READ, &hk)
            == ERROR_SUCCESS) {
            n2 = sizeof(v);
            if (RegQueryValueExA(hk, "GlideRender", NULL, &t, (BYTE*)&v, &n2) == ERROR_SUCCESS
                && t == REG_DWORD) glide_render = (int)v;
            RegCloseKey(hk);
        }
    }
    if (glide_render && !glide_n) glide_render = 0;   /* asked for, not fitted */

    /* LCD test, validated against all eight fleet panels 2026-08-29:
     *   digital input bit, OR (vertical refresh capped at <=76 Hz AND the
     *   preferred timing is 60 Hz).  Every CRT here quotes 85-180 Hz and a
     *   preferred timing of 75-85; every LCD quotes <=76 and 60. */
    lcd = 0;
    if (native_ok)
        lcd = p.digital || (p.vmax && p.vmax <= 76 && p.native_hz <= 61);

    /* ---- pick the target ------------------------------------------------
     * LCD  : the panel's NATIVE mode.  Anything else is resampled by the
     *        panel's own scaler and looks soft, and a 4:3 mode on a 16:9
     *        panel is additionally stretched or pillarboxed.
     * CRT  : the largest mode that MATCHES THE TUBE'S ASPECT and does not
     *        exceed the mode the box is set up to run at.  A CRT has no pixel
     *        grid so sharpness is not the issue - geometry is: 1280x1024 (5:4)
     *        on a 4:3 tube squashes everything vertically.
     *
     * NEITHER reads the LIVE desktop mode as the target.  Measured on this
     * fleet: a game that exits without restoring leaves the desktop at
     * 640x480 (.240 did exactly that mid-survey), and a launcher that trusted
     * the live mode would then every later game to 640x480 for good. */
    if (lcd && native_ok && (have_mode(p.native_w, p.native_h) || g_nmodes <= 2)) {
        tgt_w = p.native_w; tgt_h = p.native_h;
    } else if (lcd && native_ok) {
        double want = (double)p.native_w / (double)p.native_h;
        int bw = 0, bh = 0;
        for (i = 0; i < g_nmodes; i++) {
            double r = (double)g_modes[i].w / (double)g_modes[i].h;
            if (r > want - 0.02 && r < want + 0.02 &&
                g_modes[i].w <= p.native_w && g_modes[i].w > bw) {
                bw = g_modes[i].w; bh = g_modes[i].h;
            }
        }
        if (bw) { tgt_w = bw; tgt_h = bh; } else { tgt_w = reg_w; tgt_h = reg_h; }
    } else {
        int cls = 0, bw = 0, bh = 0;
        if (native_ok && p.hcm && p.vcm)
            cls = aspect_class_phys((double)p.hcm / (double)p.vcm);
        if (!cls && native_ok)
            cls = aspect_class_mode((double)p.native_w / (double)p.native_h);
        /* NO EDID AT ALL -> ASSUME A 4:3 TUBE. Falling through to the
         * persisted mode's own aspect is what this whole tool exists to stop:
         * .133 lost its EDID after a reboot on 2026-08-30 and was instantly
         * handed 1280x1024 back - a 5:4 mode on the 4:3 tube it had been
         * measured at 37x28cm that morning.
         *
         * The assumption is safe here because a 5:4 CRT essentially does not
         * exist - 1280x1024 on a 4:3 tube is the classic mistake, not a panel
         * shape - and because a widescreen LCD cannot reach this branch: the
         * LCD test itself needs EDID, so no EDID already means `lcd == 0`.
         * FR_EDID reports which of the two answers you are getting. */
        if (!cls) cls = 43;
        if (cls) {
            for (i = 0; i < g_nmodes; i++) {
                if (aspect_class_mode((double)g_modes[i].w / (double)g_modes[i].h) != cls) continue;
                if (g_modes[i].w > reg_w || g_modes[i].h > reg_h) continue;
                if (g_modes[i].w > bw) { bw = g_modes[i].w; bh = g_modes[i].h; }
            }
        }
        if (bw) { tgt_w = bw; tgt_h = bh; } else { tgt_w = reg_w; tgt_h = reg_h; }
    }

    if (cap_w && cap_h && (tgt_w > cap_w || tgt_h > cap_h)) {
        double want = (double)tgt_w / (double)tgt_h;
        int bw = 0, bh = 0;
        for (i = 0; i < g_nmodes; i++) {
            double r = (double)g_modes[i].w / (double)g_modes[i].h;
            if (r > want - 0.02 && r < want + 0.02 &&
                g_modes[i].w <= cap_w && g_modes[i].h <= cap_h && g_modes[i].w > bw) {
                bw = g_modes[i].w; bh = g_modes[i].h;
            }
        }
        if (bw) { tgt_w = bw; tgt_h = bh; }
        else    { tgt_w = cap_w; tgt_h = cap_h; }
    }

    /* The largest 4:3 mode that fits inside the target.  Several engines here
     * have a FIXED 4:3 mode table and no widescreen support at all (Quake II,
     * SiN, Soldier of Fortune, the pre-NewDark Dark engine, Red Alert 2), so
     * for those the honest best is a correctly-proportioned 4:3 mode rather
     * than a stretched 16:9 one. */
    /* Restricted to the CLASSIC 4:3 ladder on purpose.  A free scan picks up
     * vendor oddballs - .240's ATI offers 1360x1024 (1.328, inside any sane
     * 4:3 tolerance) which no 1999 engine has ever heard of.  These six are
     * what the era's mode tables actually contain. */
    {
        static const MODE ladder[] = {
            {640,480},{800,600},{1024,768},{1152,864},{1280,960},{1600,1200}
        };
        int k;
        t43_w = 640; t43_h = 480;
        for (k = 0; k < (int)(sizeof(ladder)/sizeof(ladder[0])); k++) {
            if (ladder[k].w > tgt_w || ladder[k].h > tgt_h) continue;
            if (!have_mode(ladder[k].w, ladder[k].h) && g_nmodes > 2) continue;
            t43_w = ladder[k].w; t43_h = ladder[k].h;
        }
    }

    aspect_str(tgt_w, tgt_h, asp);
    if (native_ok) aspect_str(p.native_w, p.native_h, nasp); else strcpy(nasp, "?");

    if (_stricmp(mode, "-info") == 0) {
        printf("panel      : %s  pnp=%s  %s\n",
               native_ok ? (p.name[0] ? p.name : "(unnamed)") : "(no EDID)",
               native_ok ? p.pnpid : "-", lcd ? "LCD/flat panel" : "CRT");
        if (native_ok) {
            printf("native     : %dx%d @%dHz  aspect %s  size %dx%dcm  input %s  vmax %dHz\n",
                   p.native_w, p.native_h, p.native_hz, nasp, p.hcm, p.vcm,
                   p.digital ? "digital" : "analog", p.vmax);
        }
        printf("desktop set: %dx%d  (persisted)\n", reg_w, reg_h);
        printf("desktop now: %dx%d %dbpp @%dHz  aspect ", desk_w, desk_h, desk_bpp, desk_hz);
        { char a2[16]; aspect_str(desk_w, desk_h, a2); printf("%s%s\n", a2,
            (native_ok && (desk_w != p.native_w || desk_h != p.native_h)) ? "  *** NOT NATIVE ***" : ""); }
        printf("target     : %dx%d  aspect %s  fov(hor+) %d\n",
               tgt_w, tgt_h, asp, horplus_fov(tgt_w, tgt_h));
        printf("target 4:3 : %dx%d  (for the engines with no widescreen)  q2mode %d  q3mode %d (%dx%d)\n",
               t43_w, t43_h, q2_mode_for(t43_w, t43_h),
               q3_mode_for(t43_w, t43_h),
               q3tab[q3_mode_for(t43_w, t43_h)].w,
               q3tab[q3_mode_for(t43_w, t43_h)].h);
        printf("glide      : %s%s%s  render device %s\n",
               glide_n ? "3dfx silicon PRESENT " : "no 3dfx silicon",
               glide_n ? glide_dev : "",
               glide_n ? "" : "",
               glide_render ? "GlideDrv" : "D3DDrv");
        printf("modes(%d)  :", g_nmodes);
        for (i = 0; i < g_nmodes; i++) printf(" %dx%d", g_modes[i].w, g_modes[i].h);
        printf("\n");
        return 0;
    }

    /* -cmd : a batch fragment to CALL */
    /* EVERY line is `set "VAR=value"`, quoted, and it has to be.
     *
     * cmd.exe splits an UNQUOTED `set` on & | < > ^, and FR_GLIDEDEV carries a
     * PCI instance id full of ampersands:
     *
     *     set FR_GLIDEDEV=VEN_121A&DEV_0002&SUBSYS_00000000&REV_02
     *
     * `call fleetres.cmd` therefore set FR_GLIDEDEV to just "VEN_121A" and
     * then tried to RUN "DEV_0002" and "SUBSYS_00000000" as commands - three
     * "is not recognized" lines in the console of every launcher on .171, and
     * a silently truncated variable that any Glide test would then read wrong.
     * FR_MON is EDID text straight off the monitor and is equally exposed.
     *
     * `set "VAR=value"` does NOT put the quotes into the value, so the numeric
     * lines are quoted too - uniformity is worth more here than saving two
     * characters, because the next variable somebody adds will be a string. */
    printf("set \"FR_W=%d\"\n",        tgt_w);
    printf("set \"FR_H=%d\"\n",        tgt_h);
    printf("set \"FR_BPP=%d\"\n",      desk_bpp);
    /* The refresh of the PERSISTED desktop mode, for the engines whose mode
     * switch takes one - Halo's `-vidmode w,h,hz`. A hardcoded 60 there is a
     * staged constant like any other and is simply wrong on the CRT boxes,
     * which run 75-100 Hz. 0 or a nonsense value from a driver is reported as
     * 60 rather than passed on. */
    printf("set \"FR_HZ=%d\"\n",
           (reg_hz >= 50 && reg_hz <= 240) ? reg_hz
           : ((desk_hz >= 50 && desk_hz <= 240) ? desk_hz : 60));
    printf("set \"FR_ASPECT=%s\"\n",   asp);
    printf("set \"FR_PANEL=%s\"\n",    lcd ? "LCD" : "CRT");
    printf("set \"FR_NATIVE_W=%d\"\n", native_ok ? p.native_w : desk_w);
    printf("set \"FR_NATIVE_H=%d\"\n", native_ok ? p.native_h : desk_h);
    printf("set \"FR_DESK_W=%d\"\n",   reg_w);
    printf("set \"FR_DESK_H=%d\"\n",   reg_h);
    printf("set \"FR_LIVE_W=%d\"\n",   desk_w);
    printf("set \"FR_LIVE_H=%d\"\n",   desk_h);
    printf("set \"FR_FOV=%d\"\n",      horplus_fov(tgt_w, tgt_h));
    printf("set \"FR_W43=%d\"\n",      t43_w);
    printf("set \"FR_H43=%d\"\n",      t43_h);
    printf("set \"FR_Q2MODE=%d\"\n",   q2_mode_for(t43_w, t43_h));
    printf("set \"FR_Q3MODE=%d\"\n",   q3_mode_for(t43_w, t43_h));
    printf("set \"FR_WIDE=%d\"\n",     (tgt_w * 3 > tgt_h * 4 + tgt_h / 8) ? 1 : 0);
    printf("set \"FR_DOSFULLRES=%s\"\n", lcd ? "desktop" : "original");
    printf("set \"FR_MON=%s\"\n",      native_ok && p.name[0] ? p.name : "unknown");
    /* 1 = the panel was MEASURED (EDID present); 0 = every answer above is an
     * inference from the persisted desktop mode plus a 4:3 assumption. Two
     * different confidences should not look identical to a reader. */
    printf("set \"FR_EDID=%d\"\n",    native_ok ? 1 : 0);
    printf("set \"FR_GLIDE=%d\"\n",   glide_n ? 1 : 0);
    printf("set \"FR_GLIDEDEV=%s\"\n", glide_n ? glide_dev : "none");
    printf("set \"FR_UE1DEV=%s\"\n",
           glide_render ? "GlideDrv.GlideRenderDevice" : "D3DDrv.D3DRenderDevice");
    return 0;
}
