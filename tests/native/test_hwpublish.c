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

/* ---- 3. the hardware address ---- */

TEST(a_mac_is_formatted_whole_not_truncated_to_its_first_octet)
{
    /* THE BUG, found while writing this module. The first octet is TWO
     * characters and every later one is THREE, so the write offset is k*3-1.
     * At k*3 - which is what you write first, and which compiles and runs -
     * the NUL from the previous octet's snprintf lands in the gap and the
     * whole address terminates after its first byte.
     *
     * The result is "00": a short, plausible-looking string that no reader
     * would flag. A MAC is one of the two things that lets a record on the
     * share be matched back to the box that wrote it, so a wrong one is worse
     * than none at all. */
    static const unsigned char addr[6] = { 0x00, 0x1A, 0x2B, 0x3C, 0x4D, 0x5E };
    char out[32], buggy[32];
    unsigned k;
    int pos;

    CHECK_EQ_I(hwpub_format_mac(addr, 6, out, sizeof(out)), 6);
    CHECK(strcmp(out, "00-1A-2B-3C-4D-5E") == 0, "the whole address");
    CHECK_EQ_I((int)strlen(out), 17);

    /* the OLD-BUGGY value, computed here so the test cannot pass by the input
     * having been harmless */
    buggy[0] = 0;
    for (k = 0; k < 6; k++) {
        pos = (int)(k * 3);
        sprintf(buggy + pos, k ? "-%02X" : "%02X", addr[k]);
    }
    CHECK(strcmp(buggy, "00") == 0, "the k*3 form really does truncate");
    CHECK(strcmp(out, buggy) != 0, "fixed and buggy differ");
}

TEST(a_mac_never_runs_past_its_buffer)
{
    static const unsigned char addr[6] = { 0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0x02 };
    char small[8];

    /* Truncation must stop on an octet boundary and stay terminated - a half
     * written octet would read as a different address. */
    CHECK(hwpub_format_mac(addr, 6, small, sizeof(small)) < 6,
          "a short buffer takes fewer octets");
    CHECK(strlen(small) < sizeof(small), "terminated inside the buffer");
    CHECK(strcmp(small, "DE-AD") == 0, "stops on an octet boundary");

    /* Degenerate inputs must not scribble. */
    CHECK_EQ_I(hwpub_format_mac(NULL, 6, small, sizeof(small)), 0);
    CHECK(small[0] == 0, "NULL address yields an empty string");
    CHECK_EQ_I(hwpub_format_mac(addr, 0, small, sizeof(small)), 0);

    /* An adapter reporting an over-long address (a token ring or a virtual
     * NIC) is clamped rather than trusted. */
    {
        static const unsigned char longaddr[8] =
            { 1, 2, 3, 4, 5, 6, 7, 8 };
        char big[64];
        CHECK_EQ_I(hwpub_format_mac(longaddr, 8, big, sizeof(big)), 6);
        CHECK(strcmp(big, "01-02-03-04-05-06") == 0, "clamped to six octets");
    }
}

/* ---- 4. a registry string that is not a REG_SZ ---- */

TEST(a_reg_binary_utf16_name_is_narrowed_not_truncated_to_one_letter)
{
    /* FOUND ON .246 (Windows 7). The display class key's DriverDesc is stored
     * as REG_BINARY holding UTF-16LE, not as REG_SZ. RegQueryValueExA converts
     * REG_SZ for you and hands REG_BINARY back RAW, so an ANSI reader sees the
     * string end at the first NUL - after ONE character. The box reported its
     * graphics card as "A": short, printable, entirely plausible, and flagged
     * by nothing. */
    static const unsigned char utf16[] = {
        'A',0,'M',0,'D',0,' ',0,'R',0,'a',0,'d',0,'e',0,'o',0,'n',0,' ',0,
        'H',0,'D',0,' ',0,'5',0,'4',0,'5',0,'0',0, 0,0
    };
    char out[64];

    CHECK_EQ_I(hwpub_looks_utf16le(utf16, (int)sizeof(utf16)), 1);
    CHECK_EQ_I(hwpub_utf16le_narrow(utf16, (int)sizeof(utf16), out,
                                    sizeof(out)), 1);
    CHECK(strcmp(out, "AMD Radeon HD 5450") == 0, "the whole adapter name");

    /* the OLD-BUGGY value: the same bytes taken as a C string */
    CHECK(strcmp((const char *)utf16, "A") == 0,
          "read as ANSI it really is just \"A\"");
}

TEST(a_genuine_ansi_string_is_left_alone)
{
    /* The far more common case must not be mangled by the fix. An ordinary
     * REG_SZ payload must be rejected by the detector so the caller uses it
     * verbatim. */
    static const unsigned char ansi[] = "NVIDIA GeForce4 Ti 4600";
    char out[64];

    CHECK_EQ_I(hwpub_looks_utf16le(ansi, (int)sizeof(ansi)), 0);
    out[0] = 'Z'; out[1] = 0;
    CHECK_EQ_I(hwpub_utf16le_narrow(ansi, (int)sizeof(ansi), out, sizeof(out)), 0);
    CHECK(strcmp(out, "Z") == 0, "a rejected buffer leaves out untouched");
}

TEST(the_utf16_detector_does_not_guess_from_too_little)
{
    /* One code unit is not evidence: a one-character ANSI string with its
     * terminator is indistinguishable from a one-character UTF-16 one, and
     * guessing wrong in that direction invents a name. Demand two. */
    static const unsigned char one_unit[] = { 'A', 0, 0, 0 };
    static const unsigned char two_units[] = { 'A', 0, 'B', 0, 0, 0 };
    static const unsigned char odd_len[]  = { 'A', 0, 'B' };
    static const unsigned char high_byte[] = { 0x41, 0x30, 0x42, 0x30, 0, 0 };
    char out[16];

    CHECK_EQ_I(hwpub_looks_utf16le(one_unit, (int)sizeof(one_unit)), 0);
    CHECK_EQ_I(hwpub_looks_utf16le(two_units, (int)sizeof(two_units)), 1);
    CHECK_EQ_I(hwpub_looks_utf16le(odd_len, (int)sizeof(odd_len)), 0);
    /* Non-ASCII UTF-16 (a CJK name) is not narrowable to ASCII, so it is not
     * claimed - better an empty field than mojibake presented as a card. */
    CHECK_EQ_I(hwpub_looks_utf16le(high_byte, (int)sizeof(high_byte)), 0);
    CHECK_EQ_I(hwpub_looks_utf16le(NULL, 8), 0);
    CHECK_EQ_I(hwpub_utf16le_narrow(NULL, 8, out, sizeof(out)), 0);

    /* And a narrow into a short buffer stays terminated. */
    {
        static const unsigned char longname[] = {
            'A',0,'B',0,'C',0,'D',0,'E',0,'F',0,'G',0,'H',0, 0,0 };
        char small[4];
        CHECK_EQ_I(hwpub_utf16le_narrow(longname, (int)sizeof(longname),
                                        small, sizeof(small)), 1);
        CHECK(strcmp(small, "ABC") == 0, "truncated and terminated");
    }
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
    RUN(a_mac_is_formatted_whole_not_truncated_to_its_first_octet);
    RUN(a_mac_never_runs_past_its_buffer);
    RUN(a_reg_binary_utf16_name_is_narrowed_not_truncated_to_one_letter);
    RUN(a_genuine_ansi_string_is_left_alone);
    RUN(the_utf16_detector_does_not_guess_from_too_little);
)
