/*
 * gameres.c - GAMESYNC's per-box resolution pass: detect the monitor, then
 *             write each staged title's own configuration so the modes this
 *             machine's panel actually supports are the modes the game uses.
 *
 * WHERE IT RUNS AND WHY THERE. gs_run() calls gameres_apply_title() at the end
 * of each title's sync - after the tree is copied and, crucially, AFTER
 * gs_merge_reg() has applied that title's staged install.reg. The ordering is
 * the whole point: install.reg is a byte-identical constant shipped to eight
 * different monitors, and Half-Life's pins
 *
 *      HKCU\Software\Valve\Half-Life\Settings  ScreenWidth = 1024
 *
 * on every box on every sync. There is no Software\Valve\CounterStrike key at
 * all (read live on .240), so that one value is the mode for every GoldSrc
 * title on the machine - and its own comment records that Counter-Strike
 * "ignores -w/-h on the command line for the same reason". A launcher cannot
 * undo something written after it ran; only a pass at the end of the sync can.
 *
 * The decision lives in agent/shared/gameres.h so the regression test compiles
 * the same code the agent runs. This file is the Win32 half: the probe, the
 * four config writers, and the command.
 *
 * IT WRITES ONLY WHEN THE VALUE IS ACTUALLY DIFFERENT. GAMESYNC runs at every
 * startup, and a pass that rewrote thirty config files on every boot would be
 * churn indistinguishable from a fault - and worse, it would make
 * `files_written` useless as the steady-state signal CLAUDE.md relies on. Each
 * writer reads the current value first and reports "unchanged" when it matches.
 */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "protocol.h"
#include "handlers.h"
#include "log.h"
#include "util.h"
#include "../shared/edid.h"
#include "../shared/gameres.h"

#ifndef ENUM_CURRENT_SETTINGS
#define ENUM_CURRENT_SETTINGS ((DWORD)-1)
#endif
#ifndef ENUM_REGISTRY_SETTINGS
#define ENUM_REGISTRY_SETTINGS ((DWORD)-2)
#endif

#define GR_REGKEY "Software\\RetroAgent"

/* Its own log tag. GAMESYNC's LOG_GS is private to gamesync.c, and this pass
 * is worth telling apart in the log anyway - it answers a different question
 * from "did the files copy". */
#define LOG_GR "GAMERES"

typedef struct {
    gr_target_t t;
    gr_modes_t  modes;
    edid_panel_t panel;
    int  reg_w, reg_h, reg_hz;      /* the PERSISTED desktop mode */
    int  live_w, live_h, live_bpp;  /* what it is showing right now */
    int  cap_w, cap_h;              /* per-box ResCapW/ResCapH */
    int  probed;
} gr_ctx_t;

static gr_ctx_t g_gr;

/* ---------------------------------------------------------------------- */
/* probe                                                                   */
/* ---------------------------------------------------------------------- */

static int gr_reg_dword(const char *sub, const char *name, DWORD *out)
{
    HKEY  hk;
    DWORD v = 0, n = sizeof(v), ty = 0;
    int   ok = 0;

    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, sub, 0, KEY_READ, &hk)
        != ERROR_SUCCESS)
        return 0;
    if (RegQueryValueExA(hk, name, NULL, &ty, (LPBYTE)&v, &n) == ERROR_SUCCESS
        && ty == REG_DWORD) {
        *out = v;
        ok = 1;
    }
    RegCloseKey(hk);
    return ok;
}

/*
 * Enumerate every mode the driver offers. This is the "all resolutions the
 * monitor supports" half - the selector consults it so it can never ask for a
 * mode that does not exist, which on .246 made RTCW set the desktop to
 * 1280x960 and then draw into a window with r_fullscreen still 1.
 *
 * Some drivers answer ENUM_CURRENT_SETTINGS and then return FALSE at index 0
 * for the NULL device (measured on .143's GeForce 6800), so a short list is
 * retried per attached adapter by name before it is believed.
 */
static void gr_enum_modes(gr_ctx_t *c, int vmax)
{
    DEVMODEA dm;
    int i;

    gr_modes_reset(&c->modes);
    /* Before any mode is added: only the best rate per resolution is kept, so
     * a rate past what the panel can sync has to be rejected on the way in. */
    c->modes.hz_cap = vmax;
    for (i = 0; ; i++) {
        memset(&dm, 0, sizeof(dm));
        dm.dmSize = sizeof(dm);
        if (!EnumDisplaySettingsA(NULL, (DWORD)i, &dm))
            break;
        if (dm.dmBitsPerPel >= 16)
            gr_modes_add(&c->modes, (int)dm.dmPelsWidth, (int)dm.dmPelsHeight,
                         (int)dm.dmDisplayFrequency);
    }
    if (c->modes.n < 4) {
        DISPLAY_DEVICEA ad;
        DWORD a;
        for (a = 0; a < 8; a++) {
            memset(&ad, 0, sizeof(ad));
            ad.cb = sizeof(ad);
            if (!EnumDisplayDevicesA(NULL, a, &ad, 0))
                break;
            if (!(ad.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP))
                continue;
            for (i = 0; ; i++) {
                memset(&dm, 0, sizeof(dm));
                dm.dmSize = sizeof(dm);
                if (!EnumDisplaySettingsA(ad.DeviceName, (DWORD)i, &dm))
                    break;
                if (dm.dmBitsPerPel >= 16)
                    gr_modes_add(&c->modes, (int)dm.dmPelsWidth,
                                 (int)dm.dmPelsHeight,
                                 (int)dm.dmDisplayFrequency);
            }
        }
    }
    /* Whatever else is true, the two modes the box is demonstrably able to
     * show are usable. Without this a driver that enumerates nothing leaves
     * the list empty and every "is it offered?" question unanswerable. */
    gr_modes_add(&c->modes, c->live_w, c->live_h, 0);
    gr_modes_add(&c->modes, c->reg_w, c->reg_h, c->reg_hz);
}

void gameres_probe(void)
{
    DEVMODEA dm;
    gr_panel_t p;
    DWORD v;

    memset(&g_gr, 0, sizeof(g_gr));
    g_gr.live_w = 1024; g_gr.live_h = 768; g_gr.live_bpp = 32;

    memset(&dm, 0, sizeof(dm));
    dm.dmSize = sizeof(dm);
    if (EnumDisplaySettingsA(NULL, ENUM_CURRENT_SETTINGS, &dm)) {
        g_gr.live_w   = (int)dm.dmPelsWidth;
        g_gr.live_h   = (int)dm.dmPelsHeight;
        g_gr.live_bpp = (int)dm.dmBitsPerPel;
    }
    /*
     * THE TARGET COMES FROM THE PERSISTED MODE, NEVER THE LIVE ONE. A game
     * that exits without restoring leaves the desktop at 640x480 - .123 and
     * .240 were both found sitting there - and a pass that trusted the live
     * mode would then WRITE 640x480 into every game's config and pin the box
     * there permanently. The live mode is reported, and used for nothing else.
     */
    g_gr.reg_w = g_gr.live_w;
    g_gr.reg_h = g_gr.live_h;
    memset(&dm, 0, sizeof(dm));
    dm.dmSize = sizeof(dm);
    if (EnumDisplaySettingsA(NULL, ENUM_REGISTRY_SETTINGS, &dm)
        && dm.dmPelsWidth >= 320) {
        g_gr.reg_w  = (int)dm.dmPelsWidth;
        g_gr.reg_h  = (int)dm.dmPelsHeight;
        g_gr.reg_hz = (int)dm.dmDisplayFrequency;
    }

    /* EDID FIRST. The mode enumeration clamps every rate to the panel's own
     * vertical-refresh ceiling as it goes, so the ceiling has to be known
     * before a single mode is added. */
    edid_probe_panel(&g_gr.panel);
    gr_enum_modes(&g_gr, g_gr.panel.ok ? g_gr.panel.vmax : 0);

    /* Optional per-box ceiling for a machine whose 3D hardware cannot drive
     * the mode its monitor deserves - .171's 3D is a Voodoo 2 with a hard
     * 800x600 limit hiding behind an Intel 865G that no display-class scan
     * reports. Set once on the box, never in a staged tree. */
    if (gr_reg_dword(GR_REGKEY, "ResCapW", &v)) g_gr.cap_w = (int)v;
    if (gr_reg_dword(GR_REGKEY, "ResCapH", &v)) g_gr.cap_h = (int)v;

    memset(&p, 0, sizeof(p));
    p.ok        = g_gr.panel.ok;
    p.native_w  = g_gr.panel.native_w;
    p.native_h  = g_gr.panel.native_h;
    p.native_hz = g_gr.panel.native_hz;
    p.digital   = g_gr.panel.digital;
    p.vmax      = g_gr.panel.vmax;
    p.hcm       = g_gr.panel.hcm;
    p.vcm       = g_gr.panel.vcm;

    gr_decide(&p, &g_gr.modes, g_gr.reg_w, g_gr.reg_h, g_gr.reg_hz,
              g_gr.live_bpp, g_gr.cap_w, g_gr.cap_h, &g_gr.t);
    g_gr.probed = 1;

    log_msg(LOG_GR, "panel %s %s%s  native %dx%d@%d  persisted %dx%d"
                    "  modes %d  ->  target %dx%d (%s), 4:3 %dx%d, "
                    "q2mode %d, q3mode %d, fov %d",
            g_gr.panel.ok ? (g_gr.panel.name[0] ? g_gr.panel.name : "(unnamed)")
                          : "(NO EDID - assuming a 4:3 tube)",
            g_gr.t.lcd ? "LCD" : "CRT",
            g_gr.cap_w ? " [capped]" : "",
            g_gr.panel.native_w, g_gr.panel.native_h, g_gr.panel.native_hz,
            g_gr.reg_w, g_gr.reg_h, g_gr.modes.n,
            g_gr.t.w, g_gr.t.h, g_gr.t.aspect, g_gr.t.w43, g_gr.t.h43,
            g_gr.t.q2mode, g_gr.t.q3mode, g_gr.t.fov);
    log_msg(LOG_GR, "refresh: best %d Hz at %dx%d, %d Hz at %dx%d, "
                    "%d Hz at the desktop %dx%d (panel max %d Hz%s); "
                    "desktop is persisted at %d Hz",
            g_gr.t.hz, g_gr.t.w, g_gr.t.h,
            g_gr.t.hz43, g_gr.t.w43, g_gr.t.h43,
            g_gr.t.desk_hz, g_gr.reg_w, g_gr.reg_h,
            g_gr.panel.vmax, g_gr.panel.ok ? "" : " - NOT MEASURED",
            g_gr.t.fr_hz);
}

const gr_target_t *gameres_target(void)
{
    if (!g_gr.probed)
        gameres_probe();
    return &g_gr.t;
}

/* ---------------------------------------------------------------------- */
/* writers - each returns 1 when it CHANGED something, 0 when the value was  */
/* already right, and -1 on a real failure.                                  */
/* ---------------------------------------------------------------------- */

static int gr_w_ini(const char *file, const char *sec, const char *key,
                    const char *val)
{
    char cur[256];

    if (GetFileAttributesA(file) == 0xFFFFFFFF)
        return -1;                          /* the title does not have it */
    cur[0] = 0;
    GetPrivateProfileStringA(sec, key, "\x01", cur, sizeof(cur), file);
    if (strcmp(cur, val) == 0)
        return 0;
    if (!WritePrivateProfileStringA(sec, key, val, file))
        return -1;
    return 1;
}

/* The first whitespace-delimited token of a line, quotes stripped. */
static int gr_first_token(const char *line, char *out, int cap)
{
    int n = 0;
    while (*line == ' ' || *line == '\t') line++;
    while (*line && *line != ' ' && *line != '\t' && *line != '\r'
           && *line != '\n') {
        if (*line != '"' && n < cap - 1) out[n++] = *line;
        line++;
    }
    out[n] = 0;
    return n;
}

/*
 * Like gr_first_token, but for the key=value configs where the whole
 * "ResolutionX=1024" is ONE whitespace token and a first-token match therefore
 * never fires. Returns 0 for any line that is not key=value, so a comment or a
 * [section] header can never be overwritten.
 */
static int gr_first_key(const char *line, char *out, int cap)
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

static int gr_w_line(const char *file, const char *key, const char *line,
                     int kv)
{
    FILE *f, *o;
    char *buf;
    long  sz, i, ls;
    int   done = 0, changed = 0;
    char  tok[160], tmp[MAX_PATH];

    f = fopen(file, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0 || sz > 8 * 1024 * 1024) { fclose(f); return -1; }
    buf = (char *)malloc((size_t)sz + 2);
    if (!buf) { fclose(f); return -1; }
    if (sz && fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        fclose(f); free(buf); return -1;
    }
    fclose(f);
    buf[sz] = 0;

    /* Decide whether anything would change BEFORE touching the file. A
     * rewrite that produces identical bytes still updates the mtime, and
     * GAMESYNC's resume test is size AND mtime - so a needless rewrite here
     * would make the file re-copy from the share on the next sync forever,
     * which is precisely the never-quiet box CLAUDE.md warns about. */
    ls = 0;
    for (i = 0; i <= sz; i++) {
        if (i == sz || buf[i] == '\n') {
            long body = i;
            char save;
            if (body > ls && buf[body - 1] == '\r') body--;
            save = buf[body];
            buf[body] = 0;
            if (kv) gr_first_key(buf + ls, tok, sizeof(tok));
            else    gr_first_token(buf + ls, tok, sizeof(tok));
            if (tok[0] && _stricmp(tok, key) == 0) {
                if (!done && strcmp(buf + ls, line) != 0)
                    changed = 1;
                done = 1;
            }
            buf[body] = save;
            ls = i + 1;
        }
    }
    if (!done) changed = 1;             /* it has to be appended */
    if (!changed) { free(buf); return 0; }

    _snprintf(tmp, sizeof(tmp) - 1, "%s.gr_tmp", file);
    tmp[sizeof(tmp) - 1] = 0;
    o = fopen(tmp, "wb");
    if (!o) { free(buf); return -1; }

    done = 0;
    ls = 0;
    for (i = 0; i <= sz; i++) {
        if (i == sz || buf[i] == '\n') {
            long end = i, body = i;
            char save;
            if (body > ls && buf[body - 1] == '\r') body--;
            save = buf[body];
            buf[body] = 0;
            if (kv) gr_first_key(buf + ls, tok, sizeof(tok));
            else    gr_first_token(buf + ls, tok, sizeof(tok));
            buf[body] = save;
            if (!done && tok[0] && _stricmp(tok, key) == 0) {
                fputs(line, o);
                fputs("\r\n", o);
                done = 1;
            } else if (end > ls || i < sz) {
                fwrite(buf + ls, 1, (size_t)(body - ls), o);
                if (i < sz) fputs("\r\n", o);
            }
            ls = i + 1;
        }
    }
    if (!done) { fputs(line, o); fputs("\r\n", o); }
    fclose(o);
    free(buf);
    DeleteFileA(file);
    if (!MoveFileA(tmp, file)) return -1;
    return 1;
}

static int gr_w_reg(const char *root, const char *sub, const char *name,
                    const char *spec)
{
    HKEY  h, k;
    DWORD disp;
    LONG  r;
    int   changed = 1;

    if (_stricmp(root, "HKLM") == 0)      h = HKEY_LOCAL_MACHINE;
    else if (_stricmp(root, "HKCU") == 0) h = HKEY_CURRENT_USER;
    else return -1;

    if (RegCreateKeyExA(h, sub, 0, NULL, REG_OPTION_NON_VOLATILE,
                        KEY_READ | KEY_SET_VALUE, NULL, &k, &disp)
        != ERROR_SUCCESS)
        return -1;

    if (strncmp(spec, "dword:", 6) == 0) {
        DWORD d = (DWORD)strtoul(spec + 6, NULL, 0), cur = 0, n = sizeof(cur),
              ty = 0;
        if (RegQueryValueExA(k, name, NULL, &ty, (LPBYTE)&cur, &n)
                == ERROR_SUCCESS && ty == REG_DWORD && cur == d)
            changed = 0;
        r = changed ? RegSetValueExA(k, name, 0, REG_DWORD, (const BYTE *)&d,
                                     sizeof(d))
                    : ERROR_SUCCESS;
    } else if (strncmp(spec, "sz:", 3) == 0) {
        const char *d = spec + 3;
        char cur[256];
        DWORD n = sizeof(cur), ty = 0;
        cur[0] = 0;
        if (RegQueryValueExA(k, name, NULL, &ty, (LPBYTE)cur, &n)
                == ERROR_SUCCESS && ty == REG_SZ) {
            cur[sizeof(cur) - 1] = 0;
            if (strcmp(cur, d) == 0) changed = 0;
        }
        r = changed ? RegSetValueExA(k, name, 0, REG_SZ, (const BYTE *)d,
                                     (DWORD)strlen(d) + 1)
                    : ERROR_SUCCESS;
    } else {
        RegCloseKey(k);
        return -1;
    }
    RegCloseKey(k);
    if (r != ERROR_SUCCESS) return -1;
    return changed;
}

/* Does `hay` contain `line` as a WHOLE line? */
static int gr_has_line(const char *hay, const char *line)
{
    const char *p = hay;
    size_t n = strlen(line);

    if (!n) return 1;
    while ((p = strstr(p, line)) != NULL) {
        int at_start = (p == hay) || p[-1] == '\n' || p[-1] == '\r';
        char after = p[n];
        if (at_start && (after == 0 || after == '\r' || after == '\n'))
            return 1;
        p += n;
    }
    return 0;
}

/*
 * Rewrite a whole small config file - but only when a SETTING it should carry
 * is missing.
 *
 * NOT a byte comparison, and that is the whole point. This file has TWO
 * writers: the title's "Play <Game>.bat" rewrites it through FLEETRES at every
 * launch, and this pass writes it at every sync. Their bytes will never match -
 * they carry different banner comments, and Soldier of Fortune II's two
 * launchers already write two DIFFERENT bodies to the same base\fleetres.cfg
 * (the single-player one adds r_customaspect, the multiplayer one does not).
 * A byte comparison would therefore report a change on every single sync
 * forever, which is exactly the "same small non-zero count on consecutive
 * no-change syncs" that CLAUDE.md names as this project's signature invisible
 * fault - and it would bury the one signal that detects it.
 *
 * So the question asked is "are the settings I need already here", line by
 * line, comments excluded. After either writer has run with the same panel,
 * the other finds its lines present and does nothing.
 */
static int gr_w_cfg(const char *file, const char *body)
{
    FILE  *f;
    size_t need = strlen(body);
    char  *cur = NULL;
    long   sz;
    int    same = 1;

    f = fopen(file, "rb");
    if (!f) {
        same = 0;
    } else {
        fseek(f, 0, SEEK_END);
        sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (sz < 0 || sz > 256 * 1024) {
            same = 0;
        } else {
            cur = (char *)malloc((size_t)sz + 1);
            if (!cur || (sz && fread(cur, 1, (size_t)sz, f) != (size_t)sz))
                same = 0;
            else
                cur[sz] = 0;
        }
        fclose(f);
    }

    if (same && cur) {
        const char *l = body;
        while (*l) {
            const char *e = strchr(l, '\n');
            size_t n = e ? (size_t)(e - l) : strlen(l);
            char one[512];
            if (n && n < sizeof(one) && !(n >= 2 && l[0] == '/' && l[1] == '/')) {
                memcpy(one, l, n);
                one[n] = 0;
                if (!gr_has_line(cur, one)) { same = 0; break; }
            }
            if (!e) break;
            l = e + 1;
        }
    }
    free(cur);
    if (same) return 0;

    /* CRLF, because the launcher's `echo` chain writes CRLF and a config a
     * person may open in Notepad on XP should not be one long line. */
    f = fopen(file, "wb");
    if (!f) return -1;
    {
        const char *l = body;
        while (*l) {
            if (*l == '\n') fputs("\r\n", f);
            else             fputc(*l, f);
            l++;
        }
    }
    (void)need;
    fclose(f);
    return 1;
}

/* ---------------------------------------------------------------------- */
/* the desktop's own refresh rate                                           */
/* ---------------------------------------------------------------------- */

/*
 * Raise the PERSISTED desktop refresh to the highest rate this monitor
 * supports at the resolution it is already set to.
 *
 * WHY THIS AND NOT A CVAR PER GAME. Most of the library has no refresh setting
 * to write. Quake II's and GoldSrc's binaries were searched and carry no
 * refresh cvar at all - only `timerefresh` and `r_norefresh` - and Unreal
 * Engine 1 keeps `RefreshRate` solely under `[GlideDrv.GlideRenderDevice]`,
 * which is not the device these boxes render on. Those engines take whatever
 * the desktop is on, so the desktop IS the setting for them, and raising it
 * is the only thing that reaches every title at once.
 *
 * THREE RULES, EACH OF WHICH IS A WAY THIS COULD GO WRONG:
 *
 *  - UPWARD ONLY, AND NEVER THE RESOLUTION. The mode's width, height and depth
 *    are re-applied exactly as they were; only the frequency moves, and only
 *    up. A box must never come back from this pass in a mode it was not in.
 *  - NO EDID, NO CHANGE. The rate has to be inside what the panel says it can
 *    sync. Without an EDID there is no measurement, and on an analogue CRT the
 *    good outcome of guessing is "out of range" on a monitor nobody is
 *    standing in front of. gr_best_hz already returns 0 there.
 *  - VERIFY, THEN BELIEVE. ChangeDisplaySettings' return code is checked AND
 *    the persisted mode is read back, because this project's recurring fault
 *    is a call that reported success. A refusal is logged and left alone.
 *
 * Kill switch: HKLM\Software\RetroAgent  RefreshMax (REG_DWORD) = 0.
 */
static int gr_raise_desktop_refresh(void)
{
    DEVMODEA dm, back;
    DWORD sw = 1;
    LONG r;
    int want = g_gr.t.desk_hz;

    if (gr_reg_dword(GR_REGKEY, "RefreshMax", &sw) && sw == 0) {
        log_msg(LOG_GR, "desktop refresh: RefreshMax=0, leaving it alone");
        return 0;
    }
    if (!g_gr.panel.ok) {
        log_msg(LOG_GR, "desktop refresh: no EDID, so no measured ceiling - "
                        "leaving %d Hz alone rather than guessing at a tube",
                g_gr.t.fr_hz);
        return 0;
    }
    if (want <= 0 || want <= g_gr.t.fr_hz)
        return 0;                       /* already at the best on offer */

    memset(&dm, 0, sizeof(dm));
    dm.dmSize = sizeof(dm);
    if (!EnumDisplaySettingsA(NULL, ENUM_REGISTRY_SETTINGS, &dm))
        return 0;
    dm.dmDisplayFrequency = (DWORD)want;
    dm.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_BITSPERPEL
                | DM_DISPLAYFREQUENCY;

    r = ChangeDisplaySettingsA(&dm, CDS_UPDATEREGISTRY);
    if (r != DISP_CHANGE_SUCCESSFUL) {
        log_msg(LOG_GR, "desktop refresh: driver REFUSED %dx%d @%d Hz (%ld) - "
                        "left at %d Hz",
                g_gr.reg_w, g_gr.reg_h, want, (long)r, g_gr.t.fr_hz);
        return 0;
    }

    /* The post-condition, not the return value. */
    memset(&back, 0, sizeof(back));
    back.dmSize = sizeof(back);
    if (EnumDisplaySettingsA(NULL, ENUM_REGISTRY_SETTINGS, &back)
        && (int)back.dmDisplayFrequency == want) {
        log_msg(LOG_GR, "desktop refresh: %dx%d raised %d -> %d Hz "
                        "(panel max %d Hz) - every engine with no refresh "
                        "setting of its own inherits this",
                g_gr.reg_w, g_gr.reg_h, g_gr.t.fr_hz, want, g_gr.panel.vmax);
        g_gr.reg_hz  = want;
        g_gr.t.fr_hz = want;
        return 1;
    }
    log_msg(LOG_GR, "desktop refresh: asked for %d Hz and the persisted mode "
                    "reads %lu Hz - NOT applied",
            want, (unsigned long)back.dmDisplayFrequency);
    return 0;
}

/*
 * Public entry: raise the desktop refresh, then report whether it moved.
 *
 * MUST RUN BEFORE THE TITLES ARE WRITTEN. The id Tech 3 bodies carry %FRHZ%,
 * the persisted desktop rate, so that they agree byte-for-byte with what the
 * title's own launcher writes at every start. Raising the desktop first is
 * what makes that number the highest the monitor supports instead of whatever
 * the box happened to be left on.
 */
int gameres_apply_display(void)
{
    if (!g_gr.probed)
        gameres_probe();
    return gr_raise_desktop_refresh();
}

/* ---------------------------------------------------------------------- */
/* the pass                                                                */
/* ---------------------------------------------------------------------- */

/*
 * Apply every rule for one title. `dst_dir` is the installed tree
 * (C:\Games\<Title>), `title` its library directory name.
 *
 * A MISSING TARGET FILE IS NOT AN ERROR AND MUST NOT BE LOUD. The library is
 * gated per box: a title can be present with a mod directory the disk had no
 * room for, and several rules deliberately name a file only some builds ship.
 * What IS reported is a rule whose file exists and whose write failed - that
 * is the case where a resolution silently did not take.
 */
int gameres_apply_title(const char *dst_dir, const char *title,
                        int *absent_out)
{
    const gr_target_t *t = gameres_target();
    int i, changed = 0, absent = 0, failed = 0;

    for (i = 0; i < GR_RULE_COUNT; i++) {
        const gr_rule_t *r = &gr_rules[i];
        char path[MAX_PATH], a1[512], a2[512], a3[512];
        int  rc;

        if (_stricmp(r->title, title) != 0)
            continue;

        if (gr_expand(r->arg1 ? r->arg1 : "", t, a1, sizeof(a1))
            || gr_expand(r->arg2 ? r->arg2 : "", t, a2, sizeof(a2))
            || gr_expand(r->arg3 ? r->arg3 : "", t, a3, sizeof(a3))) {
            log_msg(LOG_GR, "%s rule %d does not fit its buffer - "
                            "NOT applied", title, i);
            failed++;
            continue;
        }

        if (r->op == GR_OP_REG) {
            rc = gr_w_reg(r->file, a1, a2, a3);
            if (rc < 0) {
                log_msg(LOG_GR, "%s FAILED %s\\%s \"%s\" = %s",
                        title, r->file, a1, a2, a3);
                failed++;
            } else changed += rc;
            continue;
        }

        _snprintf(path, sizeof(path) - 1, "%s\\%s", dst_dir, r->file);
        path[sizeof(path) - 1] = 0;

        switch (r->op) {
        case GR_OP_INI:
            rc = gr_w_ini(path, a1, a2, a3);
            break;
        case GR_OP_SETLINE:
            rc = (GetFileAttributesA(path) == 0xFFFFFFFF)
               ? -1 : gr_w_line(path, a1, a2, 0);
            break;
        case GR_OP_KV: {
            /* The writer replaces a whole LINE, so it needs "key=value" and
             * not the bare value - see gr_kv_line(), which records what
             * handing it the bare value did to DESCENT.CFG on .191. */
            char kvline[1024];
            if (gr_kv_line(a1, a2, kvline, sizeof(kvline))) { rc = -1; break; }
            rc = (GetFileAttributesA(path) == 0xFFFFFFFF)
               ? -1 : gr_w_line(path, a1, kvline, 1);
            break;
        }
        case GR_OP_CFG: {
            /* A cfg is CREATED if missing - it is our file, not the game's -
             * but only inside a mod directory that exists, or Quake II would
             * grow an empty xatrix/ on a box the mission pack never reached. */
            const char *body = gr_cfg_body(r->arg1);
            char dir[MAX_PATH], *slash;
            char out[1024];
            lstrcpynA(dir, path, sizeof(dir));
            slash = strrchr(dir, '\\');
            if (slash) {
                *slash = 0;
                if (GetFileAttributesA(dir) == 0xFFFFFFFF) { rc = -1; break; }
            }
            if (!body || gr_expand(body, t, out, sizeof(out))) { rc = -1; break; }
            rc = gr_w_cfg(path, out);
            break;
        }
        default:
            rc = -1;
            break;
        }

        if (rc < 0) {
            if (r->op != GR_OP_CFG
                && GetFileAttributesA(path) == 0xFFFFFFFF) {
                absent++;               /* this build simply has no such file */
            } else if (r->op == GR_OP_CFG) {
                absent++;               /* the mod directory is not installed */
            } else {
                log_msg(LOG_GR, "%s FAILED %s", title, r->file);
                failed++;
            }
        } else {
            changed += rc;
        }
    }

    if (absent_out) *absent_out = absent;
    if (changed || failed)
        log_msg(LOG_GR, "%s - %d value(s) set for %dx%d%s",
                title, changed, t->w, t->h,
                failed ? " (WITH FAILURES - see above)" : "");
    return changed;
}

/* Does the table say anything at all about this title? Used only to keep the
 * log honest about which titles the pass can and cannot serve. */
int gameres_has_rules(const char *title)
{
    int i;
    for (i = 0; i < GR_RULE_COUNT; i++)
        if (_stricmp(gr_rules[i].title, title) == 0)
            return 1;
    return 0;
}

/* ---------------------------------------------------------------------- */
/* GAMERES command                                                          */
/* ---------------------------------------------------------------------- */

void handle_gameres(SOCKET sock, const char *args)
{
    const char *a = str_skip_spaces(args ? args : "");
    char  json[8192];
    int   n = 0, i;

    if (str_starts_with(a, "APPLY")) {
        const char *want = str_skip_spaces(a + 5);
        char  dir[MAX_PATH];
        int   titles = 0, changed = 0, absent = 0, absent1;
        char  done[64][64];
        int   ndone = 0;

        gameres_probe();                /* the monitor may have changed */
        gameres_apply_display();        /* before the titles - see the note */
        for (i = 0; i < GR_RULE_COUNT; i++) {
            int seen = 0, k;
            for (k = 0; k < ndone; k++)
                if (_stricmp(done[k], gr_rules[i].title) == 0) { seen = 1; break; }
            if (seen) continue;
            if (want[0] && _stricmp(want, gr_rules[i].title) != 0) continue;
            if (ndone >= (int)(sizeof(done) / sizeof(done[0])))
                break;              /* more distinct titles than we can track */
            lstrcpynA(done[ndone++], gr_rules[i].title, sizeof(done[0]));

            _snprintf(dir, sizeof(dir) - 1, "C:\\Games\\%s", gr_rules[i].title);
            dir[sizeof(dir) - 1] = 0;
            if (GetFileAttributesA(dir) == 0xFFFFFFFF)
                continue;               /* not installed on this box */
            titles++;
            absent1 = 0;
            changed += gameres_apply_title(dir, gr_rules[i].title, &absent1);
            absent  += absent1;
        }
        _snprintf(json, sizeof(json) - 1,
                  "{\"applied\":true,\"titles\":%d,\"values_changed\":%d,"
                  "\"targets_absent\":%d,\"target\":\"%dx%d\",\"target43\":"
                  "\"%dx%d\"}",
                  titles, changed, absent, g_gr.t.w, g_gr.t.h,
                  g_gr.t.w43, g_gr.t.h43);
        json[sizeof(json) - 1] = 0;
        send_text_response(sock, json);
        return;
    }

    gameres_probe();

    /*
     * The mode list is reported in full, because "which resolutions does this
     * monitor support" is the question the operator actually has and every
     * other answer here is derived from it. It is also the thing that explains
     * a surprising target: a panel whose native mode the driver does not
     * enumerate gets the largest matching mode instead, and without the list
     * that looks arbitrary.
     */
    n = _snprintf(json, sizeof(json) - 1,
        "{\"panel\":{\"edid\":%s,\"name\":\"%s\",\"pnpid\":\"%s\","
        "\"type\":\"%s\",\"digital\":%s,\"native\":\"%dx%d\",\"native_hz\":%d,"
        "\"vmax_hz\":%d,\"size_cm\":\"%dx%d\"},"
        "\"desktop\":{\"persisted\":\"%dx%d\",\"persisted_hz\":%d,"
        "\"live\":\"%dx%d\",\"bpp\":%d},"
        "\"cap\":\"%dx%d\","
        "\"target\":{\"wide\":\"%dx%d\",\"four_three\":\"%dx%d\","
        "\"aspect\":\"%s\",\"hz\":%d,\"hz_four_three\":%d,"
        "\"hz_desktop\":%d,\"hz_launcher\":%d,"
        "\"fov\":%d,\"q2mode\":%d,\"q3mode\":%d,"
        "\"d3_aspect\":%d,\"dosbox_fullresolution\":\"%s\"},"
        "\"modes\":[",
        g_gr.panel.ok ? "true" : "false",
        g_gr.panel.name, g_gr.panel.pnpid,
        g_gr.t.lcd ? "LCD" : "CRT",
        g_gr.panel.digital ? "true" : "false",
        g_gr.panel.native_w, g_gr.panel.native_h, g_gr.panel.native_hz,
        g_gr.panel.vmax, g_gr.panel.hcm, g_gr.panel.vcm,
        g_gr.reg_w, g_gr.reg_h, g_gr.reg_hz,
        g_gr.live_w, g_gr.live_h, g_gr.live_bpp,
        g_gr.cap_w, g_gr.cap_h,
        g_gr.t.w, g_gr.t.h, g_gr.t.w43, g_gr.t.h43, g_gr.t.aspect,
        g_gr.t.hz, g_gr.t.hz43, g_gr.t.desk_hz, g_gr.t.fr_hz,
        g_gr.t.fov, g_gr.t.q2mode, g_gr.t.q3mode, g_gr.t.d3ar,
        g_gr.t.lcd ? "desktop" : "original");
    if (n < 0) n = 0;

    /* _snprintf returns -1 on truncation rather than the length it wanted, so
     * a bare `n += _snprintf(...)` walks the offset BACKWARDS and the next
     * call is handed a size that underflows to something enormous. Every
     * append is bounded and its result checked. */
    for (i = 0; i < g_gr.modes.n; i++) {
        int k;
        if (n >= (int)sizeof(json) - 80) break;
        /* WITH ITS BEST RATE. The mode list is the evidence behind both the
         * resolution and the refresh, and "1024x768" alone cannot explain why
         * one target got 120 Hz and another 75. */
        k = _snprintf(json + n, sizeof(json) - 1 - (size_t)n,
                      "%s\"%dx%d@%d\"", i ? "," : "",
                      g_gr.modes.m[i].w, g_gr.modes.m[i].h,
                      g_gr.modes.m[i].hz);
        if (k < 0) break;
        n += k;
    }
    {
        int k = _snprintf(json + n, sizeof(json) - 1 - (size_t)n,
                          "],\"rules\":%d}", GR_RULE_COUNT);
        if (k > 0) n += k;
    }
    (void)n;
    json[sizeof(json) - 1] = 0;
    send_text_response(sock, json);
}
