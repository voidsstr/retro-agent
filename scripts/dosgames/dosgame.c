/*
 * dosgame.c - DOS Game Manager for the retro fleet
 *
 * A 16-bit real-mode DOS TUI that:
 *   - scans local hard drives for installed games and loose installers
 *   - shows a menu (Installed / Available-on-LAN tabs) with keyboard nav
 *   - launches games with ALL conventional memory free via the batch-swap
 *     pattern: the menu writes RUN.BAT and exits with code 42; DOSGAME.BAT
 *     runs RUN.BAT and loops back into the menu
 *   - installs games from the share catalog with scripted (non-interactive)
 *     steps: fetch (mTCP HTGET over the packet driver, or copy from a
 *     mapped drive), UNZIP, then optional INSTALL/SETUP run
 *   - shows a 320x200 256-color gameplay preview tile (mode 13h) per game
 *
 * Files it uses (all optional except the exe):
 *   C:\DOSGAME\DOSGAME.CFG   config: scan roots, share URL/drive
 *   C:\DOSGAME\GAMES.CAT     share catalog (title|zip|kind|exe|size|tile)
 *   C:\DOSGAME\TILES\*.PRV   preview tiles (768-byte pal + 64000 pixels)
 *   C:\DOSGAME\RUN.BAT       written on launch/install; run by DOSGAME.BAT
 *   C:\DOSGAME\INSTALL.LST   the game REGISTRY (see below)
 *   C:\DOSGAME\PENDING.TXT   handoff record for the post-install pass
 *   C:\DOSGAME\PREINST.LST   directory snapshot taken before an installer runs
 *
 * THE REGISTRY (INSTALL.LST) — why it exists
 * ------------------------------------------
 * Originally "what is installed and how do I run it" was re-derived on every
 * start by scanning the disk. That inference is lossy, and it broke the whole
 * point of the program: after installing a game whose INSTALL.EXE puts the
 * playable files in ITS OWN directory (C:\WOLF3D, not the C:\GAMES\<stem>
 * we unpacked into), the menu listed only the leftover unpack directory —
 * whose sole exe is INSTALL.EXE — so Enter re-ran the installer forever and
 * the game was never offered as playable.
 *
 * So an install now RECORDS what it produced, in INSTALL.LST:
 *     G|<title>|<dir>|<exe>     a playable game: run <exe> in <dir>
 *     X|<title>|<dir>|          a spent unpack dir: hide it from the menu
 * The record is written by the post-install pass (/postinst), which diffs the
 * directory listing against a snapshot taken before the installer ran — that
 * is what finds C:\WOLF3D. The disk scan still runs, for games that were on
 * the box before this program existed; the registry simply wins where both
 * have an opinion.
 *
 * Build: Open Watcom, real mode large model, 8K stack:
 *   wcl -bcl=dos -ml -os -q -k8192 dosgame.c
 *
 * Targets 8086+ (the TUI); preview tiles need VGA. Tested in DOSBox + DOSBox-X.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <ctype.h>
#include <conio.h>
#include <dos.h>
#include <i86.h>
#include <direct.h>

#define VER "0.2"

/* The longest command tail COMMAND.COM will hand a child program. Measured,
 * not assumed: a batch line of 160/200/215 chars all delivered exactly 126
 * bytes of arguments in DOSBox, silently — no error, no warning. Anything
 * emitted into RUN.BAT past this is simply lost, which is why the old
 * long-URL HTGET line failed for a quarter of the catalog and reported it
 * as "Download failed - check the network". */
#define DOS_TAIL_MAX 126

/* 256 * sizeof(game_t) must stay well under 64K — a bigger static array
 * silently wraps the data segment in the large model (learned the hard
 * way: entries past ~#420 came back corrupted). The catalog typeahead
 * filter makes 256 in-memory rows enough for ~3000 catalog titles. */
#define MAX_GAMES   256
/* games[] is shared between the disk scan and the catalog, so the scan needs
 * a ceiling: a box with a hundred game folders under C:\ would otherwise
 * leave the Available tab with a fraction of the catalogue (or, at 256 local
 * dirs, nothing at all) while the header still claimed 2,982 titles. */
#define MAX_LOCAL   96
#define MAX_TITLE   40
#define MAX_PATH_L  80
/* Catalog zip names are long filenames on the share and reach 103 characters;
 * 34 of the 2,982 entries are over 80. Truncating one used to make its stem
 * hash differ from the server's, so those titles could never be downloaded. */
#define MAX_ZIP_L   128
#define SCREEN_W    80
#define SCREEN_H    25
#define LIST_TOP    4
#define LIST_ROWS   (SCREEN_H - 6)

/* exit codes understood by DOSGAME.BAT */
#define EXIT_QUIT   0
#define EXIT_RUNBAT 42

typedef struct {
    char title[MAX_TITLE + 1];
    char path[MAX_ZIP_L + 1];    /* installed: game dir; avail: zip name */
    char exe[13];                /* main exe/bat (8.3) */
    char kind;                   /* 'R' ready, 'I' installer, 'C' cd-image */
    char tile[13];               /* preview tile file name, "" if none */
    long size;                   /* avail: archive bytes */
    int  installed;              /* 1 = local, 0 = share catalog */
} game_t;

/* Registry (INSTALL.LST) entries. Kept in a separate small array rather than
 * in games[]: the scan has to consult it while it is still deciding what to
 * put in games[]. */
#define MAX_REG 128
typedef struct {
    char flag;                   /* 'G' playable game, 'X' spent unpack dir */
    char title[MAX_TITLE + 1];
    char dir[MAX_PATH_L + 1];
    char exe[13];
    char tile[13];               /* the catalog's tile name, not a guess */
} reg_t;
static reg_t reg[MAX_REG];
static int n_reg = 0;

static game_t games[MAX_GAMES];
static int n_games = 0;
static int n_local = 0;         /* games[0..n_local) = local scan results */

/* view = filtered index into games[] for the active tab */
static int view[MAX_GAMES];
static int n_view = 0;
static int tab = 0;             /* 0 = Installed, 1 = Available */
static int sel = 0, top = 0;

static char cfg_home[MAX_PATH_L]   = "C:\\DOSGAME";
static char cfg_gamedir[MAX_PATH_L] = "C:\\GAMES";   /* where installs land */
/* Where to LOOK for already-installed games. Semicolon-separated; games on a
 * real box are rarely all in one place (the Deskpro has C:\DOOM, C:\ROTT,
 * C:\DUKE ... beside C:\GAMES), so scan the drive root too. */
static char cfg_scan[MAX_PATH_L * 2] = "C:\\GAMES;C:\\";
static char cfg_url[MAX_PATH_L]    = "";   /* http://host:port/dos */
static char cfg_drive[MAX_PATH_L]  = "";   /* e.g. Z:\Games\DOS */

/* ---- diagnostic log ----
 *
 * This program runs where nothing can watch it: a real-mode DOS session with
 * the screen taken over, on a box that is rebooted into MS-DOS mode to use it.
 * When something goes wrong there, the only evidence anyone can collect
 * afterwards is a file. So every decision the menu makes — which directories
 * it scanned and why it rejected them, which launcher it picked, the exact
 * RUN.BAT it wrote, what the post-install pass concluded — is written to
 * C:\DOSGAME\DOSGAME.LOG, and the generated RUN.BAT appends to the same file
 * so the batch half of the story lines up with the program half.
 *
 * Every line is flushed: a log that is still in a buffer when the machine
 * wedges tells you nothing, and wedging is exactly the case worth diagnosing.
 * Off with log=0 in DOSGAME.CFG.
 */
#define LOG_MAX_BYTES 262144L       /* recycle rather than fill a 1.2GB disk */

static void copy_str(char *dst, const char *src, size_t dstsz);

static FILE *g_log = NULL;
static int  g_log_enabled = 1;
static char cfg_log[MAX_PATH_L + 16] = "";
static unsigned g_log_seq = 0;

static void log_path(char *out, size_t cap)
{
    if (cfg_log[0]) copy_str(out, cfg_log, cap);
    else            sprintf(out, "%s\\DOSGAME.LOG", cfg_home);
}

/* tag identifies the process writing: the menu and its /snapdirs and
 * /postinst helper passes all append to one file. */
static char g_log_tag[10] = "menu";

static void log_open(const char *tag)
{
    char path[MAX_PATH_L + 24];
    struct find_t ft;

    copy_str(g_log_tag, tag, sizeof(g_log_tag));
    if (!g_log_enabled) return;
    log_path(path, sizeof(path));

    if (_dos_findfirst(path, _A_NORMAL, &ft) == 0 && ft.size > LOG_MAX_BYTES)
        remove(path);

    g_log = fopen(path, "a");
}

static void log_close(void)
{
    if (g_log) { fclose(g_log); g_log = NULL; }
}

static void logf(const char *fmt, ...)
{
    va_list ap;
    struct dostime_t t;
    if (!g_log) return;
    _dos_gettime(&t);
    /* The fleet's DOS boxes have no reliable date (this one reports 1980),
     * so log the time of day plus a sequence number instead. */
    fprintf(g_log, "%02d:%02d:%02d %-7s %4u  ", t.hour, t.minute, t.second,
            g_log_tag, ++g_log_seq);
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
}

/* INT 24h critical-error handler: always FAIL. Without one, a scan root on a
 * drive with no disk in it prints DOS's "Abort, Retry, Fail?" over the TUI —
 * unanswerable, because the program owns the screen and the keyboard. */
static int __far hard_error_handler(unsigned deverr, unsigned errcode,
                                    unsigned __far *devhdr)
{
    (void)deverr; (void)errcode; (void)devhdr;
    _hardresume(_HARDERR_FAIL);
    return _HARDERR_FAIL;
}

/* ---- forward declarations ----
 * is_scan_root() used to be called before it was declared, so C89 gave it an
 * implicit int() prototype and Watcom warned (W131) on every build. */
static int  is_scan_root(const char *path);
static void rebuild_view(void);
static void mark_installed(void);
static void zip_stem(const char *zipname, char *stem);
static int  reg_covers_dir(const char *dir);
static int  file_exists(const char *dir, const char *name);
static const char *stristr(const char *h, const char *n);
static void path_join_n(char *out, size_t cap, const char *root,
                        const char *leaf);
/* Callers always join into a local array, so the capacity comes for free.
 * An overlong join yields "" — treat that as "skip this path". */
#define path_join(out, root, leaf) path_join_n((out), sizeof(out), (root), (leaf))

/* strncpy does NOT terminate when the source fills the buffer, and the config
 * loader used to call it with n == sizeof(dst) — one byte short of safe. Every
 * bounded copy in this program goes through here instead. */
static void copy_str(char *dst, const char *src, size_t dstsz)
{
    size_t n;
    if (!dstsz) return;
    for (n = 0; n + 1 < dstsz && src[n]; n++) dst[n] = src[n];
    dst[n] = '\0';
}

/* ---- text UI: direct writes to text video memory ---- */

static unsigned short far *vram;
static unsigned char cur_attr = 0x07;

static void vinit(void)
{
    union REGS r;

    /* The menu draws straight into video memory at a fixed 80x25, so it must
     * not inherit whatever the program before it left behind. A game that
     * exits from 40-column text, a graphics mode, or a non-zero display page
     * used to leave the menu writing into the wrong place — a screen of
     * garbage that looks like a crash. Force mode 3 unless we are already in
     * a colour 80x25 text mode. */
    r.h.ah = 0x0F;              /* get video mode / active page */
    int86(0x10, &r, &r);
    if (r.h.al != 7 && (r.h.al != 3 || r.h.bh != 0)) {
        r.w.ax = 0x0003;        /* 80x25 colour text, page 0 */
        int86(0x10, &r, &r);
        r.h.ah = 0x0F;
        int86(0x10, &r, &r);
    }

    if (r.h.al == 7)
        vram = (unsigned short far *)MK_FP(0xB000, 0);  /* MDA/Herc */
    else
        vram = (unsigned short far *)MK_FP(0xB800, 0);
}

static void vputc(int x, int y, char c)
{
    vram[y * SCREEN_W + x] = ((unsigned short)cur_attr << 8) | (unsigned char)c;
}

static void vputs(int x, int y, const char *s)
{
    while (*s && x < SCREEN_W) vputc(x++, y, *s++);
}

static void vfill(int x, int y, int w, char c)
{
    while (w-- > 0 && x < SCREEN_W) vputc(x++, y, c);
}

static void cursor_hide(void)
{
    union REGS r;
    r.h.ah = 0x01; r.w.cx = 0x2000;
    int86(0x10, &r, &r);
}

static void cursor_show(void)
{
    union REGS r;
    r.h.ah = 0x01; r.w.cx = 0x0607;
    int86(0x10, &r, &r);
}

/* ---- keyboard ---- */

/* Keys the user mashed to get out of a game are still sitting in the BIOS
 * type-ahead buffer when the menu comes back, and the menu's first getkey()
 * would act on them — a stale Enter re-launched the game, or started a LAN
 * download nobody asked for. Drain on entry and before handing the screen
 * back to a game. */
static void kflush(void)
{
    while (kbhit()) getch();
}

#define K_UP    0x4800
#define K_DOWN  0x5000
#define K_PGUP  0x4900
#define K_PGDN  0x5100
#define K_HOME  0x4700
#define K_END   0x4F00
#define K_LEFT  0x4B00
#define K_RIGHT 0x4D00
#define K_F2    0x3C00
#define K_F3    0x3D00
#define K_F5    0x3F00
#define K_F9    0x4300
#define K_BACK  0x0008
#define K_ENTER 0x000D
#define K_ESC   0x001B
#define K_TAB   0x0009

static int getkey(void)
{
    int c = getch();
    if (c == 0 || c == 0xE0) return getch() << 8;
    return c;
}

/* ---- config + catalog ---- */

static void chomp(char *s)
{
    int n = strlen(s);
    while (n > 0 && (s[n-1] == '\n' || s[n-1] == '\r' || s[n-1] == ' '))
        s[--n] = '\0';
}

static void load_cfg(void)
{
    char path[MAX_PATH_L + 16], line[160];
    FILE *f;
    sprintf(path, "%s\\DOSGAME.CFG", cfg_home);
    f = fopen(path, "r");
    if (!f) return;
    while (fgets(line, sizeof(line), f)) {
        char *eq;
        chomp(line);
        if (line[0] == '#' || line[0] == ';' || !(eq = strchr(line, '=')))
            continue;
        *eq++ = '\0';
        if (!stricmp(line, "gamedir")) copy_str(cfg_gamedir, eq, sizeof(cfg_gamedir));
        else if (!stricmp(line, "scan")) copy_str(cfg_scan, eq, sizeof(cfg_scan));
        else if (!stricmp(line, "url")) copy_str(cfg_url, eq, sizeof(cfg_url));
        else if (!stricmp(line, "drive")) copy_str(cfg_drive, eq, sizeof(cfg_drive));
        else if (!stricmp(line, "log")) g_log_enabled = atoi(eq) ? 1 : 0;
        else if (!stricmp(line, "logfile")) copy_str(cfg_log, eq, sizeof(cfg_log));
    }
    fclose(f);

    /* gamedir= and scan= are independent settings, and pointing gamedir
     * somewhere the scan does not cover makes every install vanish from the
     * menu. Nothing warns about it, so guarantee the link here. */
    if (!stristr(cfg_scan, cfg_gamedir)) {
        char merged[sizeof(cfg_scan)];
        sprintf(merged, "%.*s;%.*s", (int)sizeof(merged) / 2 - 2, cfg_gamedir,
                (int)sizeof(merged) / 2 - 2, cfg_scan);
        copy_str(cfg_scan, merged, sizeof(cfg_scan));
    }
}

/* Split '|'-separated fields; returns count. Every slot up to max is set, so a
 * short line leaves NULL rather than a stale pointer from the previous line —
 * load_catalog used to test fld[4]/fld[5] that split() had never assigned. */
static int split(char *s, char **fld, int max)
{
    int n = 0, i;
    for (i = 0; i < max; i++) fld[i] = NULL;
    fld[n++] = s;
    while (*s && n < max) {
        if (*s == '|') { *s = '\0'; fld[n++] = s + 1; }
        s++;
    }
    return n;
}

/* The full catalog (~3000 titles) far exceeds MAX_GAMES, so the catalog
 * tab has a typeahead filter: typing letters reloads GAMES.CAT from disk
 * keeping only titles containing the filter (case-insensitive). */
static char cat_filter[24] = "";
static long cat_total = 0;      /* lines in GAMES.CAT (for the header) */

static const char *stristr(const char *h, const char *n)
{
    size_t nl = strlen(n);
    if (!nl) return h;
    for (; *h; h++)
        if (!strnicmp(h, n, nl)) return h;
    return NULL;
}

static void load_catalog(void)
{
    char path[MAX_PATH_L + 16], line[320];
    FILE *f;
    cat_total = 0;
    sprintf(path, "%s\\GAMES.CAT", cfg_home);
    f = fopen(path, "r");
    if (!f) { logf("catalog: CANNOT OPEN %s - the Available tab will be empty", path); return; }
    while (fgets(line, sizeof(line), f)) {
        char *fld[6];
        game_t *g;
        chomp(line);
        if (line[0] == '#' || !line[0]) continue;
        if (split(line, fld, 6) < 4) continue;
        cat_total++;
        if (n_games >= MAX_GAMES) continue;   /* keep counting for header */
        if (cat_filter[0] && !stristr(fld[0], cat_filter)) continue;
        g = &games[n_games];
        memset(g, 0, sizeof(*g));
        copy_str(g->title, fld[0], sizeof(g->title));
        copy_str(g->path, fld[1], sizeof(g->path));
        g->kind = (char)toupper(fld[2][0]);
        copy_str(g->exe, fld[3], sizeof(g->exe));
        g->size = 0;
        if (fld[4]) g->size = atol(fld[4]);
        if (fld[5]) copy_str(g->tile, fld[5], sizeof(g->tile));
        g->installed = 0;
        n_games++;
    }
    fclose(f);
    logf("catalog: %ld lines in GAMES.CAT, %d loaded into memory, filter=\"%s\"",
         cat_total, n_games - n_local, cat_filter);
}

/* Which of two candidate launchers does the share catalogue actually name as a
 * game's main program? Returns 1 for `a`, 2 for `b`, 0 for "can't tell".
 *
 * This is the tiebreak for the one case heuristics cannot settle: a directory
 * holding both an exe named after it and an exe whose name EXTENDS that name.
 * Guessing either way is wrong somewhere -
 *
 *   C:\KEEN  KEEN.EXE (an Apogee shell)  vs  KEEN4E.EXE (Commander Keen 4)
 *   C:\ROTT  ROTT.EXE (the game)         vs  ROTTIPX.EXE (its IPX launcher)
 *
 * - so ask the catalogue, which lists the real main exe for ~3,000 titles: it
 * has KEEN4E.EXE and ROTT.EXE, and neither KEEN.EXE nor ROTTIPX.EXE. Both
 * names are resolved in ONE pass, and the caller falls back to its own
 * heuristic when the answer is 0, so a catalogue that is missing, stale or
 * ambiguous can only leave behaviour as it was. */
/* The catalogue is 293K / ~3,000 rows and re-reading it per ambiguous
 * directory cost 6-7 s EACH in DOSBox — far worse on the Pentium-1 this runs
 * on. So it is read once into a 4K bit set (two hashes per name), which every
 * later query answers from memory. A hash hit is "probably listed" (~1% false
 * positive at this fill); a miss is definite. That asymmetry is safe here
 * because a false positive only makes catalog_prefers() return 0 and hand the
 * decision back to the name-shape heuristic — the behaviour we had before. */
#define CAT_BITS  32768u
static unsigned char cat_bits[CAT_BITS / 8];
static int cat_bits_ready = 0;

static unsigned cat_hash1(const char *s)
{
    unsigned h = 0x811CU;                       /* 16-bit FNV-1a */
    for (; *s; s++) { h ^= (unsigned char)toupper(*s); h *= 0x0193U; }
    return h;
}
static unsigned cat_hash2(const char *s)
{
    unsigned h = 0x1234U;                       /* independent of hash1 */
    for (; *s; s++) h = (unsigned)(h * 31U + (unsigned char)toupper(*s));
    return h;
}
static void cat_bits_set(const char *name)
{
    unsigned i;
    i = cat_hash1(name) % CAT_BITS; cat_bits[i >> 3] |= (unsigned char)(1 << (i & 7));
    i = cat_hash2(name) % CAT_BITS; cat_bits[i >> 3] |= (unsigned char)(1 << (i & 7));
}
static int cat_bits_test(const char *name)
{
    unsigned i;
    i = cat_hash1(name) % CAT_BITS;
    if (!(cat_bits[i >> 3] & (1 << (i & 7)))) return 0;
    i = cat_hash2(name) % CAT_BITS;
    return (cat_bits[i >> 3] & (1 << (i & 7))) ? 1 : 0;
}

static void cat_bits_build(void)
{
    char path[MAX_PATH_L + 16], line[320];
    FILE *f;
    long n = 0;

    cat_bits_ready = 1;                 /* set first: one attempt, even if it fails */
    sprintf(path, "%s\\GAMES.CAT", cfg_home);
    f = fopen(path, "r");
    if (!f) { logf("pick:   no GAMES.CAT - launcher ties fall back to name shape"); return; }
    while (fgets(line, sizeof(line), f)) {
        char *fld[6];
        chomp(line);
        if (line[0] == '#' || !line[0]) continue;
        if (split(line, fld, 6) < 4) continue;
        if (!fld[3] || !fld[3][0]) continue;
        cat_bits_set(fld[3]);
        n++;
    }
    fclose(f);
    logf("pick:   indexed %ld catalogue launcher names for tie-breaking", n);
}

static int catalog_prefers(const char *a, const char *b)
{
    int found_a, found_b;
    if (!cat_bits_ready) cat_bits_build();
    found_a = cat_bits_test(a);
    found_b = cat_bits_test(b);
    if (found_a && !found_b) return 1;
    if (found_b && !found_a) return 2;
    return 0;                           /* both or neither: caller decides */
}

/* ---- local drive scan ----
 *
 * A game dir = a subdirectory of a scan root that contains at least one
 * .EXE/.COM/.BAT. Main-exe pick order:
 *   1. exe matching the directory name
 *   2. a catalog entry whose zip stem matches the dir name (gives title+exe)
 *   3. the first .EXE that is not a known installer/config tool
 */

/* Directories that are never games. Scanning C:\ means walking into the
 * system's own folders otherwise. */
static const char *skip_dirs[] = {
    "windows","winnt","program files","progra~1","recycled","recycler",
    "temp","tmp","my documents","mydocu~1","dos","retro_agent","retro_~1",
    "dosgame","doschat","net","tiles","drivers","system","spool", NULL
};

static int is_skip_dir(const char *n)
{
    int i;
    for (i = 0; skip_dirs[i]; i++)
        if (!stricmp(n, skip_dirs[i])) return 1;
    return 0;
}

/* How big a lone .EXE must be before it is read as a self-extracting download
 * rather than a small complete game. HTIC_V10.EXE (shareware Heretic) is
 * 1.4 MB; the DOS-era games that ship as a single bare executable with no data
 * beside them are a fraction of this. */
#define SELFEXTRACT_MIN_BYTES 262144UL

static const char *skip_exes[] = {
    "install.exe","setup.exe","setsound.exe","sound.exe","uvconfig.exe",
    "config.exe","setmain.exe","readme.exe","catalog.exe","order.exe",
    "helpme.exe","dos4gw.exe","univbe.exe",
    /* archive + system tools that sit beside a game but are not it. Seen on
     * the fleet's Deskpro: TYRIAN listed as "UNZIP32.EXE", KEENDRMS as
     * "PKUNZJR.COM", and C:\GAMES itself as "CACHE.COM". */
    "unzip.exe","unzip32.exe","pkunzip.exe","pkunzjr.com","pkzip.exe",
    "arj.exe","lha.exe","cache.com","cwsdpmi.exe","deice.exe",
    "install.bat","setup.bat",
    /* Loaders, crack/no-CD stubs and support tools that sit beside a game and
     * die on their own. CB-RUN.COM is what the scan picked for Jagged
     * Alliance on the fleet's Win98 box - it is a loader, not the game, and
     * running it gave "abnormal exit". */
    "cb-run.com","cb-run.exe","loader.exe","loadgame.exe","crack.exe",
    "patch.exe","readme.bat","read.exe","vendor.exe","modem.exe",
    "uninstal.exe","uninst.exe","setsound.com","setup.com","install.com",
    "config.bat","sound.bat","mouse.com","mouse.exe","smartdrv.exe",
    /* Apogee/3D Realms shipped an advertising bundle in EVERY shareware
     * directory - an ordering catalogue, a dealer list, a BBS advert and a
     * launcher menu. On the fleet's Win98 box these outnumber the game itself
     * in C:\ROTT (10 runnable programs), C:\RAPTOR and C:\WACKY, and the
     * "first non-tool .EXE" fallback picks whichever the directory happens to
     * return first. Note the SHARE CATALOGUE cannot be trusted to rule these
     * out - it has rows that name DEALERS.EXE (3x), RAP-HELP.EXE and
     * 3DRCAT.EXE as the game's launcher. */
    "apogee.bat","dealers.exe","swcbbs.exe","3drcat.exe","vendor.bat",
    "cbytes4.com","get.com","micpatch.bat","autoexec.bat",
    /* Network setup stubs, not the game: DOOM/HEXEN/ROTT ship these beside it. */
    "sersetup.exe","ipxsetup.exe","modem.bat","serial.exe", NULL
};

/* Self-extractors / installers: not the game, but running one is exactly
 * what an un-unpacked game directory needs. Used as a last resort, and the
 * entry is then flagged as needing setup. */
static const char *setup_exes[] = {
    "install.bat","install.exe","deice.exe","setup.exe","setup.bat", NULL
};

static int is_setup_exe(const char *n)
{
    int i;
    for (i = 0; setup_exes[i]; i++)
        if (!stricmp(n, setup_exes[i])) return 1;
    return 0;
}

/* Does the part of a filename AFTER the directory name look like a support
 * tool rather than an episode? ROTT ships ROTTSND.EXE beside ROTT.EXE, and
 * that must never outrank the game. Compared against the stem only, so the
 * trailing ".EXE"/".COM" is ignored. */
static int is_util_suffix(const char *suffix)
{
    static const char *utils[] = {
        "snd","sound","cfg","config","set","setup","setmain","util","utl",
        "ins","install","ord","order","hlp","help","doc","read","readme",
        "un","uninst","tst","test","dm","demo","edit","ed","pat","patch", NULL
    };
    char stem[16];
    int i;
    const char *dot = strchr(suffix, '.');
    size_t n = dot ? (size_t)(dot - suffix) : strlen(suffix);

    if (n == 0 || n >= sizeof(stem)) return 0;
    memcpy(stem, suffix, n);
    stem[n] = '\0';
    for (i = 0; utils[i]; i++)
        if (!stricmp(stem, utils[i])) return 1;
    return 0;
}
static int is_skip_exe(const char *n)
{
    int i;
    size_t len = strlen(n);
    for (i = 0; skip_exes[i]; i++)
        if (!stricmp(n, skip_exes[i])) return 1;
    /* Every Apogee title ships its own help viewer named after the game:
     * RAP-HELP.EXE, WW-HELP.EXE, DN2-HELP.EXE, BS-HELP.EXE, DN3DHELP.EXE.
     * They cannot be listed by name, and two of them ARE named as launchers
     * by catalogue rows, so match the shape instead. */
    if (len > 8 && !stricmp(n + len - 8, "help.exe")) return 1;
    return 0;
}

/* Join a root and a subdirectory without doubling the separator: the drive
 * root "C:\\" already ends in one, and "C:\\\\DOOM" breaks the launch batch.
 *
 * Bounded, because the root comes from the scan= line of a config file the
 * operator edits: this used to be a plain sprintf into an 81-byte buffer, so
 * scan=D:\DOWNLOADS\OLD DOS GAMES\SHAREWARE COLLECTION\APOGEE 1993 wrote past
 * the end of it and over the far return address — a machine that hangs at
 * startup, before the UI ever appears. An overlong join yields "" and the
 * caller skips that root. */
static void path_join_n(char *out, size_t cap, const char *root,
                        const char *leaf)
{
    size_t n = strlen(root);
    size_t sep = (n > 0 && root[n - 1] == '\\') ? 0 : 1;
    if (!cap) return;
    if (n + sep + strlen(leaf) + 1 > cap) { out[0] = '\0'; return; }
    memcpy(out, root, n);
    if (sep) out[n] = '\\';
    strcpy(out + n + sep, leaf);
}

/* Look at ONE directory and decide what, if anything, launches a game in it.
 * Returns 0 (nothing runnable), or the needs_setup class: 0 ready, 1 run its
 * installer, 2 unpack its archive first. *best gets the launcher name. */
static int pick_launcher(const char *fulldir, const char *dirname, char *best)
{
    char pat[MAX_PATH_L * 2];
    struct find_t ft;
    char firstexe[13] = "", firstbat[13] = "", setup[13] = "", archive[13] = "";
    char want_exe[13], want_com[13], want_bat[13];
    char runlist[80] = "";
    char sibling[13] = "";
    int nrun = 0;
    int ndata = 0;
    unsigned long firstexe_size = 0;
    char cands[8][13];
    int ncand = 0;
    size_t dlen = strlen(dirname);

    best[0] = '\0';

    /* A launcher named after its directory is the strongest signal, and it
     * is often a .BAT (TYRIAN ships TYRIAN.BAT next to UNZIP32.EXE). */
    sprintf(want_exe, "%.8s.EXE", dirname);
    sprintf(want_com, "%.8s.COM", dirname);
    sprintf(want_bat, "%.8s.BAT", dirname);

    path_join(pat, fulldir, "*.*");
    if (!pat[0]) return -1;                  /* path too long to represent */
    if (_dos_findfirst(pat, _A_NORMAL, &ft) != 0) return -1;
    do {
        char *dot = strrchr(ft.name, '.');
        if (!dot) continue;
        if (!stricmp(ft.name, want_exe) || !stricmp(ft.name, want_com)
            || !stricmp(ft.name, want_bat))
            strcpy(best, ft.name);

        if (is_setup_exe(ft.name) && !setup[0]) strcpy(setup, ft.name);
        if (!archive[0] && !stricmp(dot, ".ZIP")) strcpy(archive, ft.name);

        if (!stricmp(dot, ".EXE") || !stricmp(dot, ".COM")) {
            if (!firstexe[0] && !is_skip_exe(ft.name)) {
                strcpy(firstexe, ft.name);
                firstexe_size = ft.size;
            }
        } else if (!stricmp(dot, ".BAT")) {
            if (!firstbat[0] && !is_skip_exe(ft.name)) strcpy(firstbat, ft.name);
        }
        /* Series shell vs. episode binary. Apogee/id shareware often ships a
         * front-end named for the SERIES beside the binary that is actually
         * the game, distinguished by an episode suffix: C:\KEEN holds a 642K
         * KEEN.EXE (an Apogee menu/ad shell) next to the 105K KEEN4E.EXE that
         * is Commander Keen 4. The rule below ("named after its directory")
         * picked the shell, so a game that reported itself installed re-ran
         * something installer-shaped when launched — exactly the symptom
         * reported from the Win98 box. A sibling whose name EXTENDS the
         * directory name is the game; the bare one is the wrapper. */
        if ((!stricmp(dot, ".EXE") || !stricmp(dot, ".COM")) && !sibling[0]
            && !is_skip_exe(ft.name) && dlen >= 3
            && !strnicmp(ft.name, dirname, dlen)
            && (size_t)(dot - ft.name) > dlen
            && !is_util_suffix(ft.name + dlen))
            strcpy(sibling, ft.name);

        /* Remember the alternatives so a wrong pick is visible in the log. */
        if ((!stricmp(dot, ".EXE") || !stricmp(dot, ".COM") ||
             !stricmp(dot, ".BAT")) && !is_skip_exe(ft.name)) {
            nrun++;
            if (ncand < (int)(sizeof(cands) / sizeof(cands[0])))
                strcpy(cands[ncand++], ft.name);
            if (nrun <= 4 && strlen(runlist) + 14 < sizeof(runlist)) {
                if (runlist[0]) strcat(runlist, ", ");
                strcat(runlist, ft.name);
            }
        } else {
            ndata++;                    /* anything that isn't a program */
        }
    } while (_dos_findnext(&ft) == 0);

    if (nrun > 1)
        logf("pick:   %s has %d runnable programs: %s%s", fulldir, nrun,
             runlist, nrun > 4 ? " ..." : "");

    /* One lone program and NO data files at all is not a game — it is a
     * self-extracting download that was never unpacked. C:\HERETIC on the
     * Win98 box held nothing but a 1.4 MB HTIC_V10.EXE, and because that name
     * is in no installer table it was registered as ready to play; pressing
     * Enter therefore ran the installer instead of the game, which is exactly
     * what was reported. Classify it as "needs setup run" so launching it
     * extracts, and the post-install pass then re-picks the real binary.
     * A genuine game directory always carries data beside its executable.
     *
     * The size floor matters: a self-extracting archive carries a whole game
     * inside it and is always large, whereas a lone SMALL exe with no data
     * really can be a tiny complete game. Without it this also swallowed a
     * legitimate game sitting one level below its unpack directory. */
    if (nrun == 1 && ndata == 0 && firstexe[0]
        && firstexe_size >= SELFEXTRACT_MIN_BYTES) {
        strcpy(best, firstexe);
        logf("pick:   %s -> %s (lone program, no data files: an unextracted "
             "self-extracting download; needs setup run)", fulldir, best);
        return 1;
    }
    if (best[0]) {
        char *bdot = strrchr(best, '.');
        /* A .BAT named for the directory is a deliberate launcher (TYRIAN.BAT)
         * and always wins. Only a bare .EXE/.COM gets second-guessed. */
        if (sibling[0] && bdot && stricmp(bdot, ".BAT") != 0
            && stricmp(best, sibling) != 0) {
            /* Ask the catalogue first — it names the real main exe, and the
             * name-shape heuristic alone gets ROTT exactly backwards. */
            int pref = catalog_prefers(best, sibling);
            if (pref == 1) {
                logf("pick:   %s -> %s (the catalogue names it, not %s)",
                     fulldir, best, sibling);
                return 0;
            }
            logf("pick:   %s -> %s (%s)", fulldir, sibling,
                 pref == 2 ? "the catalogue names it, not the directory-named exe"
                           : "extends the directory name; the bare name looks "
                             "like a series shell");
            strcpy(best, sibling);
            return 0;
        }
        logf("pick:   %s -> %s (named after its directory)%s", fulldir, best,
             nrun > 1 ? " - F2 in the menu picks a different one" : "");
        return 0;
    }
    /* Nothing is named after the directory, so "first non-tool .EXE" decides —
     * and "first" means whatever order DOS happened to return, which is not a
     * decision at all. It gave C:\GAMES\JAGGED~1 -> CBYTES4.COM and
     * C:\GAMES\KEENDRMS -> KEENDWEB.BAT on the box, when the catalogue names
     * DOXVIEW.EXE and KEENDR.BAT for those titles.
     *
     * Consult the catalogue, but only accept an UNAMBIGUOUS answer: exactly
     * one surviving candidate listed. The catalogue is not a clean oracle —
     * it has rows naming DEALERS.EXE, RAP-HELP.EXE and 3DRCAT.EXE as
     * launchers — so "two or more listed" must stay a fallback, not a guess. */
    if (ncand > 1) {
        int i, hits = 0, which = -1;
        if (!cat_bits_ready) cat_bits_build();
        for (i = 0; i < ncand; i++)
            if (cat_bits_test(cands[i])) { hits++; which = i; }
        if (hits == 1) {
            strcpy(best, cands[which]);
            logf("pick:   %s -> %s (the only one of %d programs the catalogue "
                 "names)", fulldir, best, ncand);
            return 0;
        }
        if (hits > 1)
            logf("pick:   %s - the catalogue names %d of these, so it cannot "
                 "choose; falling back to first-found", fulldir, hits);
    }
    if (firstexe[0]) { strcpy(best, firstexe);
        logf("pick:   %s -> %s (first non-tool .EXE/.COM)", fulldir, best); return 0; }
    if (firstbat[0]) { strcpy(best, firstbat);
        logf("pick:   %s -> %s (first .BAT)", fulldir, best); return 0; }
    /* Nothing runnable: a downloaded-but-not-unpacked game (DEICE.EXE plus
     * packed data is the classic Apogee/id shareware layout). Offer its
     * installer — running that is what makes it playable. */
    if (setup[0]) { strcpy(best, setup);
        logf("pick:   %s -> %s (installer only; needs setup run)", fulldir, best); return 1; }
    /* Only an archive in there (a download that was never unpacked, like
     * KEENDRMS = PKUNZJR.COM + a ZIP). Offer it: launching unpacks it with
     * our own UNZIP and then runs whatever installer it contained. */
    if (archive[0]) { strcpy(best, archive);
        logf("pick:   %s -> %s (archive only; needs unpacking)", fulldir, best); return 2; }
    return -1;
}

/* Next runnable file in `dir` after `current`, wrapping around. Used by F2 so
 * the operator can correct a wrong launcher guess without editing anything. */
static int next_launcher(const char *dir, const char *current, char *out)
{
    char pat[MAX_PATH_L * 2];
    struct find_t ft;
    char first[13] = "";
    int seen_current = 0;

    out[0] = '\0';
    path_join(pat, dir, "*.*");
    if (!pat[0] || _dos_findfirst(pat, _A_NORMAL, &ft) != 0) return 0;
    do {
        char *dot = strrchr(ft.name, '.');
        if (!dot) continue;
        if (stricmp(dot, ".EXE") && stricmp(dot, ".COM") && stricmp(dot, ".BAT"))
            continue;
        /* F2 must not be able to offer an installer or a support tool. It used
         * to accept every program in the directory, so one press on
         * C:\GAMES\KEEN1 (DEICE.EXE + INSTALL.BAT + a packed disk set) wrote
         * DEICE.EXE into the registry as the game - permanently, because the
         * resulting 'G' row hides the directory from the scan and there is no
         * F2 cycle back. pick_launcher is careful to exclude exactly these;
         * the manual override has to be at least as careful. */
        if (is_skip_exe(ft.name) || is_setup_exe(ft.name)) continue;
        if (!first[0]) copy_str(first, ft.name, 13);
        if (seen_current) { copy_str(out, ft.name, 13); return 1; }
        if (!stricmp(ft.name, current)) seen_current = 1;
    } while (_dos_findnext(&ft) == 0);

    /* Wrapped past the end, or the current one is not in the directory. */
    if (first[0] && stricmp(first, current)) {
        copy_str(out, first, 13);
        return 1;
    }
    return 0;
}

static void scan_game_dir(const char *root, const char *dir)
{
    char full[MAX_PATH_L + 1];
    char best[13];
    char sub[13] = "";
    int needs_setup;
    game_t *g;

    path_join(full, root, dir);
    if (!full[0]) {
        logf("scan:   SKIP %s\\%s (path too long for an 80-char buffer)", root, dir);
        return;
    }
    if (is_scan_root(full)) { logf("scan:   skip %s (it is a scan root)", full); return; }
    if (reg_covers_dir(full)) { logf("scan:   skip %s (registry owns it)", full); return; }

    needs_setup = pick_launcher(full, dir, best);

    /* Nothing runnable at the top level. Roughly a quarter of the share's
     * archives are not flat-root, so the game sits one directory further
     * down (C:\GAMES\<stem>\GAME\GAME.EXE) — and because the scan only ever
     * looked at depth 1, those games were listed nowhere at all. Descend one
     * level and adopt the first subdirectory that does have a launcher. */
    if (needs_setup < 0) {
        char pat[MAX_PATH_L * 2];
        struct find_t ft;
        path_join(pat, full, "*.*");
        if (!pat[0] || _dos_findfirst(pat, _A_SUBDIR, &ft) != 0) return;
        do {
            char subfull[MAX_PATH_L + 1];
            if (!(ft.attrib & _A_SUBDIR) || ft.name[0] == '.') continue;
            path_join(subfull, full, ft.name);
            if (!subfull[0] || reg_covers_dir(subfull)) continue;
            if (pick_launcher(subfull, ft.name, best) == 0) {
                copy_str(sub, ft.name, sizeof(sub));
                needs_setup = 0;
                break;
            }
        } while (_dos_findnext(&ft) == 0);
        if (needs_setup < 0) {
            logf("scan:   skip %s (nothing runnable at either level)", full);
            return;
        }
        logf("scan:   %s -> game is one level down in \\%s", full, sub);
    }

    if (n_games >= MAX_LOCAL) {
        logf("scan:   STOP at %s - hit the %d local-game limit; the rest of "
             "this drive was not scanned", full, MAX_LOCAL);
        return;
    }
    g = &games[n_games++];
    memset(g, 0, sizeof(*g));
    copy_str(g->title, dir, sizeof(g->title));
    if (sub[0]) {
        char deeper[MAX_PATH_L + 1];        /* path_join can't alias its own
                                             * output through sprintf */
        path_join(deeper, full, sub);
        copy_str(g->path, deeper, sizeof(g->path));
    } else {
        copy_str(g->path, full, sizeof(g->path));
    }
    copy_str(g->exe, best, sizeof(g->exe));
    g->kind = (needs_setup == 2) ? 'Z' : (needs_setup == 1 ? 'I' : 'R');
    g->installed = 1;
    /* tile name = dir name + .PRV */
    sprintf(g->tile, "%.8s.PRV", dir);
    logf("scan:   FOUND \"%s\" kind=%c exe=%s dir=%s", g->title, g->kind,
         g->exe, g->path);
}

/* Is this path one of the configured scan roots? C:\GAMES is a root AND a
 * subdirectory of C:\, so without this it gets listed as a game of its own
 * (it happened to contain CACHE.COM). */
static int is_scan_root(const char *path)
{
    char roots[MAX_PATH_L * 2];
    char *p, *next;
    strncpy(roots, cfg_scan, sizeof(roots) - 1);
    roots[sizeof(roots) - 1] = '\0';
    p = roots;
    while (p && *p) {
        int len;
        next = strchr(p, ';');
        if (next) *next++ = '\0';
        while (*p == ' ') p++;
        len = (int)strlen(p);
        if (len > 3 && p[len - 1] == '\\') p[len - 1] = '\0';
        if (*p && !stricmp(p, path)) return 1;
        p = next;
    }
    return 0;
}

static void scan_root(const char *root)
{
    char pat[MAX_PATH_L + 8];
    struct find_t ft;

    path_join(pat, root, "*.*");
    if (!pat[0] || _dos_findfirst(pat, _A_SUBDIR, &ft) != 0) return;
    do {
        if ((ft.attrib & _A_SUBDIR) && ft.name[0] != '.'
            && !is_skip_dir(ft.name))
            scan_game_dir(root, ft.name);
    } while (_dos_findnext(&ft) == 0 && n_games < MAX_LOCAL);
}

/* Walk every root in cfg_scan (semicolon-separated). A directory already
 * picked up from an earlier root is skipped, so overlapping roots (the common
 * "C:\GAMES;C:\" case) don't list a game twice. */
static void scan_local(void)
{
    char roots[MAX_PATH_L * 2];
    char *p, *next;
    logf("scan:   begin, roots=%s", cfg_scan);

    strncpy(roots, cfg_scan, sizeof(roots) - 1);
    roots[sizeof(roots) - 1] = '\0';

    p = roots;
    while (p && *p) {
        int before = n_games, i, j;
        next = strchr(p, ';');
        if (next) *next++ = '\0';
        while (*p == ' ') p++;
        if (*p) {
            /* strip a trailing backslash except on a bare drive root */
            int len = (int)strlen(p);
            if (len > 3 && p[len - 1] == '\\') p[len - 1] = '\0';
            logf("scan:   root %s", p);
            scan_root(p);
            /* Drop duplicates introduced by this root.
             *
             * This used to key on the TITLE, which for a scanned directory is
             * just its 8.3 name — and then always discarded the entry from the
             * LATER root. On the fleet's Win98 box, whose scan list is
             * "C:\GAMES;C:\", that silently deleted five games that were
             * installed and playable:
             *
             *   C:\ROTT   kind=R ROTT.EXE   lost to  C:\GAMES\ROTT   kind=I INSTALL.EXE
             *   C:\DOOM, C:\DUKE2, C:\RAPTOR, C:\WACKY  the same way
             *
             * Same 8.3 name under two roots is the NORMAL shape here, not a
             * duplicate: C:\GAMES\ROTT is the unpacked disk set and C:\ROTT is
             * what its installer produced. Keeping the C:\GAMES one meant the
             * menu offered "run setup", so pressing Enter on a game that was
             * already installed re-ran its installer — the exact complaint
             * from the box. The log said nothing, because this loop had no
             * logf and the count only showed up in the "loaded:" total.
             *
             * So: identity is the PATH; a title clash is resolved in favour of
             * whatever is actually playable; and every drop is logged. */
            for (i = before; i < n_games; i++) {
                for (j = 0; j < before; j++) {
                    int same_path = !stricmp(games[i].path, games[j].path);
                    if (!same_path && stricmp(games[i].title, games[j].title))
                        continue;
                    if (!same_path) {
                        int new_ready = (games[i].kind == 'R');
                        int old_ready = (games[j].kind == 'R');
                        if (new_ready && !old_ready) {
                            logf("scan:   \"%s\" %s is playable - it replaces "
                                 "%s (%c, needs setup)", games[i].title,
                                 games[i].path, games[j].path, games[j].kind);
                            games[j] = games[i];
                        } else if (new_ready == old_ready) {
                            /* Both equally playable and in different places:
                             * dropping either one hides a real game, so keep
                             * both rather than guess. */
                            logf("scan:   \"%s\" exists at %s AND %s - keeping "
                                 "both", games[i].title, games[j].path,
                                 games[i].path);
                            continue;
                        } else {
                            logf("scan:   dropped \"%s\" %s (%c) - %s is "
                                 "playable", games[i].title, games[i].path,
                                 games[i].kind, games[j].path);
                        }
                    } else {
                        logf("scan:   dropped \"%s\" %s (already listed)",
                             games[i].title, games[i].path);
                    }
                    memmove(&games[i], &games[i + 1],
                            (n_games - i - 1) * sizeof(game_t));
                    n_games--; i--;
                    break;
                }
            }
        }
        p = next;
    }
}

/* Drop catalog entries and reload them from disk with the current filter
 * (local scan results in games[0..n_local) are kept). */
static void reload_catalog(void)
{
    n_games = n_local;
    load_catalog();
    mark_installed();
    sel = top = 0;
    rebuild_view();
}

/* Stem of a catalog zip name = the 8.3 directory an install lands in.
 *
 * This used to be a plain 8-character truncation, which is nowhere near
 * unique: on the real 2,982-entry catalog, 1,268 rows (43%) collapsed onto a
 * shared stem — all eleven Duke Nukem titles installed into C:\GAMES\DUKE_NUK,
 * unzipping over each other, and installing any one of them made the other ten
 * show as installed. So the last three characters are now a hash of the FULL
 * zip name (base-36, from a 16-bit FNV-1a): 5 readable characters plus 3 of
 * hash gives zero collisions across the entire catalog.
 *
 * serve_dosgames.py computes the identical stem to resolve /z/<STEM>, so any
 * change here MUST be mirrored there (tests/python/test_dosgame_stem.py pins
 * the two implementations to the same vectors). */
static void zip_stem(const char *zipname, char *stem)
{
    static const char b36[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    unsigned h = 0x811CU;       /* 16-bit FNV-1a; unsigned is 16 bits here */
    const char *p;
    int i;

    for (p = zipname; *p; p++) {
        h ^= (unsigned char)toupper((unsigned char)*p);
        h *= 0x0193U;
    }

    /* Anything outside A-Z 0-9 - _ becomes '_'. Not cosmetic: ',' is an
     * argument separator to COMMAND.COM and one of FAT's reserved 8.3
     * characters (with + ; = [ ]), so "Clue, The (1994).zip" used to have
     * mkdir create C:\GAMES\Clue while UNZIP was handed Clue,_Th — the
     * install half-landed in the wrong directory and the game never showed. */
    for (i = 0; i < 5; i++) {
        char c = zipname[i];
        if (!c || c == '.') break;
        c = (char)toupper((unsigned char)c);
        if (!isalnum((unsigned char)c) && c != '-') c = '_';
        stem[i] = c;
    }
    while (i < 5) stem[i++] = '_';

    for (i = 7; i >= 5; i--) { stem[i] = b36[h % 36U]; h /= 36U; }
    stem[8] = '\0';
}

/* ---- the registry (INSTALL.LST) ---- */

static void reg_path(char *out)
{
    sprintf(out, "%s\\INSTALL.LST", cfg_home);
}

/* Does <dir>\<name> exist? Used to drop stale registry rows — a game the user
 * deleted by hand must not stay in the menu offering to launch nothing. */
static int file_exists(const char *dir, const char *name)
{
    char p[MAX_PATH_L * 2];
    struct find_t ft;
    path_join(p, dir, name);
    return p[0] && _dos_findfirst(p, _A_NORMAL, &ft) == 0;
}

static int dir_exists(const char *dir)
{
    char p[MAX_PATH_L * 2];
    struct find_t ft;
    path_join(p, dir, "*.*");
    return p[0] && _dos_findfirst(p, _A_NORMAL | _A_SUBDIR, &ft) == 0;
}

static void load_registry(void)
{
    char path[MAX_PATH_L + 16], line[256];
    FILE *f;
    int i;
    n_reg = 0;
    reg_path(path);
    f = fopen(path, "r");
    if (!f) return;
    while (n_reg < MAX_REG && fgets(line, sizeof(line), f)) {
        char *fld[5];
        reg_t *r;
        chomp(line);
        if (!line[0] || line[0] == '#') continue;
        if (split(line, fld, 5) < 3) continue;
        r = &reg[n_reg];
        memset(r, 0, sizeof(*r));
        r->flag = (char)toupper((unsigned char)fld[0][0]);
        copy_str(r->title, fld[1], sizeof(r->title));
        copy_str(r->dir, fld[2], sizeof(r->dir));
        if (fld[3]) copy_str(r->exe, fld[3], sizeof(r->exe));
        if (fld[4]) copy_str(r->tile, fld[4], sizeof(r->tile));
        /* 'S' is a 'G' row that still needs its installer run: the operator
          * chose this launcher with F2, but the directory is a disk set, not a
          * playable game. Without a class in the row, reloading turned every
          * remembered choice into "ready to run" and threw away the snapshot +
          * /postinst wrapper that finds where the installer put the game. */
        if (r->flag != 'G' && r->flag != 'S' && r->flag != 'X') continue;
        /* Drop rows whose directory (or launcher) has gone away — a game the
         * user deleted by hand must not linger in the menu offering to
         * launch nothing. */
        if (!dir_exists(r->dir)) {
            logf("registry: DROP %s (directory gone) - stale row", r->dir);
            continue;
        }
        if ((r->flag == 'G' || r->flag == 'S')
            && (!r->exe[0] || !file_exists(r->dir, r->exe))) {
            logf("registry: DROP %s (launcher \"%s\" not found) - stale row",
                 r->dir, r->exe);
            continue;
        }
        /* The file is append-only (a batch step cannot rewrite it), so
         * re-installing a game leaves several rows for the same directory.
         * Last one wins; without this the menu shows the game twice and
         * MAX_REG fills up with history. */
        for (i = 0; i < n_reg; i++) {
            if (!stricmp(reg[i].dir, r->dir)) {
                reg[i] = *r;
                break;
            }
        }
        if (i == n_reg) n_reg++;
        else logf("registry: row for %s superseded by a later one", r->dir);
    }
    fclose(f);

    /* An 'X' row only ever means "this unpack directory is spent, because the
     * game it produced is recorded elsewhere". If that 'G'/'S' row is gone,
     * the X row is now hiding a directory whose install never finished — the
     * user cannot even see it to retry the installer. That happened for real:
     * two games were recorded against the wrong directory, and removing those
     * rows left their unpack directories invisible. Self-heal instead. */
    for (i = 0; i < n_reg; i++) {
        int j, paired = 0;
        if (reg[i].flag != 'X') continue;
        for (j = 0; j < n_reg; j++)
            if (j != i && reg[j].flag != 'X'
                && !stricmp(reg[j].title, reg[i].title)) { paired = 1; break; }
        if (paired) continue;
        logf("registry: DROP X %s - no game row for \"%s\", so its install "
             "never finished and the directory must stay visible",
             reg[i].dir, reg[i].title);
        memmove(&reg[i], &reg[i + 1], (n_reg - i - 1) * sizeof(reg_t));
        n_reg--; i--;
    }

    for (i = 0; i < n_reg; i++)
        logf("registry: %c \"%s\" dir=%s exe=%s", reg[i].flag, reg[i].title,
             reg[i].dir, reg[i].exe);
    logf("registry: %d usable rows", n_reg);
}

static void reg_append(char flag, const char *title, const char *dir,
                       const char *exe, const char *tile)
{
    char path[MAX_PATH_L + 16];
    FILE *f;
    reg_path(path);
    f = fopen(path, "a");
    if (!f) return;
    fprintf(f, "%c|%s|%s|%s|%s\n", flag, title, dir, exe ? exe : "",
            tile ? tile : "");
    fclose(f);
    logf("registry: RECORD %c \"%s\" dir=%s exe=%s", flag, title, dir,
         exe ? exe : "");
}

/* Is this directory spoken for by the registry? The scan must not list a
 * spent unpack dir ('X'), nor list a registered game twice ('G'). */
static int reg_covers_dir(const char *dir)
{
    int i;
    for (i = 0; i < n_reg; i++)
        if (!stricmp(reg[i].dir, dir)) return 1;
    return 0;
}

/* Pull the registry's playable games into games[] before the disk scan, so
 * they carry their REAL title and launcher rather than whatever a directory
 * scan would have guessed. */
static void add_registry_games(void)
{
    int i;
    for (i = 0; i < n_reg && n_games < MAX_LOCAL; i++) {
        game_t *g;
        if (reg[i].flag != 'G' && reg[i].flag != 'S') continue;
        g = &games[n_games++];
        memset(g, 0, sizeof(*g));
        copy_str(g->title, reg[i].title, sizeof(g->title));
        copy_str(g->path, reg[i].dir, sizeof(g->path));
        copy_str(g->exe, reg[i].exe, sizeof(g->exe));
        g->kind = (reg[i].flag == 'S') ? 'I' : 'R';
        g->installed = 1;
        /* The catalog's own tile name. Deriving it from the directory (as the
         * disk scan must) disagreed with the name gen_catalog.py stages for
         * any title containing '(', '-' or ',' — 217 of 2,982 — so F3 said
         * "no preview tile" with the tile sitting right there. */
        copy_str(g->tile, reg[i].tile, sizeof(g->tile));
    }
}

/* Mark catalog entries that are already installed. Three ways to know:
 * the registry's recorded title, the registry's install directory stem, and
 * (for games that predate the registry) a scanned directory whose name
 * matches the stem. */
static void mark_installed(void)
{
    int i, j;

    for (i = 0; i < n_games; i++) {
        char stem[13];
        const char *base;
        if (games[i].installed) continue;
        zip_stem(games[i].path, stem);

        for (j = 0; j < n_games; j++) {
            if (!games[j].installed) continue;
            if (!strnicmp(stem, games[j].title, 8)
                || !stricmp(games[i].title, games[j].title)) {
                games[i].installed = 2;   /* in catalog AND installed */
                break;
            }
        }
        for (j = 0; !games[i].installed && j < n_reg; j++) {
            base = strrchr(reg[j].dir, '\\');
            base = base ? base + 1 : reg[j].dir;
            if (!stricmp(stem, base) || !stricmp(games[i].title, reg[j].title))
                games[i].installed = 2;
        }
    }
}

/* ---- preview tiles (VGA mode 13h) ---- */

static int show_tile(const game_t *g)
{
    char path[MAX_PATH_L * 2];
    FILE *f;
    static unsigned char pal[768];
    static unsigned char row[320];
    union REGS r;
    unsigned char far *scr = (unsigned char far *)MK_FP(0xA000, 0);
    int y, i;

    if (!g->tile[0]) return 0;
    sprintf(path, "%s\\TILES\\%s", cfg_home, g->tile);
    f = fopen(path, "rb");
    if (!f && g->installed) {           /* also try the game dir itself */
        sprintf(path, "%s\\%s", g->path, g->tile);
        f = fopen(path, "rb");
    }
    if (!f) return 0;
    if (fread(pal, 1, 768, f) != 768) { fclose(f); return 0; }

    r.w.ax = 0x0013; int86(0x10, &r, &r);       /* mode 13h */

    outp(0x3C8, 0);                              /* load palette */
    for (i = 0; i < 768; i++) outp(0x3C9, pal[i] >> 2);

    for (y = 0; y < 200; y++) {
        if (fread(row, 1, 320, f) != 320) break;
        _fmemcpy(scr + (unsigned)y * 320, row, 320);
    }
    fclose(f);

    getkey();
    r.w.ax = 0x0003; int86(0x10, &r, &r);       /* back to text */
    cursor_hide();
    return 1;
}

/* ---- post-install reconciliation ----
 *
 * The hard case, and the one that made the program feel broken: a kind 'I'
 * archive's INSTALL.EXE asks the user where to put the game and copies it
 * THERE — typically C:\WOLF3D — not into the C:\GAMES\<stem> we unpacked it
 * into. Nothing in RUN.BAT can know that directory, so we find it by
 * difference: list the top-level directories of every scan root before the
 * installer runs, list them again afterwards, and whatever appeared is where
 * the game went.
 *
 * The title/exe/dir the pass needs are handed over in PENDING.TXT rather than
 * on the command line: titles contain spaces (and the DOS command tail is
 * only 126 bytes), so putting them in a file avoids both quoting and
 * truncation. */

/* DOS packed date+time as one comparable number. The fleet's DOS boxes have
 * no reliable clock (this one still says 1980), but it is CONSISTENT - a file
 * written now gets "now" by the box's own reckoning - so comparing stamps to
 * each other works even though the absolute date is nonsense. */
/* Both stamp helpers produce the SAME 32-bit value: DOS's own packed
 * date word in the high half, packed time word in the low half. That layout
 * is already monotonic and, crucially, it FITS.
 *
 * The previous packing shifted the full year left by 26, and 1980 << 26 needs
 * 37 bits — on a 16-bit compiler where unsigned long is 32, the year silently
 * wrapped modulo 64. 1980 % 64 = 60 while 2026 % 64 = 42, so a 1980 file
 * compared as NEWER than a 2026 one and every "was this written recently?"
 * answer inverted across a year boundary. It never showed on the fleet's Win98
 * box only because its CMOS battery is dead and every stamp there is 1980. */
static unsigned long dos_stamp_now(void)
{
    struct dosdate_t d;
    struct dostime_t t;
    unsigned date, time;
    _dos_getdate(&d);
    _dos_gettime(&t);
    date = (unsigned)(((d.year - 1980) & 0x7F) << 9)
         | (unsigned)((d.month & 0x0F) << 5) | (unsigned)(d.day & 0x1F);
    time = (unsigned)((t.hour & 0x1F) << 11) | (unsigned)((t.minute & 0x3F) << 5)
         | (unsigned)((t.second / 2) & 0x1F);
    return ((unsigned long)date << 16) | (unsigned long)time;
}

/* Same packing, from a directory entry's write time. */
/* find_t already carries exactly those two words, so no unpacking is needed —
 * and none of it can overflow. */
static unsigned long dos_stamp_of(const struct find_t *ft)
{
    return ((unsigned long)ft->wr_date << 16) | (unsigned long)ft->wr_time;
}

/* Does this directory contain a file written at or after `since`? That is how
 * an install into an ALREADY EXISTING directory is detected. */
/* How many files in `dir` were written at or after `since`?
 *
 * This used to answer a yes/no question ("was anything written here?") and the
 * first directory that said yes won. One file is not evidence of an install:
 * on the fleet's Win98 box, PLAYING Duke Nukem 3D left DUKE3D.CFG and DD.CFG
 * behind, so C:\GAMES\DUKE3D answered yes to every later install and Blake
 * Stone AND Shadow Warrior were both recorded as C:\GAMES\DUKE3D\DUKE3D.EXE —
 * every game in the menu launched Duke Nukem.
 *
 * An install writes a program plus its data: many files at once. A game that
 * merely ran writes one or two. Counting separates the two cleanly. */
static int dir_new_files(const char *dir, unsigned long since)
{
    char pat[MAX_PATH_L * 2];
    struct find_t ft;
    int n = 0;
    path_join(pat, dir, "*.*");
    if (!pat[0] || _dos_findfirst(pat, _A_NORMAL, &ft) != 0) return 0;
    do {
        if (dos_stamp_of(&ft) >= since && ++n >= 99) break;
    } while (_dos_findnext(&ft) == 0);
    return n;
}

/* Below this many new files, a directory is something a game wrote to, not
 * somewhere an installer installed to. */
#define INSTALL_MIN_NEW_FILES 3

static void pending_path(char *out) { sprintf(out, "%s\\PENDING.TXT", cfg_home); }
static void presnap_path(char *out) { sprintf(out, "%s\\PREINST.LST", cfg_home); }

static void write_pending(const char *title, const char *unpackdir,
                          const char *exe, const char *tile)
{
    char path[MAX_PATH_L + 16];
    FILE *f;
    pending_path(path);
    f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "%s\n%s\n%s\n%s\n", title, unpackdir, exe ? exe : "",
            tile ? tile : "");
    fclose(f);
}

/* Write the current top-level directory list of every scan root. */
static void snap_dirs(void)
{
    char roots[MAX_PATH_L * 2], path[MAX_PATH_L + 16];
    char *p, *next;
    FILE *out;
    int n = 0;

    presnap_path(path);
    out = fopen(path, "w");
    if (!out) { logf("snap:   CANNOT WRITE %s", path); return; }
    /* First line is the moment of the snapshot; post_install compares file
     * write times against it to spot an install into an existing directory. */
    fprintf(out, "@%lu\n", dos_stamp_now());

    copy_str(roots, cfg_scan, sizeof(roots));
    p = roots;
    while (p && *p) {
        next = strchr(p, ';');
        if (next) *next++ = '\0';
        while (*p == ' ') p++;
        if (*p) {
            char pat[MAX_PATH_L + 8], full[MAX_PATH_L + 1];
            struct find_t ft;
            int len = (int)strlen(p);
            if (len > 3 && p[len - 1] == '\\') p[len - 1] = '\0';
            path_join(pat, p, "*.*");
            if (pat[0] && _dos_findfirst(pat, _A_SUBDIR, &ft) == 0) {
                do {
                    if (!(ft.attrib & _A_SUBDIR) || ft.name[0] == '.') continue;
                    path_join(full, p, ft.name);
                    if (full[0]) { fprintf(out, "%s\n", full); n++; }
                } while (_dos_findnext(&ft) == 0);
            }
        }
        p = next;
    }
    fclose(out);
    logf("snap:   recorded %d directories before the installer runs", n);
}

static int in_snapshot(const char *dir)
{
    char path[MAX_PATH_L + 16], line[MAX_PATH_L + 4];
    FILE *f;
    int hit = 0;
    presnap_path(path);
    f = fopen(path, "r");
    if (!f) return 0;
    while (fgets(line, sizeof(line), f)) {
        chomp(line);
        if (line[0] == '@') continue;                 /* the timestamp line */
        if (line[0] && !stricmp(line, dir)) { hit = 1; break; }
    }
    fclose(f);
    return hit;
}

/* Look one level below a directory for a launcher — the "non-flat archive"
 * case, where UNZIP put everything in C:\GAMES\<stem>\GAME\. Returns 1 and
 * fills subdir/best on success. */
static int find_deep_launcher(const char *parent, const char *want,
                              char *subdir, char *best)
{
    char pat[MAX_PATH_L * 2];
    struct find_t ft;
    path_join(pat, parent, "*.*");
    if (!pat[0] || _dos_findfirst(pat, _A_SUBDIR, &ft) != 0) return 0;
    do {
        char full[MAX_PATH_L + 1];
        if (!(ft.attrib & _A_SUBDIR) || ft.name[0] == '.') continue;
        path_join(full, parent, ft.name);
        if (!full[0]) continue;
        if (want[0] && file_exists(full, want)) {
            copy_str(subdir, full, MAX_PATH_L + 1);
            copy_str(best, want, 13);
            return 1;
        }
        if (pick_launcher(full, ft.name, best) == 0) {
            copy_str(subdir, full, MAX_PATH_L + 1);
            return 1;
        }
    } while (_dos_findnext(&ft) == 0);
    return 0;
}

/*
 * Log a directory's contents (bounded), and report how many Apogee-style
 * disk-set files it holds.
 *
 * The shareware BBS sets are INSTALL.EXE plus <NAME>._1, <NAME>._2 ... - the
 * game data, meant to arrive on separate floppies. When the installer is run
 * from a hard-disk directory it still asks you to "insert" the next disk, and
 * a failed install leaves those files sitting there. Knowing that is the
 * difference between "the download was bad" and "the installer needs
 * answering", so the log says which it is instead of guessing.
 */
static int log_dir_contents(const char *dir, const char *why)
{
    char pat[MAX_PATH_L * 2];
    struct find_t ft;
    int shown = 0, disks = 0;

    path_join(pat, dir, "*.*");
    if (!pat[0] || _dos_findfirst(pat, _A_NORMAL, &ft) != 0) {
        logf("post:   %s: %s is EMPTY", why, dir);
        return 0;
    }
    logf("post:   %s: contents of %s", why, dir);
    do {
        const char *dot = strrchr(ft.name, '.');
        if (dot && dot[1] == '_' && dot[2] >= '0' && dot[2] <= '9') disks++;
        if (shown < 20) {
            logf("post:     %-14s %8ld", ft.name, ft.size);
            shown++;
        }
    } while (_dos_findnext(&ft) == 0);
    if (disks)
        logf("post:   %d disk-set file(s) still present - the installer did "
             "not consume them", disks);
    return disks;
}

/* Reconcile after an install/installer run and record the result in the
 * registry. Runs headlessly from RUN.BAT (/postinst), never touches video.
 * Returns 0 when a playable game was recorded, 1 when nothing runnable could
 * be found — RUN.BAT branches on that errorlevel to tell the user, because
 * batch cannot make the judgement itself ("if exist DIR\*.*" is TRUE even for
 * an empty directory, so an unzip that produced nothing looks like success). */
/* The instant the pre-install snapshot was taken, or 0 if unknown. */
static unsigned long snapshot_stamp(void)
{
    char path[MAX_PATH_L + 16], line[64];
    FILE *f;
    unsigned long v = 0;
    presnap_path(path);
    f = fopen(path, "r");
    if (!f) return 0;
    if (fgets(line, sizeof(line), f) && line[0] == '@')
        v = strtoul(line + 1, NULL, 10);
    fclose(f);
    return v;
}

/* Read one newline-terminated record field into out[outsz], consuming the
 * whole line even when it is longer than the destination. The line buffer is
 * deliberately larger than any field so fgets always reaches the '\n'; see the
 * comment in post_install for what happens when it does not. */
static void read_field(FILE *f, char *out, size_t outsz)
{
    char line[MAX_PATH_L + 16];
    out[0] = '\0';
    if (!fgets(line, sizeof(line), f)) return;
    /* Longer than the line buffer: drain to the newline so the next read
     * starts on the next record rather than mid-line. */
    while (!strchr(line, '\n') && !feof(f)) {
        char drain[64];
        if (!fgets(drain, sizeof(drain), f)) break;
        if (strchr(drain, '\n')) break;
    }
    chomp(line);
    copy_str(out, line, outsz);
}

static int post_install(void)
{
    char path[MAX_PATH_L + 16];
    char title[MAX_TITLE + 1] = "", unpack[MAX_PATH_L + 1] = "";
    char want[13] = "", best[13], tile[13] = "";
    char gamedir[MAX_PATH_L + 1] = "";
    char roots[MAX_PATH_L * 2];
    char *p, *next;
    FILE *f;

    pending_path(path);
    f = fopen(path, "r");
    if (!f) { logf("post:   no PENDING.TXT - nothing to reconcile"); return 1; }
    /* Read each record line into a buffer BIGGER than the field it lands in.
     *
     * fgets(buf, n, f) stores at most n-1 characters and stops without
     * consuming the newline when the line is exactly that long. Reading
     * straight into title[MAX_TITLE + 1] therefore left the '\n' in the
     * stream for any title of exactly 40 characters — and the shipped
     * catalogue has 123 of them, because gen_catalog.py truncates titles to
     * 40. The next fgets then returned just that newline, unpack came out
     * empty, and post_install reported "PENDING.TXT incomplete" for an
     * install that had actually succeeded: RUN.BAT printed "Nothing runnable
     * was found", no 'G' row was written, and the game could never reach the
     * Installed tab no matter how many times it was installed.
     *
     * The same off-by-one hit want[13] for the 459 catalogue rows whose
     * launcher is a full 12-character 8.3 name (HTIC_V10.EXE), silently
     * blanking the tile that follows it. */
    read_field(f, title, sizeof(title));
    read_field(f, unpack, sizeof(unpack));
    read_field(f, want, sizeof(want));
    read_field(f, tile, sizeof(tile));
    fclose(f);
    remove(path);
    logf("post:   reconciling \"%s\" unpacked into %s, catalog exe=\"%s\"",
         title, unpack, want);
    if (!title[0] || !unpack[0]) {
        logf("post:   PENDING.TXT incomplete - giving up");
        return 1;
    }

    load_registry();

    /* 1. Did the unpack directory itself end up playable? Prefer the exe the
     *    catalog told us about — the scan's "first .EXE in directory order"
     *    guess picks things like SETSOUND.EXE or a level editor. */
    /* An installer is never the answer to "what plays this game".
     *
     * gen_catalog.py emits `exe or "INSTALL.EXE"`, so every kind-'I' archive
     * that holds nothing but an installer and a packed disk set — the classic
     * Apogee/id layout — carries INSTALL.EXE in the catalogue's exe field.
     * Taking it at face value here recorded a 'G' row pointing AT the
     * installer, and because a 'G' row makes reg_covers_dir() hide that
     * directory from the scan for good, Enter on the game then re-ran the
     * installer forever with no snapshot and no reconciliation — precisely the
     * loop the registry exists to end. */
    if (want[0] && (is_setup_exe(want) || is_skip_exe(want))) {
        logf("post:   ignoring the catalog's exe \"%s\" - that is an installer, "
             "not the game", want);
        want[0] = '\0';
    }
    if (want[0] && file_exists(unpack, want)) {
        copy_str(gamedir, unpack, sizeof(gamedir));
        copy_str(best, want, sizeof(best));
        logf("post:   the catalog's launcher %s is right there in %s", want, unpack);
    } else {
        const char *leaf = strrchr(unpack, '\\');
        if (pick_launcher(unpack, leaf ? leaf + 1 : unpack, best) == 0)
            copy_str(gamedir, unpack, sizeof(gamedir));
        else
            logf("post:   nothing runnable in the unpack dir itself");
    }

    /* 1b. Non-flat archive: the game is one level down. */
    if (!gamedir[0]) {
        char deep[MAX_PATH_L + 1];
        if (find_deep_launcher(unpack, want, deep, best)) {
            copy_str(gamedir, deep, sizeof(gamedir));
            logf("post:   found it one level down: %s (%s)", gamedir, best);
        }
    }

    /* 2. Otherwise look for a directory the installer created. */
    if (!gamedir[0]) {
        copy_str(roots, cfg_scan, sizeof(roots));
        p = roots;
        while (p && *p && !gamedir[0]) {
            next = strchr(p, ';');
            if (next) *next++ = '\0';
            while (*p == ' ') p++;
            if (*p) {
                char pat[MAX_PATH_L + 8], full[MAX_PATH_L + 1];
                struct find_t ft;
                int len = (int)strlen(p);
                if (len > 3 && p[len - 1] == '\\') p[len - 1] = '\0';
                path_join(pat, p, "*.*");
                if (pat[0] && _dos_findfirst(pat, _A_SUBDIR, &ft) == 0) {
                    do {
                        if (!(ft.attrib & _A_SUBDIR) || ft.name[0] == '.')
                            continue;
                        path_join(full, p, ft.name);
                        if (!full[0] || in_snapshot(full)) continue;   /* was already there */
                        if (!stricmp(full, unpack)) continue;
                        if (is_skip_dir(ft.name)) continue;
                        logf("post:   installer created %s - checking it", full);
                        if (want[0] && file_exists(full, want)) {
                            copy_str(gamedir, full, sizeof(gamedir));
                            copy_str(best, want, sizeof(best));
                            logf("post:   it has the catalog's launcher %s", want);
                            break;
                        }
                        if (!gamedir[0] && pick_launcher(full, ft.name, best) == 0)
                            copy_str(gamedir, full, sizeof(gamedir));
                    } while (_dos_findnext(&ft) == 0);
                }
            }
            p = next;
        }
    }

    /* 2b. Still nothing. The installer may have written into a directory that
     *     ALREADY EXISTED - which is the common case once a game has been
     *     installed here before, and it is invisible to a "what is new?"
     *     diff. Verified on the fleet's Win98 box: installing Blake Stone,
     *     Duke 1 and Duke 2 all reported "nothing runnable" because their
     *     installers targeted C:\BSTONE, C:\DUKE and C:\DUKE2, every one of
     *     which was already there. So look for a directory that was WRITTEN
     *     TO during the install instead of one that appeared. */
    /*     Evidence, not first-match. Every candidate is scored by HOW MANY
     *     files it gained, and the busiest one wins — an installer writes a
     *     program and its data, while a game that merely ran writes a config
     *     file or a save. Taking the first directory that had been touched at
     *     all made C:\GAMES\DUKE3D (left with DUKE3D.CFG and DD.CFG after
     *     someone played it) the answer for every subsequent install, so Blake
     *     Stone and Shadow Warrior were both recorded as DUKE3D.EXE and the
     *     whole menu launched Duke Nukem. */
    if (!gamedir[0]) {
        unsigned long since = snapshot_stamp();
        if (since) {
            char bestdir[MAX_PATH_L + 1] = "", bestleaf[13] = "";
            int bestcount = 0;

            copy_str(roots, cfg_scan, sizeof(roots));
            p = roots;
            while (p && *p) {
                next = strchr(p, ';');
                if (next) *next++ = '\0';
                while (*p == ' ') p++;
                if (*p) {
                    char pat[MAX_PATH_L + 8], full[MAX_PATH_L + 1];
                    struct find_t ft;
                    int len = (int)strlen(p);
                    if (len > 3 && p[len - 1] == '\\') p[len - 1] = '\0';
                    path_join(pat, p, "*.*");
                    if (pat[0] && _dos_findfirst(pat, _A_SUBDIR, &ft) == 0) {
                        do {
                            int n;
                            if (!(ft.attrib & _A_SUBDIR) || ft.name[0] == '.')
                                continue;
                            path_join(full, p, ft.name);
                            if (!full[0] || !stricmp(full, unpack)) continue;
                            if (is_skip_dir(ft.name)) continue;
                            /* Somewhere another game already lives is not
                             * where this one was just installed. */
                            if (reg_covers_dir(full)) continue;
                            n = dir_new_files(full, since);
                            if (n <= 0) continue;
                            logf("post:   %s gained %d file(s) during the "
                                 "install", full, n);
                            if (n > bestcount) {
                                bestcount = n;
                                copy_str(bestdir, full, sizeof(bestdir));
                                copy_str(bestleaf, ft.name, sizeof(bestleaf));
                            }
                        } while (_dos_findnext(&ft) == 0);
                    }
                }
                p = next;
            }

            if (bestcount && bestcount < INSTALL_MIN_NEW_FILES) {
                logf("post:   %s gained only %d file(s) - too few to be an "
                     "install, ignoring it", bestdir, bestcount);
            } else if (bestcount) {
                logf("post:   %s is where the installer wrote (%d files)",
                     bestdir, bestcount);
                if (want[0] && file_exists(bestdir, want)) {
                    copy_str(gamedir, bestdir, sizeof(gamedir));
                    copy_str(best, want, sizeof(best));
                } else if (pick_launcher(bestdir, bestleaf, best) == 0) {
                    copy_str(gamedir, bestdir, sizeof(gamedir));
                }
            }
        }
    }

    /* 3. Record it. A playable directory that is NOT the unpack directory
     *    also means the unpack directory is spent — mark it hidden so the
     *    menu stops offering "run setup" on a pile of installer leftovers. */
    presnap_path(path);
    remove(path);
    if (!gamedir[0]) {
        int disks = log_dir_contents(unpack, "nothing playable was found");
        if (disks) {
            /* This is the multi-floppy shareware layout, not a bad download:
             * the data is right there, the installer just never used it. */
            logf("post:   FAILED - this is a multi-disk installer and it did "
                 "not finish. Run setup again and answer %s if it asks where "
                 "to install FROM; press ENTER at any \"insert disk\" prompt.",
                 unpack);
        } else {
            logf("post:   FAILED - no playable directory found and no disk "
                 "files left behind, so the installer wrote nothing at all "
                 "(cancelled, or the download is bad).");
        }
        return 1;
    }
    logf("post:   OK - \"%s\" is playable: %s\\%s", title, gamedir, best);

    reg_append('G', title, gamedir, best, tile);
    logf("post:   if %s turns out to be the wrong program, press F2 on this "
         "game in the menu to pick another - the choice is remembered", best);
    if (stricmp(gamedir, unpack) && dir_exists(unpack))
        reg_append('X', title, unpack, "", "");
    return 0;
}

/* ---- RUN.BAT writing (launch + install scripts) ---- */

static FILE *open_runbat(void)
{
    char path[MAX_PATH_L + 16];
    FILE *f;
    sprintf(path, "%s\\RUN.BAT", cfg_home);
    f = fopen(path, "w");
    if (f) fprintf(f, "@echo off\n");
    return f;
}

/* A .BAT launcher must be CALLed. Chaining to one (a bare "TYRIAN.BAT")
 * abandons the rest of RUN.BAT — verified in DOSBox: the line after a bare
 * .BAT never runs, while the line after "call GAME.BAT" does. Harmless while
 * nothing followed the launcher, but the post-launch bookkeeping below has to
 * survive a .BAT game. */
static int is_bat(const char *name)
{
    const char *dot = strrchr(name, '.');
    return dot && !stricmp(dot, ".BAT");
}

static void emit_run(FILE *f, const char *exe)
{
    fprintf(f, "%s%s\n", is_bat(exe) ? "call " : "", exe);
}

static void emit_pause(FILE *f);

/*
 * Print what the game's own installer is about to ask, before it asks.
 *
 * The shareware installers are floppy-era: they want to know which drive to
 * install FROM, and they say "insert disk 2" even when every disk file is
 * already sitting in one directory on the hard disk. Someone meeting that
 * prompt with no context reasonably concludes a disk is missing and stops -
 * which is exactly what happened with Blake Stone and Keen 4.
 */
static void emit_installer_hint(FILE *f, const char *dir)
{
    fprintf(f, "echo.\n");
    fprintf(f, "echo   This game has its own installer, from the floppy era.\n");
    fprintf(f, "echo   If it asks where to install FROM, answer:  %s\n", dir);
    fprintf(f, "echo   If it asks you to insert a disk, the files are already\n");
    fprintf(f, "echo   there - just press ENTER.\n");
    fprintf(f, "echo.\n");
    emit_pause(f);
}

/* Every "press a key" in a generated script goes through here so the whole
 * install path can run unattended: touch C:\DOSGAME\QUIET.FLG and the pauses
 * skip themselves. A marker file rather than an environment variable because
 * COMMAND.COM's IF "%VAR%"=="x" quoting is a trap not worth stepping in. */
/* Make the generated script narrate itself into the same log the program
 * writes, so a failed install reads as one story: what the menu decided, then
 * what the batch actually did and what each tool returned. */
/* Longest line COMMAND.COM will read from a .BAT. It loads one into a 128-byte
 * buffer and silently CHOPS the rest — which for a logging line means the
 * ">> C:\DOSGAME\DOSGAME.LOG" tail is cut, so the redirect either lands in a
 * stray file or fails outright, and the one message explaining a failed
 * install never reaches the log at all. */
#define BAT_LINE_MAX 126

/* `prefix` is anything already written on this line (e.g. "if errorlevel 1 ")
 * and must be counted against the limit. Over-long text is truncated HERE,
 * where the result is still a well-formed command, instead of by COMMAND.COM
 * in the middle of the redirect. */
static void emit_log_p(FILE *f, const char *prefix, const char *text)
{
    char path[MAX_PATH_L + 24];
    char buf[BAT_LINE_MAX + 1];
    int room;

    log_path(path, sizeof(path));
    /* "echo run:    " = 13, " >> " = 4 */
    room = BAT_LINE_MAX - (int)strlen(prefix) - 13 - 4 - (int)strlen(path);
    if (room < 8) room = 8;                  /* pathological log path */
    if ((int)strlen(text) <= room) {
        fprintf(f, "echo run:    %s >> %s\n", text, path);
        return;
    }
    memcpy(buf, text, (size_t)room);
    buf[room] = '\0';
    fprintf(f, "echo run:    %s >> %s\n", buf, path);
    logf("run:    NOTE - shortened a RUN.BAT message to fit DOS's %d-byte "
         "line limit: \"%s\"", BAT_LINE_MAX, text);
}

static void emit_log(FILE *f, const char *text)
{
    emit_log_p(f, "", text);
}

/* Redirect a tool's own output into the log. HTGET and UNZIP explain their
 * failures on stdout, and on a box booted into MS-DOS mode nobody is going to
 * be reading the screen when it scrolls past. */
static void emit_redirect(FILE *f)
{
    char path[MAX_PATH_L + 24];
    log_path(path, sizeof(path));
    fprintf(f, " >> %s\n", path);
}

static void emit_pause(FILE *f)
{
    fprintf(f, "if not exist %s\\QUIET.FLG pause > nul\n", cfg_home);
}

static int write_launch(const game_t *g)
{
    FILE *f = open_runbat();
    if (!f) { logf("launch: CANNOT WRITE RUN.BAT"); return 0; }
    logf("launch: \"%s\" kind=%c exe=%s dir=%s", g->title, g->kind, g->exe,
         g->path);
    emit_log(f, "RUN.BAT starting");
    fprintf(f, "%c:\ncd %s\n", g->path[0], g->path + 2);

    if (g->kind == 'Z') {
        /* Unpack in place, then run whatever installer it contained, so one
         * keypress takes a downloaded archive all the way to playable. */
        fprintf(f, "echo Unpacking %s ...\n", g->exe);
        fprintf(f, "if not exist %s\\UNZIP.EXE echo UNZIP.EXE missing under %s - re-stage the DOS tools.\n",
                cfg_home, cfg_home);
        fprintf(f, "if exist %s\\UNZIP.EXE %s\\UNZIP -qq -o %s\n",
                cfg_home, cfg_home, g->exe);
        fprintf(f, "if exist INSTALL.EXE INSTALL.EXE\n");
        fprintf(f, "if exist INSTALL.BAT if not exist INSTALL.EXE call INSTALL.BAT\n");
        fprintf(f, "if exist SETUP.EXE if not exist INSTALL.EXE if not exist INSTALL.BAT SETUP.EXE\n");
        fprintf(f, "echo Done - press a key, then pick it again to play.\n");
        emit_pause(f);
    } else if (g->kind == 'I') {
        /* Running a local installer is an install, not a launch: snapshot
         * the drive first and reconcile afterwards, so that wherever this
         * installer decides to put the game, the menu learns about it. */
        write_pending(g->title, g->path, "", g->tile);
        emit_log(f, "snapshotting directories before the installer runs");
        fprintf(f, "%s\\DOSGAME /snapdirs\n", cfg_home);
        emit_installer_hint(f, g->path);
        fprintf(f, "echo Running setup for %s ...\n", g->title);
        emit_run(f, g->exe);
        emit_log(f, "installer finished; working out where the game went");
        fprintf(f, "%s\\DOSGAME /postinst\n", cfg_home);
        fprintf(f, "if errorlevel 1 goto nogame\n");
        fprintf(f, "echo Setup finished - it is on the menu now. Press a key.\n");
        emit_pause(f);
        fprintf(f, "goto end\n");
        fprintf(f, ":nogame\n");
        fprintf(f, "echo Setup finished but nothing runnable was found.\n");
        emit_pause(f);
        fprintf(f, ":end\n");
    } else {
        emit_log(f, "launching the game");
        emit_run(f, g->exe);
        emit_log(f, "back from the game");
    }
    fclose(f);
    return 1;
}

/* Scripted install:
 *   fetch zip  (HTGET from cfg_url, else COPY from cfg_drive)
 *   UNZIP -qq -o into %gamedir%\<stem8>   (survey: 76% of zips are flat)
 *   if kind == 'I' run INSTALL/SETUP interactively afterwards
 */
static int write_install(const game_t *g, int run_installer)
{
    FILE *f;
    char stem[13];
    char dir[MAX_PATH_L + 1];
    int taillen;

    /* A cd-image row is an ISO/BIN set up to 648 MB; fetching one over mTCP
     * onto a Win98 box is not an install, it is an accident. */
    if (g->kind == 'C') {
        logf("install: REFUSED \"%s\" - cd-image rows are far too big to fetch",
             g->title);
        return -2;
    }
    /* Nothing to fetch from: answer before a single `goto notool` is written,
     * because the early return below never writes the :notool label and
     * COMMAND.COM aborts the script with "Label not found". */
    if (!cfg_url[0] && !cfg_drive[0]) {
        logf("install: REFUSED \"%s\" - no url= or drive= in DOSGAME.CFG",
             g->title);
        return -3;
    }

    zip_stem(g->path, stem);
    /* Bounded join, NOT sprintf. cfg_gamedir holds up to 79 operator-supplied
     * characters from DOSGAME.CFG and dir[] is 81 bytes, so a gamedir= of 72
     * characters or more used to write past the end of this frame — in real
     * mode that lands on the saved BP and return address, and the box hangs or
     * reboots somewhere unrelated. path_join_n exists because exactly this bug
     * was already found once, in the scan path. */
    path_join(dir, cfg_gamedir, stem);
    if (!dir[0]) {
        logf("install: REFUSED \"%s\" - gamedir= in DOSGAME.CFG is too long to "
             "append an install directory to", g->title);
        return -3;
    }

    /* The fetch is one command line, and DOS silently truncates a command
     * tail at 126 bytes. The old code pasted the full URL-encoded zip name
     * (61 chars on average, 137 at worst) onto the URL, so 845 of the 2,982
     * catalogue entries fetched a chopped-off URL, 404'd, and reported
     * "Download failed - check the network". The server now resolves the
     * 8-char stem via /z/<STEM>, which is a fixed, short line — but check it
     * anyway and refuse to write a script that cannot work. */
    taillen = 3 + (int)strlen(cfg_home) + 6 + 4      /* -o <home>\<stem>.ZIP */
              + 1 + (int)strlen(cfg_url) + 3 + 8;    /*  <url>/z/<stem>      */
    logf("install: \"%s\" kind=%c zip=%s", g->title, g->kind, g->path);
    logf("install: stem=%s dir=%s exe(catalog)=%s", stem, dir, g->exe);
    if (cfg_url[0])
        logf("install: fetch command tail is %d bytes (DOS truncates above %d)",
             taillen, DOS_TAIL_MAX);
    if (cfg_url[0] && taillen > DOS_TAIL_MAX) {
        logf("install: REFUSED - url= is too long; DOS would silently chop the "
             "URL and the download would 404");
        return -1;
    }

    f = open_runbat();
    if (!f) return 0;

    emit_log(f, "RUN.BAT starting (install)");
    fprintf(f, "echo Installing %s...\n", g->title);
    /* The staged UNZIP.EXE is a DJGPP build, so its go32 stub needs a DPMI
     * host — which "Restart in MS-DOS mode" does NOT provide (EMM386 supplies
     * VCPI, not DPMI). CWSDPMI.EXE covers that, but the stub looks for it on
     * the PATH rather than beside the exe, so put our directory there. */
    fprintf(f, "SET PATH=%s;%%PATH%%\n", cfg_home);
    /* Missing tools must fail loudly, not as COMMAND.COM's "Bad command or
     * file name" followed by a broken half-install. */
    fprintf(f, "if not exist %s\\UNZIP.EXE goto notool\n", cfg_home);
    if (cfg_url[0])
        fprintf(f, "if not exist %s\\NET\\HTGET.EXE goto notool\n", cfg_home);
    fprintf(f, "if not exist %s\\nul mkdir %s\n", cfg_gamedir, cfg_gamedir);
    fprintf(f, "if not exist %s\\nul mkdir %s\n", dir, dir);
    if (cfg_url[0]) {
        emit_log(f, "fetching the archive with HTGET");
        fprintf(f, "%s\\NET\\HTGET -o %s\\%s.ZIP %s/z/%s",
                cfg_home, cfg_home, stem, cfg_url, stem);
        emit_redirect(f);
        fprintf(f, "if errorlevel 1 ");
        {
            /* Name the server. "no lease, server down, or bad stem" sent the
             * operator looking at the DOS box's networking when the box had a
             * perfectly good DHCP lease and it was the host-side bridge that
             * was not running. NETUP already logged whether the lease came, so
             * the one fact missing from the log was WHICH host we failed to
             * reach. Budgeted by emit_log_p against the 126-byte line limit. */
            char msg[80];
            sprintf(msg, "HTGET failed - is %.40s serving?", cfg_url);
            emit_log_p(f, "if errorlevel 1 ", msg);
        }
    } else {
        emit_log(f, "copying the archive from the mapped drive");
        fprintf(f, "copy \"%s\\%s\" %s\\%s.ZIP",
                cfg_drive, g->path, cfg_home, stem);
        emit_redirect(f);
        fprintf(f, "if errorlevel 1 ");
        emit_log_p(f, "if errorlevel 1 ",
                   "copy failed - drive not mapped, or long filename");
    }
    /* No zip -> the fetch failed; bail with a real message instead of
     * unzipping nothing and "installing" an empty directory. */
    fprintf(f, "if not exist %s\\%s.ZIP goto nofetch\n", cfg_home, stem);
    emit_log(f, "archive downloaded; unpacking");
    fprintf(f, "%s\\UNZIP -qq -o %s\\%s.ZIP -d %s",
            cfg_home, cfg_home, stem, dir);
    emit_redirect(f);
    fprintf(f, "if errorlevel 1 ");
    emit_log_p(f, "if errorlevel 1 ",
               "UNZIP failed - corrupt zip, disk full, or no DPMI");

    /* Hand the reconciliation pass the facts it needs (title, where we
     * unpacked, and the launcher the catalogue named for this game). */
    write_pending(g->title, dir, g->exe, g->tile);
    fprintf(f, "%s\\DOSGAME /snapdirs\n", cfg_home);
    if (g->kind == 'I' && run_installer) {
        emit_log(f, "running the game's own installer");
        fprintf(f, "%c:\ncd %s\n", dir[0], dir + 2);
        emit_installer_hint(f, dir);
        fprintf(f, "if exist INSTALL.EXE INSTALL.EXE\n");
        fprintf(f, "if exist INSTALL.BAT if not exist INSTALL.EXE call INSTALL.BAT\n");
        fprintf(f, "if exist SETUP.EXE if not exist INSTALL.EXE if not exist INSTALL.BAT SETUP.EXE\n");
    }
    /* /postinst records where the game actually ended up — that record is
     * what puts it on the Installed tab as playable. It also answers the
     * question batch cannot: "did anything runnable come out of this?"
     * ("if exist DIR\*.*" is TRUE even for an empty directory, so a corrupt
     * download used to be indistinguishable from a good one.) */
    emit_log(f, "reconciling what the install produced");
    fprintf(f, "%s\\DOSGAME /postinst\n", cfg_home);
    fprintf(f, "if errorlevel 1 goto nogame\n");
    fprintf(f, "del %s\\%s.ZIP\n", cfg_home, stem);
    emit_log(f, "install finished OK");
    fprintf(f, "echo Installed. Press a key to play it from the menu.\n");
    emit_pause(f);
    fprintf(f, "goto end\n");
    fprintf(f, ":nogame\n");
    emit_log(f, "FAILED - nothing runnable came out of this install");
    fprintf(f, "if exist %s\\%s.ZIP del %s\\%s.ZIP\n",
            cfg_home, stem, cfg_home, stem);
    fprintf(f, "echo Nothing runnable was found for this game.\n");
    /* Name the most likely cause. In MS-DOS mode a missing DPMI host makes
     * the DJGPP UNZIP die on "no DPMI - Get csdpmi*b.zip" and leave the
     * directory empty, which otherwise just looks like a bad download. */
    fprintf(f, "if not exist %s\\CWSDPMI.EXE echo (CWSDPMI.EXE is not in %s - UNZIP needs it in MS-DOS mode.)\n",
            cfg_home, cfg_home);
    fprintf(f, "echo The download may be damaged, or it needs setup run by hand:\n");
    fprintf(f, "echo   %s\n", dir);
    emit_pause(f);
    fprintf(f, "goto end\n");
    fprintf(f, ":notool\n");
    emit_log(f, "FAILED - UNZIP.EXE or HTGET.EXE is not staged");
    fprintf(f, "echo UNZIP.EXE or NET\\HTGET.EXE missing under %s - re-stage the DOS tools.\n",
            cfg_home);
    emit_pause(f);
    fprintf(f, "goto end\n");
    fprintf(f, ":nofetch\n");
    emit_log(f, "FAILED - no archive arrived; the fetch did not work");
    fprintf(f, "echo Could not download %s\n", stem);
    fprintf(f, "echo Check that the game server is running and the network is up.\n");
    emit_pause(f);
    fprintf(f, ":end\n");
    fclose(f);
    return 1;
}

/* Why write_install() declined, in the words the footer shows. A single
 * "Download failed" used to cover all of these, which sent people hunting a
 * networking problem that was really a config or catalogue one. */
static const char *install_error(int rc)
{
    switch (rc) {
    case -1: return "Cannot install: the url= in DOSGAME.CFG is too long for DOS.";
    case -2: return "This is a CD image - too big to install over the network.";
    case -3: return "No game server configured: set url= in DOSGAME.CFG.";
    default: return "Could not write RUN.BAT!";
    }
}

/* ---- UI drawing ---- */

static void rebuild_view(void)
{
    int i;
    n_view = 0;
    for (i = 0; i < n_games; i++) {
        if (tab == 0 && games[i].installed == 1) view[n_view++] = i;
        if (tab == 1 && games[i].installed != 1) view[n_view++] = i;
    }
    if (sel >= n_view) sel = n_view ? n_view - 1 : 0;
    if (top > sel) top = sel;
}

static void draw_header(void)
{
    char buf[81];
    cur_attr = 0x1F;
    vfill(0, 0, SCREEN_W, ' ');
    sprintf(buf, " DOS GAME MANAGER v%s", VER);
    vputs(0, 0, buf);
    sprintf(buf, "%d installed%s / %ld catalog ", n_local,
            n_local >= MAX_LOCAL ? "+" : "", cat_total);
    vputs(SCREEN_W - strlen(buf), 0, buf);

    cur_attr = 0x70;
    vfill(0, 1, SCREEN_W, ' ');
    cur_attr = (tab == 0) ? 0x0F : 0x78;
    vputs(2, 1, " Installed ");
    cur_attr = (tab == 1) ? 0x0F : 0x78;
    vputs(15, 1, " Available (LAN) ");

    cur_attr = 0x08;
    vfill(0, 2, SCREEN_W, '-');
    cur_attr = 0x07;
    vfill(0, 3, SCREEN_W, ' ');
    vputs(0, 3, tab == 0 ? "  Title                                    Action"
                         : "  Title                                Size  Type");
}

static void draw_list(void)
{
    int r;
    for (r = 0; r < LIST_ROWS; r++) {
        int idx = top + r;
        char line[81];
        cur_attr = 0x07;
        vfill(0, LIST_TOP + r, SCREEN_W, ' ');
        if (idx >= n_view) continue;
        {
            game_t *g = &games[view[idx]];
            if (tab == 0) {
                const char *what = g->kind == 'Z' ? "unpack + setup"
                                 : g->kind == 'I' ? "run setup"
                                 : g->exe;
                sprintf(line, "  %-40.40s %-14.14s", g->title, what);
            }
            else {
                const char *k = g->kind == 'R' ? "ready" :
                                g->kind == 'I' ? "installer" : "cd";
                char sz[16];
                if (g->size >= 1048576L)
                    sprintf(sz, "%ldM", g->size / 1048576L);
                else
                    sprintf(sz, "%ldK", g->size / 1024L);
                sprintf(line, "%c %-36.36s %6s  %-9s",
                        g->installed == 2 ? '*' : ' ', g->title, sz, k);
            }
            cur_attr = (idx == sel) ? 0x70 : (g->installed == 2 ? 0x02 : 0x07);
            vputs(0, LIST_TOP + r, line);
        }
    }
}

static void draw_footer(const char *msg)
{
    cur_attr = 0x08;
    vfill(0, SCREEN_H - 2, SCREEN_W, '-');
    cur_attr = 0x0E;
    vfill(0, SCREEN_H - 1, SCREEN_W, ' ');
    if (msg && msg[0])
        vputs(1, SCREEN_H - 1, msg);
    else if (tab == 0)
        vputs(1, SCREEN_H - 1,
              "Enter=Play  F2=Change program  F3=Preview  Tab=Catalog  F5=Rescan  Esc=Quit");
    else {
        /* 74 chars of literal plus a filter of up to 23 overflowed the old
         * char[81] and smashed the stack — on a real box that is a hung
         * machine, not a garbled line. Size for the worst case and let vputs
         * clip at the screen edge. */
        char buf[128];
        sprintf(buf, "Find[%s]  Enter=Install  F9=+Setup  F3=Preview  Tab=Back  Esc=Quit",
                cat_filter);
        vputs(1, SCREEN_H - 1, buf);
    }
}

static void draw_all(const char *msg)
{
    draw_header();
    draw_list();
    draw_footer(msg);
}

/* Hand the screen back to DOS (or to a game): restore an 80x25 text mode,
 * show the cursor again, and drop any keys still queued so the next program
 * does not inherit the ones that were meant for the menu. */
static void leave_ui(void)
{
    union REGS r;
    cursor_show();
    r.w.ax = 0x0003;
    int86(0x10, &r, &r);
    kflush();
}

/* ---- main loop ---- */

/* Scan + registry + catalog, in the one order that is correct: the registry
 * first (it has the authoritative title and launcher for anything installed
 * through this program), then the disk scan (which skips whatever the
 * registry already covers), then the catalog. */
static void load_everything(void)
{
    n_games = 0;
    load_registry();
    add_registry_games();
    scan_local();
    n_local = n_games;
    load_catalog();
    mark_installed();
}

/* Find the first game whose title contains `want`, restricted to installed
 * games (installed==1) or catalog rows. Backs /play: and /install:. */
static int find_by_title(const char *want, int want_installed)
{
    int i;
    for (i = 0; i < n_games; i++) {
        if (want_installed != (games[i].installed == 1)) continue;
        if (stristr(games[i].title, want)) return i;
    }
    return -1;
}

int main(int argc, char **argv)
{
    int i, selftest = 0, mode_snap = 0, mode_post = 0, mode_kflush = 0;
    const char *want_play = NULL, *want_inst = NULL;
    for (i = 1; i < argc; i++) {
        if (!strnicmp(argv[i], "/home:", 6))
            copy_str(cfg_home, argv[i] + 6, sizeof(cfg_home));
        if (!stricmp(argv[i], "/selftest")) selftest = 1;
        if (!stricmp(argv[i], "/kflush")) mode_kflush = 1;
        if (!stricmp(argv[i], "/snapdirs")) mode_snap = 1;
        if (!stricmp(argv[i], "/postinst")) mode_post = 1;
        /* Headless equivalents of pressing Enter on a row. They make the
         * whole install->play path scriptable (which is how it is regression
         * tested in DOSBox), and are handy by hand: DOSGAME /play:doom */
        if (!strnicmp(argv[i], "/play:", 6))    want_play = argv[i] + 6;
        if (!strnicmp(argv[i], "/install:", 9)) want_inst = argv[i] + 9;
    }

    /* A not-ready floppy or an empty CD drive under a scan root would pop
     * DOS's "Abort, Retry, Fail?" straight over the TUI, with no way to
     * answer it from a program that has taken over the screen. Fail those
     * calls instead; the scan just sees an empty drive. */
    _harderr(hard_error_handler);

    load_cfg();
    log_open(mode_snap ? "snap" : mode_post ? "post" : "menu");
    logf("---- DOSGAME %s starting ----", VER);
    for (i = 1; i < argc; i++) logf("argv[%d] = %s", i, argv[i]);
    logf("config: home=%s gamedir=%s", cfg_home, cfg_gamedir);
    logf("config: scan=%s", cfg_scan);
    logf("config: url=%s drive=%s", cfg_url[0] ? cfg_url : "(none)",
         cfg_drive[0] ? cfg_drive : "(none)");

    /* Headless helper passes, run from RUN.BAT. They must not touch video. */
    if (mode_kflush) {
        /* Nothing else - just empty the keyboard buffer. mTCP's DHCP treats
         * ANY pending keystroke as the advertised "[ESC] to abort", so a key
         * left over from the menu kills the lease request before it starts. */
        kflush();
        logf("kflush: keyboard buffer drained");
        log_close();
        return 0;
    }
    if (mode_snap) { snap_dirs(); log_close(); return 0; }
    if (mode_post) {
        int rc = post_install();
        logf("post:   exit code %d (%s)", rc,
             rc ? "nothing playable found" : "recorded");
        log_close();
        return rc;
    }

    load_everything();
    logf("loaded: %d installed, %d rows in memory, %ld in the catalog",
         n_local, n_games, cat_total);

    if (want_play || want_inst) {
        int idx = want_play ? find_by_title(want_play, 1)
                            : find_by_title(want_inst, 0);
        if (idx < 0) {
            logf("cli:    no match for \"%s\"", want_play ? want_play : want_inst);
            printf("No match for \"%s\"\n", want_play ? want_play : want_inst);
            log_close();
            return 1;
        }
        if (want_play) {
            if (write_launch(&games[idx]) != 1) { log_close(); return 1; }
        } else {
            if (write_install(&games[idx], games[idx].kind == 'I') != 1) {
                log_close();
                return 1;
            }
        }
        log_close();
        return EXIT_RUNBAT;      /* DOSGAME.BAT runs RUN.BAT, as for Enter */
    }

    if (selftest) {
        /* Deterministic no-input mode for the DOSBox CI loop: dump what
         * the scan+catalog produced, then exit without touching video. */
        char path[MAX_PATH_L + 16];
        FILE *f;
        sprintf(path, "%s\\DGSELF.TXT", cfg_home);
        f = fopen(path, "w");
        if (!f) return 1;
        for (i = 0; i < n_games; i++) {
            char stem[13] = "";
            /* The install stem is emitted for catalog rows so the DOS-side
             * hash can be diffed against serve_dosgames.py's, which has to
             * agree with it byte for byte to resolve /z/<STEM>. */
            if (!games[i].installed) zip_stem(games[i].path, stem);
            fprintf(f, "%d|%c|%s|%s|%s|%s\n", games[i].installed,
                    games[i].kind, games[i].title, games[i].exe,
                    games[i].path, stem);
        }
        fclose(f);
        logf("selftest: wrote %s", path);
        log_close();
        return 0;
    }

    vinit();
    cursor_hide();
    kflush();          /* keys left over from the game we just came back from */
    rebuild_view();
    draw_all(NULL);

    for (;;) {
        int k = getkey();
        int dirty = 1;

        switch (k) {
        case K_ESC:
            leave_ui();
            logf("ui:     quit");
            log_close();
            return EXIT_QUIT;
        case K_TAB:
        case K_LEFT:
        case K_RIGHT:
            tab ^= 1; sel = top = 0; rebuild_view(); break;
        case K_UP:   if (sel > 0) sel--; break;
        case K_DOWN: if (sel < n_view - 1) sel++; break;
        case K_PGUP: sel -= LIST_ROWS; if (sel < 0) sel = 0; break;
        case K_PGDN: sel += LIST_ROWS; if (sel >= n_view) sel = n_view - 1;
                     if (sel < 0) sel = 0; break;
        case K_HOME: sel = 0; break;
        case K_END:  sel = n_view ? n_view - 1 : 0; break;
        case K_F3:
            if (n_view) {
                logf("ui:     preview for \"%s\" (tile %s)",
                     games[view[sel]].title, games[view[sel]].tile);
                if (!show_tile(&games[view[sel]]))
                    draw_all("No preview tile for this game.");
                else
                    draw_all(NULL);
            }
            dirty = 0;
            break;
        case K_F2:
            /* Pick a different program for this game.
             *
             * No heuristic gets this right every time: a game directory can
             * hold a loader, a setup tool, a level editor and the game, and
             * "first non-tool .EXE" picked CB-RUN.COM for Jagged Alliance on
             * the fleet's Win98 box - which exits abnormally on its own. So
             * cycle through what is actually runnable in the directory and
             * remember the choice in the registry, where it outlives a
             * rescan. */
            if (tab == 0 && n_view) {
                game_t *g = &games[view[sel]];
                char next[13];
                if (next_launcher(g->path, g->exe, next)) {
                    copy_str(g->exe, next, sizeof(g->exe));
                    /* Do NOT force 'R'. A row that still needs its installer
                     * run is launched through the snapshot + /postinst wrapper
                     * that works out where the game landed; hardcoding 'R'
                     * threw that away and, since the 'G' row written below
                     * hides the directory from every later scan, there was no
                     * way back to it. Keep the class the scan decided on. */
                    if (g->kind != 'I' && g->kind != 'Z') g->kind = 'R';
                    reg_append(g->kind == 'R' ? 'G' : 'S',
                               g->title, g->path, g->exe, g->tile);
                    logf("ui:     launcher for \"%s\" set to %s",
                         g->title, g->exe);
                    {
                        char msg[81];
                        sprintf(msg, "%.30s will now run %.12s  (F2 for the next one)",
                                g->title, g->exe);
                        draw_all(msg);
                    }
                } else {
                    draw_all("Nothing else runnable in that folder.");
                }
                dirty = 0;
            }
            break;
        case K_F5:
            logf("ui:     rescan requested");
            load_everything();
            sel = top = 0; rebuild_view();
            break;
        case K_ENTER:
            if (!n_view) break;
            {
                game_t *g = &games[view[sel]];
                /* Enter does the RIGHT thing for the kind of archive:
                 * a ready-to-run zip just needs extracting, but an
                 * installer archive (INSTALL.EXE + packed payload — the
                 * majority of the share's catalogue) is not playable until
                 * its installer has run. Extracting and stopping there
                 * leaves the user with a directory that the menu then
                 * won't even list. */
                int ok = g->installed == 1 ? write_launch(g)
                                           : write_install(g, g->kind == 'I');
                if (ok == 1) {
                    leave_ui();
                    log_close();
                    return EXIT_RUNBAT;
                }
                logf("ui:     could not act on \"%s\" (rc=%d): %s",
                     g->title, ok, install_error(ok));
                draw_all(install_error(ok));
                dirty = 0;
            }
            break;
        case K_F9:
            if (tab == 1 && n_view) {
                int ok = write_install(&games[view[sel]], 1);
                if (ok == 1) {
                    leave_ui();
                    log_close();
                    return EXIT_RUNBAT;
                }
                draw_all(install_error(ok));
                dirty = 0;
            }
            break;
        case K_BACK:
            if (tab == 1 && cat_filter[0]) {
                cat_filter[strlen(cat_filter) - 1] = '\0';
                if (!kbhit()) reload_catalog();   /* debounce fast typing */
            }
            break;
        default:
            if (tab == 1 && k >= 32 && k < 127
                && strlen(cat_filter) < sizeof(cat_filter) - 1) {
                size_t fl = strlen(cat_filter);
                cat_filter[fl] = (char)k;
                cat_filter[fl + 1] = '\0';
                /* Reload is a full GAMES.CAT parse — skip it while more
                 * keys are already waiting so fast typing stays smooth
                 * on slow CPUs; the final key triggers the real reload. */
                if (!kbhit()) reload_catalog();
            } else {
                dirty = 0;
            }
        }

        if (sel < top) top = sel;
        if (sel >= top + LIST_ROWS) top = sel - LIST_ROWS + 1;
        if (dirty) draw_all(NULL);
    }
}
