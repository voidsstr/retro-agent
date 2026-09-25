/*
 * rwcompare.h - the "is it already like that?" decisions behind retrowall's
 * startup pass (agent 1.85.0).
 *
 * retrowall re-applies the fleet desktop on every agent start. Until 1.85.0 it
 * re-applied ALL of it every time, whatever the box already looked like:
 *   - 29 registry writes and a SetSysColors() - which broadcasts
 *     WM_SYSCOLORCHANGE to every top-level window, so every application
 *     repaints - plus an explicit WM_SYSCOLORCHANGE and WM_THEMECHANGED
 *     broadcast of its own, and a SetSystemVisualStyle() call;
 *   - two SystemParametersInfo(SPIF_SENDWININICHANGE) for the screensaver,
 *     each another broadcast;
 *   - SPI_SETDESKWALLPAPER with SPIF_SENDWININICHANGE: reload and decode the
 *     BMP (6 MB at 1920x1080x24), repaint the desktop, broadcast again;
 *   - and on XP a cmd.exe + taskkill.exe spawn to kill a rotate_wall.exe that
 *     had been renamed .superseded boxes ago and could not be running.
 * On a Pentium that is a visible stall at every boot for a desktop that was
 * already exactly right.
 *
 * Now each step compares first and does only what differs. These helpers are
 * the comparisons; Win32-free so tests/native/test_rwcompare.c compiles them.
 */
#ifndef RETRO_RWCOMPARE_H
#define RETRO_RWCOMPARE_H

#ifdef __GNUC__
#define RWC_UNUSED __attribute__((unused))
#else
#define RWC_UNUSED
#endif

/* Do the live system colours need SetSysColors? `supported[i]` is 0 for an
 * index this Windows does not have (GetSysColorBrush returns NULL for it -
 * COLOR_MENUHILIGHT on Win98, say): it can never read back as set, and
 * counting it would make the answer "yes" on every boot forever. */
RWC_UNUSED static int rw_colors_need_apply(const unsigned long *cur,
                                           const unsigned long *want,
                                           const int *supported, int n)
{
    int i;
    for (i = 0; i < n; i++)
        if (supported[i] && (cur[i] & 0xFFFFFFUL) != (want[i] & 0xFFFFFFUL))
            return 1;
    return 0;
}

/* The screensaver SPI calls still needed. A GET that failed (`*_known` = 0)
 * counts as different - better one redundant SET than a setting never made. */
#define RW_SS_SET_ACTIVE   1
#define RW_SS_SET_TIMEOUT  2
RWC_UNUSED static int rw_screensaver_sets(int active_known, int active,
                                          int timeout_known, int timeout,
                                          int want_timeout)
{
    int sets = 0;
    if (!active_known || !active)
        sets |= RW_SS_SET_ACTIVE;
    if (!timeout_known || timeout != want_timeout)
        sets |= RW_SS_SET_TIMEOUT;
    return sets;
}

/* ASCII case-insensitive string equality (registry values, Windows paths). */
RWC_UNUSED static int rw_ieq(const char *a, const char *b)
{
    for (;; a++, b++) {
        char x = *a, y = *b;
        if (x >= 'A' && x <= 'Z') x = (char)(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = (char)(y - 'A' + 'a');
        if (x != y)
            return 0;
        if (!x)
            return 1;
    }
}

/* Is this process image `exe`? Toolhelp's szExeFile is a bare name on NT but
 * the FULL PATH on Windows 9x, so compare the last path component. */
RWC_UNUSED static int rw_image_is(const char *szExeFile, const char *exe)
{
    const char *base = szExeFile, *p;
    for (p = szExeFile; *p; p++)
        if (*p == '\\' || *p == '/' || *p == ':')
            base = p + 1;
    return rw_ieq(base, exe);
}

#endif /* RETRO_RWCOMPARE_H */
