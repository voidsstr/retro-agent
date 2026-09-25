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
 * WHAT A PASS COSTS, AND WHEN ONE RUNS (agent 1.85.0) - see
 * agent/shared/gimatch.h. In short: one FindFirstFile listing per directory
 * instead of ~68 GetFileAttributesA probes, no directory walked twice, a
 * cheap fingerprint every 15 minutes that skips the pass when nothing an
 * install touches has changed, a forced full pass hourly, an immediate pass
 * when GAMESYNC has just deployed something (gameindex_poke), and the last
 * index kept in C:\RETRO_AGENT\gameindex.cache so a reboot answers GAMEINDEX
 * at once instead of scanning. The thread runs at THREAD_PRIORITY_IDLE.
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
#include "bgwork.h"
#include "gameindex.h"
#include "../shared/gimatch.h"
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

#ifndef AGENT_VERSION
#define AGENT_VERSION "0.0.0"
#endif

/* Cadence. The host asks for GAMEINDEX HASH every 5 minutes; until 1.85.0 the
 * agent re-scanned every 4 minutes to stay fresher than that, which on a
 * Pentium meant a 6-65 s disk walk every 4 minutes forever. Now:
 *   - the FIRST check waits until the rest of the boot has settled (retrowall
 *     20 s, gamesync 40 s, this 120 s) - sooner when there is no cached index,
 *     because until there is one the host forces a synchronous GAMEINDEX SCAN
 *     on the command thread;
 *   - then a fingerprint check every GameIndexPeriodMs (default 15 min) that
 *     runs the full pass only if something changed;
 *   - a forced full pass once an hour, for changes the fingerprint cannot see
 *     (a new file deep inside an existing game tree on FAT, which does not
 *     update directory times);
 *   - and an immediate pass when GAMESYNC deploys something (gameindex_poke),
 *     which is when the host most wants the answer to move.
 * GameIndexPeriodMs (HKLM\Software\RetroAgent) still overrides the check
 * period, with the same 60 s floor. */
#define GI_FIRST_DELAY_MS          120000
#define GI_FIRST_DELAY_NOCACHE_MS   60000
#define GI_PERIOD_MS_DEF           900000
#define GI_PERIOD_MS_MIN            60000
#define GI_FULL_EVERY_MS          3600000
#define GI_CACHE_PATH  "C:\\RETRO_AGENT\\gameindex.cache"
#define GI_CACHE_MAX   (512u * 1024u)

#define GI_MAX_GAMES        256
#define GI_MAX_DEPTH        3

/* ---------------------------------------------------------------------- */
/* Known-game signatures                                                   */
/* ---------------------------------------------------------------------- */

/*
 * game_sig_t (key, name, exe, moddir, engine) is defined in
 * agent/shared/gimatch.h beside the matcher that reads it; see there for what
 * `engine` and `moddir` mean.
 */
static const game_sig_t g_sigs[] = {
    /* Quake III engine - getstatus, favorites via autoexec.cfg server1..16 */
    { "quake3",     "Quake III Arena",        "quake3.exe",           "baseq3",   "q3" },
    { "quake3",     "Quake III Arena",        "quake3.exe",           NULL,       "q3" },
    { "ioquake3",   "ioquake3",               "ioquake3.exe",         NULL,       "q3" },
    /* the win32 build in the fleet library keeps upstream's .x86 suffix */
    { "ioquake3",   "ioquake3",               "ioquake3.x86.exe",     NULL,       "q3" },
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

    /*
     * The fleet's staged library (\\192.168.1.122\files\Files\Games-Library).
     * Every title there lands at C:\Games\<Title> with a desktop shortcut, and
     * before these rows only ten of the twenty-nine were recognised - the rest
     * were invisible to the host, so it could not set favourites or even say
     * what a box had. None of them has a server browser we can populate, hence
     * engine "-": being DETECTED is the point.
     *
     * The exe chosen for each is the most distinctive one in the title's own
     * directory, not necessarily the one the shortcut runs. Two rules were
     * applied: never pick a name generic enough to collide (Tiberian Sun is
     * matched on SUN.EXE, not GAME.EXE; Red Alert 2 on Ra2.exe/gamemd.exe, not
     * game.exe; Descent 3 on "Descent 3.exe", not main.exe), and never depend
     * on a launcher .bat, which is ours and could be renamed.
     */
    { "avp",        "Aliens versus Predator", "avp.exe",            NULL,       "-" },
    { "carmageddon","Carmageddon",            "MAINPROG.EXE",       NULL,       "-" },
    { "carmageddon2","Carmageddon 2",         "carma2.exe",         NULL,       "-" },
    { "descent",    "Descent",                "DESCENTR.EXE",       NULL,       "-" },
    { "descent2",   "Descent II",             "DESCENTW.EXE",       NULL,       "-" },
    { "descent3",   "Descent 3",              "Descent 3.exe",      NULL,       "-" },
    { "jk",         "Jedi Knight: Dark Forces II", "JK.EXE",        NULL,       "-" },
    { "jkmots",     "Jedi Knight: Mysteries of the Sith", "JKM.EXE", NULL,      "-" },
    { "redfaction", "Red Faction",            "rf.exe",             NULL,       "-" },
    { "redneck",    "Redneck Rampage",        "RR.EXE",             NULL,       "-" },
    { "shogo",      "Shogo: Mobile Armor Division", "Shogo.exe",    NULL,       "-" },
    { "sin",        "SiN Gold",               "sin.exe",            NULL,       "-" },
    { "sof",        "Soldier of Fortune",     "SoF.exe",            NULL,       "-" },
    { "sshock",     "System Shock",           "sshock.exe",         NULL,       "-" },
    { "sshock2",    "System Shock 2",         "shock2.exe",         NULL,       "-" },
    { "thief",      "Thief: The Dark Project","THIEF.EXE",          NULL,       "-" },
    { "thief2",     "Thief II: The Metal Age","Thief2.exe",         NULL,       "-" },
    { "tibsun",     "C&C: Tiberian Sun",      "SUN.EXE",            NULL,       "-" },
    { "ra2",        "C&C: Red Alert 2",       "Ra2.exe",            NULL,       "-" },
    { "ra2yr",      "Red Alert 2: Yuri's Revenge", "gamemd.exe",    NULL,       "-" },
    /* Added to the library after the rows above. Both have LAN multiplayer
     * (each ships Host/Join .bat launchers) but neither has a server list to
     * populate, so engine "-": being SEEN is the point - an unlisted title is
     * indistinguishable from an uninstalled one to the host.
     * Hexen II is matched on the OpenGL build's name rather than h2.exe,
     * which is short enough to collide during the depth-3 drive walk. */
    { "hexen2",     "Hexen II",               "glh2.exe",           NULL,       "-" },
    { "hd",         "Hidden & Dangerous",     "HDE.exe",            NULL,       "-" },
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

/* One pass at a time: g_ents and everything below are shared by the
 * background thread and GAMEINDEX SCAN. Until 1.85.0 nothing stopped the two
 * running gi_scan() concurrently over the same g_ents. */
static CRITICAL_SECTION g_gi_scan_lock;
static HANDLE g_gi_wake;            /* auto-reset; gameindex_poke() sets it  */
static volatile LONG g_gi_poked;    /* ...and says why                        */
static HANDLE g_gi_thread;          /* the scanner, so SCAN can lift it       */
static DWORD  g_gi_fp;              /* fingerprint the index reflects         */
static DWORD  g_gi_full_at;         /* GetTickCount of the last full pass     */
static DWORD  g_gi_cache_fp;        /* what the on-disk cache holds, so an    */
static DWORD  g_gi_cache_hash;      /* unchanged index is not rewritten       */
static gim_table_t g_gi_tab;        /* g_sigs with lengths, for gim_note()    */

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

/* Trees that are large and never hold a game, so a walk of C:\ on a slow
 * disk stays measured in seconds. The fingerprint skips the same names. */
static int gi_skip_name(const char *name)
{
    return ieq(name, "WINDOWS") || ieq(name, "WINNT")
        || ieq(name, "System Volume Information")
        || ieq(name, "RECYCLER") || ieq(name, "RECYCLED")
        || ieq(name, "$Recycle.Bin");
}

/* The game roots of the drive being walked (C:\Games, C:\Program Files...).
 * Each is walked on its own from depth 0, so a walk that meets one as a child
 * - the drive root meets C:\Games, Program Files meets Program Files\Games -
 * must not descend into it again. Until 1.85.0 it did, and C:\Games, where
 * every staged title lives, was walked twice per pass. */
#define GI_MAX_ROOTS 8
static char g_gi_roots[GI_MAX_ROOTS][MAX_PATH];
static int  g_gi_nroots;

static int gi_is_own_root(const char *path)
{
    int i;
    for (i = 0; i < g_gi_nroots; i++)
        if (ieq(path, g_gi_roots[i]))
            return 1;
    return 0;
}

/* Record what a directory listing matched. A signature with a moddir only
 * matches when that subdirectory is present, which is what separates the
 * GoldSrc mods from each other and from plain Half-Life. */
static void gi_emit(const char *dir, const gim_hits_t *hits,
                    const char *launcher, const char *launcher_exe,
                    const char *source)
{
    char path[MAX_PATH];
    int  i;

    for (i = 0; i < g_gi_tab.n; i++) {
        if (!gim_matches(hits, &g_gi_tab, i))
            continue;
        join(path, sizeof(path), dir, g_sigs[i].exe);
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

/*
 * One directory, ONE listing. The FindFirstFile enumeration both matches the
 * signatures (each entry's long and 8.3 name against the table in memory -
 * gim_note()) and, when `recurse` allows, finds the subdirectories to descend
 * into. Until 1.85.0 each directory cost that enumeration PLUS ~68
 * GetFileAttributesA probes, one per signature, each a path lookup that is a
 * linear directory search on FAT.
 */
static void gi_scan_dir(const char *dir, int depth, int recurse,
                        const char *launcher, const char *launcher_exe,
                        const char *source)
{
    WIN32_FIND_DATAA fd;
    HANDLE           h;
    char             pat[MAX_PATH];
    char             child[MAX_PATH];
    gim_hits_t       hits;

    if (g_ent_count >= GI_MAX_GAMES)
        return;
    gim_reset(&hits);
    join(pat, sizeof(pat), dir, "*");
    h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return;
    do {
        int is_dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;

        gim_note(&hits, &g_gi_tab, fd.cFileName, fd.cAlternateFileName, is_dir);
        if (!recurse || !is_dir || depth >= GI_MAX_DEPTH)
            continue;
        if (fd.cFileName[0] == '.' || gi_skip_name(fd.cFileName))
            continue;
        join(child, sizeof(child), dir, fd.cFileName);
        if (gi_is_own_root(child))
            continue;                    /* walked on its own - see above */
        gi_scan_dir(child, depth + 1, 1, NULL, NULL, source);
    } while (g_ent_count < GI_MAX_GAMES && FindNextFileA(h, &fd));
    FindClose(h);

    gi_emit(dir, &hits, launcher, launcher_exe, source);
}

/* Test one directory against every signature, without descending. */
static void match_dir(const char *dir, const char *launcher,
                      const char *launcher_exe, const char *source)
{
    gi_scan_dir(dir, GI_MAX_DEPTH, 0, launcher, launcher_exe, source);
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

/* The four shortcut folders, resolved once - the scan AND the fingerprint read
 * them, and they do not move while the agent runs. Retried while none has
 * resolved yet (the shell may not be up at the first ask). */
#define GI_LNKDIRS 4
static char g_gi_lnkdir[GI_LNKDIRS][MAX_PATH];
static int  g_gi_nlnkdirs = -1;

static void gi_lnkdirs_resolve(void)
{
    static const int csidl[GI_LNKDIRS] = {
        CSIDL_DESKTOPDIRECTORY, CSIDL_COMMON_DESKTOPDIRECTORY,
        CSIDL_PROGRAMS, CSIDL_COMMON_PROGRAMS
    };
    HMODULE  sh;
    shgsfp_t fn;
    int      i, j;

    if (g_gi_nlnkdirs > 0)
        return;
    g_gi_nlnkdirs = 0;
    sh = LoadLibraryA("shell32.dll");
    if (!sh)
        return;
    fn = (shgsfp_t)GetProcAddress(sh, "SHGetSpecialFolderPathA");
    for (i = 0; fn && i < GI_LNKDIRS; i++) {
        char path[MAX_PATH];
        int  dup = 0;
        if (!fn(NULL, path, csidl[i], FALSE) || !path[0])
            continue;
        for (j = 0; j < g_gi_nlnkdirs; j++)
            if (ieq(g_gi_lnkdir[j], path))
                dup = 1;         /* Win9x without profiles: one Desktop */
        if (!dup)
            safe_strncpy(g_gi_lnkdir[g_gi_nlnkdirs++], path, MAX_PATH);
    }
    FreeLibrary(sh);
}

static void scan_shortcuts(void)
{
    HRESULT   hr;
    int       inited = 0, i;

    if (!ole_load()) {
        log_msg(LOG_GI, "ole32 unavailable - skipping shortcut scan");
        return;
    }
    hr = g_CoInitialize ? g_CoInitialize(NULL) : E_FAIL;
    inited = (SUCCEEDED(hr) || hr == S_FALSE);

    gi_lnkdirs_resolve();
    for (i = 0; i < g_gi_nlnkdirs; i++)
        scan_shortcut_dir(g_gi_lnkdir[i]);

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
        /* This drive's game roots, BEFORE any walk, so the drive-root walk
         * already knows which children it must leave to their own walk. */
        g_gi_nroots = 0;
        for (i = 0; g_roots[i]; i++) {
            if (!g_roots[i][0] || g_gi_nroots >= GI_MAX_ROOTS)
                continue;
            join(root, sizeof(root), p, g_roots[i]);
            if (dir_exists(root))
                safe_strncpy(g_gi_roots[g_gi_nroots++], root, MAX_PATH);
        }
        /* The drive root itself, one level down - a lot of retro installs
         * live at C:\Quake III Arena and nowhere tidier. */
        safe_strncpy(root, p, MAX_PATH);
        gi_scan_dir(root, GI_MAX_DEPTH - 2, 1, NULL, NULL, "scan");
        for (i = 0; i < g_gi_nroots; i++)
            gi_scan_dir(g_gi_roots[i], 0, 1, NULL, NULL, "scan");
    }
    g_gi_nroots = 0;
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

/* ---------------------------------------------------------------------- */
/* Has anything an install touches changed? (the fingerprint)              */
/* ---------------------------------------------------------------------- */

#define GI_FP_DIR_TIMES   1     /* fold in each subdirectory's write time */
#define GI_FP_FILE_TIMES  2     /* fold in each file's write time         */
#define GI_FP_SKIP_SYS    4     /* leave out WINDOWS, RECYCLER, ...       */

/* One listing's contribution: names always, write times where asked. A folder
 * that does not exist contributes a distinct constant, so its appearance is a
 * change too. */
static DWORD gi_fp_listing(const char *dir, const char *pattern, int flags)
{
    WIN32_FIND_DATAA fd;
    HANDLE           h;
    char             pat[MAX_PATH];
    DWORD            sum = 0, n = 0;

    join(pat, sizeof(pat), dir, pattern);
    h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return (DWORD)gi_fnv1a(dir) ^ 0x5EED5EEDUL;
    do {
        int   is_dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        int   times  = is_dir ? (flags & GI_FP_DIR_TIMES) : (flags & GI_FP_FILE_TIMES);
        if (fd.cFileName[0] == '.' &&
            (!fd.cFileName[1] || (fd.cFileName[1] == '.' && !fd.cFileName[2])))
            continue;
        if ((flags & GI_FP_SKIP_SYS) && is_dir && gi_skip_name(fd.cFileName))
            continue;
        sum += (DWORD)gi_fp_entry(fd.cFileName, is_dir,
                                  times ? fd.ftLastWriteTime.dwLowDateTime : 0,
                                  times ? fd.ftLastWriteTime.dwHighDateTime : 0);
        n++;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return sum + n * 16777619UL + (DWORD)gi_fnv1a(dir);
}

/*
 * A few directory listings and one registry enumeration - milliseconds, where a
 * pass is seconds. It covers where an install shows up: a new directory in a
 * drive root or a game root, a changed directory time under a game root (NTFS
 * updates it when a title's own directory gains or loses an entry), a new or
 * rewritten shortcut, a new uninstall entry. AGENT_VERSION is folded in so an
 * agent update - which may carry new signatures - re-indexes once.
 *
 * What it cannot see - a file appearing deep inside an existing tree on FAT,
 * which updates no directory time the fingerprint reads - the hourly full pass
 * and gameindex_poke() cover.
 */
static DWORD gi_fingerprint(void)
{
    DWORD fp = (DWORD)gi_fnv1a(AGENT_VERSION);
    char  drives[512], root[MAX_PATH];
    char *p;
    DWORD n;
    int   i;
    HKEY  k;

    gi_lnkdirs_resolve();
    for (i = 0; i < g_gi_nlnkdirs; i++)
        fp += gi_fp_listing(g_gi_lnkdir[i], "*.lnk", GI_FP_FILE_TIMES);

    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE,
                      "Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall",
                      0, KEY_READ, &k) == ERROR_SUCCESS) {
        DWORD j;
        for (j = 0; ; j++) {
            char  name[256];
            DWORD nlen = sizeof(name);
            if (RegEnumKeyExA(k, j, name, &nlen, NULL, NULL, NULL, NULL)
                    != ERROR_SUCCESS)
                break;
            fp += (DWORD)gi_fp_entry(name, 1, 0, 0);
        }
        fp += j * 2654435761UL;
        RegCloseKey(k);
    }

    n = GetLogicalDriveStringsA(sizeof(drives) - 1, drives);
    if (n == 0 || n >= sizeof(drives))
        return fp;
    for (p = drives; *p; p += lstrlenA(p) + 1) {
        if (GetDriveTypeA(p) != DRIVE_FIXED)
            continue;
        /* the drive root: names only - its own entries (pagefile, the agent's
         * directory) change for reasons that have nothing to do with games */
        fp += gi_fp_listing(p, "*", GI_FP_SKIP_SYS);
        for (i = 0; g_roots[i]; i++) {
            if (!g_roots[i][0])
                continue;
            join(root, sizeof(root), p, g_roots[i]);
            fp += gi_fp_listing(root, "*", GI_FP_DIR_TIMES);
        }
    }
    return fp;
}

/* ---------------------------------------------------------------------- */
/* The on-disk copy of the last index                                      */
/* ---------------------------------------------------------------------- */

/*
 * A reboot used to mean "no index until the first scan" - GAMEINDEX answered
 * {"pending":true}, and the host answered THAT with a synchronous GAMEINDEX
 * SCAN on the command thread (on Win9x, the only thread). Keeping the last
 * index on disk lets the agent answer straight away, and lets the first check
 * skip the pass entirely when the fingerprint still matches. A torn or foreign
 * file fails gi_cache_parse() and is ignored.
 */
static void gi_cache_load(void)
{
    HANDLE h;
    DWORD  size, got = 0;
    char  *buf, *doc;
    unsigned long fp, hash, off, len;

    h = CreateFileA(GI_CACHE_PATH, GENERIC_READ, FILE_SHARE_READ, NULL,
                    OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return;
    /* under the scan lock: a GAMEINDEX SCAN can already be running */
    EnterCriticalSection(&g_gi_scan_lock);
    if (g_gi_have) {                    /* ...and may already have finished */
        LeaveCriticalSection(&g_gi_scan_lock);
        CloseHandle(h);
        return;
    }
    size = GetFileSize(h, NULL);
    buf = (size == 0xFFFFFFFF || size < 32 || size > GI_CACHE_MAX)
          ? NULL : (char *)HeapAlloc(GetProcessHeap(), 0, size + 1);
    if (!buf || !ReadFile(h, buf, size, &got, NULL) || got != size ||
        !gi_cache_parse(buf, size, &fp, &hash, &off, &len)) {
        LeaveCriticalSection(&g_gi_scan_lock);
        CloseHandle(h);
        if (buf)
            HeapFree(GetProcessHeap(), 0, buf);
        log_msg(LOG_GI, "cache %s unreadable - will scan", GI_CACHE_PATH);
        return;
    }
    CloseHandle(h);
    doc = (char *)HeapAlloc(GetProcessHeap(), 0, len + 1);
    if (doc) {
        memcpy(doc, buf + off, len);
        doc[len] = 0;
        EnterCriticalSection(&g_gi_lock);
        if (g_gi_json)
            HeapFree(GetProcessHeap(), 0, g_gi_json);
        g_gi_json = doc;
        g_gi_hash = (DWORD)hash;
        g_gi_have = 1;
        LeaveCriticalSection(&g_gi_lock);
        g_gi_fp = g_gi_cache_fp = (DWORD)fp;
        g_gi_cache_hash = (DWORD)hash;
        g_gi_full_at = GetTickCount();
        log_msg(LOG_GI, "serving the last index from %s (hash=%08lx) until "
                "the first check", GI_CACHE_PATH, hash);
    }
    LeaveCriticalSection(&g_gi_scan_lock);
    HeapFree(GetProcessHeap(), 0, buf);
}

static void gi_cache_save(DWORD fp, DWORD hash, const char *doc)
{
    char   hdr[GI_CACHE_HDR_MAX];
    DWORD  len = (DWORD)lstrlenA(doc), wr = 0, wr2 = 0;
    int    hl;
    HANDLE h;

    if (fp == g_gi_cache_fp && hash == g_gi_cache_hash)
        return;                 /* same index, same fingerprint: nothing new */
    hl = gi_cache_header(hdr, fp, hash, len);
    CreateDirectoryA("C:\\RETRO_AGENT", NULL);
    h = CreateFileA(GI_CACHE_PATH, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return;
    WriteFile(h, hdr, (DWORD)hl, &wr, NULL);
    WriteFile(h, doc, len, &wr2, NULL);
    CloseHandle(h);
    if (wr == (DWORD)hl && wr2 == len) {
        g_gi_cache_fp = fp;
        g_gi_cache_hash = hash;
    }
}

/* ---------------------------------------------------------------------- */
/* A pass                                                                  */
/* ---------------------------------------------------------------------- */

/* Caller holds g_gi_scan_lock. `fp` is the fingerprint taken BEFORE the walk
 * (so a change made during it is seen by the next check), or 0 to take it
 * here. */
static void gi_scan_locked(DWORD fp, const char *why)
{
    DWORD t0 = GetTickCount();
    DWORD hash = 0;
    DWORD took;
    char *doc;

    if (!fp)
        fp = gi_fingerprint();
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
    g_gi_fp      = fp;
    g_gi_full_at = GetTickCount();

    log_msg(LOG_GI, "scan complete (%s): %d game(s), hash=%08lx, %lums",
            why, g_ent_count, (unsigned long)hash, (unsigned long)took);
    gi_cache_save(fp, hash, doc);
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
    HANDLE me = NULL;
    (void)param;

    thread_background();         /* a disk walk is not worth a game frame */
    /* A real handle to this thread, so GAMEINDEX SCAN can lift it while it
     * waits for a pass this thread holds (see handle_gameindex). */
    if (DuplicateHandle(GetCurrentProcess(), GetCurrentThread(),
                        GetCurrentProcess(), &me, 0, FALSE,
                        DUPLICATE_SAME_ACCESS))
        g_gi_thread = me;

    gi_cache_load();
    Sleep(g_gi_have ? GI_FIRST_DELAY_MS : GI_FIRST_DELAY_NOCACHE_MS);

    while (g_running) {
        int   reason, poked;
        DWORD fp;

        thread_background();     /* SCAN may have lifted us for its wait */
        poked = (int)InterlockedExchange((LONG *)&g_gi_poked, 0);

        EnterCriticalSection(&g_gi_scan_lock);
        fp = gi_fingerprint();
        reason = gi_scan_reason(g_gi_have, fp, g_gi_fp,
                                GetTickCount() - g_gi_full_at,
                                GI_FULL_EVERY_MS, poked);
        if (reason != GI_SCAN_SKIP)
            gi_scan_locked(fp, gi_scan_reason_name(reason));
        LeaveCriticalSection(&g_gi_scan_lock);

        /* Sleep until the next check - or until GAMESYNC says it deployed
         * something, which is exactly when the host wants a fresh answer. */
        if (g_gi_wake)
            WaitForSingleObject(g_gi_wake, gi_period_ms());
        else
            Sleep(gi_period_ms());
    }
    return 0;
}

void gameindex_poke(void)
{
    InterlockedExchange((LONG *)&g_gi_poked, 1);
    if (g_gi_wake)
        SetEvent(g_gi_wake);
}

void gameindex_init(void)
{
    if (!g_gi_lock_ready) {
        InitializeCriticalSection(&g_gi_lock);
        InitializeCriticalSection(&g_gi_scan_lock);
        g_gi_wake = CreateEventA(NULL, FALSE, FALSE, NULL);   /* auto-reset */
        if (gim_table_init(&g_gi_tab, g_sigs) < 0)
            log_msg(LOG_GI, "signature table has more than %d rows - raise "
                    "GIM_MAX_SIGS; the rest are NOT matched", GIM_MAX_SIGS);
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

    if (a[0] && str_starts_with(a, "SCAN")) {
        /* A background pass holds g_gi_scan_lock at THREAD_PRIORITY_IDLE.
         * While a game keeps the CPU busy an IDLE thread barely runs, and
         * this command - on Win9x, every command - would wait behind it. Lift
         * the scanner first; it drops itself back before its next check. */
        if (g_gi_thread)
            SetThreadPriority(g_gi_thread, THREAD_PRIORITY_NORMAL);
        EnterCriticalSection(&g_gi_scan_lock);
        gi_scan_locked(0, "requested");
        LeaveCriticalSection(&g_gi_scan_lock);
    }

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
