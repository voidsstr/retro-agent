/* test_lnkcheck.c - TRUE-SOURCE: compiles the REAL agent/shared/lnkcheck.h,
 * the "does this .lnk already point there?" test gs_tool_shortcut() makes
 * before rebuilding the Retro Agent / Retro Chat desktop icons (agent 1.85.0).
 *
 * THE OLD-BUGGY BEHAVIOUR (gamesync.c gs_tool_shortcut, <= 1.84.x): no check at
 * all - every agent start created a ShellLink through COM and saved both
 * shortcuts again, which also makes Explorer refresh the desktop.
 *
 * What must hold: a shortcut whose LocalBasePath is the exe matches (any case);
 * a shortcut to a DIFFERENT file whose path merely starts the same does not -
 * a false match would leave the icon pointing at the wrong program, which is
 * the one thing this check must never do.
 */
#include "munit.h"
#include <string.h>

#include "../../agent/shared/lnkcheck.h"

/* A shortcut file, reduced to the parts that matter: the 0x4C-byte header,
 * some binary IDList bytes (with NULs), then LinkInfo's NUL-terminated
 * LocalBasePath, then a UTF-16 copy the way XP's IDList extension carries it. */
static size_t fake_lnk(unsigned char *out, const char *path)
{
    size_t n = 0, i;
    static const unsigned char head[] = { 0x4C, 0, 0, 0, 0x01, 0x14, 0x02, 0 };
    memcpy(out, head, sizeof(head)); n += sizeof(head);
    memset(out + n, 0, 0x44); n += 0x44;
    out[n++] = 0x14; out[n++] = 0; out[n++] = 0x1F; out[n++] = 0x50;
    for (i = 0; path[i]; i++)
        out[n++] = (unsigned char)path[i];
    out[n++] = 0;
    for (i = 0; path[i]; i++) {               /* UTF-16LE copy */
        out[n++] = (unsigned char)path[i];
        out[n++] = 0;
    }
    out[n++] = 0; out[n++] = 0;
    return n;
}

#define EXE "C:\\RETRO_AGENT\\retro_agent.exe"

TEST(an_existing_shortcut_to_the_exe_is_recognised)
{
    unsigned char b[1024];
    size_t n = fake_lnk(b, EXE);
    CHECK_EQ_I(lnk_bytes_name_path(b, n, EXE), 1);
    /* case: Windows paths are case-insensitive */
    CHECK_EQ_I(lnk_bytes_name_path(b, n, "c:\\retro_agent\\RETRO_AGENT.EXE"), 1);
}

TEST(a_look_alike_target_is_not_a_match)
{
    unsigned char b[1024];
    size_t n = fake_lnk(b, "C:\\RETRO_AGENT\\retro_agent.exe.old");
    CHECK_EQ_I(lnk_bytes_name_path(b, n, EXE), 0);
    n = fake_lnk(b, "D:\\RETRO_AGENT\\retro_agent.exe");     /* the other boot volume */
    CHECK_EQ_I(lnk_bytes_name_path(b, n, EXE), 0);
}

TEST(a_utf16_only_copy_does_not_count_so_the_shortcut_is_rebuilt)
{
    /* No ANSI path at all: "no match" means "recreate", i.e. the old
     * behaviour - safe by construction. */
    unsigned char b[256];
    size_t n = 0, i;
    for (i = 0; EXE[i]; i++) { b[n++] = (unsigned char)EXE[i]; b[n++] = 0; }
    b[n++] = 0; b[n++] = 0;
    CHECK_EQ_I(lnk_bytes_name_path(b, n, EXE), 0);
}

TEST(degenerate_inputs_are_no_match)
{
    unsigned char b[4] = { 'C', ':', 0, 0 };
    CHECK_EQ_I(lnk_bytes_name_path(b, 0, EXE), 0);
    CHECK_EQ_I(lnk_bytes_name_path(b, sizeof(b), ""), 0);
    CHECK_EQ_I(lnk_bytes_name_path(NULL, 10, EXE), 0);
    /* the path at the very end with no terminator is truncated: no match */
    {
        unsigned char t[64];
        size_t n = strlen(EXE);
        memcpy(t, EXE, n);
        CHECK_EQ_I(lnk_bytes_name_path(t, n, EXE), 0);
        t[n] = 0;
        CHECK_EQ_I(lnk_bytes_name_path(t, n + 1, EXE), 1);
    }
}

MUNIT_MAIN("tool-shortcut check (agent/shared/lnkcheck.h, agent 1.85.0)",
    RUN(an_existing_shortcut_to_the_exe_is_recognised);
    RUN(a_look_alike_target_is_not_a_match);
    RUN(a_utf16_only_copy_does_not_count_so_the_shortcut_is_rebuilt);
    RUN(degenerate_inputs_are_no_match);
)
