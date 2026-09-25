/* test_gimatch.c - TRUE-SOURCE: compiles the REAL agent/shared/gimatch.h, the
 * logic behind GAMEINDEX's scan (agent 1.85.0).
 *
 * THE OLD WAY (gameindex.c match_dir, <= 1.84.x): for every directory the walk
 * visited, one GetFileAttributesA per signature - "is <dir>\<exe> a file?", and
 * for a GoldSrc row "is <dir>\<moddir> a directory?" - ~68 path lookups per
 * directory, every 240 s, forever. old_matches() below models exactly what those
 * probes answered: a path probe matches an entry by its long name OR its 8.3
 * alias, ASCII case-insensitively, and distinguishes files from directories.
 *
 * THE FIX feeds the listing the walk already reads to gim_note() and asks
 * gim_matches(). The central assertion here is EQUIVALENCE: on every listing,
 * for every signature, the new answer equals the old probe's. Plus the pass
 * decision (gi_scan_reason), the fingerprint's properties, and the on-disk
 * cache format.
 */
#include "munit.h"
#include <string.h>
#include <stdlib.h>

#include "../../agent/shared/gimatch.h"

/* A representative slice of gameindex.c's g_sigs: the GoldSrc moddir split,
 * names with spaces, short 8.3-style names, and look-alikes. */
static const game_sig_t SIGS[] = {
    { "quake3",   "Quake III Arena", "quake3.exe",       "baseq3",  "q3" },
    { "quake3",   "Quake III Arena", "quake3.exe",       NULL,      "q3" },
    { "cs16",     "Counter-Strike",  "hl.exe",           "cstrike", "goldsrc" },
    { "ts",       "The Specialists", "hl.exe",           "ts",      "goldsrc" },
    { "dod",      "Day of Defeat",   "hl.exe",           "dod",     "goldsrc" },
    { "halflife", "Half-Life",       "hl.exe",           "valve",   "goldsrc" },
    { "descent3", "Descent 3",       "Descent 3.exe",    NULL,      "-" },
    { "diablo2",  "Diablo II",       "Diablo II.exe",    NULL,      "-" },
    { "redneck",  "Redneck Rampage", "RR.EXE",           NULL,      "-" },
    { "carma",    "Carmageddon",     "MAINPROG.EXE",     NULL,      "-" },
    { "ut2004",   "UT2004",          "UT2004.exe",       NULL,      "ut2k4" },
    { NULL, NULL, NULL, NULL, NULL }
};

typedef struct { const char *name, *alt; int is_dir; } ent_t;

/* ---- the OLD probe semantics, modelled -------------------------------- */
static int ieq(const char *a, const char *b)
{
    size_t la = strlen(a), lb = strlen(b);
    return la == lb && gim_ieq_n(a, b, la);
}
static int probe(const ent_t *e, int n, const char *leaf, int want_dir)
{
    int i;
    for (i = 0; i < n; i++)
        if (!!e[i].is_dir == !!want_dir &&
            (ieq(e[i].name, leaf) || (e[i].alt[0] && ieq(e[i].alt, leaf))))
            return 1;
    return 0;
}
static int old_matches(const ent_t *e, int n, const game_sig_t *s)
{
    if (!probe(e, n, s->exe, 0))                 /* file_exists(dir\exe)   */
        return 0;
    return !s->moddir || probe(e, n, s->moddir, 1);   /* dir_exists(dir\mod) */
}

/* ---- the NEW way ------------------------------------------------------- */
static gim_table_t T;
static void new_listing(gim_hits_t *h, const ent_t *e, int n)
{
    int i;
    gim_reset(h);
    for (i = 0; i < n; i++)
        gim_note(h, &T, e[i].name, e[i].alt, e[i].is_dir);
}

static int check_equivalent(const ent_t *e, int n)
{
    gim_hits_t h;
    int i, bad = 0;
    new_listing(&h, e, n);
    for (i = 0; i < T.n; i++)
        if (!!gim_matches(&h, &T, i) != !!old_matches(e, n, &SIGS[i])) {
            fprintf(stderr, "    mismatch on sig %d (%s/%s)\n", i,
                    SIGS[i].key, SIGS[i].moddir ? SIGS[i].moddir : "-");
            bad++;
        }
    return bad;
}

static int matched(const ent_t *e, int n, const char *key, const char *mod)
{
    gim_hits_t h;
    int i;
    new_listing(&h, e, n);
    for (i = 0; i < T.n; i++)
        if (!strcmp(SIGS[i].key, key) &&
            ((!mod && !SIGS[i].moddir) || (mod && SIGS[i].moddir &&
                                           !strcmp(mod, SIGS[i].moddir))))
            return gim_matches(&h, &T, i);
    return -1;
}

TEST(the_table_loads)
{
    CHECK_EQ_I(gim_table_init(&T, SIGS), 11);
}

TEST(goldsrc_mods_are_split_by_their_directories)
{
    static const ent_t e[] = {
        { ".", "", 1 }, { "..", "", 1 },
        { "hl.exe", "", 0 }, { "cstrike", "", 1 }, { "valve", "", 1 },
        { "hw.dll", "", 0 }, { "config.cfg", "", 0 },
    };
    int n = (int)(sizeof(e) / sizeof(e[0]));
    CHECK_EQ_I(check_equivalent(e, n), 0);
    CHECK_EQ_I(matched(e, n, "cs16", "cstrike"), 1);
    CHECK_EQ_I(matched(e, n, "halflife", "valve"), 1);
    CHECK_EQ_I(matched(e, n, "ts", "ts"), 0);
    CHECK_EQ_I(matched(e, n, "dod", "dod"), 0);
}

TEST(a_file_named_like_a_moddir_is_not_a_moddir_and_vice_versa)
{
    static const ent_t e[] = {
        { "hl.exe", "", 0 }, { "cstrike", "", 0 },   /* a FILE called cstrike */
        { "quake3.exe", "", 1 },                     /* a DIRECTORY called quake3.exe */
        { "baseq3", "", 1 },
    };
    int n = (int)(sizeof(e) / sizeof(e[0]));
    CHECK_EQ_I(check_equivalent(e, n), 0);
    CHECK_EQ_I(matched(e, n, "cs16", "cstrike"), 0);
    CHECK_EQ_I(matched(e, n, "quake3", "baseq3"), 0);
    CHECK_EQ_I(matched(e, n, "quake3", NULL), 0);
}

TEST(case_does_not_matter_on_windows_filesystems)
{
    static const ent_t e[] = {
        { "QUAKE3.EXE", "", 0 }, { "BaseQ3", "", 1 },
        { "ut2004.EXE", "", 0 }, { "mainprog.exe", "", 0 },
    };
    int n = (int)(sizeof(e) / sizeof(e[0]));
    CHECK_EQ_I(check_equivalent(e, n), 0);
    CHECK_EQ_I(matched(e, n, "quake3", "baseq3"), 1);
    CHECK_EQ_I(matched(e, n, "quake3", NULL), 1);
    CHECK_EQ_I(matched(e, n, "ut2004", NULL), 1);
    CHECK_EQ_I(matched(e, n, "carma", NULL), 1);
}

TEST(the_8_3_alias_matches_as_a_path_probe_would)
{
    static const ent_t e[] = {
        /* long name differs, the 8.3 alias is the signature: a probe for
         * "RR.EXE" resolves through the alias, so the listing must too */
        { "Redneck Rampage Launcher.exe", "RR.EXE", 0 },
        /* the long name is the signature; its alias is irrelevant */
        { "Descent 3.exe", "DESCEN~1.EXE", 0 },
        /* neither name is Diablo II.exe */
        { "Diablo II Setup.exe", "DIABLO~1.EXE", 0 },
    };
    int n = (int)(sizeof(e) / sizeof(e[0]));
    CHECK_EQ_I(check_equivalent(e, n), 0);
    CHECK_EQ_I(matched(e, n, "redneck", NULL), 1);
    CHECK_EQ_I(matched(e, n, "descent3", NULL), 1);
    CHECK_EQ_I(matched(e, n, "diablo2", NULL), 0);
}

TEST(look_alikes_do_not_match)
{
    static const ent_t e[] = {
        { "hl.exe.bak", "", 0 }, { "xhl.exe", "", 0 }, { "hl.ex", "", 0 },
        { "cstrike_old", "", 1 }, { "UT2004.exe.old", "", 0 },
    };
    int n = (int)(sizeof(e) / sizeof(e[0]));
    CHECK_EQ_I(check_equivalent(e, n), 0);
    CHECK_EQ_I(matched(e, n, "cs16", "cstrike"), 0);
    CHECK_EQ_I(matched(e, n, "ut2004", NULL), 0);
}

TEST(random_listings_always_agree_with_the_old_probes)
{
    /* 2,000 random listings drawn from the signature names, their case
     * variants, look-alikes and aliases, as files or directories. */
    static const char *pool[] = {
        "quake3.exe", "QUAKE3.EXE", "baseq3", "BASEQ3", "hl.exe", "HL.EXE",
        "cstrike", "ts", "TS", "dod", "valve", "Valve", "Descent 3.exe",
        "Diablo II.exe", "RR.EXE", "rr.exe", "MAINPROG.EXE", "UT2004.exe",
        "hl.exe.bak", "readme.txt", "x", "ts2", "DESCEN~1.EXE",
    };
    int npool = (int)(sizeof(pool) / sizeof(pool[0]));
    int round, bad = 0;
    srand(1234);
    for (round = 0; round < 2000; round++) {
        ent_t e[12];
        int n = rand() % 12, i;
        for (i = 0; i < n; i++) {
            e[i].name = pool[rand() % npool];
            e[i].alt = (rand() % 4 == 0) ? pool[rand() % npool] : "";
            e[i].is_dir = rand() % 2;
        }
        bad += check_equivalent(e, n);
    }
    CHECK_EQ_I(bad, 0);
}

TEST(a_table_too_big_is_refused_not_truncated_silently)
{
    static game_sig_t big[GIM_MAX_SIGS + 3];
    gim_table_t t;
    int i;
    for (i = 0; i < GIM_MAX_SIGS + 2; i++) {
        big[i].key = "k"; big[i].name = "n"; big[i].exe = "a.exe";
        big[i].moddir = NULL; big[i].engine = "-";
    }
    big[GIM_MAX_SIGS + 2].key = NULL;
    CHECK_EQ_I(gim_table_init(&t, big), -1);
    CHECK_EQ_I(t.n, GIM_MAX_SIGS);
}

/* ---- when a pass runs --------------------------------------------------- */
TEST(a_pass_runs_only_when_something_says_so)
{
    const unsigned long H = 3600000UL;
    /* the OLD rule: every 240 s, unconditionally - 15 passes an hour */
    CHECK_EQ_I(gi_scan_reason(0, 1, 0, 0, H, 0), GI_SCAN_FIRST);
    CHECK_EQ_I(gi_scan_reason(1, 5, 5, 60000, H, 1), GI_SCAN_POKED);
    CHECK_EQ_I(gi_scan_reason(1, 6, 5, 60000, H, 0), GI_SCAN_CHANGED);
    CHECK_EQ_I(gi_scan_reason(1, 5, 5, H, H, 0), GI_SCAN_FULL_DUE);
    CHECK_EQ_I(gi_scan_reason(1, 5, 5, H - 1, H, 0), GI_SCAN_SKIP);
    CHECK(strcmp(gi_scan_reason_name(GI_SCAN_FULL_DUE), "hourly") == 0, "names");
}

TEST(an_idle_hour_costs_one_pass_not_fifteen)
{
    /* checks every 15 min for an hour on an unchanged disk, starting just
     * after a full pass: the safety net fires once, at the hour */
    unsigned long t, since = 0;
    int passes = 0;
    for (t = 900000UL; t <= 3600000UL; t += 900000UL) {
        since += 900000UL;
        if (gi_scan_reason(1, 42, 42, since, 3600000UL, 0) != GI_SCAN_SKIP) {
            passes++;
            since = 0;
        }
    }
    CHECK_EQ_I(passes, 1);
}

/* ---- the fingerprint ---------------------------------------------------- */
TEST(the_fingerprint_ignores_order_and_case_but_not_content)
{
    unsigned long a = gi_fp_entry("Quake3", 1, 10, 20);
    unsigned long b = gi_fp_entry("HalfLife1", 1, 30, 40);
    CHECK_EQ_U((a + b) & 0xFFFFFFFFUL, (b + a) & 0xFFFFFFFFUL);
    CHECK_EQ_U(gi_fp_entry("QUAKE3", 1, 10, 20), a);          /* case */
    CHECK(gi_fp_entry("Quake3", 0, 10, 20) != a, "file vs dir differs");
    CHECK(gi_fp_entry("Quake3", 1, 11, 20) != a, "a new write time differs");
    CHECK(gi_fp_entry("Quake4", 1, 10, 20) != a, "a new name differs");
}

/* ---- the on-disk cache -------------------------------------------------- */
TEST(the_cache_round_trips_and_rejects_a_torn_file)
{
    const char *doc = "{\"hash\":\"0000abcd\",\"count\":0,\"games\":[]}";
    char buf[256];
    char hdr[GI_CACHE_HDR_MAX];
    unsigned long fp = 0, hash = 0, off = 0, len = 0, dl = strlen(doc);
    int hl = gi_cache_header(hdr, 0xDEADBEEFUL, 0xABCDUL, dl);

    CHECK(hl < GI_CACHE_HDR_MAX, "header fits");
    memcpy(buf, hdr, hl);
    memcpy(buf + hl, doc, dl);
    CHECK_EQ_I(gi_cache_parse(buf, hl + dl, &fp, &hash, &off, &len), 1);
    CHECK_EQ_U(fp, 0xDEADBEEFUL);
    CHECK_EQ_U(hash, 0xABCDUL);
    CHECK_EQ_U(len, dl);
    CHECK(memcmp(buf + off, doc, dl) == 0, "the document comes back intact");

    /* torn: a power cut mid-write leaves it short */
    CHECK_EQ_I(gi_cache_parse(buf, hl + dl - 5, &fp, &hash, &off, &len), 0);
    /* foreign */
    buf[0] = 'X';
    CHECK_EQ_I(gi_cache_parse(buf, hl + dl, &fp, &hash, &off, &len), 0);
    buf[0] = 'G';
    /* not a JSON object */
    buf[hl] = '[';
    CHECK_EQ_I(gi_cache_parse(buf, hl + dl, &fp, &hash, &off, &len), 0);
    CHECK_EQ_I(gi_cache_parse("", 0, &fp, &hash, &off, &len), 0);
}

MUNIT_MAIN("GAMEINDEX scan logic (agent/shared/gimatch.h, agent 1.85.0)",
    RUN(the_table_loads);
    RUN(goldsrc_mods_are_split_by_their_directories);
    RUN(a_file_named_like_a_moddir_is_not_a_moddir_and_vice_versa);
    RUN(case_does_not_matter_on_windows_filesystems);
    RUN(the_8_3_alias_matches_as_a_path_probe_would);
    RUN(look_alikes_do_not_match);
    RUN(random_listings_always_agree_with_the_old_probes);
    RUN(a_table_too_big_is_refused_not_truncated_silently);
    RUN(a_pass_runs_only_when_something_says_so);
    RUN(an_idle_hour_costs_one_pass_not_fifteen);
    RUN(the_fingerprint_ignores_order_and_case_but_not_content);
    RUN(the_cache_round_trips_and_rejects_a_torn_file);
)
