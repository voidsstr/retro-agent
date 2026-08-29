/*
 * drvprefs.h - the pure logic behind C:\D\PREFER.TXT, the list of drivers the
 * agent must FORCE over the one Windows chose for itself.
 *
 * Header-only and free of Win32 so the same code the agent runs is the code the
 * regression test compiles - see tests/native/test_driver_prefs.c. Everything
 * that needs SetupAPI stays in agent/src/gamesync.c; what lives here is the
 * three decisions that are easy to get subtly wrong:
 *
 *   drvpref_split()   parse one line of PREFER.TXT
 *   drvpref_present() does this machine actually have that hardware
 *   drvpref_blocks()  may the 2.4 GB driver payload be reclaimed yet
 *
 * WHY THIS EXISTS AT ALL. A driver shipped in $OEM$ and pointed at by
 * DevicePath is still not a driver that gets installed: winnt.sif carries only
 * the short LAN+chipset path, DevicePath is written at T-12 (after GUI setup
 * has already installed the devices), and even a visible unsigned INF is
 * penalised +0x8000 in XP's driver rank and loses to any trusted in-box match.
 * The device then reports problem code 0 - it looks perfect. The only way past
 * that is an explicit forced install from an explicit list.
 */
#ifndef RETRO_DRVPREFS_H
#define RETRO_DRVPREFS_H

#include <string.h>

/*
 * Split one PREFER.TXT line, in place, into "<hardware id>\t<INF path>".
 * Comments start with ';' or '#'. Returns 1 when the line yields both halves.
 *
 * The separator is a TAB and nothing else. Windows driver paths and hardware
 * ids both contain backslashes and neither contains a tab, but an INF path
 * absolutely can contain SPACES ("C:\D\V001\Amigamerlin 3.1 R1\..."), so
 * splitting on whitespace would truncate the path at the first space and leave
 * a file that does not exist - which the caller can only report as "the staged
 * tree is broken".
 */
static int drvpref_split(char *line, char **hwid, char **inf)
{
    char *tab, *cr;

    if (!line || !hwid || !inf)
        return 0;
    cr = strchr(line, '\r');
    if (cr)
        *cr = 0;
    if (*line == ';' || *line == '#' || *line == 0)
        return 0;
    tab = strchr(line, '\t');
    if (!tab)
        return 0;
    *tab = 0;
    *hwid = line;
    *inf  = tab + 1;
    return (**hwid && **inf) ? 1 : 0;
}

/*
 * Does a device matching this hardware id exist on the machine?
 *
 * idbuf holds every hardware AND compatible id of every present device,
 * upper-cased, one per line, with a leading and a trailing newline. A
 * preference names a device id WITHOUT the subsystem and revision suffixes
 * Windows appends, so the test is "some line STARTS WITH this id":
 *
 *   pref  PCI\VEN_10DE&DEV_0150
 *   line  PCI\VEN_10DE&DEV_0150&SUBSYS_002E10DE&REV_A4     -> match
 *
 * The line anchor is not decoration. A bare strstr() would also match an id
 * that merely CONTAINS the string - so a preference written as
 * "CTL00E4_DEV0000" (forgetting the ISAPNP\ enumerator prefix, an easy thing to
 * do) would appear to work here and then fail everywhere the enumerator differs.
 * A rule that matches by accident is worse than one that does not match at all,
 * because it is the accident that gets copied to the next entry.
 */
static int drvpref_present(const char *idbuf, const char *hwid)
{
    const char *p;
    size_t      n;

    if (!idbuf || !hwid || !*hwid)
        return 0;
    n = strlen(hwid);
    for (p = idbuf; (p = strstr(p, hwid)) != NULL; p += n) {
        if (p > idbuf && p[-1] == '\n')
            return 1;
    }
    return 0;
}

/*
 * May the staged driver payload be deleted, as far as THIS preference is
 * concerned? Non-zero means "keep C:\D".
 *
 * The bug this replaces: the reclaim used to fire whenever no device carried a
 * problem code, and a device on the wrong-but-working driver carries no problem
 * code. On .124 that deleted 2.4 GB of NVIDIA drivers off a machine whose
 * GeForce2 GTS was sitting on Microsoft's in-box nv4 - leaving it with neither
 * the right driver nor the payload to fix itself.
 *
 * Three rules, and the middle one is what keeps the reclaim useful:
 *   - hardware we do not have never blocks. Otherwise a list written for the
 *     fleet would hold 2.4 GB on the 6 GB Gateway forever.
 *   - hardware we DO have, not yet on our driver, blocks.
 *   - so does a preference whose INF is missing: better a full disk than a
 *     silent half-install nobody can retry.
 */
static int drvpref_blocks(int present, int satisfied, int inf_exists)
{
    if (!present)
        return 0;
    if (!inf_exists)
        return 1;
    return satisfied ? 0 : 1;
}

#endif /* RETRO_DRVPREFS_H */
