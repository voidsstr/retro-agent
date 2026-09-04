/*
 * gamesroot.h - validate an override for where game trees are installed.
 *
 * Shared between the agent (agent/src/gamesync.c, gs_dest_path) and the host
 * test (tests/native/test_gamesroot.c) so the rule has exactly one copy.
 *
 * WHY IT VALIDATES AT ALL. The value comes from
 * HKLM\Software\RetroAgent\GamesRoot and decides where a ~32 GB copy lands.
 * A typo there - a bare drive letter, a UNC, a relative path, an empty string
 * - must fall back to the compiled default rather than be honoured, because
 * every one of those failure modes is silent: GAMESYNC would report
 * state=done having created a directory nobody looks in.
 *
 * A trailing separator is STRIPPED rather than rejected. "D:\Games\" is what
 * a person types and it is unambiguous; refusing it would make the feature
 * feel broken for the most likely input.
 */
#ifndef RETRO_GAMESROOT_H
#define RETRO_GAMESROOT_H

/* Returns 1 and fills `out` (NUL-terminated) when `in` is an acceptable games
 * root; returns 0 and leaves `out` untouched when it is not. */
static int gr_valid_root(const char *in, char *out, unsigned cch)
{
    char tmp[260];
    int  n = 0;

    if (!in || !out || cch < 8)
        return 0;
    while (in[n] && n < (int)sizeof(tmp) - 1) {
        tmp[n] = in[n];
        n++;
    }
    tmp[n] = 0;
    while (n > 3 && tmp[n - 1] == '\\')
        tmp[--n] = 0;
    /* "X:\d" is the shortest acceptable form: drive, colon, separator, and at
     * least one character of directory name. A bare "D:\" is REJECTED on
     * purpose - dropping 32 GB of game trees into the root of a volume is
     * never what was meant, and it would mix them with whatever is there. */
    if (n < 4)
        return 0;
    if (!((tmp[0] >= 'A' && tmp[0] <= 'Z') || (tmp[0] >= 'a' && tmp[0] <= 'z')))
        return 0;
    if (tmp[1] != ':' || tmp[2] != '\\')
        return 0;
    if (n >= (int)cch)
        return 0;
    for (n = 0; tmp[n]; n++)
        out[n] = tmp[n];
    out[n] = 0;
    return 1;
}

#endif /* RETRO_GAMESROOT_H */
