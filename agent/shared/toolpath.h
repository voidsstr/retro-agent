/* toolpath.h - where the tools that ship BESIDE the agent actually live.
 *
 * Win32-free on purpose, so tests/native/test_toolpath.c compiles the very
 * code the agent runs rather than a paraphrase of it.
 *
 * WHY THIS EXISTS (agent 1.98.1, found on .186 2026-11-08)
 * -------------------------------------------------------
 * gs_place_tool_shortcuts() asked Windows for the agent's own path - with a
 * comment saying a dual-boot box can run it from the other volume, so never
 * assume C:\RETRO_AGENT - and then, three lines later, named
 * "C:\RETRO_AGENT\retro_chat.exe" for the chat client's desktop shortcut.
 *
 * .186 runs the whole stack from D:\RETRO_AGENT. The chat client itself was
 * fine: autoupdate.c's update_retro_chat() has always built
 * "<install_dir>\retro_chat.exe" from the running agent, so it installed,
 * updated and relaunched correctly. Only the ICON was missing - and
 * gs_tool_shortcut() returns silently when the exe is not there ("not
 * installed here - not an error"), so a box with 97 game icons and no Retro
 * Chat icon produced not one line of log to explain it.
 *
 * The rule: a tool that ships beside the agent is looked for beside the agent
 * FIRST, and at the historical fixed path second. The fallback is not
 * decoration - fxpanel_ensure() still copies 3dfxctl.exe to a hard-coded
 * FXP_DEST ("C:\RETRO_AGENT"), so dropping it would break every box that
 * mechanism has ever served.
 */
#ifndef RETRO_TOOLPATH_H
#define RETRO_TOOLPATH_H

#include <string.h>

#define TP_FIXED_DIR "C:\\RETRO_AGENT"

/* tp_dir_of - the directory part of a path, with no trailing backslash.
 *
 * Returns 0 (and leaves buf empty) when the path has no directory part at
 * all, because "" would silently become a RELATIVE path, and a relative
 * shortcut target resolves against whatever the shell's current directory
 * happens to be. A caller that gets 0 must use TP_FIXED_DIR, not buf. */
static int tp_dir_of(const char *exe, char *buf, size_t cap)
{
    size_t n;
    const char *slash;

    if (!buf || cap == 0)
        return 0;
    buf[0] = 0;
    if (!exe || !*exe)
        return 0;
    n = strlen(exe);
    slash = exe + n;
    while (slash > exe && *(slash - 1) != '\\' && *(slash - 1) != '/')
        slash--;
    if (slash == exe)
        return 0;                       /* bare filename - no directory */
    n = (size_t)(slash - exe) - 1;      /* drop the separator itself */
    /* A root such as "D:\" keeps its separator, or the join below would
     * produce "D:retro_chat.exe" - which Windows reads as a path relative to
     * the current directory ON drive D, not the root of it. */
    if (n == 2 && exe[1] == ':')
        n = 3;
    if (n == 0 || n >= cap)
        return 0;
    memcpy(buf, exe, n);
    buf[n] = 0;
    return 1;
}

/* tp_join - "<dir>\<leaf>", tolerating a dir that already ends in a slash. */
static int tp_join(const char *dir, const char *leaf, char *buf, size_t cap)
{
    size_t dn, ln;
    int sep;

    if (!buf || cap == 0)
        return 0;
    buf[0] = 0;
    if (!dir || !*dir || !leaf || !*leaf)
        return 0;
    dn = strlen(dir);
    ln = strlen(leaf);
    sep = !(dir[dn - 1] == '\\' || dir[dn - 1] == '/');
    if (dn + (size_t)sep + ln + 1 > cap)
        return 0;
    memcpy(buf, dir, dn);
    if (sep)
        buf[dn++] = '\\';
    memcpy(buf + dn, leaf, ln + 1);
    return 1;
}

/* tp_pick - the candidate to use, given whether each one exists.
 *
 * Split out from the filesystem so the PREFERENCE ORDER is testable on the
 * dev host: beside the agent wins; the fixed path is the fallback; and when
 * NEITHER exists we still return the beside-the-agent path, because the
 * caller (gs_tool_shortcut) treats a missing target as "not installed here"
 * and must not be handed an empty string. */
#define TP_PICK_BESIDE 1
#define TP_PICK_FIXED  2
static int tp_pick(int beside_exists, int fixed_exists, int have_beside)
{
    if (!have_beside)
        return TP_PICK_FIXED;
    if (beside_exists)
        return TP_PICK_BESIDE;
    if (fixed_exists)
        return TP_PICK_FIXED;
    return TP_PICK_BESIDE;
}

#endif /* RETRO_TOOLPATH_H */
