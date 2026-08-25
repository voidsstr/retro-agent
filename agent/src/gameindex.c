/*
 * gameindex.c - Installed-game index with a background refresh thread.
 *
 * The host's game-server pipeline needs to know, cheaply and often, which
 * games are installed on each box. Walking the disk on demand would make
 * every query cost seconds on a Pentium III, so the agent keeps a cached
 * index and refreshes it on its own schedule:
 *
 *   GAMEINDEX        -> the cached JSON, returned instantly
 *   GAMEINDEX SCAN   -> force a rescan first, then return it
 *   GAMEINDEX HASH   -> just the hash, so the host can skip an unchanged pull
 *
 * The hash is order-independent (a sum of per-entry FNV-1a hashes), so the
 * host can compare it without caring how the scan happened to enumerate.
 *
 * Three detection sources, merged and deduped by (key, dir):
 *   1. Desktop shortcuts (all-users + current user), resolved through
 *      IShellLink. This is the one that matters most: a box often has several
 *      trees of the same game and the shortcut is the one the user actually
 *      launches, which is the copy whose config we must edit.
 *   2. The registry uninstall keys, for InstallLocation.
 *   3. A depth-limited walk of the usual game roots on every fixed drive.
 *
 * Win98SE compatible: ANSI APIs only, no C99 declarations-after-statement,
 * COM used defensively (any failure just drops that source).
 */

#include "handlers.h"
#include "protocol.h"
#include "util.h"
#include "log.h"
#include <windows.h>
#include <shlobj.h>
#include <objbase.h>
#include <stdio.h>
#include <string.h>

/*
 * ole32 is bound at RUNTIME, not linked. A static import would add ole32.dll
 * to the agent's load-time dependency list, and this binary has to load on
 * everything from Win95 to Win7 - the codebase already resolves
 * SHGetSpecialFolderPathA and SetHandleInformation this way for the same
 * reason. The GUIDs are spelled out here so we do not need libuuid either.
 */
typedef HRESULT (WINAPI *coinit_t)(LPVOID);
typedef void    (WINAPI *councoinit_t)(void);
typedef HRESULT (WINAPI *cocreate_t)(REFCLSID, LPUNKNOWN, DWORD, REFIID, LPVOID *);

static const GUID GI_CLSID_ShellLink =
    { 0x00021401, 0x0000, 0x0000, { 0xC0,0,0,0,0,0,0,0x46 } };
static const GUID GI_IID_IShellLinkA =
    { 0x000214EE, 0x0000, 0x0000, { 0xC0,0,0,0,0,0,0,0x46 } };
static const GUID GI_IID_IPersistFile =
    { 0x0000010B, 0x0000, 0x0000, { 0xC0,0,0,0,0,0,0,0x46 } };

static HMODULE       g_ole32;
static coinit_t      g_CoInitialize;
static councoinit_t  g_CoUninitialize;
static cocreate_t    g_CoCreateInstance;

static int ole_load(void)
{
    if (g_ole32)
        return g_CoCreateInstance != NULL;
    g_ole32 = LoadLibraryA("ole32.dll");
    if (!g_ole32)
        return 0;
    g_CoInitialize     = (coinit_t)GetProcAddress(g_ole32, "CoInitialize");
    g_CoUninitialize   = (councoinit_t)GetProcAddress(g_ole32, "CoUninitialize");
    g_CoCreateInstance = (cocreate_t)GetProcAddress(g_ole32, "CoCreateInstance");
    return g_CoCreateInstance != NULL;
}

#define LOG_GI "GAMEINDEX"

/* Refresh cadence. The host polls every 5 minutes and wants the index no
 * staler than that, so re-scan a little more often than it asks. Overridable
 * from HKLM\Software\RetroAgent\GameIndexPeriodMs for slow boxes. */
#define GI_FIRST_DELAY_MS   20000
#define GI_PERIOD_MS_DEF    240000
#define GI_PERIOD_MS_MIN    60000

#define GI_MAX_GAMES        256
#define GI_MAX_DEPTH        3

/* ---------------------------------------------------------------------- */
/* Known-game signatures                                                   */
/* ---------------------------------------------------------------------- */

/*
 * `engine` tells the host which server-query protocol and which favorites
 * mechanism apply. "-" means we can detect the game but have no server
 * browser to populate, which is still worth reporting.
 *
 * `moddir`, when set, must exist as a subdirectory next to the exe. That is
 * how the GoldSrc family is split apart: Half-Life, Counter-Strike and The
 * Specialists are all hl.exe, distinguished only by the mod directory.
 */
typedef struct {
    const char *key;
    const char *name;
    const char *exe;
    const char *moddir;
    const char *engine;
} game_sig_t;

static const game_sig_t g_sigs[] = {
    /* Quake III engine - getstatus, favorites via autoexec.cfg server1..16 */
    { "quake3",     "Quake III Arena",        "quake3.exe",           "baseq3",   "q3" },
    { "quake3",     "Quake III Arena",        "quake3.exe",           NULL,       "q3" },
    { "ioquake3",   "ioquake3",               "ioquake3.exe",         NULL,       "q3" },
    { "openarena",  "OpenArena",              "openarena.exe",        NULL,       "q3" },
    { "openarena",  "OpenArena",              "oa_ded.exe",           NULL,       "q3" },
    { "wolfmp",     "Return to Castle Wolfenstein", "WolfMP.exe",     NULL,       "rtcw" },
    { "wolfsp",     "RtCW (single player)",   "WolfSP.exe",           NULL,       "-" },
    { "et",         "Wolfenstein: Enemy Territory", "ET.exe",         NULL,       "q3" },
    { "jk2",        "Jedi Knight II",         "jk2mp.exe",            NULL,       "q3" },
    { "jka",        "Jedi Academy",           "jamp.exe",             NULL,       "q3" },
    { "sof2",       "Soldier of Fortune II",  "sof2mp.exe",           NULL,       "q3" },

    /* GoldSrc - A2S, favorites via the ServerBrowser vdf */
    { "cs16",       "Counter-Strike 1.6",     "hl.exe",               "cstrike",  "goldsrc" },
    { "ts",         "The Specialists",        "hl.exe",               "ts",       "goldsrc" },
    { "dod",        "Day of Defeat",          "hl.exe",               "dod",      "goldsrc" },
    { "tfc",        "Team Fortress Classic",  "hl.exe",               "tfc",      "goldsrc" },
    { "halflife",   "Half-Life",              "hl.exe",               "valve",    "goldsrc" },

    /* Quake II - `status`, favorites via a config alias */
    { "quake2",     "Quake II",               "quake2.exe",           NULL,       "q2" },
    { "q2pro",      "Q2PRO",                  "q2pro.exe",            NULL,       "q2" },
    { "yquake2",    "Yamagi Quake II",        "yquake2.exe",          NULL,       "q2" },

    /* Quake / QuakeWorld */
    { "quakeworld", "QuakeWorld",             "qwcl.exe",             NULL,       "qw" },
    { "quakeworld", "QuakeWorld",             "glqwcl.exe",           NULL,       "qw" },
    { "ezquake",    "ezQuake",                "ezquake-gl.exe",       NULL,       "qw" },
    { "quake",      "Quake",                  "glquake.exe",          NULL,       "nq" },
    { "quake",      "Quake",                  "winquake.exe",         NULL,       "nq" },

    /* Unreal engine - GameSpy query on port+1, favorites in the ini */
    { "ut99",       "Unreal Tournament (99)", "UnrealTournament.exe", NULL,       "unreal" },
    { "unreal",     "Unreal",                 "Unreal.exe",           NULL,       "unreal" },
    { "ut2003",     "Unreal Tournament 2003", "UT2003.exe",           NULL,       "ut2k4" },
    { "ut2004",     "Unreal Tournament 2004", "UT2004.exe",           NULL,       "ut2k4" },
    { "deusex",     "Deus Ex",                "DeusEx.exe",           NULL,       "-" },

    /* Others with live masters */
    { "tribes2",    "Tribes 2",               "Tribes2.exe",          NULL,       "t2" },
    { "mohaa",      "Medal of Honor: AA",     "MOHAA.exe",            NULL,       "q3" },

    /* Detected, but nothing to populate: no master or no favorites store */
    { "doom2",      "DOOM II",                "doom2.exe",            NULL,       "-" },
    { "zdoom",      "ZDoom",                  "zdoom.exe",            NULL,       "-" },
    { "gzdoom",     "GZDoom",                 "gzdoom.exe",           NULL,       "-" },
    { "prboom",     "PrBoom",                 "prboom.exe",           NULL,       "-" },
    { "heretic",    "Heretic",                "heretic.exe",          NULL,       "-" },
    { "hexen",      "Hexen",                  "hexen.exe",            NULL,       "-" },
    { "starcraft",  "StarCraft",              "StarCraft.exe",        NULL,       "-" },
    { "diablo2",    "Diablo II",              "Diablo II.exe",        NULL,       "-" },
    { "bf1942",     "Battlefield 1942",       "BF1942.exe",           NULL,       "-" },
    { "aoe2",       "Age of Empires II",      "empires2.exe",         NULL,       "-" },
    { "hl2",        "Half-Life 2",            "hl2.exe",              NULL,       "-" },
    { "sam",        "Serious Sam",            "SeriousSam.exe",       NULL,       "-" },
    { "nfs3",       "Need for Speed III",     "nfs3.exe",             NULL,       "-" },
    { "3dmark2000", "3DMark2000",             "3DMark2000.exe",       NULL,       "-" },
    { NULL,         NULL,                     NULL,                   NULL,       NULL }
};

/* Directories worth walking, relative to each fixed drive's root. */
static const char *g_roots[] = {
    "",
    "Games",
    "Program Files",
    "Program Files (x86)",
    "Program Files\\Games",
    "GOG Games",
    NULL
};

/* ---------------------------------------------------------------------- */
/* Collected entries                                                       */
/* ---------------------------------------------------------------------- */

typedef struct {
    char key[32];
    char name[64];
    char dir[MAX_PATH];
    char exe[MAX_PATH];
    char engine[16];
    char launcher[MAX_PATH];   /* resolved desktop shortcut, when there is one */
    char source[16];
} game_ent_t;

static game_ent_t  g_ents[GI_MAX_GAMES];
static int         g_ent_count;

static CRITICAL_SECTION g_gi_lock;
static int    g_gi_lock_ready;
static char  *g_gi_json;        /* cached, heap-allocated */
static DWORD  g_gi_hash;
static DWORD  g_gi_scanned_at;  /* GetTickCount at last successful scan */
static DWORD  g_gi_scan_ms;     /* how long the last scan took */
static int    g_gi_have;

/* ---------------------------------------------------------------------- */

static DWORD fnv1a(const char *s)
{
    DWORD h = 2166136261UL;
    while (*s) {
        h ^= (DWORD)(unsigned char)(*s++);
        h *= 16777619UL;
    }
    return h;
}

/* Case-insensitive filename compare; the fleet spans FAT32 and NTFS and the
 * casing of an exe on disk is not something to depend on. */
static int ieq(const char *a, const char *b)
{
    return lstrcmpiA(a, b) == 0;
}

static int file_exists(const char *path)
{
    DWORD a = GetFileAttributesA(path);
    return a != 0xFFFFFFFF && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static int dir_exists(const char *path)
{
    DWORD a = GetFileAttributesA(path);
    return a != 0xFFFFFFFF && (a & FILE_ATTRIBUTE_DIRECTORY);
}

static void join(char *out, int outlen, const char *dir, const char *leaf)
{
    int n = lstrlenA(dir);
    if (n > 0 && (dir[n - 1] == '\\' || dir[n - 1] == '/'))
        _snprintf(out, outlen, "%s%s", dir, leaf);
    else
        _snprintf(out, outlen, "%s\\%s", dir, leaf);
    out[outlen - 1] = 0;
}

/* Strip trailing separators so the same directory reached by different
 * sources compares equal. The registry's InstallLocation usually ends in a
 * backslash and the shortcut target's directory never does, so without this
 * every registry-detected game is stored a SECOND time -- verified on .240,
 * which reported Counter-Strike, Half-Life, HL2 and Unreal twice each. */
static void norm_dir(char *dir)
{
    int n = lstrlenA(dir);
    /* Keep "C:\" intact: a bare drive root is a real path, not a stray sep. */
    while (n > 1 && (dir[n - 1] == '\\' || dir[n - 1] == '/')
           && !(n == 3 && dir[1] == ':')) {
        dir[n - 1] = 0;
        n--;
    }
}

/* Record one hit, unless (key, dir) is already present. When it is already
 * present but this hit carries a launcher and the stored one does not, keep
 * the launcher: the shortcut pass and the walk pass find the same install. */
static void add_entry(const game_sig_t *sig, const char *dir, const char *exe,
                      const char *launcher, const char *source)
{
    char norm[MAX_PATH];
    int i;

    safe_strncpy(norm, dir, MAX_PATH);
    norm_dir(norm);
    dir = norm;

    for (i = 0; i < g_ent_count; i++) {
        if (ieq(g_ents[i].key, sig->key) && ieq(g_ents[i].dir, dir)) {
            if (launcher && launcher[0] && !g_ents[i].launcher[0])
                safe_strncpy(g_ents[i].launcher, launcher, MAX_PATH);
            return;
        }
    }
    if (g_ent_count >= GI_MAX_GAMES)
        return;

    safe_strncpy(g_ents[g_ent_count].key,    sig->key,    32);
    safe_strncpy(g_ents[g_ent_count].name,   sig->name,   64);
    safe_strncpy(g_ents[g_ent_count].dir,    dir,         MAX_PATH);
    safe_strncpy(g_ents[g_ent_count].exe,    exe,         MAX_PATH);
    safe_strncpy(g_ents[g_ent_count].engine, sig->engine, 16);
    safe_strncpy(g_ents[g_ent_count].source, source,      16);
    if (launcher)
        safe_strncpy(g_ents[g_ent_count].launcher, launcher, MAX_PATH);
    else
        g_ents[g_ent_count].launcher[0] = 0;
    g_ent_count++;
}

/* Test one directory against every signature. A signature with a moddir only
 * matches when that subdirectory is present, which is what separates the
 * GoldSrc mods from each other and from plain Half-Life. */
static void match_dir(const char *dir, const char *launcher,
                      const char *launcher_exe, const char *source)
{
    char path[MAX_PATH];
    char mod[MAX_PATH];
    int  i;

    for (i = 0; g_sigs[i].key; i++) {
        join(path, sizeof(path), dir, g_sigs[i].exe);
        if (!file_exists(path))
            continue;
        if (g_sigs[i].moddir) {
            join(mod, sizeof(mod), dir, g_sigs[i].moddir);
            if (!dir_exists(mod))
                continue;
        }
        /* Only claim the shortcut for the game it actually launches. One
         * directory can satisfy several signatures -- C:\UT2004\System holds
         * UT2004 plus every mod's shortcut -- and attributing the first .lnk
         * found to all of them told us "Play AirBuccaneers.lnk" launches
         * UT2004, which would then be the config we edited. */
        if (launcher && launcher_exe && ieq(launcher_exe, g_sigs[i].exe))
            add_entry(&g_sigs[i], dir, path, launcher, source);
        else
            add_entry(&g_sigs[i], dir, path, NULL, source);
    }
}

/* ---------------------------------------------------------------------- */
/* Source 1: desktop shortcuts                                             */
/* ---------------------------------------------------------------------- */

/* Resolve a .lnk to its target path. Returns 0 on any failure - a box without
 * a usable shell32 just loses this source, it does not lose the scan. */
static int resolve_lnk(const char *lnk, char *target, int tlen)
{
    IShellLinkA  *sl  = NULL;
    IPersistFile *pf  = NULL;
    WCHAR         wpath[MAX_PATH];
    HRESULT       hr;
    int           ok = 0;

    if (!g_CoCreateInstance)
        return 0;

    hr = g_CoCreateInstance(&GI_CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER,
                            &GI_IID_IShellLinkA, (void **)&sl);
    if (FAILED(hr) || !sl)
        return 0;

    hr = sl->lpVtbl->QueryInterface(sl, &GI_IID_IPersistFile, (void **)&pf);
    if (SUCCEEDED(hr) && pf) {
        MultiByteToWideChar(CP_ACP, 0, lnk, -1, wpath, MAX_PATH);
        hr = pf->lpVtbl->Load(pf, wpath, STGM_READ);
        if (SUCCEEDED(hr)) {
            /* No SLR_ANY_MATCH / no UI: a broken shortcut must fail fast,
             * not pop an "item has been moved" dialog on a headless box. */
            sl->lpVtbl->Resolve(sl, NULL, SLR_NO_UI | SLR_NOUPDATE | SLR_NOSEARCH);
            if (SUCCEEDED(sl->lpVtbl->GetPath(sl, target, tlen, NULL, 0))
                && target[0])
                ok = 1;
        }
        pf->lpVtbl->Release(pf);
    }
    sl->lpVtbl->Release(sl);
    return ok;
}

/* Filename portion of a path, without copying. */
static const char *leaf_of(const char *path)
{
    const char *p = path;
    const char *leaf = path;
    for (; *p; p++)
        if (*p == '\\' || *p == '/')
            leaf = p + 1;
    return leaf;
}

static void strip_leaf(char *path)
{
    int n = lstrlenA(path);
    while (n > 0 && path[n - 1] != '\\' && path[n - 1] != '/')
        n--;
    if (n > 1)
        path[n - 1] = 0;
}

static void scan_shortcut_dir(const char *dir)
{
    WIN32_FIND_DATAA fd;
    HANDLE           h;
    char             pat[MAX_PATH];
    char             lnk[MAX_PATH];
    char             target[MAX_PATH];
    char             tdir[MAX_PATH];

    if (!dir || !dir[0] || !dir_exists(dir))
        return;

    join(pat, sizeof(pat), dir, "*.lnk");
    h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return;

    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            continue;
        join(lnk, sizeof(lnk), dir, fd.cFileName);
        if (!resolve_lnk(lnk, target, sizeof(target)))
            continue;
        if (!file_exists(target))
            continue;
        safe_strncpy(tdir, target, MAX_PATH);
        strip_leaf(tdir);
        match_dir(tdir, lnk, leaf_of(target), "shortcut");
    } while (FindNextFileA(h, &fd));

    FindClose(h);
}

/* SHGetSpecialFolderPathA is shell32 4.71+; resolve it dynamically so a
 * stripped Win95/98 shell just skips this source instead of failing to load. */
typedef BOOL (WINAPI *shgsfp_t)(HWND, LPSTR, int, BOOL);

static void scan_shortcuts(void)
{
    HMODULE   sh;
    shgsfp_t  fn;
    char      path[MAX_PATH];
    HRESULT   hr;
    int       inited = 0;

    if (!ole_load()) {
        log_msg(LOG_GI, "ole32 unavailable - skipping shortcut scan");
        return;
    }
    hr = g_CoInitialize ? g_CoInitialize(NULL) : E_FAIL;
    inited = (SUCCEEDED(hr) || hr == S_FALSE);

    sh = LoadLibraryA("shell32.dll");
    if (sh) {
        fn = (shgsfp_t)GetProcAddress(sh, "SHGetSpecialFolderPathA");
        if (fn) {
            if (fn(NULL, path, CSIDL_DESKTOPDIRECTORY, FALSE))
                scan_shortcut_dir(path);
            if (fn(NULL, path, CSIDL_COMMON_DESKTOPDIRECTORY, FALSE))
                scan_shortcut_dir(path);
            if (fn(NULL, path, CSIDL_PROGRAMS, FALSE))
                scan_shortcut_dir(path);
            if (fn(NULL, path, CSIDL_COMMON_PROGRAMS, FALSE))
                scan_shortcut_dir(path);
        }
        FreeLibrary(sh);
    }

    if (inited && g_CoUninitialize)
        g_CoUninitialize();
}

/* ---------------------------------------------------------------------- */
/* Source 2: registry uninstall keys                                       */
/* ---------------------------------------------------------------------- */

static void scan_uninstall(void)
{
    HKEY  root, sub;
    char  name[256];
    char  loc[MAX_PATH];
    DWORD i, nlen, type, len;

    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE,
                      "Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall",
                      0, KEY_READ, &root) != ERROR_SUCCESS)
        return;

    for (i = 0; ; i++) {
        nlen = sizeof(name);
        if (RegEnumKeyExA(root, i, name, &nlen, NULL, NULL, NULL, NULL)
                != ERROR_SUCCESS)
            break;
        if (RegOpenKeyExA(root, name, 0, KEY_READ, &sub) != ERROR_SUCCESS)
            continue;
        len = sizeof(loc);
        if (RegQueryValueExA(sub, "InstallLocation", NULL, &type,
                             (BYTE *)loc, &len) == ERROR_SUCCESS
            && (type == REG_SZ || type == REG_EXPAND_SZ) && loc[0]) {
            loc[sizeof(loc) - 1] = 0;
            if (dir_exists(loc))
                match_dir(loc, NULL, NULL, "registry");
        }
        RegCloseKey(sub);
    }
    RegCloseKey(root);
}

/* ---------------------------------------------------------------------- */
/* Source 3: depth-limited walk of the usual roots                         */
/* ---------------------------------------------------------------------- */

static void walk(const char *dir, int depth)
{
    WIN32_FIND_DATAA fd;
    HANDLE           h;
    char             pat[MAX_PATH];
    char             child[MAX_PATH];

    match_dir(dir, NULL, NULL, "scan");
    if (depth >= GI_MAX_DEPTH || g_ent_count >= GI_MAX_GAMES)
        return;

    join(pat, sizeof(pat), dir, "*");
    h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return;

    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            continue;
        if (fd.cFileName[0] == '.')
            continue;
        /* Skip the trees that are large and never hold a game, so a walk of
         * C:\ on a slow disk stays measured in seconds. */
        if (ieq(fd.cFileName, "WINDOWS") || ieq(fd.cFileName, "WINNT")
            || ieq(fd.cFileName, "System Volume Information")
            || ieq(fd.cFileName, "RECYCLER") || ieq(fd.cFileName, "RECYCLED")
            || ieq(fd.cFileName, "$Recycle.Bin"))
            continue;
        join(child, sizeof(child), dir, fd.cFileName);
        walk(child, depth + 1);
    } while (FindNextFileA(h, &fd) && g_ent_count < GI_MAX_GAMES);

    FindClose(h);
}

static void scan_drives(void)
{
    char  drives[512];
    char *p;
    char  root[MAX_PATH];
    DWORD n;
    int   i;

    n = GetLogicalDriveStringsA(sizeof(drives) - 1, drives);
    if (n == 0 || n >= sizeof(drives))
        return;

    for (p = drives; *p; p += lstrlenA(p) + 1) {
        if (GetDriveTypeA(p) != DRIVE_FIXED)
            continue;
        for (i = 0; g_roots[i]; i++) {
            if (g_roots[i][0] == 0) {
                /* The drive root itself, one level down - a lot of retro
                 * installs live at C:\Quake III Arena and nowhere tidier. */
                safe_strncpy(root, p, MAX_PATH);
                walk(root, GI_MAX_DEPTH - 2);
            } else {
                join(root, sizeof(root), p, g_roots[i]);
                if (dir_exists(root))
                    walk(root, 0);
            }
        }
    }
}

/* ---------------------------------------------------------------------- */
/* Build the cached document                                               */
/* ---------------------------------------------------------------------- */

static char *build_json(DWORD *out_hash, DWORD scan_ms)
{
    json_t j;
    DWORD  hash = 0;
    char   line[MAX_PATH * 2];
    int    i;

    for (i = 0; i < g_ent_count; i++) {
        _snprintf(line, sizeof(line), "%s|%s|%s",
                  g_ents[i].key, g_ents[i].dir, g_ents[i].exe);
        line[sizeof(line) - 1] = 0;
        /* Order-independent so the host's comparison never sees a spurious
         * change just because the walk enumerated in a different order. */
        hash += fnv1a(line);
    }
    *out_hash = hash;

    json_init(&j);
    json_object_start(&j);
    _snprintf(line, sizeof(line), "%08lx", (unsigned long)hash);
    line[sizeof(line) - 1] = 0;
    json_kv_str(&j, "hash", line);
    json_kv_uint(&j, "count", (DWORD)g_ent_count);
    json_kv_uint(&j, "scan_ms", scan_ms);
    json_key(&j, "games");
    json_array_start(&j);
    for (i = 0; i < g_ent_count; i++) {
        json_object_start(&j);
        json_kv_str(&j, "key", g_ents[i].key);
        json_kv_str(&j, "name", g_ents[i].name);
        json_kv_str(&j, "engine", g_ents[i].engine);
        json_kv_str(&j, "dir", g_ents[i].dir);
        json_kv_str(&j, "exe", g_ents[i].exe);
        json_kv_str(&j, "launcher", g_ents[i].launcher);
        json_kv_str(&j, "source", g_ents[i].source);
        json_object_end(&j);
    }
    json_array_end(&j);
    json_object_end(&j);
    return json_finish(&j);
}

static void gi_scan(void)
{
    DWORD t0 = GetTickCount();
    DWORD hash = 0;
    DWORD took;
    char *doc;

    g_ent_count = 0;
    scan_shortcuts();
    scan_uninstall();
    scan_drives();

    took = GetTickCount() - t0;
    doc  = build_json(&hash, took);
    if (!doc)
        return;

    EnterCriticalSection(&g_gi_lock);
    if (g_gi_json)
        HeapFree(GetProcessHeap(), 0, g_gi_json);
    g_gi_json       = doc;
    g_gi_hash       = hash;
    g_gi_scanned_at = GetTickCount();
    g_gi_scan_ms    = took;
    g_gi_have       = 1;
    LeaveCriticalSection(&g_gi_lock);

    log_msg(LOG_GI, "scan complete: %d game(s), hash=%08lx, %lums",
            g_ent_count, (unsigned long)hash, (unsigned long)took);
}

static DWORD gi_period_ms(void)
{
    HKEY  k;
    DWORD v = 0, len = sizeof(v), type;

    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "Software\\RetroAgent", 0,
                      KEY_READ, &k) == ERROR_SUCCESS) {
        if (RegQueryValueExA(k, "GameIndexPeriodMs", NULL, &type,
                             (BYTE *)&v, &len) != ERROR_SUCCESS
            || type != REG_DWORD)
            v = 0;
        RegCloseKey(k);
    }
    if (v < GI_PERIOD_MS_MIN)
        v = GI_PERIOD_MS_DEF;
    return v;
}

DWORD WINAPI gameindex_thread(LPVOID param)
{
    DWORD period;
    (void)param;

    Sleep(GI_FIRST_DELAY_MS);
    for (;;) {
        gi_scan();
        period = gi_period_ms();
        Sleep(period);
    }
}

void gameindex_init(void)
{
    if (!g_gi_lock_ready) {
        InitializeCriticalSection(&g_gi_lock);
        g_gi_lock_ready = 1;
    }
}

/* ---------------------------------------------------------------------- */
/* Command handler                                                         */
/* ---------------------------------------------------------------------- */

void handle_gameindex(SOCKET sock, const char *args)
{
    const char *a = str_skip_spaces(args ? args : "");
    char        buf[64];
    char       *copy = NULL;
    DWORD       len;

    gameindex_init();

    if (a[0] && str_starts_with(a, "SCAN"))
        gi_scan();

    EnterCriticalSection(&g_gi_lock);
    if (!g_gi_have) {
        LeaveCriticalSection(&g_gi_lock);
        /* The background thread has not produced one yet and the caller did
         * not ask for a forced scan. Say so rather than returning an empty
         * list that the host would store as "this box has no games". */
        send_text_response(sock, "{\"pending\":true,\"hash\":\"\",\"games\":[]}");
        return;
    }
    if (a[0] && str_starts_with(a, "HASH")) {
        _snprintf(buf, sizeof(buf), "%08lx", (unsigned long)g_gi_hash);
        buf[sizeof(buf) - 1] = 0;
        LeaveCriticalSection(&g_gi_lock);
        send_text_response(sock, buf);
        return;
    }
    /* Copy under the lock: a concurrent rescan frees the old buffer. */
    len  = (DWORD)lstrlenA(g_gi_json) + 1;
    copy = (char *)HeapAlloc(GetProcessHeap(), 0, len);
    if (copy)
        memcpy(copy, g_gi_json, len);
    LeaveCriticalSection(&g_gi_lock);

    if (!copy) {
        send_text_response(sock, "ERROR out of memory");
        return;
    }
    send_text_response(sock, copy);
    HeapFree(GetProcessHeap(), 0, copy);
}
