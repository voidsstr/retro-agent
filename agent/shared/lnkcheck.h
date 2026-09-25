/*
 * lnkcheck.h - does an existing .lnk already point at this file? (agent 1.85.0)
 *
 * gs_place_tool_shortcuts() puts the "Retro Agent" and "Retro Chat" icons back
 * on the desktop at every agent start. Its comment promised "cheap no-ops when
 * the shortcut already exists and points at the same place", but nothing
 * checked: each start created a ShellLink through COM, resolved the target and
 * saved the file again - which also makes Explorer refresh the desktop - for
 * two icons that were already there.
 *
 * A shortcut to a file on a local drive stores the target's full path as ANSI
 * text (the LinkInfo block's LocalBasePath, NUL-terminated), so reading the
 * file's bytes answers the question without COM. The caller tries the path as
 * the agent knows it and its short/long forms; no match means "recreate", which
 * is exactly the old behaviour - this can only ever save work, never skip a
 * shortcut that needed writing because of a false match, as long as a match
 * requires the WHOLE path followed by its terminator.
 *
 * Win32-free so tests/native/test_lnkcheck.c compiles it.
 */
#ifndef RETRO_LNKCHECK_H
#define RETRO_LNKCHECK_H

#include <stddef.h>

#ifdef __GNUC__
#define LNKC_UNUSED __attribute__((unused))
#else
#define LNKC_UNUSED
#endif

/* Does buf[0..len) contain `path` (ASCII case-insensitive) immediately
 * followed by a NUL byte? The NUL is what stops "retro_agent.exe" matching
 * inside "retro_agent.exe.old". */
LNKC_UNUSED static int lnk_bytes_name_path(const unsigned char *buf, size_t len,
                                           const char *path)
{
    size_t n = 0, i, j;
    if (!buf || !path)
        return 0;
    while (path[n])
        n++;
    if (!n || len < n + 1)
        return 0;
    for (i = 0; i + n < len; i++) {
        for (j = 0; j < n; j++) {
            unsigned char x = buf[i + j], y = (unsigned char)path[j];
            if (x >= 'A' && x <= 'Z') x = (unsigned char)(x - 'A' + 'a');
            if (y >= 'A' && y <= 'Z') y = (unsigned char)(y - 'A' + 'a');
            if (x != y)
                break;
        }
        if (j == n && buf[i + n] == 0)
            return 1;
    }
    return 0;
}

#endif /* RETRO_LNKCHECK_H */
