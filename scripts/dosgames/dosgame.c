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
    if (!f) return;
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

static const char *skip_exes[] = {
    "install.exe","setup.exe","setsound.exe","sound.exe","uvconfig.exe",
    "config.exe","setmain.exe","readme.exe","catalog.exe","order.exe",
    "helpme.exe","dos4gw.exe","univbe.exe",
    /* archive + system tools that sit beside a game but are not it. Seen on
     * the fleet's Deskpro: TYRIAN listed as "UNZIP32.EXE", KEENDRMS as
     * "PKUNZJR.COM", and C:\GAMES itself as "CACHE.COM". */
    "unzip.exe","unzip32.exe","pkunzip.exe","pkunzjr.com","pkzip.exe",
    "arj.exe","lha.exe","cache.com","cwsdpmi.exe","deice.exe",
    "install.bat","setup.bat", NULL
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

static int is_skip_exe(const char *n)
{
    int i;
    for (i = 0; skip_exes[i]; i++)
        if (!stricmp(n, skip_exes[i])) return 1;
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
            if (!firstexe[0] && !is_skip_exe(ft.name)) strcpy(firstexe, ft.name);
        } else if (!stricmp(dot, ".BAT")) {
            if (!firstbat[0] && !is_skip_exe(ft.name)) strcpy(firstbat, ft.name);
        }
    } while (_dos_findnext(&ft) == 0);

    if (best[0]) return 0;
    if (firstexe[0]) { strcpy(best, firstexe); return 0; }
    if (firstbat[0]) { strcpy(best, firstbat); return 0; }
    /* Nothing runnable: a downloaded-but-not-unpacked game (DEICE.EXE plus
     * packed data is the classic Apogee/id shareware layout). Offer its
     * installer — running that is what makes it playable. */
    if (setup[0]) { strcpy(best, setup); return 1; }
    /* Only an archive in there (a download that was never unpacked, like
     * KEENDRMS = PKUNZJR.COM + a ZIP). Offer it: launching unpacks it with
     * our own UNZIP and then runs whatever installer it contained. */
    if (archive[0]) { strcpy(best, archive); return 2; }
    return -1;
}

static void scan_game_dir(const char *root, const char *dir)
{
    char full[MAX_PATH_L + 1];
    char best[13];
    char sub[13] = "";
    int needs_setup;
    game_t *g;

    path_join(full, root, dir);
    if (!full[0]) return;                    /* path too long to represent */
    if (is_scan_root(full)) return;          /* a root is not a game */
    if (reg_covers_dir(full)) return;        /* registry already owns it */

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
        if (needs_setup < 0) return;
    }

    if (n_games >= MAX_LOCAL) return;
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
            scan_root(p);
            /* drop duplicates introduced by this root */
            for (i = before; i < n_games; i++) {
                for (j = 0; j < before; j++) {
                    if (!stricmp(games[i].title, games[j].title)) {
                        memmove(&games[i], &games[i + 1],
                                (n_games - i - 1) * sizeof(game_t));
                        n_games--; i--;
                        break;
                    }
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
        if (r->flag != 'G' && r->flag != 'X') continue;
        /* Drop rows whose directory (or launcher) has gone away — a game the
         * user deleted by hand must not linger in the menu offering to
         * launch nothing. */
        if (!dir_exists(r->dir)) continue;
        if (r->flag == 'G' && (!r->exe[0] || !file_exists(r->dir, r->exe)))
            continue;
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
    }
    fclose(f);
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
        if (reg[i].flag != 'G') continue;
        g = &games[n_games++];
        memset(g, 0, sizeof(*g));
        copy_str(g->title, reg[i].title, sizeof(g->title));
        copy_str(g->path, reg[i].dir, sizeof(g->path));
        copy_str(g->exe, reg[i].exe, sizeof(g->exe));
        g->kind = 'R';
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

    presnap_path(path);
    out = fopen(path, "w");
    if (!out) return;

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
                    if (full[0]) fprintf(out, "%s\n", full);
                } while (_dos_findnext(&ft) == 0);
            }
        }
        p = next;
    }
    fclose(out);
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

/* Reconcile after an install/installer run and record the result in the
 * registry. Runs headlessly from RUN.BAT (/postinst), never touches video.
 * Returns 0 when a playable game was recorded, 1 when nothing runnable could
 * be found — RUN.BAT branches on that errorlevel to tell the user, because
 * batch cannot make the judgement itself ("if exist DIR\*.*" is TRUE even for
 * an empty directory, so an unzip that produced nothing looks like success). */
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
    if (!f) return 1;
    if (fgets(title, sizeof(title), f)) chomp(title);
    if (fgets(unpack, sizeof(unpack), f)) chomp(unpack);
    if (fgets(want, sizeof(want), f)) chomp(want);
    if (fgets(tile, sizeof(tile), f)) chomp(tile);
    fclose(f);
    remove(path);
    if (!title[0] || !unpack[0]) return 1;

    load_registry();

    /* 1. Did the unpack directory itself end up playable? Prefer the exe the
     *    catalog told us about — the scan's "first .EXE in directory order"
     *    guess picks things like SETSOUND.EXE or a level editor. */
    if (want[0] && file_exists(unpack, want)) {
        copy_str(gamedir, unpack, sizeof(gamedir));
        copy_str(best, want, sizeof(best));
    } else {
        const char *leaf = strrchr(unpack, '\\');
        if (pick_launcher(unpack, leaf ? leaf + 1 : unpack, best) == 0)
            copy_str(gamedir, unpack, sizeof(gamedir));
    }

    /* 1b. Non-flat archive: the game is one level down. */
    if (!gamedir[0]) {
        char deep[MAX_PATH_L + 1];
        if (find_deep_launcher(unpack, want, deep, best))
            copy_str(gamedir, deep, sizeof(gamedir));
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
                        if (want[0] && file_exists(full, want)) {
                            copy_str(gamedir, full, sizeof(gamedir));
                            copy_str(best, want, sizeof(best));
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

    /* 3. Record it. A playable directory that is NOT the unpack directory
     *    also means the unpack directory is spent — mark it hidden so the
     *    menu stops offering "run setup" on a pile of installer leftovers. */
    presnap_path(path);
    remove(path);
    if (!gamedir[0]) return 1;

    reg_append('G', title, gamedir, best, tile);
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

/* Every "press a key" in a generated script goes through here so the whole
 * install path can run unattended: touch C:\DOSGAME\QUIET.FLG and the pauses
 * skip themselves. A marker file rather than an environment variable because
 * COMMAND.COM's IF "%VAR%"=="x" quoting is a trap not worth stepping in. */
static void emit_pause(FILE *f)
{
    fprintf(f, "if not exist %s\\QUIET.FLG pause > nul\n", cfg_home);
}

static int write_launch(const game_t *g)
{
    FILE *f = open_runbat();
    if (!f) return 0;
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
        fprintf(f, "%s\\DOSGAME /snapdirs\n", cfg_home);
        fprintf(f, "echo Running setup for %s ...\n", g->title);
        emit_run(f, g->exe);
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
        emit_run(f, g->exe);
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
    if (g->kind == 'C') return -2;
    /* Nothing to fetch from: answer before a single `goto notool` is written,
     * because the early return below never writes the :notool label and
     * COMMAND.COM aborts the script with "Label not found". */
    if (!cfg_url[0] && !cfg_drive[0]) return -3;

    zip_stem(g->path, stem);
    sprintf(dir, "%s\\%s", cfg_gamedir, stem);

    /* The fetch is one command line, and DOS silently truncates a command
     * tail at 126 bytes. The old code pasted the full URL-encoded zip name
     * (61 chars on average, 137 at worst) onto the URL, so 845 of the 2,982
     * catalogue entries fetched a chopped-off URL, 404'd, and reported
     * "Download failed - check the network". The server now resolves the
     * 8-char stem via /z/<STEM>, which is a fixed, short line — but check it
     * anyway and refuse to write a script that cannot work. */
    taillen = 3 + (int)strlen(cfg_home) + 6 + 4      /* -o <home>\<stem>.ZIP */
              + 1 + (int)strlen(cfg_url) + 3 + 8;    /*  <url>/z/<stem>      */
    if (cfg_url[0] && taillen > DOS_TAIL_MAX)
        return -1;

    f = open_runbat();
    if (!f) return 0;

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
        fprintf(f, "%s\\NET\\HTGET -o %s\\%s.ZIP %s/z/%s\n",
                cfg_home, cfg_home, stem, cfg_url, stem);
    } else {
        fprintf(f, "copy \"%s\\%s\" %s\\%s.ZIP\n",
                cfg_drive, g->path, cfg_home, stem);
    }
    /* No zip -> the fetch failed; bail with a real message instead of
     * unzipping nothing and "installing" an empty directory. */
    fprintf(f, "if not exist %s\\%s.ZIP goto nofetch\n", cfg_home, stem);
    fprintf(f, "%s\\UNZIP -qq -o %s\\%s.ZIP -d %s\n",
            cfg_home, cfg_home, stem, dir);

    /* Hand the reconciliation pass the facts it needs (title, where we
     * unpacked, and the launcher the catalogue named for this game). */
    write_pending(g->title, dir, g->exe, g->tile);
    fprintf(f, "%s\\DOSGAME /snapdirs\n", cfg_home);
    if (g->kind == 'I' && run_installer) {
        fprintf(f, "%c:\ncd %s\n", dir[0], dir + 2);
        fprintf(f, "if exist INSTALL.EXE INSTALL.EXE\n");
        fprintf(f, "if exist INSTALL.BAT if not exist INSTALL.EXE call INSTALL.BAT\n");
        fprintf(f, "if exist SETUP.EXE if not exist INSTALL.EXE if not exist INSTALL.BAT SETUP.EXE\n");
    }
    /* /postinst records where the game actually ended up — that record is
     * what puts it on the Installed tab as playable. It also answers the
     * question batch cannot: "did anything runnable come out of this?"
     * ("if exist DIR\*.*" is TRUE even for an empty directory, so a corrupt
     * download used to be indistinguishable from a good one.) */
    fprintf(f, "%s\\DOSGAME /postinst\n", cfg_home);
    fprintf(f, "if errorlevel 1 goto nogame\n");
    fprintf(f, "del %s\\%s.ZIP\n", cfg_home, stem);
    fprintf(f, "echo Installed. Press a key to play it from the menu.\n");
    emit_pause(f);
    fprintf(f, "goto end\n");
    fprintf(f, ":nogame\n");
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
    fprintf(f, "echo UNZIP.EXE or NET\\HTGET.EXE missing under %s - re-stage the DOS tools.\n",
            cfg_home);
    emit_pause(f);
    fprintf(f, "goto end\n");
    fprintf(f, ":nofetch\n");
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
              "Enter=Play  F3=Preview  Tab=Catalog  F5=Rescan  Esc=Quit");
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
    int i, selftest = 0, mode_snap = 0, mode_post = 0;
    const char *want_play = NULL, *want_inst = NULL;
    for (i = 1; i < argc; i++) {
        if (!strnicmp(argv[i], "/home:", 6))
            copy_str(cfg_home, argv[i] + 6, sizeof(cfg_home));
        if (!stricmp(argv[i], "/selftest")) selftest = 1;
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

    /* Headless helper passes, run from RUN.BAT. They must not touch video. */
    if (mode_snap) { snap_dirs(); return 0; }
    if (mode_post) return post_install();

    load_everything();

    if (want_play || want_inst) {
        int idx = want_play ? find_by_title(want_play, 1)
                            : find_by_title(want_inst, 0);
        if (idx < 0) {
            printf("No match for \"%s\"\n", want_play ? want_play : want_inst);
            return 1;
        }
        if (want_play) {
            if (write_launch(&games[idx]) != 1) return 1;
        } else {
            if (write_install(&games[idx], games[idx].kind == 'I') != 1)
                return 1;
        }
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
                if (!show_tile(&games[view[sel]]))
                    draw_all("No preview tile for this game.");
                else
                    draw_all(NULL);
            }
            dirty = 0;
            break;
        case K_F5:
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
                    return EXIT_RUNBAT;
                }
                draw_all(install_error(ok));
                dirty = 0;
            }
            break;
        case K_F9:
            if (tab == 1 && n_view) {
                int ok = write_install(&games[view[sel]], 1);
                if (ok == 1) {
                    leave_ui();
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
