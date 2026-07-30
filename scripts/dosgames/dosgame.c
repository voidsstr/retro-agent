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
 *
 * Build: Open Watcom, real mode large model:
 *   wcl -bcl=dos -ml -os -q dosgame.c
 *
 * Targets 8086+ (the TUI); preview tiles need VGA. Tested in DOSBox-X.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <conio.h>
#include <dos.h>
#include <i86.h>
#include <direct.h>

#define VER "0.1"

/* 256 * sizeof(game_t) must stay well under 64K — a bigger static array
 * silently wraps the data segment in the large model (learned the hard
 * way: entries past ~#420 came back corrupted). The catalog typeahead
 * filter makes 256 in-memory rows enough for ~3000 catalog titles. */
#define MAX_GAMES   256
#define MAX_TITLE   40
#define MAX_PATH_L  80
#define SCREEN_W    80
#define SCREEN_H    25
#define LIST_TOP    4
#define LIST_ROWS   (SCREEN_H - 6)

/* exit codes understood by DOSGAME.BAT */
#define EXIT_QUIT   0
#define EXIT_RUNBAT 42

typedef struct {
    char title[MAX_TITLE + 1];
    char path[MAX_PATH_L + 1];   /* installed: game dir; avail: zip name */
    char exe[13];                /* main exe/bat (8.3) */
    char kind;                   /* 'R' ready, 'I' installer, 'C' cd-image */
    char tile[13];               /* preview tile file name, "" if none */
    long size;                   /* avail: archive bytes */
    int  installed;              /* 1 = local, 0 = share catalog */
} game_t;

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

/* ---- text UI: direct writes to text video memory ---- */

static unsigned short far *vram;
static unsigned char cur_attr = 0x07;

static void vinit(void)
{
    union REGS r;
    r.h.ah = 0x0F;              /* get video mode */
    int86(0x10, &r, &r);
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
        if (!stricmp(line, "gamedir")) strncpy(cfg_gamedir, eq, MAX_PATH_L);
        else if (!stricmp(line, "scan")) strncpy(cfg_scan, eq, sizeof(cfg_scan) - 1);
        else if (!stricmp(line, "url")) strncpy(cfg_url, eq, MAX_PATH_L);
        else if (!stricmp(line, "drive")) strncpy(cfg_drive, eq, MAX_PATH_L);
    }
    fclose(f);
}

/* split '|'-separated fields; returns count */
static int split(char *s, char **fld, int max)
{
    int n = 0;
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
    char path[MAX_PATH_L + 16], line[256];
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
        strncpy(g->title, fld[0], MAX_TITLE);
        strncpy(g->path, fld[1], MAX_PATH_L);
        g->kind = (char)toupper(fld[2][0]);
        strncpy(g->exe, fld[3], 12);
        g->size = 0;
        if (fld[4]) g->size = atol(fld[4]);
        if (fld[5]) strncpy(g->tile, fld[5], 12);
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
    "helpme.exe","dos4gw.exe","univbe.exe", NULL
};

static int is_skip_exe(const char *n)
{
    int i;
    for (i = 0; skip_exes[i]; i++)
        if (!stricmp(n, skip_exes[i])) return 1;
    return 0;
}

/* Join a root and a subdirectory without doubling the separator: the drive
 * root "C:\\" already ends in one, and "C:\\\\DOOM" breaks the launch batch. */
static void path_join(char *out, const char *root, const char *leaf)
{
    int n = (int)strlen(root);
    if (n > 0 && root[n - 1] == '\\')
        sprintf(out, "%s%s", root, leaf);
    else
        sprintf(out, "%s\\%s", root, leaf);
}

static void scan_game_dir(const char *root, const char *dir)
{
    char pat[MAX_PATH_L * 2];
    struct find_t ft;
    char best[13] = "", firstexe[13] = "", firstbat[13] = "";
    char want[13];
    game_t *g;

    sprintf(want, "%.8s.EXE", dir);

    path_join(pat, root, dir);
    strcat(pat, "\\*.*");
    if (_dos_findfirst(pat, _A_NORMAL, &ft) != 0) return;
    do {
        char *dot = strrchr(ft.name, '.');
        if (!dot) continue;
        if (!stricmp(dot, ".EXE") || !stricmp(dot, ".COM")) {
            if (!stricmp(ft.name, want)) strcpy(best, ft.name);
            if (!firstexe[0] && !is_skip_exe(ft.name)) strcpy(firstexe, ft.name);
        } else if (!stricmp(dot, ".BAT")) {
            if (!firstbat[0] && !is_skip_exe(ft.name)) strcpy(firstbat, ft.name);
        }
    } while (_dos_findnext(&ft) == 0);

    if (!best[0]) strcpy(best, firstexe);
    if (!best[0]) strcpy(best, firstbat);
    if (!best[0] || n_games >= MAX_GAMES) return;

    g = &games[n_games++];
    memset(g, 0, sizeof(*g));
    strncpy(g->title, dir, MAX_TITLE);
    path_join(g->path, root, dir);
    strcpy(g->exe, best);
    g->kind = 'R';
    g->installed = 1;
    /* tile name = dir name + .PRV */
    sprintf(g->tile, "%.8s.PRV", dir);
}

static void scan_root(const char *root)
{
    char pat[MAX_PATH_L + 8];
    struct find_t ft;

    path_join(pat, root, "*.*");
    if (_dos_findfirst(pat, _A_SUBDIR, &ft) != 0) return;
    do {
        if ((ft.attrib & _A_SUBDIR) && ft.name[0] != '.'
            && !is_skip_dir(ft.name))
            scan_game_dir(root, ft.name);
    } while (_dos_findnext(&ft) == 0 && n_games < MAX_GAMES);
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

static void rebuild_view(void);
static void mark_installed(void);

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

/* mark catalog entries already installed (dir stem match) */
static void mark_installed(void)
{
    int i, j;
    for (i = 0; i < n_games; i++) {
        char stem[13];
        char *dot;
        if (games[i].installed) continue;
        /* stem of zip name, truncated to 8 chars like the install dir */
        strncpy(stem, games[i].path, 12); stem[12] = '\0';
        dot = strrchr(stem, '.');
        if (dot) *dot = '\0';
        stem[8] = '\0';
        for (j = 0; j < n_games; j++) {
            if (!games[j].installed) continue;
            if (!strnicmp(stem, games[j].title, 8)) {
                games[i].installed = 2;  /* in catalog AND installed */
                break;
            }
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

static int write_launch(const game_t *g)
{
    FILE *f = open_runbat();
    if (!f) return 0;
    fprintf(f, "%c:\ncd %s\n%s\n", g->path[0], g->path + 2, g->exe);
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
    FILE *f = open_runbat();
    char stem[13];
    char *dot;
    if (!f) return 0;

    strncpy(stem, g->path, 12); stem[12] = '\0';
    dot = strrchr(stem, '.'); if (dot) *dot = '\0';
    stem[8] = '\0';
    for (dot = stem; *dot; dot++)
        if (*dot == ' ' || *dot == '.') *dot = '_';

    fprintf(f, "echo Installing %s...\n", g->title);
    fprintf(f, "if not exist %s\\nul mkdir %s\n", cfg_gamedir, cfg_gamedir);
    fprintf(f, "if not exist %s\\%s\\nul mkdir %s\\%s\n",
            cfg_gamedir, stem, cfg_gamedir, stem);
    if (cfg_url[0]) {
        /* URL-encode the zip name; '%' doubled for batch expansion */
        const char *p;
        fprintf(f, "%s\\NET\\HTGET -o %s\\%s.ZIP %s/",
                cfg_home, cfg_home, stem, cfg_url);
        for (p = g->path; *p; p++) {
            unsigned char c = (unsigned char)*p;
            if (isalnum(c) || strchr("-_.~", c))
                fputc(c, f);
            else
                fprintf(f, "%%%%%02X", c);
        }
        fputc('\n', f);
    } else if (cfg_drive[0]) {
        fprintf(f, "copy \"%s\\%s\" %s\\%s.ZIP\n",
                cfg_drive, g->path, cfg_home, stem);
    } else {
        fprintf(f, "echo No share source configured (url= or drive= in DOSGAME.CFG)\n");
        fprintf(f, "pause\n");
        fclose(f);
        return 1;
    }
    fprintf(f, "%s\\UNZIP -qq -o %s\\%s.ZIP -d %s\\%s\n",
            cfg_home, cfg_home, stem, cfg_gamedir, stem);
    fprintf(f, "del %s\\%s.ZIP\n", cfg_home, stem);
    if (g->kind == 'I' && run_installer) {
        fprintf(f, "%c:\ncd %s\\%s\n", cfg_gamedir[0], cfg_gamedir + 2, stem);
        fprintf(f, "if exist INSTALL.EXE INSTALL.EXE\n");
        fprintf(f, "if exist INSTALL.BAT if not exist INSTALL.EXE call INSTALL.BAT\n");
        fprintf(f, "if exist SETUP.EXE if not exist INSTALL.EXE if not exist INSTALL.BAT SETUP.EXE\n");
    }
    fprintf(f, "echo Done. Press a key.\npause > nul\n");
    fclose(f);
    return 1;
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
    sprintf(buf, "%d installed / %ld catalog ", n_local, cat_total);
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
    vputs(0, 3, tab == 0 ? "  Title                                    Run"
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
            if (tab == 0)
                sprintf(line, "  %-40.40s %-12.12s", g->title, g->exe);
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
        char buf[81];
        sprintf(buf, "Search[%s]  Enter=Install+Setup  F9=Setup too  F3=Preview  Tab=Back  Esc=Quit",
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

/* ---- main loop ---- */

int main(int argc, char **argv)
{
    int i, selftest = 0;
    for (i = 1; i < argc; i++) {
        if (!strnicmp(argv[i], "/home:", 6))
            strncpy(cfg_home, argv[i] + 6, MAX_PATH_L - 1);
        if (!stricmp(argv[i], "/selftest")) selftest = 1;
    }

    load_cfg();
    scan_local();
    n_local = n_games;
    load_catalog();
    mark_installed();

    if (selftest) {
        /* Deterministic no-input mode for the DOSBox CI loop: dump what
         * the scan+catalog produced, then exit without touching video. */
        char path[MAX_PATH_L + 16];
        FILE *f;
        sprintf(path, "%s\\DGSELF.TXT", cfg_home);
        f = fopen(path, "w");
        if (!f) return 1;
        for (i = 0; i < n_games; i++)
            fprintf(f, "%d|%c|%s|%s|%s\n", games[i].installed,
                    games[i].kind, games[i].title, games[i].exe,
                    games[i].path);
        fclose(f);
        return 0;
    }

    vinit();
    cursor_hide();
    rebuild_view();
    draw_all(NULL);

    for (;;) {
        int k = getkey();
        int dirty = 1;

        switch (k) {
        case K_ESC:
            cursor_show();
            {
                union REGS r; r.w.ax = 0x0003; int86(0x10, &r, &r);
            }
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
            n_games = 0;
            scan_local();
            n_local = n_games;
            load_catalog(); mark_installed();
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
                if (ok) {
                    cursor_show();
                    {
                        union REGS r; r.w.ax = 0x0003; int86(0x10, &r, &r);
                    }
                    return EXIT_RUNBAT;
                }
                draw_all("Could not write RUN.BAT!");
                dirty = 0;
            }
            break;
        case K_F9:
            if (tab == 1 && n_view) {
                if (write_install(&games[view[sel]], 1)) {
                    cursor_show();
                    {
                        union REGS r; r.w.ax = 0x0003; int86(0x10, &r, &r);
                    }
                    return EXIT_RUNBAT;
                }
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
