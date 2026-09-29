/* test_grledger.c - GAMESYNC and GAMERES must not undo each other.
 *
 * TRUE-SOURCE: compiles agent/shared/grledger.h (the ledger gs_copy_file()
 * consults, agent/src/gamesync.c) and agent/shared/gameres.h (gr_reg_owner,
 * which gs_merge_reg() asks), the code the agent runs.
 *
 * THE DEFECT (agent 1.90.0, 2026-09-28). gs_run() copies a title, then GAMERES
 * writes the box's resolution into the title's own config. That leaves the
 * file different from the library's copy, so the NEXT sync's resume test (size
 * AND mtime, gsresume.h) copied the library's back, and GAMERES changed it
 * again - on every sync, forever:
 *
 *   .110 (XP)   Thief2\cam.cfg 1379 B "game_screen_size 800 600" -> 1380 B
 *               "1024 768"; Daggerfall's dosbox conf fullresolution
 *               desktop <-> original; 5 quiet syncs wrote 22/20/11/11/20
 *               files, GAMERES reported 23 values changed EVERY run, and
 *               every run rebuilt the icons ("desktop changed ... arranging")
 *   .243 (98SE) Descent1\DESCENT.CFG: ResolutionX/Y appended every run
 *               ("Descent1 - 2 value(s) set"), 1 file written every run
 *
 * and the registry did the same: install.reg's constants (CounterStrike16 pins
 * the shared GoldSrc key at 800x600) were re-merged every sync and GAMERES put
 * the box's values back - "CounterStrike16 - 4 value(s) set" forever.
 *
 * Each test asserts the FIXED behaviour and, where it can be modelled, the
 * OLD behaviour the fix replaces, so a regression shows as the old numbers.
 */
#include "munit.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "../../agent/shared/grledger.h"
#include "../../agent/shared/gameres.h"

#define SEC 10000000LL                      /* one second in FILETIME ticks */
#define T_LIB   (133400000000000000LL)      /* the library file's mtime (2023) */
#define T_NOW   (134050000000000000LL)      /* when GAMERES wrote (2025)       */

static grl_t L;                             /* 37 KB: keep it off the stack */

/* ---------------------------------------------------------------------- */
/* a model of one file through repeated syncs                              */
/* ---------------------------------------------------------------------- */

typedef struct {
    long long size, time;       /* the file's metadata          */
    int adjusted;               /* its CONTENT: 1 = the box's resolution */
} file_t;

/* One sync of one file: the resume test (+ the ledger when use_ledger), then
 * GAMERES. Returns the number of COPIES this sync made; *gr_changes gets the
 * number of values GAMERES changed. The clock advances every call. */
static int one_sync(file_t *box, const file_t *lib, int use_ledger,
                    long long *clock, int *gr_changes)
{
    int copies = 0, v;
    const char *path = "C:\\Games\\Thief2\\cam.cfg";

    v = gsr_decide(1, box->size, box->time, lib->size, 1, lib->time);
    if (v == GSR_ASK_SOURCE)
        v = gsr_decide_source(1, lib->time, box->time);
    if (v != GSR_SKIP && use_ledger) {
        int i = grl_find(&L, path);
        if (i >= 0) {
            int lv = grl_decide(&L.e[i], box->size, box->time, lib->size, 1, lib->time);
            if (lv == GRL_ASK_SOURCE)
                lv = grl_decide_source(&L.e[i], 1, lib->time);
            if (lv == GRL_SKIP)
                v = GSR_SKIP;
        }
    }
    if (v != GSR_SKIP) {
        *box = *lib;                    /* copied, stamped with the library's time */
        copies = 1;
        if (use_ledger)
            grl_forget(&L, path);
    }
    /* GAMERES: the library's copy says 800x600; this box wants 1024x768. */
    *gr_changes = 0;
    if (!box->adjusted) {
        long long pre_s = box->size, pre_t = box->time;
        box->adjusted = 1;
        box->size += 1;                 /* "800 600" -> "1024 768" */
        box->time = *clock;
        *gr_changes = 1;
        if (use_ledger)
            grl_note(&L, path, pre_s, pre_t, box->size, box->time);
    }
    *clock += 600 * SEC;                /* the next sync is ten minutes later */
    return copies;
}

TEST(t_the_fight_old_and_fixed)
{
    file_t lib = { 1379, T_LIB, 0 };
    file_t box = { 0, 0, 0 };
    long long clock = T_NOW;
    int s, copies_old = 0, copies_new = 0, gr_old = 0, gr_new = 0, g;

    /* OLD (1.90.0): no ledger. Every sync copies and GAMERES changes it. */
    box.size = 0; box.time = 0; box.adjusted = 0;
    for (s = 0; s < 5; s++) {
        copies_old += one_sync(&box, &lib, 0, &clock, &g);
        gr_old += g;
    }
    CHECK_EQ_I(copies_old, 5);          /* the defect: a copy on every sync  */
    CHECK_EQ_I(gr_old, 5);              /* ...and a GAMERES change every sync */

    /* FIXED: the first sync copies and adjusts; every later one keeps it. */
    grl_reset(&L);
    box.size = 0; box.time = 0; box.adjusted = 0;
    for (s = 0; s < 5; s++) {
        int c = one_sync(&box, &lib, 1, &clock, &g);
        copies_new += c;
        gr_new += g;
        if (s > 0) {
            CHECK_EQ_I(c, 0);           /* a settled box: 0 file(s) written  */
            CHECK_EQ_I(g, 0);           /* ...and 0 value(s) changed          */
        }
    }
    CHECK_EQ_I(copies_new, 1);
    CHECK_EQ_I(gr_new, 1);
    CHECK(box.adjusted, "the box keeps ITS resolution, not the library's");
}

TEST(t_library_update_is_still_taken)
{
    file_t lib = { 1379, T_LIB, 0 };
    file_t box = { 0, 0, 0 };
    long long clock = T_NOW;
    int g, c;

    grl_reset(&L);
    one_sync(&box, &lib, 1, &clock, &g);
    one_sync(&box, &lib, 1, &clock, &g);

    /* the library ships a new cam.cfg (bigger) */
    lib.size = 1400; lib.time = T_LIB + 86400 * SEC;
    c = one_sync(&box, &lib, 1, &clock, &g);
    CHECK_EQ_I(c, 1);                   /* the new library copy is taken ...  */
    CHECK_EQ_I(g, 1);                   /* ... and GAMERES adjusts IT         */
    CHECK_EQ_I(L.e[grl_find(&L, "C:\\Games\\Thief2\\cam.cfg")].base_size, 1400);
    c = one_sync(&box, &lib, 1, &clock, &g);
    CHECK_EQ_I(c, 0);                   /* and it settles again               */
    CHECK_EQ_I(g, 0);

    /* the library's file is edited to the SAME size - only its time moves */
    lib.time += 3600 * SEC;
    c = one_sync(&box, &lib, 1, &clock, &g);
    CHECK_EQ_I(c, 1);
    CHECK_EQ_I(g, 1);
}

/* ---------------------------------------------------------------------- */
/* the decision, case by case                                              */
/* ---------------------------------------------------------------------- */

static grl_entry_t ent(long long bs, long long bt, long long os, long long ot)
{
    grl_entry_t e;
    memset(&e, 0, sizeof(e));
    strcpy(e.path, "C:\\Games\\Daggerfall\\dosbox_daggerfall.conf");
    e.base_size = bs; e.base_time = bt; e.out_size = os; e.out_time = ot;
    return e;
}

TEST(t_source_unchanged_skips)
{
    grl_entry_t e = ent(11453, T_LIB, 11454, T_NOW);
    CHECK_EQ_I(grl_decide(&e, 11454, T_NOW, 11453, 1, T_LIB), GRL_SKIP);
    /* the OLD test alone copies this file: sizes differ */
    CHECK_EQ_I(gsr_decide(1, 11454, T_NOW, 11453, 1, T_LIB), GSR_COPY);
}

TEST(t_source_changed_copies)
{
    grl_entry_t e = ent(11453, T_LIB, 11454, T_NOW);
    /* size moved */
    CHECK_EQ_I(grl_decide(&e, 11454, T_NOW, 11460, 1, T_LIB), GRL_SRC_CHANGED);
    /* same size, time moved: ask the source, which confirms the change */
    CHECK_EQ_I(grl_decide(&e, 11454, T_NOW, 11453, 1, T_LIB + 60 * SEC), GRL_ASK_SOURCE);
    CHECK_EQ_I(grl_decide_source(&e, 1, T_LIB + 60 * SEC), GRL_SRC_CHANGED);
    /* the source cannot be asked: never a skip on no evidence */
    CHECK_EQ_I(grl_decide_source(&e, 0, 0), GRL_SRC_CHANGED);
}

TEST(t_listing_disagrees_but_source_agrees_skips)
{
    /* gsresume.h's case: a listing time that disagrees with the file's own */
    grl_entry_t e = ent(307, T_LIB, 343, T_NOW);
    CHECK_EQ_I(grl_decide(&e, 343, T_NOW, 307, 1, T_LIB + 3600 * SEC), GRL_ASK_SOURCE);
    CHECK_EQ_I(grl_decide_source(&e, 1, T_LIB), GRL_SKIP);
    /* no listing time at all: ask */
    CHECK_EQ_I(grl_decide(&e, 343, T_NOW, 307, 0, 0), GRL_ASK_SOURCE);
}

TEST(t_dest_changed_on_the_box_is_not_kept)
{
    grl_entry_t e = ent(307, T_LIB, 343, T_NOW);
    /* a person (or the game) edited the file: size moved */
    CHECK_EQ_I(grl_decide(&e, 350, T_NOW + 99 * SEC, 307, 1, T_LIB), GRL_DST_CHANGED);
    /* same size, touched later */
    CHECK_EQ_I(grl_decide(&e, 343, T_NOW + 99 * SEC, 307, 1, T_LIB), GRL_DST_CHANGED);
    /* no record at all: no opinion either */
    CHECK_EQ_I(grl_decide(NULL, 343, T_NOW, 307, 1, T_LIB), GRL_DST_CHANGED);
}

TEST(t_fat32_rounds_to_two_seconds)
{
    /* Win98 (.243) stores write times to 2 s: the SAME slack as the resume test */
    grl_entry_t e = ent(307, T_LIB + 1 * SEC, 343, T_NOW + 1 * SEC);
    CHECK_EQ_I(grl_decide(&e, 343, T_NOW, 307, 1, T_LIB), GRL_SKIP);
    CHECK_EQ_I(grl_decide(&e, 343, T_NOW + 5 * SEC, 307, 1, T_LIB), GRL_DST_CHANGED);
}

/* ---------------------------------------------------------------------- */
/* the table                                                               */
/* ---------------------------------------------------------------------- */

TEST(t_chain_keeps_the_library_base)
{
    /* Descent: ResolutionX then ResolutionY, two rules on ONE file. The second
     * write's "before" is the first write's "after"; the base must stay the
     * library's copy or the next sync compares the library to GAMERES's own
     * intermediate file and copies forever. */
    const char *p = "C:\\Games\\Descent1\\DESCENT.CFG";
    int i;
    grl_reset(&L);
    grl_note(&L, p, 307, T_LIB, 325, T_NOW);
    grl_note(&L, p, 325, T_NOW, 343, T_NOW + 1 * SEC);
    CHECK_EQ_I(L.n, 1);
    i = grl_find(&L, p);
    CHECK(i >= 0, "recorded");
    CHECK_EQ_I(L.e[i].base_size, 307);
    CHECK_EQ_I(L.e[i].base_time, T_LIB);
    CHECK_EQ_I(L.e[i].out_size, 343);
    CHECK_EQ_I(grl_decide(&L.e[i], 343, T_NOW + 1 * SEC, 307, 1, T_LIB), GRL_SKIP);
}

TEST(t_monitor_change_keeps_the_library_base)
{
    /* The file was KEPT (skipped) and GAMERES now adjusts it again because the
     * target moved: the base is still the library's copy. */
    const char *p = "C:\\Games\\Thief2\\cam.cfg";
    grl_reset(&L);
    grl_note(&L, p, 1379, T_LIB, 1380, T_NOW);
    grl_note(&L, p, 1380, T_NOW, 1381, T_NOW + 3600 * SEC);
    CHECK_EQ_I(L.e[0].base_size, 1379);
    CHECK_EQ_I(L.e[0].out_size, 1381);
}

TEST(t_fresh_copy_is_its_own_base)
{
    /* The library changed, the file was copied (record forgotten), GAMERES
     * adjusts the new copy: the new copy is the base, not the old record's. */
    const char *p = "C:\\Games\\Thief2\\cam.cfg";
    grl_reset(&L);
    grl_note(&L, p, 1379, T_LIB, 1380, T_NOW);
    grl_forget(&L, p);
    CHECK_EQ_I(L.n, 0);
    grl_note(&L, p, 1400, T_LIB + 99 * SEC, 1401, T_NOW + 99 * SEC);
    CHECK_EQ_I(L.e[0].base_size, 1400);
    /* and a stale record whose "after" does not match is replaced, not chained */
    grl_note(&L, p, 1500, T_LIB + 500 * SEC, 1501, T_NOW + 500 * SEC);
    CHECK_EQ_I(L.n, 1);
    CHECK_EQ_I(L.e[0].base_size, 1500);
}

TEST(t_paths_compare_case_insensitively)
{
    grl_reset(&L);
    grl_note(&L, "C:\\Games\\BF1942\\Mods\\bf1942\\Settings\\Profiles\\Default\\Video.con",
             100, T_LIB, 101, T_NOW);
    CHECK(grl_find(&L, "c:\\games\\bf1942\\MODS\\BF1942\\settings\\profiles\\default\\VIDEO.CON") == 0,
          "Windows paths are case-insensitive");
    CHECK(grl_find(&L, "C:\\Games\\BF1942\\Mods\\bf1942\\Settings\\Profiles\\Custom\\Video.con") < 0,
          "a different file is a different file");
}

TEST(t_bounded_one_record_per_path_oldest_evicted)
{
    char p[GRL_PATH_MAX];
    int i;
    grl_reset(&L);
    for (i = 0; i < 3 * GRL_MAX_ENTRIES; i++) {
        snprintf(p, sizeof(p), "C:\\Games\\T%03d\\x.cfg", i);
        grl_note(&L, p, i, T_LIB, i + 1, T_NOW);
    }
    CHECK_EQ_I(L.n, GRL_MAX_ENTRIES);
    CHECK(grl_find(&L, "C:\\Games\\T000\\x.cfg") < 0, "the oldest went first");
    snprintf(p, sizeof(p), "C:\\Games\\T%03d\\x.cfg", 3 * GRL_MAX_ENTRIES - 1);
    CHECK(grl_find(&L, p) == GRL_MAX_ENTRIES - 1, "the newest is last");

    /* re-noting one file a thousand times does not grow anything */
    grl_reset(&L);
    for (i = 0; i < 1000; i++)
        grl_note(&L, "C:\\Games\\Thief2\\cam.cfg", 1379, T_LIB, 1380, T_NOW + i * SEC * 10);
    CHECK_EQ_I(L.n, 1);

    /* an impossible path is refused, not truncated into someone else's */
    {
        char big[GRL_PATH_MAX + 40];
        memset(big, 'a', sizeof(big));
        big[GRL_PATH_MAX] = 0;                  /* one past what a record holds */
        CHECK_EQ_I(grl_note(&L, big, 1, 1, 2, 2), 0);
        big[GRL_PATH_MAX - 1] = 0;              /* the longest it does hold    */
        CHECK_EQ_I(grl_note(&L, big, 1, 1, 2, 2), 1);
    }
    CHECK_EQ_I(grl_note(&L, "", 1, 1, 2, 2), 0);
}

/* ---------------------------------------------------------------------- */
/* the file                                                                */
/* ---------------------------------------------------------------------- */

static char buf[GRL_FILE_MAX + 1];
static grl_t L2;

TEST(t_round_trip)
{
    int n, bad = -1, i;
    grl_reset(&L);
    grl_note(&L, "C:\\Games\\Thief2\\cam.cfg", 1379, T_LIB, 1380, T_NOW);
    grl_note(&L, "C:\\Games\\Daggerfall\\dosbox_daggerfall.conf", 11453, T_LIB + 2, 11454, T_NOW + 7);
    grl_note(&L, "C:\\Games\\Descent1\\DESCENT.CFG", 307, 0, 343, 999999999999999999LL);
    n = grl_format(&L, buf, sizeof(buf));
    CHECK(n > 0, "formats");
    CHECK(memcmp(buf, GRL_HEADER "\r\n", strlen(GRL_HEADER) + 2) == 0, "header first");
    CHECK_EQ_I(grl_parse(&L2, buf, (size_t)n, &bad), 3);
    CHECK_EQ_I(bad, 0);
    CHECK_EQ_I(L2.dirty, 0);
    for (i = 0; i < 3; i++) {
        CHECK(strcmp(L.e[i].path, L2.e[i].path) == 0, "path");
        CHECK_EQ_I(L.e[i].base_size, L2.e[i].base_size);
        CHECK_EQ_I(L.e[i].base_time, L2.e[i].base_time);
        CHECK_EQ_I(L.e[i].out_size, L2.e[i].out_size);
        CHECK_EQ_I(L.e[i].out_time, L2.e[i].out_time);
    }
}

TEST(t_full_table_fits_the_size_limit)
{
    char p[GRL_PATH_MAX];
    int i, n;
    grl_reset(&L);
    for (i = 0; i < GRL_MAX_ENTRIES; i++) {
        memset(p, 'x', sizeof(p));
        snprintf(p, 16, "C:\\G\\%04d\\", i);
        p[strlen(p)] = 'x';
        p[GRL_PATH_MAX - 1] = 0;         /* the longest path a record holds */
        grl_note(&L, p, 999999999999999999LL, 999999999999999999LL,
                 999999999999999999LL, 999999999999999999LL);
    }
    CHECK_EQ_I(L.n, GRL_MAX_ENTRIES);
    n = grl_format(&L, buf, sizeof(buf));
    CHECK(n > 0 && n <= GRL_FILE_MAX, "a full ledger is still a readable ledger");
    CHECK_EQ_I(grl_parse(&L2, buf, (size_t)n, NULL), GRL_MAX_ENTRIES);
    CHECK_EQ_I(grl_format(&L, buf, 100), -1);   /* too small: write nothing */
}

/* A damaged ledger must only ever cost a COPY. Each case: after parsing, the
 * record for cam.cfg is gone, so gs_copy_file() has no entry and the resume
 * test's COPY stands - the old behaviour, exactly. */
static int keeps_cam(const char *text)
{
    int i;
    grl_parse(&L2, text, strlen(text), NULL);
    i = grl_find(&L2, "C:\\Games\\Thief2\\cam.cfg");
    return i >= 0 && grl_decide(&L2.e[i], 1380, T_NOW, 1379, 1, T_LIB) == GRL_SKIP;
}

TEST(t_corrupt_ledger_means_copy)
{
    char good[512], bad[512];
    char *nl;
    int n, bdrop = 0;

    grl_reset(&L);
    grl_note(&L, "C:\\Games\\Thief2\\cam.cfg", 1379, T_LIB, 1380, T_NOW);
    n = grl_format(&L, good, sizeof(good));
    CHECK(n > 0, "formats");
    CHECK(keeps_cam(good), "the intact ledger keeps the file");

    /* no header / wrong header */
    CHECK_EQ_I(grl_parse(&L2, good + strlen(GRL_HEADER) + 2,
                         strlen(good) - strlen(GRL_HEADER) - 2, NULL), -1);
    strcpy(bad, good); bad[0] = 'X';
    CHECK(!keeps_cam(bad), "a foreign header is not our ledger");
    strcpy(bad, good); bad[strlen(GRL_HEADER) - 1] = '2';
    CHECK(!keeps_cam(bad), "a future format is not guessed at");

    /* a flipped digit in the record: the checksum drops it */
    strcpy(bad, good);
    nl = strchr(bad, '\n') + 1;
    nl[12] = (char)(nl[12] == '1' ? '2' : '1');
    CHECK(!keeps_cam(bad), "a bit flip must not produce a skip");
    grl_parse(&L2, bad, strlen(bad), &bdrop);
    CHECK_EQ_I(bdrop, 1);
    CHECK_EQ_I(L2.dirty, 1);             /* rewritten clean at the next save */

    /* truncated mid-record (a crash during the write) */
    strcpy(bad, good);
    bad[strlen(bad) - 8] = 0;
    CHECK(!keeps_cam(bad), "a truncated record is dropped");

    /* a bad checksum field */
    strcpy(bad, good);
    nl = strchr(bad, '\n') + 1;
    nl[0] = 'z';
    CHECK(!keeps_cam(bad), "a non-hex checksum is dropped");

    /* empty and garbage */
    CHECK(!keeps_cam(""), "empty");
    CHECK(!keeps_cam("\x01\x02\x03\xff\xfe"), "binary garbage");
    CHECK_EQ_I(grl_parse(&L2, NULL, 0, NULL), -1);

    /* too big to be ours */
    memset(buf, 'a', sizeof(buf) - 1);
    memcpy(buf, good, strlen(good));
    buf[sizeof(buf) - 1] = 0;
    CHECK_EQ_I(grl_parse(&L2, buf, GRL_FILE_MAX + 1, NULL), -1);
    CHECK_EQ_I(L2.n, 0);
}

TEST(t_corrupt_record_does_not_take_the_others)
{
    char text[1024], *second;
    grl_reset(&L);
    grl_note(&L, "C:\\Games\\Thief2\\cam.cfg", 1379, T_LIB, 1380, T_NOW);
    grl_note(&L, "C:\\Games\\Shogo\\autoexec.cfg", 500, T_LIB, 510, T_NOW);
    grl_format(&L, text, sizeof(text));
    second = strstr(text, "Shogo");
    second[0] = 's';                     /* its checksum no longer matches */
    CHECK_EQ_I(grl_parse(&L2, text, strlen(text), NULL), 1);
    CHECK(grl_find(&L2, "C:\\Games\\Thief2\\cam.cfg") == 0, "the intact record survives");
    CHECK(grl_find(&L2, "C:\\Games\\Shogo\\autoexec.cfg") < 0, "the damaged one is gone");
}

TEST(t_numbers_are_ours_not_printf)
{
    /* Win98's msvcrt has no %lld; the header formats by hand. 18 digits max:
     * a 19-digit field is refused on reading, never wrapped. */
    char text[256];
    const char *body = "0 0 0 0 C:\\x";
    unsigned long h = grl_fnv(body, strlen(body));
    snprintf(text, sizeof(text), GRL_HEADER "\r\n%08lx %s\r\n", h, body);
    CHECK_EQ_I(grl_parse(&L2, text, strlen(text), NULL), 1);
    body = "1234567890123456789 0 0 0 C:\\x";
    h = grl_fnv(body, strlen(body));
    snprintf(text, sizeof(text), GRL_HEADER "\r\n%08lx %s\r\n", h, body);
    CHECK_EQ_I(grl_parse(&L2, text, strlen(text), NULL), 0);
    body = "-5 0 0 0 C:\\x";
    h = grl_fnv(body, strlen(body));
    snprintf(text, sizeof(text), GRL_HEADER "\r\n%08lx %s\r\n", h, body);
    CHECK_EQ_I(grl_parse(&L2, text, strlen(text), NULL), 0);
    /* a control character in the path */
    body = "1 2 3 4 C:\\a\tb";
    h = grl_fnv(body, strlen(body));
    snprintf(text, sizeof(text), GRL_HEADER "\r\n%08lx %s\r\n", h, body);
    CHECK_EQ_I(grl_parse(&L2, text, strlen(text), NULL), 0);
}

/* ---------------------------------------------------------------------- */
/* the registry half: which values are the box's                           */
/* ---------------------------------------------------------------------- */

TEST(t_reg_owner_names_the_values_install_reg_pins)
{
    /* The values the staged install.reg files set AND a GAMERES rule sets
     * (measured against the library 2026-09-28). */
    CHECK(gr_reg_owner("HKCU", "Software\\Valve\\Half-Life\\Settings", "ScreenWidth") != NULL,
          "GoldSrc width");
    CHECK(strcmp(gr_reg_owner("HKCU", "Software\\Valve\\Half-Life\\Settings", "EngineModeH"),
                 "CounterStrike16") == 0, "the rule belongs to CounterStrike16");
    CHECK(gr_reg_owner("hkcu", "SOFTWARE\\VALVE\\HALF-LIFE\\SETTINGS", "screenheight") != NULL,
          "the registry is case-insensitive, so is this");
    CHECK(gr_reg_owner("HKCU", "Software\\Remedy Entertainment\\Max Payne\\Video Settings",
                       "Display Width") != NULL, "Max Payne");
    CHECK(gr_reg_owner("HKLM", "Software\\Lonely Cat Games\\Hidden and Dangerous Deluxe\\Config",
                       "Display height") != NULL, "Hidden and Dangerous");
    /* ...and nothing else */
    CHECK(gr_reg_owner("HKCU", "Software\\Valve\\Half-Life\\Settings", "Language") == NULL,
          "a value no rule sets stays install.reg's");
    CHECK(gr_reg_owner("HKLM", "Software\\Valve\\Half-Life\\Settings", "ScreenWidth") == NULL,
          "the root is part of the identity");
    CHECK(gr_reg_owner("HKCU", "Software\\Valve\\Half-Life", "ScreenWidth") == NULL,
          "the key is part of the identity");
    CHECK(gr_reg_owner(NULL, "x", "y") == NULL, "no crash on NULL");
}

/* A model of gs_merge_reg() + the GAMERES REG writer over several syncs: the
 * CounterStrike16 install.reg pins ScreenWidth 800, the box wants 1024. */
TEST(t_registry_fight_old_and_fixed)
{
    int s, old_changes = 0, new_changes = 0;
    long reg;                                     /* the live value */
    const long INSTALL_REG = 800, TARGET = 1024;
    int owned = gr_reg_owner("HKCU", "Software\\Valve\\Half-Life\\Settings",
                             "ScreenWidth") != NULL;
    CHECK(owned, "the value is owned");

    /* OLD: regedit writes the constant, GAMERES puts the box's value back */
    reg = -1;
    for (s = 0; s < 5; s++) {
        reg = INSTALL_REG;                        /* regedit /s install.reg */
        if (reg != TARGET) { reg = TARGET; old_changes++; }
    }
    CHECK_EQ_I(old_changes, 5);                   /* "4 value(s) set" forever */

    /* FIXED: an owned value that exists is captured before regedit and put
     * back after it, so GAMERES finds its own value in place */
    reg = -1;                                     /* a fresh box: no value yet */
    for (s = 0; s < 5; s++) {
        int have = reg != -1;
        long snap = reg;
        reg = INSTALL_REG;                        /* regedit /s install.reg */
        if (owned && have)
            reg = snap;                           /* gs_reg_keep_restore      */
        if (reg != TARGET) { reg = TARGET; new_changes++; }
    }
    CHECK_EQ_I(new_changes, 1);                   /* the first sync only      */
    CHECK_EQ_I(reg, TARGET);
}

MUNIT_MAIN("grledger (GAMESYNC vs GAMERES)", {
    RUN(t_the_fight_old_and_fixed);
    RUN(t_library_update_is_still_taken);
    RUN(t_source_unchanged_skips);
    RUN(t_source_changed_copies);
    RUN(t_listing_disagrees_but_source_agrees_skips);
    RUN(t_dest_changed_on_the_box_is_not_kept);
    RUN(t_fat32_rounds_to_two_seconds);
    RUN(t_chain_keeps_the_library_base);
    RUN(t_monitor_change_keeps_the_library_base);
    RUN(t_fresh_copy_is_its_own_base);
    RUN(t_paths_compare_case_insensitively);
    RUN(t_bounded_one_record_per_path_oldest_evicted);
    RUN(t_round_trip);
    RUN(t_full_table_fits_the_size_limit);
    RUN(t_corrupt_ledger_means_copy);
    RUN(t_corrupt_record_does_not_take_the_others);
    RUN(t_numbers_are_ours_not_printf);
    RUN(t_reg_owner_names_the_values_install_reg_pins);
    RUN(t_registry_fight_old_and_fixed);
})
