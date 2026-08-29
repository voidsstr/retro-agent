/* test_reg_argparse.c - the registry commands must not truncate a key path at
 * a space.
 *
 * THE BUG. handle_regread/regwrite/regdelete parsed their arguments with
 * sscanf("%31s %511s ..."). %s stops at whitespace, so ANY path containing a
 * space was silently cut in half:
 *
 *   REGREAD HKLM SOFTWARE\Microsoft\Windows NT\CurrentVersion
 *     -> path  = "SOFTWARE\Microsoft\Windows"
 *        value = "NT\CurrentVersion"
 *
 * and the open then failed with "Cannot open key: error 0" - which is
 * indistinguishable from the key not existing. "Windows NT" is one of the most
 * common paths on the system. It cost real time on .133, where a staged game's
 * install.reg was nearly reported as never merged because the key it wrote
 * could not be read back.
 *
 * THE FIX, in three parts:
 *   - a path may be QUOTED, which says exactly where it ends;
 *   - REGDELETE takes the WHOLE remainder as the path, because nothing follows
 *     it and there is therefore no ambiguity at all;
 *   - REGREAD, given unquoted input with spaces, tries the whole remainder as
 *     a path FIRST and only then falls back to treating the last token as a
 *     value name. That is what makes the Windows NT case work unquoted.
 *
 * The two parsing helpers below are copied VERBATIM from
 * agent/src/registry.c. tests/python/test_reg_argparse_mirror.py extracts both
 * functions from that file and from this one and asserts they are byte
 * identical, so this copy cannot drift from the code it is testing.
 */
#include "munit.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define _snprintf snprintf
static void safe_strncpy(char *d, const char *s, size_t n)
{ if (!n) return; strncpy(d, s, n - 1); d[n - 1] = '\0'; }

/* ==== VERBATIM from agent/src/registry.c - do not edit by hand ==== */
static int reg_next_arg(const char **pp, char *buf, size_t cap, int *was_quoted)
{
    const char *p = *pp;
    size_t n = 0;

    if (was_quoted) *was_quoted = 0;
    while (*p == ' ' || *p == '\t') p++;
    if (!*p) { *pp = p; buf[0] = '\0'; return 0; }

    if (*p == '"') {
        p++;
        if (was_quoted) *was_quoted = 1;
        while (*p && *p != '"') {
            if (n + 1 < cap) buf[n++] = *p;
            p++;
        }
        if (*p == '"') p++;
    } else {
        while (*p && *p != ' ' && *p != '\t') {
            if (n + 1 < cap) buf[n++] = *p;
            p++;
        }
    }
    buf[n] = '\0';
    *pp = p;
    return 1;
}

static void reg_rest_arg(const char *p, char *buf, size_t cap)
{
    size_t n;

    while (*p == ' ' || *p == '\t') p++;
    safe_strncpy(buf, p, cap);

    n = strlen(buf);
    while (n > 0 && (buf[n-1] == ' ' || buf[n-1] == '\t' ||
                     buf[n-1] == '\r' || buf[n-1] == '\n'))
        buf[--n] = '\0';

    if (buf[0] == '"' && n >= 2 && buf[n-1] == '"') {
        memmove(buf, buf + 1, n - 2);
        buf[n-2] = '\0';
    }
}
/* ==== end verbatim ==== */

/* Mirrors handle_regdelete(): root, then the whole rest is the path. */
static void parse_delete(const char *args, char *root, size_t rc,
                         char *path, size_t pc)
{
    const char *p = args;
    reg_next_arg(&p, root, rc, NULL);
    reg_rest_arg(p, path, pc);
}

/* The exact case that cost the time: "Windows NT" in a REGREAD path. */
TEST(quoted_path_survives_spaces)
{
    const char *args =
        "HKLM \"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\" ProductName";
    char root[32], path[512], value[256];
    const char *q = args;
    int quoted = 0;

    reg_next_arg(&q, root,  sizeof(root),  NULL);
    reg_next_arg(&q, path,  sizeof(path),  &quoted);
    reg_next_arg(&q, value, sizeof(value), NULL);

    CHECK(strcmp(root, "HKLM") == 0, "root parsed");
    CHECK_EQ_I(quoted, 1);
    CHECK(strcmp(path, "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion") == 0,
          "quoted path keeps its spaces");
    /* The old sscanf(\"%511s\") produced exactly this - assert it is gone. */
    CHECK(strcmp(path, "SOFTWARE\\Microsoft\\Windows") != 0,
          "path is NOT truncated at the first space (the bug)");
    CHECK(strcmp(value, "ProductName") == 0, "value name follows the quotes");
}

/* Every existing unquoted caller must behave exactly as before. */
TEST(unquoted_two_token_form_unchanged)
{
    const char *args = "HKLM SOFTWARE\\RetroAgent AIEngine";
    char root[32], path[512], value[256];
    const char *q = args;
    int quoted = 1;

    reg_next_arg(&q, root,  sizeof(root),  NULL);
    reg_next_arg(&q, path,  sizeof(path),  &quoted);
    reg_next_arg(&q, value, sizeof(value), NULL);

    CHECK_EQ_I(quoted, 0);
    CHECK(strcmp(path,  "SOFTWARE\\RetroAgent") == 0, "unquoted path intact");
    CHECK(strcmp(value, "AIEngine") == 0,             "unquoted value intact");
}

/* REGDELETE has nothing after the path, so spaces are never ambiguous. */
TEST(regdelete_takes_the_whole_remainder)
{
    char root[32], path[512];

    parse_delete("HKLM SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Winlogon",
                 root, sizeof(root), path, sizeof(path));

    CHECK(strcmp(root, "HKLM") == 0, "root parsed");
    CHECK(strcmp(path,
        "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Winlogon") == 0,
        "whole path kept, spaces and all");
    CHECK(strcmp(path, "SOFTWARE\\Microsoft\\Windows") != 0,
          "not truncated at the first space (the bug)");
}

TEST(regdelete_strips_quotes_and_trailing_junk)
{
    char root[32], path[512];

    parse_delete("HKCU \"Software\\Valve\\Half-Life\\Settings\"   ",
                 root, sizeof(root), path, sizeof(path));
    CHECK(strcmp(path, "Software\\Valve\\Half-Life\\Settings") == 0,
          "surrounding quotes removed");

    /* A line-oriented client's CR must not become part of the key name. */
    parse_delete("HKLM SOFTWARE\\RetroAgent\r\n",
                 root, sizeof(root), path, sizeof(path));
    CHECK(strcmp(path, "SOFTWARE\\RetroAgent") == 0, "CRLF trimmed");
}

/* The old code used fixed-width %s conversions; the new one must be no less
 * safe about a hostile or merely long argument. */
TEST(long_input_truncates_rather_than_overflows)
{
    char small[8];
    char big[600];
    const char *q;

    memset(big, 'A', sizeof(big) - 1);
    big[sizeof(big) - 1] = '\0';

    q = big;
    reg_next_arg(&q, small, sizeof(small), NULL);
    CHECK_EQ_U(strlen(small), sizeof(small) - 1);
    CHECK(small[sizeof(small) - 1] == '\0', "always NUL terminated");

    reg_rest_arg(big, small, sizeof(small));
    CHECK_EQ_U(strlen(small), sizeof(small) - 1);
    CHECK(small[sizeof(small) - 1] == '\0', "always NUL terminated");
}

TEST(empty_missing_and_unterminated_quote)
{
    char buf[32];
    const char *q = "";

    CHECK_EQ_I(reg_next_arg(&q, buf, sizeof(buf), NULL), 0);
    CHECK(buf[0] == '\0', "empty input yields an empty argument");

    q = "   ";
    CHECK_EQ_I(reg_next_arg(&q, buf, sizeof(buf), NULL), 0);

    /* An unterminated quote must stop at the end of the string, not run past. */
    q = "\"SOFTWARE\\Windows NT";
    reg_next_arg(&q, buf, sizeof(buf), NULL);
    CHECK(strlen(buf) < sizeof(buf), "unterminated quote is bounded");
    CHECK(*q == '\0', "consumed to end of input");
}

MUNIT_MAIN("registry argument parsing (agent/src/registry.c)",
    RUN(quoted_path_survives_spaces);
    RUN(unquoted_two_token_form_unchanged);
    RUN(regdelete_takes_the_whole_remainder);
    RUN(regdelete_strips_quotes_and_trailing_junk);
    RUN(long_input_truncates_rather_than_overflows);
    RUN(empty_missing_and_unterminated_quote);
)
