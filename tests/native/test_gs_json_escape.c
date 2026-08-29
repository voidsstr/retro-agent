/* test_gs_json_escape.c - GAMESYNC's status JSON must survive a Windows path.
 *
 * WHY THIS EXISTS. The GAMESYNC status response is assembled with a raw
 * _snprintf rather than the json_t builder, and for a long time every field it
 * emitted was either a fixed word or a bare FILENAME (fd.cFileName), so nothing
 * ever needed escaping and the omission was invisible.
 *
 * Then `failed_file` was added - the first field carrying a FULL PATH. A
 * Windows path is dense with backslashes, and the backslash is JSON's escape
 * character, so "C:\Games\CounterStrike16\restart_debug.bat" emitted raw
 * contains \G, \C and \r - none of them valid escapes. The host's json.loads()
 * would raise and the ENTIRE status response would be lost: strictly worse than
 * the missing field the change was meant to provide, and it would look like the
 * agent had stopped answering rather than like a formatting bug.
 *
 * (\r is the nastier one: it is a *valid* JSON escape meaning carriage return,
 * so the document would parse and silently yield a mangled path.)
 *
 * The helper below is copied VERBATIM from agent/src/gamesync.c;
 * tests/python/test_gs_json_escape_mirror.py asserts the copy has not drifted.
 */
#include "munit.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* ==== VERBATIM from agent/src/gamesync.c - do not edit by hand ==== */
static void gs_json_escape(const char *in, char *out, size_t cap)
{
    size_t o = 0;

    if (!cap) return;
    for (; in && *in && o + 2 < cap; in++) {
        unsigned char c = (unsigned char)*in;
        if (c == '\\' || c == '"') {
            out[o++] = '\\';
            out[o++] = (char)c;
        } else if (c >= 0x20) {
            out[o++] = (char)c;
        }
        /* control characters are dropped rather than escaped: they cannot
         * appear in a real path and \uXXXX would need four more bytes */
    }
    out[o] = '\0';
}
/* ==== end verbatim ==== */

/* The real failure: a full Windows path, which is what failed_file carries. */
TEST(windows_path_backslashes_are_escaped)
{
    char out[512];
    gs_json_escape("C:\\Games\\CounterStrike16\\restart_debug.bat",
                   out, sizeof(out));
    CHECK(strcmp(out,
        "C:\\\\Games\\\\CounterStrike16\\\\restart_debug.bat") == 0,
        "every backslash is doubled");
    /* the old-buggy output, asserted absent */
    CHECK(strcmp(out, "C:\\Games\\CounterStrike16\\restart_debug.bat") != 0,
          "raw path is NOT emitted (that produced invalid JSON)");
}

/* \r is a VALID escape, so an unescaped one parses and silently corrupts. */
TEST(a_path_element_starting_with_r_cannot_become_a_carriage_return)
{
    char out[256];
    gs_json_escape("C:\\retro\\rev.ini", out, sizeof(out));
    CHECK(strcmp(out, "C:\\\\retro\\\\rev.ini") == 0, "\\r is not left raw");
    CHECK(strstr(out, "\\r") != NULL, "the literal two chars survive...");
    CHECK(strchr(out, '\r') == NULL, "...and no actual CR is produced");
}

TEST(quotes_are_escaped)
{
    char out[128];
    gs_json_escape("a\"b", out, sizeof(out));
    CHECK(strcmp(out, "a\\\"b") == 0, "a quote would close the JSON string");
}

TEST(control_characters_are_dropped)
{
    char out[128];
    gs_json_escape("a\tb\nc\x01" "d", out, sizeof(out));
    CHECK(strcmp(out, "abcd") == 0, "raw control chars are invalid in JSON");
}

/* The buffer is sized 2n+1 because every byte can double - but a caller could
 * still pass something smaller, and truncating mid-escape would emit a
 * trailing lone backslash and break the document anyway. */
TEST(truncation_never_emits_a_dangling_escape)
{
    char out[8];
    gs_json_escape("\\\\\\\\\\\\\\\\\\\\", out, sizeof(out));   /* all backslashes */
    CHECK(strlen(out) < sizeof(out), "stays in bounds");
    CHECK(strlen(out) % 2 == 0, "backslashes are emitted in complete pairs");
    CHECK(out[strlen(out)] == '\0', "always terminated");
}

TEST(empty_and_null_are_safe)
{
    char out[16];
    gs_json_escape("", out, sizeof(out));
    CHECK(out[0] == '\0', "empty in, empty out");
    out[0] = 'x';
    gs_json_escape(NULL, out, sizeof(out));
    CHECK(out[0] == '\0', "NULL in, empty out - not a crash");
    /* a zero-cap call must not write at all */
    gs_json_escape("abc", out, 0);
}

TEST(ordinary_text_is_untouched)
{
    char out[128];
    gs_json_escape("ONSNewTank-A.ukx", out, sizeof(out));
    CHECK(strcmp(out, "ONSNewTank-A.ukx") == 0, "no needless rewriting");
}

MUNIT_MAIN("GAMESYNC status JSON escaping (agent/src/gamesync.c)",
    RUN(windows_path_backslashes_are_escaped);
    RUN(a_path_element_starting_with_r_cannot_become_a_carriage_return);
    RUN(quotes_are_escaped);
    RUN(control_characters_are_dropped);
    RUN(truncation_never_emits_a_dangling_escape);
    RUN(empty_and_null_are_safe);
    RUN(ordinary_text_is_untouched);
)
