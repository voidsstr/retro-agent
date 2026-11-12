/*
 * shortcutdir.h - ONE FOLDER ON THE DESKTOP, not 150 loose icons (agent 1.99.0)
 *
 * HKLM\Software\RetroAgent\ShortcutFolder (REG_SZ) names a folder on the
 * desktop that GAMESYNC writes every shortcut into - the games AND the
 * operator's tools - instead of dropping them on the desktop itself. Absent or
 * empty = the desktop, exactly as every version before this one.
 *
 * WHY THIS IS MORE THAN A PATH CHANGE. Three other mechanisms are about the
 * DESKTOP, and with our icons no longer on it they must not act on it:
 *
 *   - THE SWEEP (deskset.h) moves every .lnk/.pif/.url that a run did not put
 *     back into C:\retro-desktop-backup. That is right on a fleet box, whose
 *     desktop is ours to curate, and it is vandalism on a machine where the
 *     desktop belongs to somebody - which is exactly the kind of machine that
 *     asks for a folder. So when a folder is set, the run samples and sweeps
 *     THE FOLDER and never looks at either desktop.
 *   - AUTO ARRANGE packs the desktop's icons. A folder's window has its own
 *     view, and re-arranging the user's desktop because we added a folder is
 *     a change nobody asked for. Skipped.
 *   - THE ICON BAY places icons in cells the fleet wallpaper draws. Also moot.
 *
 * NAME, NOT PATH. The value is a single folder NAME, validated here, and the
 * folder is created beside the shortcuts' usual home. A path would let one
 * registry value redirect the whole shortcut set anywhere on the disk - a
 * "C:\Windows\System32" in that value must not be reachable by typo, and the
 * sweep, which DELETES, follows wherever this leads.
 *
 * Win32-free so tests/native/test_shortcutdir.c compiles THIS file.
 */
#ifndef RETRO_SHORTCUTDIR_H
#define RETRO_SHORTCUTDIR_H

#ifdef __GNUC__
#define SCD_UNUSED __attribute__((unused))
#else
#define SCD_UNUSED
#endif

#define SCD_NAME_MAX 64         /* a desktop folder name; not a path          */
#define SCD_PATH_MAX 260        /* MAX_PATH                                   */

/*
 * Is `name` a folder name this may create on the desktop?
 *
 * Refused, each for a reason that has cost somebody time somewhere:
 *   - empty, or longer than SCD_NAME_MAX                  (nothing to create)
 *   - any of \ / : * ? " < > |                   (a path, or an illegal name)
 *   - "." or ".."                                      (that IS the desktop)
 *   - a control character                      (unprintable in the shell, and
 *                                               unmatchable by the sweep)
 *   - a leading or trailing space, or a trailing dot - WINDOWS SILENTLY STRIPS
 *     THOSE when it creates the directory, so the folder we then look for and
 *     sweep is not the folder we made, and every run re-adds the shortcuts
 *     into a directory it cannot see.
 *
 * Returns 1 = usable, 0 = refused (caller falls back to the desktop and says
 * so; a refusal must never silently mean "the desktop", which is how 150 icons
 * reappear on a machine that asked for a folder).
 */
SCD_UNUSED static int scd_name_ok(const char *name)
{
    int i, n = 0;

    if (!name || !name[0])
        return 0;
    while (name[n])
        n++;
    if (n > SCD_NAME_MAX)
        return 0;
    if (name[0] == ' ' || name[n - 1] == ' ' || name[n - 1] == '.')
        return 0;
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)name[i];
        if (c < 0x20)
            return 0;
        if (c == '\\' || c == '/' || c == ':' || c == '*' || c == '?' ||
            c == '"'  || c == '<' || c == '>' || c == '|')
            return 0;
    }
    if (name[0] == '.' && (n == 1 || (n == 2 && name[1] == '.')))
        return 0;
    return 1;
}

/*
 * Build "<parent>\<name>". Returns 1 on success, 0 if the name is refused, the
 * parent is empty, or the result would not fit in `cap` - never a truncated
 * path. A truncated directory path is the one failure mode that could have the
 * sweep delete files from somewhere else entirely.
 */
SCD_UNUSED static int scd_join(const char *parent, const char *name,
                               char *out, int cap)
{
    int p = 0, n = 0, i;

    if (out && cap > 0)
        out[0] = 0;
    if (!out || cap <= 0 || !parent || !parent[0] || !scd_name_ok(name))
        return 0;
    while (parent[p])
        p++;
    while (name[n])
        n++;
    /* one '\' between them, unless the parent already ends in one */
    if (parent[p - 1] == '\\')
        p--;
    if (p + 1 + n + 1 > cap)
        return 0;
    for (i = 0; i < p; i++)
        out[i] = parent[i];
    out[p] = '\\';
    for (i = 0; i < n; i++)
        out[p + 1 + i] = name[i];
    out[p + 1 + n] = 0;
    return 1;
}

#endif /* RETRO_SHORTCUTDIR_H */
