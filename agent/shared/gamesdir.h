/*
 * gamesdir.h - is a GamesDir value a usable local folder? (agent 1.93.0)
 *
 * HKLM\Software\RetroAgent\GamesDir moves GAMESYNC's titles off C:\Games on a
 * box whose C: is too small (.243: 1.2 GB C:, 72 GB E:). Only a local
 * "X:\folder" path is accepted - no UNC, no relative path, no "..", no
 * characters a Windows path cannot hold, not a bare drive root - and trailing
 * backslashes are dropped. Whether the drive is present is the caller's
 * question (GetDriveType); a value that fails either test makes GAMESYNC
 * refuse rather than fall back to C:.
 *
 * Win32-free: tests/native/test_gamesdir.c compiles this exact code.
 */
#ifndef RETRO_GAMESDIR_H
#define RETRO_GAMESDIR_H

#include <string.h>

#if defined(__GNUC__)
#define GAMESDIR_API static __attribute__((unused))
#else
#define GAMESDIR_API static
#endif

/* 1 = usable: `out` holds the normalised folder and `root` "X:\" (4 bytes).
 * 0 = not a local X:\folder path. */
GAMESDIR_API int gamesdir_normalise(const char *v, char *out, size_t cch, char *root)
{
    char   t[260];
    size_t n, i;
    if (!v || cch < 5) return 0;
    for (n = 0; v[n] && n < sizeof(t) - 1; n++) t[n] = v[n];
    if (v[n]) return 0;                              /* too long to be sane */
    t[n] = 0;
    while (n > 3 && t[n - 1] == '\\') t[--n] = 0;    /* "E:\GAMES\" -> "E:\GAMES" */
    if (n < 4 || n >= cch) return 0;                 /* "E:\" alone is a root, not a folder */
    if (!((t[0] >= 'A' && t[0] <= 'Z') || (t[0] >= 'a' && t[0] <= 'z')) || t[1] != ':' || t[2] != '\\')
        return 0;
    if (strstr(t, "..") || strchr(t, '/') || strstr(t + 2, "\\\\")) return 0;
    for (i = 0; i < n; i++)
        if ((unsigned char)t[i] < 0x20 || strchr("*?\"<>|", t[i]) || (i > 1 && t[i] == ':'))
            return 0;
    memcpy(out, t, n + 1);
    root[0] = t[0]; root[1] = ':'; root[2] = '\\'; root[3] = 0;
    return 1;
}

#endif
