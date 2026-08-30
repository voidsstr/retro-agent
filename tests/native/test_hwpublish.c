/* test_hwpublish.c - TRUE-SOURCE test: compiles the REAL agent/shared/hwpub.h,
 * the pure logic behind the fleet inventory publish (agent v1.73.0).
 *
 * Every box writes its own hardware record to
 *   \\192.168.1.122\files\Utility\Retro Automation\fleet-inventory\<host>.json
 * on every startup, so the fleet documentation is measured rather than
 * remembered. Two decisions in that job are cheap to write and expensive to
 * get wrong, and both are asserted here against BOTH the fixed and the
 * old-buggy value:
 *
 * 1. THE FILENAME. The leaf is built from GetComputerNameA, and a NetBIOS name
 *    is not a filename - it is whatever somebody typed into the System control
 *    panel. The OLD-BUGGY form is to paste it straight into the path, which
 *    does not fail: a name containing '\', '/' or ".." writes SOMEWHERE ELSE on
 *    the share, silently, under the credentials the box has. One host then
 *    overwrites another host's record, or lands outside the inventory
 *    directory entirely - and the renderer reports a perfectly healthy machine
 *    as "never seen" while its data sits where nobody reads it. A record in
 *    the wrong place is worse than no record.
 *
 * 2. THE RETRY IS BOUNDED. The share is frequently unreachable at startup, so
 *    the publish has to retry - but a cosmetic feature must never cost a box
 *    its agent. This project has already killed one: dosstage copying an 11 MB
 *    payload at startup took the 31 MB Pentium-1 Deskpro off the network ~45s
 *    after every boot, and it looked like a startup crash for hours. The
 *    OLD-BUGGY form is an unbounded retry loop against an absent file server,
 *    which eats a single-core box forever while reporting nothing. The
 *    schedule is finite and says so by returning 0 past its end.
 */
#include "munit.h"
#include <string.h>
#include <stdio.h>

#include "../../agent/shared/hwpub.h"

/* ---- 1. the hostname -> filename mapping ---- */

TEST(real_fleet_names_pass_through_untouched)
{
    /* Every name on the fleet today is already a legal filename, and the
     * sanitizer must not "fix" any of them - a renamed record would read as a
     * new machine and the old one as never seen. */
    static const char *names[] = {
        "P3-DUAL", "1GHZ", "DELL", "ADMIN", "ADMIN-PC",
        "NSC-5B996B81319", "NSC-B20C188E96D", "NSC-CABE14B7486",
        "USER-41EA3B3330"
    };
    unsigned i;
    for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        char out[160];
        CHECK_EQ_I(hwpub_safe_name(names[i], out, sizeof(out)), 1);
        CHECK(strcmp(out, names[i]) == 0, "fleet name passed through unchanged");
    }
}

TEST(a_separator_can_never_survive_into_the_path)
{
    /* THE FIX vs THE OLD-BUGGY VALUE. Old: the raw name is used, so the path
     * contains a separator and the write escapes the directory. Fixed: every
     * separator becomes '_' and the leaf stays a leaf. */
    static const char *evil[] = {
        "A\\B",            /* backslash - writes into a sibling directory   */
        "A/B",             /* forward slash - SMB accepts it just as well   */
        "..\\..\\evil",    /* traversal out of the inventory directory      */
        "C:evil",          /* a drive-relative path                         */
        "host name",       /* merely a space, but still not the same file   */
        "host\ttab"
    };
    unsigned i;
    for (i = 0; i < sizeof(evil) / sizeof(evil[0]); i++) {
        char out[160];
        /* the fixed value: rewritten, and reported as rewritten */
        CHECK_EQ_I(hwpub_safe_name(evil[i], out, sizeof(out)), 0);
        CHECK(strchr(out, '\\') == NULL, "no backslash survives");
        CHECK(strchr(out, '/') == NULL, "no forward slash survives");
        CHECK(strchr(out, ':') == NULL, "no colon survives");
        CHECK(strchr(out, ' ') == NULL, "no space survives");
        CHECK(strstr(out, "..\\") == NULL && strstr(out, "../") == NULL,
              "no traversal survives");
        /* the old-buggy value, asserted to be genuinely dangerous so this test
         * cannot pass by the input having been harmless all along */
        CHECK(strpbrk(evil[i], "\\/: \t") != NULL,
              "the raw name really did carry a separator");
    }
}

TEST(dots_only_and_empty_become_unknown)
{
    char out[160];

    /* "." and ".." are not filenames at all. Left alone, "." cannot be created
     * on some servers - which looks exactly like an unreachable share, sending
     * you to debug the network instead of the name. */
    CHECK_EQ_I(hwpub_safe_name(".", out, sizeof(out)), 0);
    CHECK(strcmp(out, "unknown") == 0, ". becomes unknown");

    CHECK_EQ_I(hwpub_safe_name("..", out, sizeof(out)), 0);
    CHECK(strcmp(out, "unknown") == 0, ".. becomes unknown");

    CHECK_EQ_I(hwpub_safe_name("", out, sizeof(out)), 0);
    CHECK(strcmp(out, "unknown") == 0, "empty becomes unknown");

    CHECK_EQ_I(hwpub_safe_name(NULL, out, sizeof(out)), 0);
    CHECK(strcmp(out, "unknown") == 0, "NULL becomes unknown");

    /* A name that merely CONTAINS dots is fine and must be kept. */
    CHECK_EQ_I(hwpub_safe_name("box.v2", out, sizeof(out)), 1);
    CHECK(strcmp(out, "box.v2") == 0, "interior dots are legal");
}

TEST(truncation_is_bounded_and_reported)
{
    char out[8];
    /* A long name must truncate rather than overflow, and must SAY it was
     * changed - a silently truncated name is two boxes sharing one record. */
    CHECK_EQ_I(hwpub_safe_name("ABCDEFGHIJKLMNOP", out, sizeof(out)), 0);
    CHECK(strlen(out) == sizeof(out) - 1, "truncated to fit");
    CHECK(strcmp(out, "ABCDEFG") == 0, "truncated from the front");

    /* Degenerate buffers must not be written past. */
    {
        char tiny[1];
        tiny[0] = 'X';
        CHECK_EQ_I(hwpub_safe_name("host", tiny, 1), 0);
        CHECK(tiny[0] == 'X', "a 1-byte buffer is refused, not scribbled on");
    }
}

/* ---- 2. the retry schedule ---- */

TEST(the_retry_schedule_terminates)
{
    int i, total = 0;

    /* The fixed value: a finite schedule that returns 0 once exhausted. The
     * old-buggy value is "retry forever", i.e. a non-zero delay for every
     * attempt however large - which is what an unbounded loop looks like. */
    for (i = 0; i < HWPUB_MAX_ATTEMPTS; i++) {
        int d = hwpub_retry_delay_sec(i);
        CHECK(d > 0, "each scheduled attempt has a real delay");
        total += d;
    }
    CHECK_EQ_I(hwpub_retry_delay_sec(HWPUB_MAX_ATTEMPTS), 0);
    CHECK_EQ_I(hwpub_retry_delay_sec(HWPUB_MAX_ATTEMPTS + 100), 0);
    CHECK_EQ_I(hwpub_retry_delay_sec(-1), 0);

    /* And the whole schedule is bounded - under half an hour, so a box with no
     * share reachable has stopped trying long before anyone notices. */
    CHECK(total > 0 && total <= 30 * 60, "the whole schedule fits in 30 min");
}

TEST(the_first_attempt_yields_the_boot_window)
{
    /* dosstage starts at T+45s and is still streaming files on the slowest
     * box. Publishing before that competes with it for the same NIC on a
     * single-core machine, which is the exact shape of the Deskpro failure. */
    CHECK(hwpub_retry_delay_sec(0) >= 60,
          "the first publish waits out the boot window");
    CHECK_EQ_I(hwpub_retry_delay_sec(0), 90);
}

TEST(delays_never_shrink)
{
    /* A backoff that goes backwards hammers the server hardest exactly when it
     * has proved it is not answering. */
    int i;
    for (i = 1; i < HWPUB_MAX_ATTEMPTS; i++)
        CHECK(hwpub_retry_delay_sec(i) >= hwpub_retry_delay_sec(i - 1),
              "the backoff is monotonic");
}

TEST(the_record_cap_is_sane)
{
    /* The record is a couple of KB. The cap exists so a runaway probe writes
     * nothing rather than streaming garbage onto the share, where eight boxes
     * share one directory. */
    CHECK(HWPUB_MAX_BYTES >= 8 * 1024, "room for a real profile");
    CHECK(HWPUB_MAX_BYTES <= 256 * 1024, "but not room for a runaway one");
    CHECK(strstr(HWPUB_DIR_DEFAULT, "fleet-inventory") != NULL,
          "the default directory is the one the renderer reads");
}

MUNIT_MAIN("fleet inventory publish (agent/shared/hwpub.h)",
    RUN(real_fleet_names_pass_through_untouched);
    RUN(a_separator_can_never_survive_into_the_path);
    RUN(dots_only_and_empty_become_unknown);
    RUN(truncation_is_bounded_and_reported);
    RUN(the_retry_schedule_terminates);
    RUN(the_first_attempt_yields_the_boot_window);
    RUN(delays_never_shrink);
    RUN(the_record_cap_is_sane);
)
