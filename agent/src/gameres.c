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
#include "hostpolicy.h"
#include "../shared/grledger.h"

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
    gr_panel_t gp;                  /* the panel as gr_decide() saw it - kept
                                     * so VERIFY can re-decide a launcher's
                                     * -cap exactly as FLEETRES.EXE does */
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

    g_gr.gp = p;
    gr_decide(&p, &g_gr.modes, g_gr.reg_w, g_gr.reg_h, g_gr.reg_hz,
              g_gr.live_bpp, g_gr.cap_w, g_gr.cap_h, &g_gr.t);
    /* The OS is not the monitor, so gr_decide cannot know it - but Serious
     * Engine 1's gfx_iRefreshRate depends on it (gr_se1_hz: a rate makes the
     * game unstartable on Windows 7). host_os_version is RtlGetVersion, else
     * GetVersionEx on 9x; FLEETRES.EXE asks the same way, and the test is
     * major >= 6, which the GetVersionEx shim (6.2 on 10/11) cannot flip. */
    {
        DWORD maj = 0, mnr = 0, bld = 0;
        if (host_os_version(&maj, &mnr, &bld))
            g_gr.t.os_major = (int)maj;
    }
    g_gr.probed = 1;

    log_msg(LOG_GR, "panel %s %s%s  native %dx%d@%d  persisted %dx%d"
                    "  modes %d  ->  target %dx%d (%s), 4:3 %dx%d, "
                    "q2mode %d, q2wide %d, q3mode %d, fov %d",
            g_gr.panel.ok ? (g_gr.panel.name[0] ? g_gr.panel.name : "(unnamed)")
                          : "(NO EDID - assuming a 4:3 tube)",
            g_gr.t.lcd ? "LCD" : "CRT",
            g_gr.cap_w ? " [capped]" : "",
            g_gr.panel.native_w, g_gr.panel.native_h, g_gr.panel.native_hz,
            g_gr.reg_w, g_gr.reg_h, g_gr.modes.n,
            g_gr.t.w, g_gr.t.h, g_gr.t.aspect, g_gr.t.w43, g_gr.t.h43,
            g_gr.t.q2mode, g_gr.t.q2wide, g_gr.t.q3mode, g_gr.t.fov);
    log_msg(LOG_GR, "refresh: %d Hz at %dx%d, %d Hz at %dx%d, %d/%d Hz at the "
                    "id Tech 2/3 index modes (%s), %d Hz at the desktop %dx%d "
                    "(panel max %d Hz%s); desktop is persisted at %d Hz",
            g_gr.t.hz, g_gr.t.w, g_gr.t.h,
            g_gr.t.hz43, g_gr.t.w43, g_gr.t.h43,
            g_gr.t.hzq2, g_gr.t.hzq3, gr_hz_src_name(g_gr.t.hz_src),
            g_gr.t.desk_hz, g_gr.reg_w, g_gr.reg_h,
            g_gr.panel.vmax, g_gr.panel.ok ? "" : " - NOT MEASURED",
            g_gr.t.fr_hz);
    log_msg(LOG_GR, "Serious Engine refresh (TFE gfx_iRefreshRate, TSE gap_iRefreshRate): %d (Windows NT major %d%s)",
            gr_se1_hz(g_gr.t.fr_hz, g_gr.t.os_major), g_gr.t.os_major,
            g_gr.t.os_major >= GR_SE1_NO_RATE_FROM_NT_MAJOR
                ? " - no rate: Windows 7 refuses the engine's mode switch with one"
                : "");
}

const gr_target_t *gameres_target(void)
{
    if (!g_gr.probed)
        gameres_probe();
    return &g_gr.t;
}

/* ---------------------------------------------------------------------- */
/* the ledger: which files THIS pass rewrote, so GAMESYNC keeps them        */
/* ---------------------------------------------------------------------- */

/*
 * GAMESYNC AND THIS PASS USED TO UNDO EACH OTHER ON EVERY SYNC. The pass
 * rewrites a title's config for this box's monitor, which leaves the file
 * different from the library's copy - so GAMESYNC's resume test (size AND
 * mtime) copied the library's copy back at the next sync, and this pass
 * changed it again. On .110 (agent 1.90.0, 2026-09-28) five syncs with nothing
 * else changing wrote 22/20/11/11/20 files, this pass reported 23 values
 * changed every time, and every sync rebuilt the icon layout.
 *
 * So each file this pass rewrites is recorded here - its state before the
 * write (the library's copy as GAMESYNC stamped it) and after - and
 * gs_copy_file() leaves such a file alone while BOTH still hold. The rules,
 * and why a missing or damaged ledger only ever costs one more copy, are in
 * agent/shared/grledger.h.
 *
 * Kept in C:\RETRO_AGENT (per box, never staged), text, one checksummed record
 * per line, rewritten through a temporary file with DeleteFileA + MoveFileA -
 * MoveFileExA does not exist on Win9x.
 */
#define GR_LEDGER_PATH "C:\\RETRO_AGENT\\GRLEDGER.TXT"
#define GR_LEDGER_TMP  "C:\\RETRO_AGENT\\GRLEDGER.TMP"

static grl_t            g_grl;
static CRITICAL_SECTION g_grl_lock;
static volatile LONG    g_grl_ready;
static int              g_grl_loaded;

void gameres_init(void)
{
    if (!g_grl_ready) {
        InitializeCriticalSection(&g_grl_lock);
        g_grl_ready = 1;
    }
}

static int gr_stat(const char *path, long long *size, long long *mtime)
{
    WIN32_FILE_ATTRIBUTE_DATA ad;
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &ad)
        || (ad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
        return 0;
    *size  = ((long long)ad.nFileSizeHigh << 32) | ad.nFileSizeLow;
    *mtime = gsr_ft64(ad.ftLastWriteTime.dwHighDateTime,
                      ad.ftLastWriteTime.dwLowDateTime);
    return 1;
}

/* Caller holds g_grl_lock. Loaded once per process: this process is the only
 * writer, so after that the table in memory is the truth. */
static void gr_ledger_load_locked(void)
{
    HANDLE h;
    DWORD  size, got = 0;
    char  *buf;
    int    n, bad = 0;

    if (g_grl_loaded)
        return;
    g_grl_loaded = 1;
    grl_reset(&g_grl);
    h = CreateFileA(GR_LEDGER_PATH, GENERIC_READ, FILE_SHARE_READ, NULL,
                    OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return;                         /* none yet: the old behaviour */
    size = GetFileSize(h, NULL);
    if (size == 0xFFFFFFFF || size > GRL_FILE_MAX) {
        CloseHandle(h);
        log_msg(LOG_GR, "ledger %s is not one of ours (%lu bytes) - ignored; "
                        "adjusted files will be re-copied once",
                GR_LEDGER_PATH, (unsigned long)size);
        g_grl.dirty = 1;                /* rewritten clean at the next save */
        return;
    }
    buf = (char *)HeapAlloc(GetProcessHeap(), 0, size + 1);
    if (buf && !ReadFile(h, buf, size, &got, NULL))
        got = 0;
    CloseHandle(h);
    if (!buf)
        return;
    buf[got] = 0;
    n = grl_parse(&g_grl, buf, got, &bad);
    HeapFree(GetProcessHeap(), 0, buf);
    if (n < 0) {
        log_msg(LOG_GR, "ledger %s is damaged (no header) - ignored; adjusted "
                        "files will be re-copied once", GR_LEDGER_PATH);
        g_grl.dirty = 1;
    } else if (bad) {
        log_msg(LOG_GR, "ledger %s: %d damaged record(s) dropped, %d kept - "
                        "those files will be re-copied once", GR_LEDGER_PATH,
                bad, n);
    }
}

/* Is there a record for this destination? Copies it out, so the caller never
 * holds a pointer into a table another thread may change. */
int gameres_ledger_lookup(const char *dst, long long *base_size,
                          long long *base_time, long long *out_size,
                          long long *out_time)
{
    int i, found = 0;
    gameres_init();
    EnterCriticalSection(&g_grl_lock);
    gr_ledger_load_locked();
    i = grl_find(&g_grl, dst);
    if (i >= 0) {
        *base_size = g_grl.e[i].base_size;
        *base_time = g_grl.e[i].base_time;
        *out_size  = g_grl.e[i].out_size;
        *out_time  = g_grl.e[i].out_time;
        found = 1;
    }
    LeaveCriticalSection(&g_grl_lock);
    return found;
}

void gameres_ledger_forget(const char *dst)
{
    gameres_init();
    EnterCriticalSection(&g_grl_lock);
    gr_ledger_load_locked();
    grl_forget(&g_grl, dst);
    LeaveCriticalSection(&g_grl_lock);
}

static void gr_ledger_note(const char *path, long long pre_size, long long pre_time)
{
    long long post_size, post_time;
    if (!gr_stat(path, &post_size, &post_time))
        return;
    gameres_init();
    EnterCriticalSection(&g_grl_lock);
    gr_ledger_load_locked();
    grl_note(&g_grl, path, pre_size, pre_time, post_size, post_time);
    LeaveCriticalSection(&g_grl_lock);
}

/* Write the ledger if it changed. A failure is logged and costs nothing but
 * one more copy of each adjusted file at the next sync. */
void gameres_ledger_save(void)
{
    char  *buf;
    int    len, i;
    HANDLE h;
    DWORD  wr = 0;
    BOOL   ok;

    gameres_init();
    EnterCriticalSection(&g_grl_lock);
    gr_ledger_load_locked();
    /* A record for a file that is no longer there describes nothing. */
    for (i = g_grl.n - 1; i >= 0; i--)
        if (GetFileAttributesA(g_grl.e[i].path) == 0xFFFFFFFF)
            grl_remove(&g_grl, i);
    if (!g_grl.dirty) {
        LeaveCriticalSection(&g_grl_lock);
        return;
    }
    buf = (char *)HeapAlloc(GetProcessHeap(), 0, GRL_FILE_MAX + 1);
    len = buf ? grl_format(&g_grl, buf, GRL_FILE_MAX + 1) : -1;
    if (len < 0) {
        if (buf)
            HeapFree(GetProcessHeap(), 0, buf);
        LeaveCriticalSection(&g_grl_lock);
        log_msg(LOG_GR, "ledger: could not format %d record(s) - not saved",
                g_grl.n);
        return;
    }
    CreateDirectoryA("C:\\RETRO_AGENT", NULL);
    h = CreateFileA(GR_LEDGER_TMP, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL, NULL);
    ok = h != INVALID_HANDLE_VALUE
      && WriteFile(h, buf, (DWORD)len, &wr, NULL) && wr == (DWORD)len;
    if (h != INVALID_HANDLE_VALUE)
        CloseHandle(h);
    HeapFree(GetProcessHeap(), 0, buf);
    if (ok) {
        DeleteFileA(GR_LEDGER_PATH);
        ok = MoveFileA(GR_LEDGER_TMP, GR_LEDGER_PATH);
    }
    if (ok) {
        g_grl.dirty = 0;
    } else {
        DeleteFileA(GR_LEDGER_TMP);
        log_msg(LOG_GR, "ledger: could not write %s (error %lu) - the files "
                        "this pass adjusted will be re-copied at the next sync",
                GR_LEDGER_PATH, (unsigned long)GetLastError());
    }
    LeaveCriticalSection(&g_grl_lock);
}

/* Which title's rule owns this registry value? NULL if none. */
const char *gameres_reg_owner(const char *root, const char *subkey,
                              const char *name)
{
    return gr_reg_owner(root, subkey, name);
}

/* ---------------------------------------------------------------------- */
/* checks - the ONE answer to "is this value right?", shared by the         */
/* writers (write only when it is not) and by GAMERES VERIFY (report it).   */
/* The Win32-free comparisons are in gameres.h (gr_line_check,              */
/* gr_cfg_check, gr_reg_cmp); these wrap them in the file/registry reads.   */
/* ---------------------------------------------------------------------- */

/* A whole file, NUL-terminated, from the heap (free()); NULL when missing,
 * unreadable or larger than `max`. */
static char *gr_read_file(const char *file, long max, long *len_out)
{
    FILE *f;
    char *buf;
    long  sz;

    f = fopen(file, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0 || sz > max) { fclose(f); return NULL; }
    buf = (char *)malloc((size_t)sz + 2);
    if (!buf) { fclose(f); return NULL; }
    if (sz && fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        fclose(f); free(buf); return NULL;
    }
    fclose(f);
    buf[sz] = 0;
    if (len_out) *len_out = sz;
    return buf;
}

#define GR_LINE_MAX (8L * 1024 * 1024)      /* SETLINE/KV files             */
#define GR_CFG_MAX  (256L * 1024)           /* a cfg we rewrite whole       */

/* INI: GR_ST_ABSENT when the file is not in this build; found gets the
 * current value ("" and *present 0 when the key is not there). */
static int gr_ini_check(const char *file, const char *sec, const char *key,
                        const char *val, char *found, size_t fcap,
                        int *present)
{
    char cur[256];

    if (found && fcap) found[0] = 0;
    if (present) *present = 0;
    if (GetFileAttributesA(file) == 0xFFFFFFFF)
        return GR_ST_ABSENT;                /* the title does not have it */
    cur[0] = 0;
    GetPrivateProfileStringA(sec, key, "\x01", cur, sizeof(cur), file);
    if (strcmp(cur, "\x01") != 0) {
        if (present) *present = 1;
        gr_copy_trim(cur, strlen(cur), found, fcap);
    }
    return strcmp(cur, val) == 0 ? GR_ST_OK : GR_ST_WRONG;
}

static HKEY gr_reg_root(const char *root)
{
    if (_stricmp(root, "HKLM") == 0) return HKEY_LOCAL_MACHINE;
    if (_stricmp(root, "HKCU") == 0) return HKEY_CURRENT_USER;
    return NULL;
}

/* REG: READ-ONLY (RegOpenKeyEx, never RegCreateKeyEx). A missing key or
 * value is WRONG - APPLY creates it. -1 for a rule the writer would refuse. */
static int gr_reg_check(const char *root, const char *sub, const char *name,
                        const char *spec, char *found, size_t fcap,
                        int *present)
{
    HKEY  h = gr_reg_root(root), k;
    char  sz[256];
    DWORD ty = 0, n, dv = 0;
    int   have = 0;

    if (found && fcap) found[0] = 0;
    if (present) *present = 0;
    if (!h) return -1;
    sz[0] = 0;
    if (RegOpenKeyExA(h, sub, 0, KEY_READ, &k) == ERROR_SUCCESS) {
        BYTE data[256];
        n = sizeof(data);
        if (RegQueryValueExA(k, name, NULL, &ty, data, &n) == ERROR_SUCCESS) {
            have = 1;
            if (ty == REG_DWORD && n >= sizeof(DWORD))
                memcpy(&dv, data, sizeof(DWORD));
            else if (ty == REG_SZ) {
                size_t m = n < sizeof(sz) ? (size_t)n : sizeof(sz) - 1;
                memcpy(sz, data, m);
                sz[m] = 0;
                sz[sizeof(sz) - 1] = 0;
            }
        }
        RegCloseKey(k);
    }
    if (present) *present = have;
    return gr_reg_cmp(spec, have, have && ty == REG_DWORD,
                      have && ty == REG_SZ, (int)ty, (unsigned long)dv, sz,
                      found, fcap);
}

/* ---------------------------------------------------------------------- */
/* writers - each returns 1 when it CHANGED something, 0 when the value was  */
/* already right, and -1 on a real failure. "Already right" is decided by    */
/* the check above - the same call GAMERES VERIFY reports from.              */
/* ---------------------------------------------------------------------- */

static int gr_w_ini(const char *file, const char *sec, const char *key,
                    const char *val)
{
    int st = gr_ini_check(file, sec, key, val, NULL, 0, NULL);
    if (st == GR_ST_ABSENT)
        return -1;                          /* the title does not have it */
    if (st == GR_ST_OK)
        return 0;
    if (!WritePrivateProfileStringA(sec, key, val, file))
        return -1;
    /* Win9x CACHES profile writes and flushes them when it pleases; all three
     * NULL flushes now. Without it the file's size and time read back below
     * for the ledger would be the OLD file's, and GAMESYNC would re-copy the
     * real one at the next sync. Harmless on NT, which writes through. */
    WritePrivateProfileStringA(NULL, NULL, NULL, file);
    return 1;
}

static int gr_w_line(const char *file, const char *key, const char *line,
                     int kv)
{
    FILE *o;
    char *buf;
    long  sz = 0, i, ls;
    int   done = 0;
    char  tmp[MAX_PATH];

    buf = gr_read_file(file, GR_LINE_MAX, &sz);
    if (!buf) return -1;

    /* Decide whether anything would change BEFORE touching the file. A
     * rewrite that produces identical bytes still updates the mtime, and
     * GAMESYNC's resume test is size AND mtime - so a needless rewrite here
     * would make the file re-copy from the share on the next sync forever,
     * which is precisely the never-quiet box CLAUDE.md warns about. The
     * decision is gr_line_check() - GAMERES VERIFY's own. */
    if (gr_line_check(buf, (size_t)sz, key, line, kv, NULL, 0, NULL)
            == GR_ST_OK) {
        free(buf);
        return 0;
    }

    _snprintf(tmp, sizeof(tmp) - 1, "%s.gr_tmp", file);
    tmp[sizeof(tmp) - 1] = 0;
    o = fopen(tmp, "wb");
    if (!o) { free(buf); return -1; }

    ls = 0;
    for (i = 0; i <= sz; i++) {
        if (i == sz || buf[i] == '\n') {
            long end = i, body = i;
            int  mine;
            if (body > ls && buf[body - 1] == '\r') body--;
            /* the same key match gr_line_check() made */
            mine = (i > ls) && gr_line_key_is(buf + ls, key, kv);
            if (!done && mine) {
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
    HKEY  h = gr_reg_root(root), k;
    DWORD disp;
    LONG  r;
    int   st;

    st = gr_reg_check(root, sub, name, spec, NULL, 0, NULL);
    if (st < 0) return -1;                  /* no such root, or a bad spec */
    if (st == GR_ST_OK) return 0;

    if (RegCreateKeyExA(h, sub, 0, NULL, REG_OPTION_NON_VOLATILE,
                        KEY_READ | KEY_SET_VALUE, NULL, &k, &disp)
        != ERROR_SUCCESS)
        return -1;
    if (strncmp(spec, "dword:", 6) == 0) {
        DWORD d = (DWORD)strtoul(spec + 6, NULL, 0);
        r = RegSetValueExA(k, name, 0, REG_DWORD, (const BYTE *)&d, sizeof(d));
    } else {
        const char *d = spec + 3;           /* "sz:" - gr_reg_cmp vouched */
        r = RegSetValueExA(k, name, 0, REG_SZ, (const BYTE *)d,
                           (DWORD)strlen(d) + 1);
    }
    RegCloseKey(k);
    return r == ERROR_SUCCESS ? 1 : -1;
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
 * line, comments excluded - gr_cfg_check(), which GAMERES VERIFY asks too.
 * After either writer has run with the same panel, the other finds its lines
 * present and does nothing.
 */
static int gr_w_cfg(const char *file, const char *body)
{
    FILE  *f;
    char  *cur = gr_read_file(file, GR_CFG_MAX, NULL);
    int    st  = gr_cfg_check(cur, body, NULL, 0);

    free(cur);
    if (st == GR_ST_OK) return 0;

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
    int have_pre;
    long long pre_size = 0, pre_time = 0;

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

        /* The file as it is BEFORE this rule writes it - normally the
         * library's copy exactly as GAMESYNC stamped it - for the ledger. */
        have_pre = gr_stat(path, &pre_size, &pre_time);

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
            /* Record what this rule did to a file the LIBRARY ships, so the
             * next sync keeps it instead of copying the library's back and
             * handing it to this pass again (grledger.h). A file that did not
             * exist before (a fleetres.cfg this pass creates) has no library
             * copy to protect, so nothing is recorded for it. */
            if (rc > 0 && have_pre)
                gr_ledger_note(path, pre_size, pre_time);
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
/* VERIFY - is every INSTALLED title actually set to its resolution?        */
/* ---------------------------------------------------------------------- */

/*
 * APPLY reports how many values it CHANGED; that says the pass ran, not that
 * the box is right. VERIFY answers the second question, per installed title,
 * and it is READ-ONLY: it opens registry keys KEY_READ, never creates one,
 * and never writes a file.
 *
 * It covers every directory in the games folder - not only the titles with a
 * rule - because the titles whose mode lives on a COMMAND LINE (Quake 1,
 * Half-Life, Hexen II, Descent 3, Halo, Doom 3) are served by FLEETRES.BAT in
 * their launchers and would otherwise never be looked at. For those it reads
 * the launchers and says whether each one calls FLEETRES.BAT, and what mode
 * that call produces here (the same gr_decide(), with the launcher's -cap).
 */

typedef struct {
    int  st;                    /* GR_ST_*                                    */
    const char *op;             /* reg | ini | line | kv | cfg                */
    char where[MAX_PATH];       /* file under the title, or ROOT\subkey       */
    char key[300];              /* value, "[section] key", line key; cfg ""   */
    char expected[1024];        /* expanded; a cfg's body, '\n'-separated     */
    char found[1024];           /* trimmed; a cfg's setting lines             */
    int  present;               /* found is real (else JSON null)             */
    char missing[256];          /* cfg: the first required line not present   */
} gr_chk_t;

typedef struct {
    int ok, wrong, absent;                  /* config TARGETS                */
    int titles, config, launcher_only, unmanaged;
    int titles_wrong, launcher_gaps;
    int logged, log_cap, log_on;            /* per-target log lines          */
} gr_vsum_t;

static const char *gr_op_name(int op)
{
    switch (op) {
    case GR_OP_INI:     return "ini";
    case GR_OP_SETLINE: return "line";
    case GR_OP_KV:      return "kv";
    case GR_OP_REG:     return "reg";
    case GR_OP_CFG:     return "cfg";
    default:            return "?";
    }
}

/* A cfg file's setting lines - comments and blanks dropped, each trimmed -
 * joined by '\n'. What "found" means for a file we rewrite whole. */
static void gr_cfg_settings(const char *cur, char *out, size_t cap)
{
    size_t o = 0;
    const char *l = cur;
    out[0] = 0;
    while (l && *l) {
        const char *e = l;
        char one[256];
        while (*e && *e != '\n') e++;
        gr_copy_trim(l, (size_t)(e - l), one, sizeof(one));
        if (one[0] && !(one[0] == '/' && one[1] == '/')) {
            size_t n = strlen(one);
            if (o + n + 2 >= cap) break;
            if (o) out[o++] = '\n';
            memcpy(out + o, one, n);
            o += n;
            out[o] = 0;
        }
        l = *e ? e + 1 : e;
    }
}

/*
 * Check ONE rule against the box. The same expansion, the same path and the
 * same comparison as gameres_apply_title() - so after an APPLY that reported
 * no failure, nothing here can say WRONG.
 */
static void gr_check_rule(const char *dst_dir, const gr_rule_t *r,
                          const gr_target_t *t, gr_chk_t *c)
{
    char a1[512], a2[512], a3[512], path[MAX_PATH];

    memset(c, 0, sizeof(*c));
    c->st = GR_ST_WRONG;
    c->op = gr_op_name(r->op);
    lstrcpynA(c->where, r->file ? r->file : "", sizeof(c->where));

    if (gr_expand(r->arg1 ? r->arg1 : "", t, a1, sizeof(a1))
        || gr_expand(r->arg2 ? r->arg2 : "", t, a2, sizeof(a2))
        || gr_expand(r->arg3 ? r->arg3 : "", t, a3, sizeof(a3))) {
        lstrcpynA(c->found, "(rule does not fit its buffer - APPLY fails it too)",
                  sizeof(c->found));
        c->present = 1;
        return;
    }
    _snprintf(path, sizeof(path) - 1, "%s\\%s", dst_dir, r->file);
    path[sizeof(path) - 1] = 0;

    switch (r->op) {
    case GR_OP_REG: {
        int st;
        _snprintf(c->where, sizeof(c->where) - 1, "%s\\%s", r->file, a1);
        c->where[sizeof(c->where) - 1] = 0;
        lstrcpynA(c->key, a2, sizeof(c->key));
        lstrcpynA(c->expected, a3, sizeof(c->expected));
        st = gr_reg_check(r->file, a1, a2, a3, c->found, sizeof(c->found),
                          &c->present);
        c->st = st < 0 ? GR_ST_WRONG : st;
        break;
    }
    case GR_OP_INI:
        _snprintf(c->key, sizeof(c->key) - 1, "[%s] %s", a1, a2);
        c->key[sizeof(c->key) - 1] = 0;
        lstrcpynA(c->expected, a3, sizeof(c->expected));
        c->st = gr_ini_check(path, a1, a2, a3, c->found, sizeof(c->found),
                             &c->present);
        break;
    case GR_OP_SETLINE:
    case GR_OP_KV: {
        char *buf;
        long  len = 0;
        lstrcpynA(c->key, a1, sizeof(c->key));
        if (r->op == GR_OP_KV) {
            if (gr_kv_line(a1, a2, c->expected, sizeof(c->expected))) break;
        } else {
            lstrcpynA(c->expected, a2, sizeof(c->expected));
        }
        if (GetFileAttributesA(path) == 0xFFFFFFFF) { c->st = GR_ST_ABSENT; break; }
        buf = gr_read_file(path, GR_LINE_MAX, &len);
        if (!buf) {
            lstrcpynA(c->found, "(unreadable)", sizeof(c->found));
            c->present = 1;
            break;
        }
        c->st = gr_line_check(buf, (size_t)len, a1, c->expected,
                              r->op == GR_OP_KV, c->found, sizeof(c->found),
                              &c->present);
        free(buf);
        break;
    }
    case GR_OP_CFG: {
        const char *body = gr_cfg_body(r->arg1);
        char dir[MAX_PATH], *slash, *cur;
        lstrcpynA(dir, path, sizeof(dir));
        slash = strrchr(dir, '\\');
        if (slash) {
            *slash = 0;
            if (GetFileAttributesA(dir) == 0xFFFFFFFF) {
                c->st = GR_ST_ABSENT;   /* the mod directory is not installed */
                break;
            }
        }
        if (!body || gr_expand(body, t, c->expected, sizeof(c->expected))) {
            lstrcpynA(c->found, "(cfg body does not expand)", sizeof(c->found));
            c->present = 1;
            break;
        }
        cur = gr_read_file(path, GR_CFG_MAX, NULL);
        c->st = gr_cfg_check(cur, c->expected, c->missing, sizeof(c->missing));
        if (cur) {
            gr_cfg_settings(cur, c->found, sizeof(c->found));
            c->present = 1;
            free(cur);
        }
        break;
    }
    default:
        break;
    }
}

/* A '\n'-separated list as a JSON array, comment lines dropped. */
static void gr_json_lines(json_t *j, const char *key, const char *text)
{
    const char *l = text;
    json_key(j, key);
    json_array_start(j);
    while (l && *l) {
        const char *e = l;
        char one[512];
        while (*e && *e != '\n') e++;
        gr_copy_trim(l, (size_t)(e - l), one, sizeof(one));
        if (one[0] && !(one[0] == '/' && one[1] == '/'))
            json_str(j, one);
        l = *e ? e + 1 : e;
    }
    json_array_end(j);
}

/* The launchers of one title: every .bat launch.txt names (read the way the
 * shortcut maker reads it - the first 1023 bytes) plus any "Play*.bat". */
#define GR_LMAX 32
typedef struct {
    int  with, without;
    char without_names[GR_LMAX][64];
    int  kind;                  /* from the first launcher that sets a mode */
    int  cap_w, cap_h;          /* that launcher's -cap                     */
    char via[64];               /* its name                                 */
} gr_lrep_t;

static void gr_scan_launchers(const char *dir, gr_lrep_t *o)
{
    char  names[GR_LMAX][64], path[MAX_PATH], one[MAX_PATH];
    int   n = 0, i, k;
    char *buf;
    long  len = 0;
    WIN32_FIND_DATAA fd;
    HANDLE h;

    memset(o, 0, sizeof(*o));

    _snprintf(path, sizeof(path) - 1, "%s\\launch.txt", dir);
    path[sizeof(path) - 1] = 0;
    buf = gr_read_file(path, 256 * 1024, &len);
    if (buf) {
        const char *p = buf;
        if (len > 1023) buf[1023] = 0;      /* the agent reads 1023 bytes */
        while (n < GR_LMAX && gr_launch_txt_next(&p, one, sizeof(one))) {
            int dup = 0;
            if (!gr_is_bat(one) || gr_ieq(one, "FLEETRES.BAT")) continue;
            for (k = 0; k < n; k++) if (gr_ieq(names[k], one)) dup = 1;
            if (!dup) lstrcpynA(names[n++], one, sizeof(names[0]));
        }
        free(buf);
    }
    _snprintf(path, sizeof(path) - 1, "%s\\Play*.bat", dir);
    path[sizeof(path) - 1] = 0;
    h = FindFirstFileA(path, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            int dup = 0;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            for (k = 0; k < n; k++) if (gr_ieq(names[k], fd.cFileName)) dup = 1;
            if (!dup && n < GR_LMAX)
                lstrcpynA(names[n++], fd.cFileName, sizeof(names[0]));
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }

    for (i = 0; i < n; i++) {
        gr_lscan_t sc;
        _snprintf(path, sizeof(path) - 1, "%s\\%s", dir, names[i]);
        path[sizeof(path) - 1] = 0;
        buf = gr_read_file(path, 256 * 1024, NULL);
        memset(&sc, 0, sizeof(sc));
        if (buf) {
            gr_launcher_scan(buf, &sc);
            free(buf);
        }
        if (sc.calls) {
            o->with++;
            if (o->kind == GR_TK_NONE && gr_launcher_kind(sc.uses) != GR_TK_NONE) {
                o->kind  = gr_launcher_kind(sc.uses);
                o->cap_w = sc.cap_w;
                o->cap_h = sc.cap_h;
                lstrcpynA(o->via, names[i], sizeof(o->via));
            }
        } else {
            if (o->without < GR_LMAX) {
                if (buf == NULL && GetFileAttributesA(path) == 0xFFFFFFFF)
                    _snprintf(o->without_names[o->without], 63, "%s (missing)", names[i]);
                else
                    lstrcpynA(o->without_names[o->without], names[i], 64);
                o->without_names[o->without][63] = 0;
            }
            o->without++;
        }
    }
}

static void gr_verify_log_target(gr_vsum_t *s, const char *title,
                                 const gr_chk_t *c)
{
    if (!s->log_on) return;
    if (s->logged++ >= s->log_cap) return;
    if (c->st == GR_ST_ABSENT)
        log_msg(LOG_GR, "verify: absent %s %s%s%s (not in this build)",
                title, c->where, c->key[0] ? " " : "", c->key);
    else if (!strcmp(c->op, "cfg"))
        log_msg(LOG_GR, "verify: WRONG %s %s - %s \"%s\"", title, c->where,
                c->present ? "missing" : "no file, needs", c->missing);
    else
        log_msg(LOG_GR, "verify: WRONG %s %s %s - expected %s, found %s",
                title, c->where, c->key, c->expected,
                c->present ? c->found : "(not set)");
}

/* One installed title. j may be NULL (counts and log only). */
static void gr_verify_title(const char *root, const char *title,
                            const gr_target_t *t, json_t *j, gr_vsum_t *s,
                            gr_chk_t *c)
{
    char dir[MAX_PATH], tgt[48], why[200];
    int  i, nrules = 0, ok = 0, wrong = 0, absent = 0, kind = GR_TK_NONE;
    int  engine_cap = 0;
    const char *mech;
    const gr_incap_t *inc = gr_incapable_for(title);
    gr_lrep_t lr;
    gr_res_t  res;

    _snprintf(dir, sizeof(dir) - 1, "%s\\%s", root, title);
    dir[sizeof(dir) - 1] = 0;
    gr_scan_launchers(dir, &lr);
    for (i = 0; i < GR_RULE_COUNT; i++)
        if (gr_ieq(gr_rules[i].title, title)) nrules++;

    mech = nrules ? "config" : lr.with ? "launcher" : "none";
    tgt[0] = 0;
    why[0] = 0;
    res.w = res.h = 0;

    if (nrules) {
        kind = gr_rules_kind(title);
        res  = gr_kind_res(kind, t);
    } else if (lr.with) {
        kind = lr.kind;
        if (lr.cap_w && lr.cap_h) {
            /* FLEETRES.EXE re-decides with the launcher's ceiling */
            gr_target_t lt;
            int cw, ch;
            gr_launch_cap(lr.cap_w, lr.cap_h, g_gr.cap_w, g_gr.cap_h, &cw, &ch);
            gr_decide(&g_gr.gp, &g_gr.modes, g_gr.reg_w, g_gr.reg_h,
                      g_gr.reg_hz, g_gr.live_bpp, cw, ch, &lt);
            res = gr_kind_res(kind, &lt);
        } else {
            res = gr_kind_res(kind, t);
        }
    }
    if (kind == GR_TK_DOS)
        _snprintf(tgt, sizeof(tgt) - 1, "dosbox:%s", t->lcd ? "desktop" : "original");
    else if (res.w && res.h)
        _snprintf(tgt, sizeof(tgt) - 1, "%dx%d", res.w, res.h);
    else if (inc && inc->target)
        lstrcpynA(tgt, inc->target, sizeof(tgt));
    tgt[sizeof(tgt) - 1] = 0;

    if (inc) {
        engine_cap = 1;
        lstrcpynA(why, inc->why, sizeof(why));
    } else if (kind == GR_TK_DOS) {
        engine_cap = 1;
        lstrcpynA(why, gr_kind_cap_reason(kind), sizeof(why));
    } else if (kind != GR_TK_NONE && (res.w != t->w || res.h != t->h)) {
        engine_cap = 1;
        if (!nrules && lr.cap_w)
            _snprintf(why, sizeof(why) - 1, "launcher %s caps FLEETRES at %dx%d%s%s",
                      lr.via, lr.cap_w, lr.cap_h,
                      kind != GR_TK_WIDE ? "; " : "",
                      kind != GR_TK_WIDE ? gr_kind_cap_reason(kind) : "");
        else
            lstrcpynA(why, gr_kind_cap_reason(kind), sizeof(why));
        why[sizeof(why) - 1] = 0;
    }

    s->titles++;
    if (nrules)        s->config++;
    else if (lr.with)  s->launcher_only++;
    else               s->unmanaged++;
    if ((nrules || lr.with) && lr.without)
        s->launcher_gaps++;

    if (j) {
        json_object_start(j);
        json_kv_str(j, "title", title);
        json_kv_str(j, "mechanism", mech);
        json_kv_str(j, "kind", gr_kind_name(kind));
        json_kv_str(j, "target", tgt[0] ? tgt : NULL);
        json_kv_bool(j, "engine_cap", engine_cap);
        if (engine_cap) json_kv_str(j, "cap_reason", why);
        json_kv_int(j, "launchers_with_fleetres", lr.with);
        json_key(j, "launchers_without");
        json_array_start(j);
        for (i = 0; i < lr.without && i < GR_LMAX; i++)
            json_str(j, lr.without_names[i]);
        json_array_end(j);
        if (!nrules && lr.with && lr.via[0]) {
            json_kv_str(j, "launcher", lr.via);
            if (lr.cap_w) {
                char cap[32];
                _snprintf(cap, sizeof(cap) - 1, "%dx%d", lr.cap_w, lr.cap_h);
                cap[sizeof(cap) - 1] = 0;
                json_kv_str(j, "launcher_cap", cap);
            }
        }
        if (nrules) {
            json_key(j, "targets");
            json_array_start(j);
        }
    }

    for (i = 0; i < GR_RULE_COUNT; i++) {
        if (!gr_ieq(gr_rules[i].title, title)) continue;
        gr_check_rule(dir, &gr_rules[i], t, c);
        if (c->st == GR_ST_OK)          ok++;
        else if (c->st == GR_ST_ABSENT) absent++;
        else                            wrong++;
        if (c->st != GR_ST_OK)
            gr_verify_log_target(s, title, c);
        if (j) {
            int cfg = !strcmp(c->op, "cfg");
            json_object_start(j);
            json_kv_str(j, "op", c->op);
            json_kv_str(j, "file", c->where);
            if (!cfg) json_kv_str(j, "key", c->key);
            if (cfg) gr_json_lines(j, "expected", c->expected);
            else     json_kv_str(j, "expected", c->expected);
            if (cfg && c->present) gr_json_lines(j, "found", c->found);
            else json_kv_str(j, "found", c->present ? c->found : NULL);
            if (cfg && c->st == GR_ST_WRONG) json_kv_str(j, "missing", c->missing);
            json_key(j, "ok");
            if (c->st == GR_ST_ABSENT) json_str(j, "absent");
            else                       json_bool(j, c->st == GR_ST_OK);
            json_object_end(j);
        }
    }
    s->ok += ok; s->wrong += wrong; s->absent += absent;
    if (wrong) s->titles_wrong++;

    if (j) {
        if (nrules) {
            json_array_end(j);
            json_kv_int(j, "ok", ok);
            json_kv_int(j, "wrong", wrong);
            json_kv_int(j, "absent", absent);
        }
        json_object_end(j);
    }
}

static int gr_name_cmp(const void *a, const void *b)
{
    return _stricmp((const char *)a, (const char *)b);
}

/*
 * Walk every installed title (a directory in the games folder; `_`-prefixed
 * support directories are not titles, as GAMESYNC treats them). want = one
 * title, or NULL/"" for all. Returns the number of titles, -1 when the games
 * folder is unusable.
 */
#define GR_VMAX_TITLES 256
static int gr_verify_run(const char *want, json_t *j, gr_vsum_t *s)
{
    char  root[160], pat[MAX_PATH];
    char (*names)[80];
    int   n = 0, i;
    WIN32_FIND_DATAA fd;
    HANDLE h;
    gr_chk_t *c;
    const gr_target_t *t = gameres_target();

    if (!gs_games_dir(root, sizeof(root)))
        return -1;
    names = (char (*)[80])malloc(GR_VMAX_TITLES * sizeof(*names));
    c = (gr_chk_t *)malloc(sizeof(gr_chk_t));
    if (!names || !c) { free(names); free(c); return -1; }

    _snprintf(pat, sizeof(pat) - 1, "%s\\*", root);
    pat[sizeof(pat) - 1] = 0;
    h = FindFirstFileA(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
            if (fd.cFileName[0] == '.' || fd.cFileName[0] == '_') continue;
            if (want && want[0] && !gr_ieq(want, fd.cFileName)) continue;
            if (strlen(fd.cFileName) >= sizeof(names[0])) continue;
            if (n < GR_VMAX_TITLES) lstrcpynA(names[n++], fd.cFileName, sizeof(names[0]));
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    /* FAT (every Win9x box) returns directory order, not sorted - sort, so
     * two boxes' answers line up. */
    qsort(names, (size_t)n, sizeof(names[0]), gr_name_cmp);

    if (j) {
        json_key(j, "titles");
        json_array_start(j);
    }
    for (i = 0; i < n; i++)
        gr_verify_title(root, names[i], t, j, s, c);
    if (j) json_array_end(j);

    free(names);
    free(c);
    return n;
}

/* The box's own target, as VERIFY judges against it. */
static void gr_json_box(json_t *j)
{
    char w[32];
    const gr_target_t *t = &g_gr.t;
    gr_res_t q = gr_q2wide_res(t->q2wide);

    json_key(j, "box");
    json_object_start(j);
    json_kv_str(j, "panel", t->lcd ? "LCD" : "CRT");
    json_kv_bool(j, "edid", g_gr.panel.ok);
    json_kv_str(j, "name", g_gr.panel.name);
    _snprintf(w, sizeof(w) - 1, "%dx%d", t->w, t->h);     w[sizeof(w) - 1] = 0;
    json_kv_str(j, "wide", w);
    _snprintf(w, sizeof(w) - 1, "%dx%d", t->w43, t->h43); w[sizeof(w) - 1] = 0;
    json_kv_str(j, "four_three", w);
    json_kv_str(j, "aspect", t->aspect);
    _snprintf(w, sizeof(w) - 1, "%dx%d", q.w, q.h);       w[sizeof(w) - 1] = 0;
    json_kv_str(j, "idtech2_q2wide", w);
    _snprintf(w, sizeof(w) - 1, "%dx%d", gr_turok2_sel(t->w43),
              gr_turok2_sel(t->w43) * 3 / 4);              w[sizeof(w) - 1] = 0;
    json_kv_str(j, "turok2", w);
    json_kv_str(j, "dosbox_fullresolution", t->lcd ? "desktop" : "original");
    _snprintf(w, sizeof(w) - 1, "%dx%d", g_gr.cap_w, g_gr.cap_h); w[sizeof(w) - 1] = 0;
    json_kv_str(j, "cap", w);
    json_object_end(j);
}

static void gr_json_summary(json_t *j, const gr_vsum_t *s)
{
    json_key(j, "summary");
    json_object_start(j);
    json_kv_int(j, "titles", s->titles);
    json_kv_int(j, "ok", s->ok);
    json_kv_int(j, "wrong", s->wrong);
    json_kv_int(j, "absent", s->absent);
    json_kv_int(j, "launcher_only", s->launcher_only);
    json_kv_int(j, "unmanaged", s->unmanaged);
    json_kv_int(j, "config_titles", s->config);
    json_kv_int(j, "titles_wrong", s->titles_wrong);
    json_kv_int(j, "launcher_gaps", s->launcher_gaps);
    json_object_end(j);
}

/*
 * End of a GAMESYNC run: verify once, log a summary and one line per WRONG or
 * ABSENT target (bounded), and hand back the counts for GAMESYNC STATUS.
 * Uses the target the sync already probed - no re-probe mid-run. Returns
 * the number of titles, -1 when the games folder is unusable.
 */
int gameres_verify_sync(int *wrong, int *absent)
{
    gr_vsum_t s;
    int n;

    memset(&s, 0, sizeof(s));
    s.log_on  = 1;
    s.log_cap = 40;
    n = gr_verify_run(NULL, NULL, &s);
    if (wrong)  *wrong  = s.wrong;
    if (absent) *absent = s.absent;
    if (n < 0) {
        log_msg(LOG_GR, "verify: games folder unusable - nothing verified");
        return -1;
    }
    if (s.logged > s.log_cap)
        log_msg(LOG_GR, "verify: ... %d more wrong/absent target(s) not listed "
                        "- GAMERES VERIFY has them all", s.logged - s.log_cap);
    log_msg(LOG_GR, "verify: %d title(s) installed for %dx%d - %d by config "
                    "(%d ok, %d WRONG, %d absent from this build), %d by "
                    "launcher only, %d unmanaged; %d with a launcher that "
                    "skips FLEETRES",
            n, g_gr.t.w, g_gr.t.h, s.config, s.ok, s.wrong, s.absent,
            s.launcher_only, s.unmanaged, s.launcher_gaps);
    return n;
}

/* ---------------------------------------------------------------------- */
/* GAMERES command                                                          */
/* ---------------------------------------------------------------------- */

void handle_gameres(SOCKET sock, const char *args)
{
    const char *a = str_skip_spaces(args ? args : "");
    char  json[8192];
    int   n = 0, i;

    if (str_starts_with(a, "VERIFY")) {
        const char *want = str_skip_spaces(a + 6);
        json_t j;
        gr_vsum_t s;
        int n;
        char *out;

        gameres_probe();                /* judge against the monitor NOW */
        memset(&s, 0, sizeof(s));
        json_init(&j);
        json_object_start(&j);
        json_kv_bool(&j, "verify", 1);
        if (want[0]) json_kv_str(&j, "filter", want);
        gr_json_box(&j);
        n = gr_verify_run(want, &j, &s);
        if (n < 0)
            json_kv_str(&j, "error", "games folder unusable (GamesDir set but "
                                     "unusable) - nothing verified");
        gr_json_summary(&j, &s);
        json_object_end(&j);
        out = json_finish(&j);
        send_text_response(sock, out);
        json_free(&j);
        return;
    }

    if (str_starts_with(a, "APPLY")) {
        const char *want = str_skip_spaces(a + 5);
        char  dir[MAX_PATH], root[160];
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

            if (!gs_games_dir(root, sizeof(root)))
                break;                  /* GamesDir set but unusable: nothing is "installed" */
            _snprintf(dir, sizeof(dir) - 1, "%s\\%s", root, gr_rules[i].title);
            dir[sizeof(dir) - 1] = 0;
            if (GetFileAttributesA(dir) == 0xFFFFFFFF)
                continue;               /* not installed on this box */
            titles++;
            absent1 = 0;
            changed += gameres_apply_title(dir, gr_rules[i].title, &absent1);
            absent  += absent1;
        }
        /* THE POST-CONDITION, not the count of writes: the same checks the
         * writers just made, read back. After a clean pass `wrong` is 0. */
        {
            gr_vsum_t vs;
            memset(&vs, 0, sizeof(vs));
            if (gr_verify_run(want, NULL, &vs) < 0)
                vs.wrong = vs.ok = vs.absent = -1;
            _snprintf(json, sizeof(json) - 1,
                      "{\"applied\":true,\"titles\":%d,\"values_changed\":%d,"
                      "\"targets_absent\":%d,\"target\":\"%dx%d\",\"target43\":"
                      "\"%dx%d\",\"verify\":{\"ok\":%d,\"wrong\":%d,"
                      "\"absent\":%d}}",
                      titles, changed, absent, g_gr.t.w, g_gr.t.h,
                      g_gr.t.w43, g_gr.t.h43, vs.ok, vs.wrong, vs.absent);
        }
        json[sizeof(json) - 1] = 0;
        /* What this pass rewrote must be on disk before the next sync looks,
         * or that sync copies the library's files straight back. */
        gameres_ledger_save();
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
        "\"hz_q2\":%d,\"hz_q3\":%d,\"hz_q2wide\":%d,\"hz_src\":\"%s\","
        "\"hz_desktop\":%d,\"hz_launcher\":%d,"
        "\"fov\":%d,\"q2mode\":%d,\"q2wide\":%d,\"q3mode\":%d,"
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
        g_gr.t.hz, g_gr.t.hz43, g_gr.t.hzq2, g_gr.t.hzq3, g_gr.t.hzq2wide,
        gr_hz_src_name(g_gr.t.hz_src),
        g_gr.t.desk_hz, g_gr.t.fr_hz,
        g_gr.t.fov, g_gr.t.q2mode, g_gr.t.q2wide, g_gr.t.q3mode, g_gr.t.d3ar,
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
