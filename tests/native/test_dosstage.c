/* test_dosstage.c — TRUE-SOURCE test: compiles the REAL agent/src/dosstage.c
 * (agent v1.19.0) against a fake Win32 (stubs/dosstage_env.h) and runs the
 * actual staging job.
 *
 * What this protects (all of it costs real time to discover on hardware):
 *   - the OS gate: an NT/XP box must NEVER stage the DOS payload (~11 MB of
 *     preview tiles it can't use), a Win9x box always must
 *   - default-on with an explicit registry off-switch, and `force` overriding
 *     the off-switch but NOT the OS gate
 *   - idempotence: a file already present at the same size is not re-copied,
 *     so a reboot costs a directory scan and nothing else (this is what keeps
 *     the Pentium-1 Deskpro usable on every boot)
 *   - ordering + pacing: programs and network tools first, the bulk tile
 *     payload last and paced, so the box isn't saturated
 */

#include "munit.h"
#include "stubs/dosstage_env.h"

fake_env_t g_env;

/* symbols dosstage.c references from the rest of the agent */
volatile int g_running = 1;
static char g_last_response[512];
void log_msg(const char *tag, const char *fmt, ...) { (void)tag; (void)fmt; }
int send_text_response(int sock, const char *text)
{
    (void)sock;
    strncpy(g_last_response, text, sizeof(g_last_response) - 1);
    g_last_response[sizeof(g_last_response) - 1] = '\0';
    return 0;
}
void safe_strncpy(char *dst, const char *src, int size)
{
    strncpy(dst, src, size - 1);
    dst[size - 1] = '\0';
}
int str_starts_with(const char *s, const char *prefix)
{
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

/* the real module under test */
#include "../../agent/src/dosstage.c"

#define SHARE "\\\\192.168.1.122\\files\\Utility\\Retro Automation"

/* Seed the share payload under an arbitrary root (UNC or mapped drive). */
static void seed_payload(const char *root)
{
    char p[MAX_PATH];
    snprintf(p, sizeof(p), "%s\\doschat\\DOSCHAT.EXE", root); fake_add_file(p, 87844);
    snprintf(p, sizeof(p), "%s\\doschat\\NE2000.COM", root);  fake_add_file(p, 5571);
    snprintf(p, sizeof(p), "%s\\doschat\\DCSTART.BAT", root); fake_add_file(p, 194);
    snprintf(p, sizeof(p), "%s\\dosgame\\DOSGAME.EXE", root); fake_add_file(p, 106778);
    snprintf(p, sizeof(p), "%s\\dosgame\\GAMES.CAT", root);   fake_add_file(p, 293502);
    snprintf(p, sizeof(p), "%s\\dosgame\\DOSGAME.BAT", root); fake_add_file(p, 260);
    snprintf(p, sizeof(p), "%s\\dosgame\\NET\\HTGET.EXE", root); fake_add_file(p, 40000);
    snprintf(p, sizeof(p), "%s\\dosgame\\TILES\\DOOM.PRV", root); fake_add_file(p, 64768);
    snprintf(p, sizeof(p), "%s\\dosgame\\TILES\\KEEN.PRV", root); fake_add_file(p, 64768);
}

static void env_reset_empty(DWORD platform)
{
    memset(&g_env, 0, sizeof(g_env));
    g_env.platform_id = platform;
    g_env.avail_mb = 64;          /* a healthy box unless a test says otherwise */
    g_running = 1;
}

static void env_reset(DWORD platform)
{
    env_reset_empty(platform);
    seed_payload(SHARE);          /* readable via the configured UNC */
}

static int copied_contains(const char *needle)
{
    int i;
    for (i = 0; i < g_env.n_copied; i++)
        if (strstr(g_env.copied[i], needle)) return 1;
    return 0;
}

static int copied_index(const char *needle)
{
    int i;
    for (i = 0; i < g_env.n_copied; i++)
        if (strstr(g_env.copied[i], needle)) return i;
    return -1;
}

TEST(nt_family_never_stages) {
    env_reset(VER_PLATFORM_WIN32_NT);
    CHECK(dosstage_os_is_dos_capable() == 0, "NT is not DOS-capable");
    dosstage_run(0);
    CHECK_EQ_I(g_env.n_copied, 0);
    CHECK(g_env.marker[0] == '\0', "no marker written on a skipped box");
}

TEST(force_does_not_override_the_os_gate) {
    /* force exists to bypass the registry off-switch, never the platform:
     * an XP box has no DOS to boot into no matter what the operator asks. */
    env_reset(VER_PLATFORM_WIN32_NT);
    dosstage_run(1);
    CHECK_EQ_I(g_env.n_copied, 0);
}

TEST(win9x_stages_both_programs) {
    env_reset(VER_PLATFORM_WIN32_WINDOWS);
    CHECK(dosstage_os_is_dos_capable() == 1, "Win9x is DOS-capable");
    dosstage_run(0);
    CHECK(copied_contains("C:\\DOSCHAT\\DOSCHAT.EXE"), "DOSCHAT staged");
    CHECK(copied_contains("C:\\DOSCHAT\\NE2000.COM"), "DOSCHAT packet driver staged");
    CHECK(copied_contains("C:\\DOSGAME\\DOSGAME.EXE"), "DOSGAME staged");
    CHECK(copied_contains("C:\\DOSGAME\\GAMES.CAT"), "catalog staged");
    CHECK(copied_contains("C:\\DOSGAME\\NET\\HTGET.EXE"), "mTCP tools staged");
    /* tiles are opt-in now — see tiles_are_opt_in_but_programs_always_stage */
    CHECK(copied_contains("C:\\DOSGAME.BAT"),
          "the launcher wrapper must land at the root of C: (the exit-42 "
          "relaunch is what frees conventional memory for the game)");
    CHECK(g_env.marker[0] != '\0', "marker records the stage");
}

TEST(programs_come_before_the_bulk_tile_payload) {
    env_reset(VER_PLATFORM_WIN32_WINDOWS);
    g_env.reg_tiles_present = 1; g_env.reg_tiles_value = 1;
    dosstage_run(0);
    CHECK(copied_index("DOSCHAT.EXE") < copied_index("TILES\\DOOM.PRV"),
          "the exes must be on disk before the 11MB tile stream");
    CHECK(copied_index("DOSGAME.EXE") < copied_index("TILES\\DOOM.PRV"),
          "game manager before tiles");
    CHECK(copied_index("NET\\HTGET.EXE") < copied_index("TILES\\DOOM.PRV"),
          "network tools before tiles");
}

TEST(tile_copies_are_paced_and_others_are_not) {
    env_reset(VER_PLATFORM_WIN32_WINDOWS);
    g_env.reg_tiles_present = 1; g_env.reg_tiles_value = 1;
    dosstage_run(0);
    /* exactly one pause per tile, none for the programs */
    CHECK_EQ_I(g_env.n_sleeps, 2);          /* two tiles seeded */
    CHECK(g_env.sleeps[0] >= 100,
          "tile pacing must be a real breather on a Pentium 1");
}

TEST(second_run_copies_nothing_new) {
    int first;
    env_reset(VER_PLATFORM_WIN32_WINDOWS);
    dosstage_run(0);
    first = g_env.n_copied;
    CHECK(first > 0, "first run stages the payload");
    g_env.n_copied = 0;
    g_env.n_sleeps = 0;
    dosstage_run(0);
    CHECK_EQ_I(g_env.n_copied, 0);
    CHECK_EQ_I(g_env.n_sleeps, 0);
}

TEST(changed_share_file_is_restaged) {
    fake_file_t *f;
    env_reset(VER_PLATFORM_WIN32_WINDOWS);
    dosstage_run(0);
    g_env.n_copied = 0;
    /* new DOSCHAT build published to the share */
    f = fake_find(SHARE "\\doschat\\DOSCHAT.EXE");
    f->size = 90000;
    dosstage_run(0);
    CHECK(copied_contains("C:\\DOSCHAT\\DOSCHAT.EXE"),
          "a different size on the share must re-copy");
    CHECK_EQ_I(g_env.n_copied, 1);
}

/* The bug this pins: copy_if_different compared SIZE only, so a same-size edit
 * to a staged file could never reach a box while dosstage_run logged
 * "staged 0 file(s)" and wrote the success marker. The DOS payload is mostly
 * batch scripts, where the ordinary fix preserves length exactly -- NETUP.BAT's
 * probe io 0x300 -> 0x320, "goto try4" -> "goto try5", PACKETINT 0x62 -> 0x63.
 * The agent's own updater carries a .ver sidecar because it learned this once;
 * the DOS staging path never got the same treatment. */
TEST(same_size_but_newer_share_file_is_restaged) {
    fake_file_t *f;
    env_reset(VER_PLATFORM_WIN32_WINDOWS);
    dosstage_run(0);
    g_env.n_copied = 0;

    /* a one-character fix to a batch file: identical length, newer mtime */
    f = fake_find(SHARE "\\dosgame\\DOSGAME.BAT");
    CHECK(f != NULL, "fixture must have the batch file");
    f->mtime = 5000;                      /* staged copy is still at 0 */
    dosstage_run(0);
    CHECK(copied_contains("C:\\DOSGAME\\DOSGAME.BAT"),
          "a same-size but newer share file must re-copy");
}

TEST(an_older_share_file_does_not_overwrite_a_newer_local_one) {
    fake_file_t *f;
    env_reset(VER_PLATFORM_WIN32_WINDOWS);
    dosstage_run(0);
    g_env.n_copied = 0;

    /* Nothing changed on the share, so a second pass must stay a no-op --
     * CopyFileA preserves the timestamp, so the staged copy matches its
     * source and re-running costs one directory scan, not a re-copy. */
    f = fake_find(SHARE "\\dosgame\\DOSGAME.BAT");
    f->mtime = 0;
    dosstage_run(0);
    CHECK_EQ_I(g_env.n_copied, 0);
}

TEST(force_restages_even_when_nothing_changed) {
    env_reset(VER_PLATFORM_WIN32_WINDOWS);
    dosstage_run(0);
    g_env.n_copied = 0;
    /* DOSSTAGE force used to only bypass the DosStage=0 enable flag, so an
     * operator looking at a stale file on a box had no way to replace it
     * short of deleting it by hand. */
    dosstage_run(1);
    CHECK(g_env.n_copied > 0, "force must re-copy an unchanged file");
}

TEST(registry_switch_disables_and_force_overrides_it) {
    env_reset(VER_PLATFORM_WIN32_WINDOWS);
    g_env.reg_enable_present = 1;
    g_env.reg_enable_value = 0;
    dosstage_run(0);
    CHECK_EQ_I(g_env.n_copied, 0);

    dosstage_run(1);                     /* force overrides the off-switch */
    CHECK(g_env.n_copied > 0, "force stages despite DosStage=0");
}

TEST(default_is_enabled_when_the_value_is_absent) {
    env_reset(VER_PLATFORM_WIN32_WINDOWS);
    g_env.reg_enable_present = 0;        /* fresh box, no value written */
    dosstage_run(0);
    CHECK(g_env.n_copied > 0, "staging is on by default on a 9x box");
}

TEST(share_path_override_is_honored) {
    env_reset(VER_PLATFORM_WIN32_WINDOWS);       /* default share also seeded */
    strcpy(g_env.reg_path, "\\\\10.0.0.9\\pub\\retro");
    seed_payload("\\\\10.0.0.9\\pub\\retro");
    dosstage_run(0);
    CHECK(copied_contains("C:\\DOSCHAT\\DOSCHAT.EXE"),
          "DosStagePath must redirect the source share");
    CHECK(g_env.n_copied > 0, "override root is used");
}

/* A staging root carrying only one package must still work — insisting on
 * both would reject a legitimate chat-only DosStagePath. */
TEST(a_root_with_only_one_package_still_stages) {
    env_reset_empty(VER_PLATFORM_WIN32_WINDOWS);
    strcpy(g_env.reg_path, "\\\\10.0.0.9\\pub\\chatonly");
    fake_add_file("\\\\10.0.0.9\\pub\\chatonly\\doschat\\DOSCHAT.EXE", 1234);
    dosstage_run(0);
    CHECK(copied_contains("C:\\DOSCHAT\\DOSCHAT.EXE"),
          "a doschat-only root must still stage the chat program");
}

TEST(shutdown_stops_the_copy_midway) {
    env_reset(VER_PLATFORM_WIN32_WINDOWS);
    g_running = 0;                       /* agent shutting down */
    dosstage_run(0);
    CHECK(g_env.n_copied <= 3,
          "a shutdown must not be blocked by a full 11MB stage");
}

/* Hardware-confirmed on the Deskpro (Win98, 2026-07-29): a bare UNC path is
 * not readable without an authenticated session, so the first v1.19.0 build
 * reported "staging started" and copied nothing. The fleet maps the share to
 * a drive letter; staging must fall back to it. */
TEST(falls_back_to_the_mapped_drive_when_unc_is_unreadable) {
    env_reset_empty(VER_PLATFORM_WIN32_WINDOWS);
    strcpy(g_env.remote_drives, "D");          /* D: is the mapped share */
    seed_payload("D:\\Utility\\Retro Automation");
    dosstage_run(0);
    CHECK(copied_contains("C:\\DOSCHAT\\DOSCHAT.EXE"),
          "must reach the payload through the mapped drive");
    CHECK(copied_contains("C:\\DOSGAME\\DOSGAME.EXE"), "game manager staged");
    CHECK(copied_contains("C:\\DOSCHAT\\NE2000.COM"), "net payload staged");
}

TEST(unreachable_share_stages_nothing_and_writes_no_marker) {
    env_reset_empty(VER_PLATFORM_WIN32_WINDOWS);
    /* no UNC access, no mapped drive */
    dosstage_run(0);
    CHECK_EQ_I(g_env.n_copied, 0);
    CHECK(g_env.marker[0] == '\0',
          "an unreachable share must NOT look like a successful stage");
}

TEST(local_drives_are_not_scanned_as_share_candidates) {
    env_reset_empty(VER_PLATFORM_WIN32_WINDOWS);
    /* a local disk that happens to hold the same layout is not the share */
    seed_payload("E:\\Utility\\Retro Automation");
    dosstage_run(0);
    CHECK_EQ_I(g_env.n_copied, 0);
}

TEST(unc_is_preferred_when_it_works) {
    env_reset(VER_PLATFORM_WIN32_WINDOWS);        /* UNC payload seeded */
    strcpy(g_env.remote_drives, "D");
    seed_payload("D:\\Utility\\Retro Automation");
    dosstage_run(0);
    CHECK(copied_contains("C:\\DOSCHAT\\DOSCHAT.EXE"), "staged");
    /* both sources exist; the configured UNC wins, so the mapped-drive
     * fallback must not have been needed (sizes come from the UNC seed) */
    CHECK_EQ_I(g_env.n_copied > 0, 1);
}

/* The Deskpro (31MB, 0MB free) lost its agent outright while staging the
 * 11MB tile payload — repeatedly, ~45s after every start. A cosmetic feature
 * must never cost a box its agent. */
TEST(low_memory_box_is_not_staged_at_all) {
    env_reset(VER_PLATFORM_WIN32_WINDOWS);
    g_env.avail_mb = 0;
    dosstage_run(0);
    CHECK_EQ_I(g_env.n_copied, 0);
    CHECK(g_env.marker[0] == '\0', "no marker when staging was skipped");

    env_reset(VER_PLATFORM_WIN32_WINDOWS);
    g_env.avail_mb = 2;                     /* still under the floor */
    dosstage_run(1);                        /* force must not override memory */
    CHECK_EQ_I(g_env.n_copied, 0);
}

TEST(tiles_are_opt_in_but_programs_always_stage) {
    env_reset(VER_PLATFORM_WIN32_WINDOWS);
    dosstage_run(0);
    CHECK(copied_contains("C:\\DOSCHAT\\DOSCHAT.EXE"), "programs stage");
    CHECK(copied_contains("C:\\DOSGAME\\DOSGAME.EXE"), "game manager stages");
    CHECK(copied_contains("C:\\DOSGAME\\NET\\HTGET.EXE"), "net tools stage");
    CHECK(!copied_contains("TILES"),
          "the 11MB tile payload must NOT be staged by default");

    env_reset(VER_PLATFORM_WIN32_WINDOWS);
    g_env.reg_tiles_present = 1;
    g_env.reg_tiles_value = 1;              /* explicitly opted in */
    dosstage_run(0);
    CHECK(copied_contains("C:\\DOSGAME\\TILES\\DOOM.PRV"),
          "tiles stage when explicitly enabled");
}

TEST(command_declines_politely_on_nt) {
    env_reset(VER_PLATFORM_WIN32_NT);
    g_last_response[0] = '\0';
    handle_dosstage(0, NULL);
    CHECK(strstr(g_last_response, "not DOS-capable") != NULL,
          "DOSSTAGE on XP must say why, not silently no-op");
    CHECK_EQ_I(g_env.n_copied, 0);
}

MUNIT_MAIN("dosstage — DOS program staging on DOS-capable boxes (true-source)", {
    RUN(nt_family_never_stages);
    RUN(force_does_not_override_the_os_gate);
    RUN(win9x_stages_both_programs);
    RUN(programs_come_before_the_bulk_tile_payload);
    RUN(tile_copies_are_paced_and_others_are_not);
    RUN(second_run_copies_nothing_new);
    RUN(changed_share_file_is_restaged);
    RUN(same_size_but_newer_share_file_is_restaged);
    RUN(an_older_share_file_does_not_overwrite_a_newer_local_one);
    RUN(force_restages_even_when_nothing_changed);
    RUN(registry_switch_disables_and_force_overrides_it);
    RUN(default_is_enabled_when_the_value_is_absent);
    RUN(share_path_override_is_honored);
    RUN(a_root_with_only_one_package_still_stages);
    RUN(shutdown_stops_the_copy_midway);
    RUN(falls_back_to_the_mapped_drive_when_unc_is_unreadable);
    RUN(unreachable_share_stages_nothing_and_writes_no_marker);
    RUN(local_drives_are_not_scanned_as_share_candidates);
    RUN(unc_is_preferred_when_it_works);
    RUN(low_memory_box_is_not_staged_at_all);
    RUN(tiles_are_opt_in_but_programs_always_stage);
    RUN(command_declines_politely_on_nt);
})
