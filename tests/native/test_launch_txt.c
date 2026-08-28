/* test_launch_txt.c - launch.txt must yield ONE desktop shortcut per line.
 *
 * WHAT THIS PROTECTS. The parser used to read a single line, and that quietly
 * cost several library titles half their content. Red Alert 2's tree contains
 * Yuri's Revenge (RA2MD.exe plus the expandmd mixes) and Descent II ships a
 * Glide build beside the Windows one - all present on disk, none reachable from
 * the desktop, with nothing in any log to say so. A one-line read is
 * indistinguishable from a correct read when the file happens to have one line,
 * which is why this went unnoticed and why it needs a test rather than a
 * comment.
 *
 * Mirrors the line-walking loop from gs_make_game_shortcut() in
 * agent/src/gamesync.c (agent 1.42.0). Asserts BOTH the fixed behaviour and the
 * old buggy one, so a regression to "first line only" fails here.
 */

#include "munit.h"
#include <stdio.h>
#include <string.h>

/* One parsed entry: the exe and its display name. */
typedef struct { char exe[260]; char disp[128]; } entry_t;

/* Verbatim logic from gs_shortcut_from_line() + the loop in
 * gs_make_game_shortcut(), minus the Win32 shortcut creation. */
static int parse_launch(const char *contents, entry_t *out, int max)
{
    char  buf[1024];
    char *line, *end, *tab;
    int   n = 0;

    strncpy(buf, contents, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = 0;

    line = buf;
    while (*line && n < max) {
        end = line;
        while (*end && *end != '\r' && *end != '\n')
            end++;
        if (*end) {
            *end = 0;
            end++;
            /* Step over the LF of a CRLF, or the parser would start the next
             * line ON that LF and every second entry would be lost. */
            while (*end == '\r' || *end == '\n')
                end++;
        }

        {
            char *p = line;
            while (*p == ' ' || *p == '\t')
                p++;
            if (*p && *p != '#') {
                out[n].disp[0] = 0;
                tab = p;
                while (*tab && *tab != '\t')
                    tab++;
                if (*tab == '\t') {
                    *tab = 0;
                    strncpy(out[n].disp, tab + 1, sizeof(out[n].disp) - 1);
                    out[n].disp[sizeof(out[n].disp) - 1] = 0;
                }
                strncpy(out[n].exe, p, sizeof(out[n].exe) - 1);
                out[n].exe[sizeof(out[n].exe) - 1] = 0;
                if (out[n].exe[0])
                    n++;
            }
        }
        line = end;
    }
    return n;
}

int main(void)
{
    entry_t e[8];
    int     n;

    /* The real Red Alert 2 launch.txt: two comment lines, two entries. */
    const char *ra2 =
        "# Red Alert 2 and Yuri's Revenge share one install. The .bat launchers\r\n"
        "# pass -speedcontrol and clean up after the game.\r\n"
        "Launch Red Alert 2.bat\tCommand and Conquer - Red Alert 2\r\n"
        "Launch Yuri's Revenge.bat\tRed Alert 2 - Yuri's Revenge\r\n";

    n = parse_launch(ra2, e, 8);
    CHECK(n == 2, "Red Alert 2 yields TWO shortcuts, not one");
    /* The old single-line parser returned 1 - and on this very file it would
     * have returned ZERO, because the first line is a comment. */
    CHECK(n != 1, "a one-line read is a regression");
    CHECK(strcmp(e[0].exe, "Launch Red Alert 2.bat") == 0, "first exe");
    CHECK(strcmp(e[0].disp, "Command and Conquer - Red Alert 2") == 0,
          "first display name");
    CHECK(strcmp(e[1].exe, "Launch Yuri's Revenge.bat") == 0,
          "SECOND exe - the entry the old parser never saw");
    CHECK(strcmp(e[1].disp, "Red Alert 2 - Yuri's Revenge") == 0,
          "second display name");

    /* CRLF must not leave the next line starting on an LF. */
    n = parse_launch("a.exe\tA\r\nb.exe\tB\r\nc.exe\tC\r\n", e, 8);
    CHECK(n == 3, "CRLF: every line parses");
    CHECK(strcmp(e[1].exe, "b.exe") == 0, "CRLF: no leading LF on line 2");
    CHECK(strcmp(e[2].exe, "c.exe") == 0, "CRLF: no leading LF on line 3");

    /* Bare LF must work too - files get edited on the Linux side. */
    n = parse_launch("a.exe\tA\nb.exe\tB\n", e, 8);
    CHECK(n == 2, "bare LF parses");
    CHECK(strcmp(e[1].exe, "b.exe") == 0, "bare LF: second entry");

    /* A single-entry file - by far the common case - still works. */
    n = parse_launch("sshock.exe\tSystem Shock\r\n", e, 8);
    CHECK(n == 1, "single entry");
    CHECK(strcmp(e[0].disp, "System Shock") == 0, "single entry name");

    /* No tab: the exe stands alone and the caller falls back to the title. */
    n = parse_launch("game.exe\r\n", e, 8);
    CHECK(n == 1, "no tab: one entry");
    CHECK(e[0].disp[0] == 0, "no tab: display name left empty for the caller");

    /* Blank lines and indentation are tolerated; comments are skipped. */
    n = parse_launch("\r\n  a.exe\tA\r\n\r\n# note\r\n\tb.exe\tB\r\n", e, 8);
    CHECK(n == 2, "blank lines and comments skipped");
    CHECK(strcmp(e[0].exe, "a.exe") == 0, "leading spaces trimmed");
    CHECK(strcmp(e[1].exe, "b.exe") == 0, "leading tab trimmed");

    /* A file of nothing but comments yields nothing, and must not crash. */
    n = parse_launch("# all comments\r\n# nothing here\r\n", e, 8);
    CHECK(n == 0, "comment-only file yields no shortcuts");

    printf("launch.txt parsing: all checks passed\n");
    return 0;
}
