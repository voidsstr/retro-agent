/* test_toolpath.c - agent/shared/toolpath.h
 *
 * Fix: agent 1.98.1, gs_place_tool_shortcuts() (agent/src/gamesync.c).
 * Before it, the chat client's desktop shortcut named a hard-coded
 * "C:\RETRO_AGENT\retro_chat.exe", so a box running the stack from any other
 * directory got no Retro Chat icon and no log line saying why (.186,
 * D:\RETRO_AGENT, Windows 7, 2026-11-08).
 *
 * Each case asserts the FIXED value and, where it differs, names the OLD
 * BUGGY value explicitly so this cannot regress quietly.
 */
#include <stdio.h>
#include <string.h>
#include "../../agent/shared/toolpath.h"

static int fails = 0;

static void eq(const char *what, const char *got, const char *want)
{
    if (strcmp(got, want) != 0) {
        printf("  FAIL %s: got \"%s\" want \"%s\"\n", what, got, want);
        fails++;
    }
}
static void eqi(const char *what, int got, int want)
{
    if (got != want) {
        printf("  FAIL %s: got %d want %d\n", what, got, want);
        fails++;
    }
}

int main(void)
{
    char dir[260], out[260];

    /* --- tp_dir_of ------------------------------------------------------ */
    eqi("dir_of(D:\\RETRO_AGENT\\retro_agent.exe) ok",
        tp_dir_of("D:\\RETRO_AGENT\\retro_agent.exe", dir, sizeof(dir)), 1);
    eq("dir_of D: install", dir, "D:\\RETRO_AGENT");

    eqi("dir_of(C:\\RETRO_AGENT\\retro_agent.exe) ok",
        tp_dir_of("C:\\RETRO_AGENT\\retro_agent.exe", dir, sizeof(dir)), 1);
    eq("dir_of C: install", dir, "C:\\RETRO_AGENT");

    /* A drive root must KEEP its separator: "D:" + "retro_chat.exe" is
     * "D:retro_chat.exe", which Windows resolves against the current
     * directory on D:, not the root of D:. */
    eqi("dir_of(D:\\retro_agent.exe) ok",
        tp_dir_of("D:\\retro_agent.exe", dir, sizeof(dir)), 1);
    eq("dir_of drive root keeps the slash", dir, "D:\\");

    /* No directory part at all must FAIL, not yield "" - a relative target
     * resolves against the shell's current directory. */
    eqi("dir_of(bare filename) refuses",
        tp_dir_of("retro_agent.exe", dir, sizeof(dir)), 0);
    eq("dir_of(bare filename) leaves buf empty", dir, "");
    eqi("dir_of(NULL) refuses", tp_dir_of(NULL, dir, sizeof(dir)), 0);
    eqi("dir_of into a tiny buffer refuses",
        tp_dir_of("D:\\RETRO_AGENT\\retro_agent.exe", dir, 4), 0);

    /* --- tp_join -------------------------------------------------------- */
    eqi("join ok", tp_join("D:\\RETRO_AGENT", "retro_chat.exe", out, sizeof(out)), 1);
    eq("join", out, "D:\\RETRO_AGENT\\retro_chat.exe");
    /* THE WHOLE POINT: this must NOT be the old hard-coded path. */
    if (strcmp(out, "C:\\RETRO_AGENT\\retro_chat.exe") == 0) {
        printf("  FAIL regression: a D: install resolved to the old hard-coded C: path\n");
        fails++;
    }

    eqi("join tolerates a trailing slash",
        tp_join("D:\\", "retro_chat.exe", out, sizeof(out)), 1);
    eq("join at a drive root", out, "D:\\retro_chat.exe");

    eqi("join refuses to overflow",
        tp_join("D:\\RETRO_AGENT", "retro_chat.exe", out, 8), 0);
    eq("join leaves buf empty on overflow", out, "");

    /* --- tp_pick: preference order -------------------------------------- */
    eqi("beside wins when it exists",        tp_pick(1, 1, 1), TP_PICK_BESIDE);
    eqi("beside wins even if fixed is gone", tp_pick(1, 0, 1), TP_PICK_BESIDE);
    /* The fallback is load-bearing: fxpanel_ensure() still copies 3dfxctl.exe
     * to the fixed FXP_DEST, so a box with the tool only there must keep it. */
    eqi("fixed is used when only it exists", tp_pick(0, 1, 1), TP_PICK_FIXED);
    /* Neither present: hand back the beside path, never "" - gs_tool_shortcut
     * reads a missing target as "not installed here" and returns quietly. */
    eqi("neither present -> beside",         tp_pick(0, 0, 1), TP_PICK_BESIDE);
    /* GetModuleFileName failed: the fixed path is all we have. */
    eqi("no agent dir -> fixed",             tp_pick(0, 0, 0), TP_PICK_FIXED);
    eqi("no agent dir -> fixed even so",     tp_pick(1, 1, 0), TP_PICK_FIXED);

    /* --- the end-to-end shape the agent uses ---------------------------- */
    tp_dir_of("D:\\RETRO_AGENT\\retro_agent.exe", dir, sizeof(dir));
    tp_join(dir, "retro_chat.exe", out, sizeof(out));
    eq("D: box gets its own chat client", out, "D:\\RETRO_AGENT\\retro_chat.exe");
    tp_dir_of("C:\\RETRO_AGENT\\retro_agent.exe", dir, sizeof(dir));
    tp_join(dir, "retro_chat.exe", out, sizeof(out));
    eq("C: box is unchanged by the fix", out, "C:\\RETRO_AGENT\\retro_chat.exe");

    if (fails == 0)
        printf("test_toolpath: OK\n");
    else
        printf("test_toolpath: %d FAILURE(S)\n", fails);
    return fails ? 1 : 0;
}
