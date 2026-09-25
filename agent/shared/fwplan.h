/*
 * fwplan.h - should the agent run netsh to open the Windows Firewall for
 *            itself, and has that already been done?
 *
 * WHAT IT USED TO DO (agent <= 1.84.x), on every start, BEFORE listening:
 *   spawn `netsh firewall add allowedprogram ...` and wait up to 5 s, then
 *   spawn `netsh advfirewall firewall add rule ...` and wait up to 5 s, and log
 *   "Firewall exception added" after each if the process merely STARTED.
 * So every boot paid two process creations of a tool that is slow to start on a
 * PIII (up to 10 s during which the agent was not yet listening), on Win9x it
 * tried to start a netsh that does not exist, on XP the advfirewall context
 * does not exist so that half always failed - and the log said "added" for
 * both, every time, regardless.
 *
 * NOW: Win9x (no Windows Firewall) and Windows 2000 (none either) skip it;
 * the exception is looked up in the registry first and netsh runs only when it
 * is missing; `advfirewall` is only tried on Vista and later (major >= 6); the
 * work runs on a background thread after the listener is up; and the log
 * reports netsh's exit code and whether the exception is present afterwards.
 *
 * Header-only and Win32-free, so tests/native/test_fwplan.c compiles the code
 * the agent runs.
 */
#ifndef RETRO_FWPLAN_H
#define RETRO_FWPLAN_H

#ifdef __GNUC__
#define FWPLAN_UNUSED __attribute__((unused))
#else
#define FWPLAN_UNUSED
#endif

#define FWP_NETSH_FIREWALL  1   /* netsh firewall add allowedprogram (XP SP2+) */
#define FWP_NETSH_ADV       2   /* netsh advfirewall firewall add rule (Vista+) */

/* Which netsh commands to run. `already_allowed` is the registry lookup. */
FWPLAN_UNUSED static int fw_plan(int is_nt, unsigned os_major,
                                 unsigned os_minor, int already_allowed)
{
    if (!is_nt)
        return 0;                       /* Win9x: there is no Windows Firewall */
    if (os_major < 5 || (os_major == 5 && os_minor == 0))
        return 0;                       /* NT4 / Windows 2000: none either */
    if (already_allowed)
        return 0;
    return FWP_NETSH_FIREWALL | (os_major >= 6 ? FWP_NETSH_ADV : 0);
}

/* ASCII case-insensitive compare of the first n bytes (Windows paths). */
FWPLAN_UNUSED static int fw_nieq(const char *a, const char *b, unsigned n)
{
    unsigned i;
    for (i = 0; i < n; i++) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x = (char)(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = (char)(y - 'A' + 'a');
        if (x != y)
            return 0;
        if (!x)
            return 1;
    }
    return 1;
}

FWPLAN_UNUSED static unsigned fw_len(const char *s)
{
    unsigned n = 0;
    while (s && s[n])
        n++;
    return n;
}

/* Case-insensitive substring search. */
FWPLAN_UNUSED static int fw_contains(const char *hay, const char *needle)
{
    unsigned n = fw_len(needle), h = fw_len(hay), i;
    if (!n)
        return 1;
    for (i = 0; i + n <= h; i++)
        if (fw_nieq(hay + i, needle, n))
            return 1;
    return 0;
}

/*
 * XP's list: HKLM\SYSTEM\CurrentControlSet\Services\SharedAccess\Parameters\
 * FirewallPolicy\StandardProfile\AuthorizedApplications\List holds one value
 * per program, NAMED by its path, with data
 *     "<path>:<scope>:<Enabled|Disabled>:<description>"
 * e.g. "C:\RETRO_AGENT\retro_agent.exe:*:Enabled:Retro Agent". The path itself
 * contains a colon (the drive letter), so match the path as a prefix and parse
 * what follows it. Only an ENABLED entry for THIS exe counts.
 */
FWPLAN_UNUSED static int fw_list_entry_enabled(const char *data, const char *exe)
{
    unsigned n = fw_len(exe);
    const char *p;
    if (!data || !exe || !n || !fw_nieq(data, exe, n))
        return 0;
    p = data + n;
    if (*p != ':')
        return 0;                      /* a longer path that merely starts alike */
    p++;
    while (*p && *p != ':')
        p++;                           /* the scope: "*", "LocalSubNet", ... */
    if (*p != ':')
        return 0;
    p++;
    return fw_nieq(p, "Enabled:", 8);
}

/*
 * Vista+'s store: ...\FirewallPolicy\FirewallRules holds one value per rule,
 * data like
 *     "v2.10|Action=Allow|Active=TRUE|Dir=In|Protocol=6|LPort=9898|
 *      App=C:\RETRO_AGENT\retro_agent.exe|Name=Retro Agent|"
 * An inbound, active, allow rule naming THIS exe is what netsh would add.
 */
FWPLAN_UNUSED static int fw_rule_allows(const char *rule, const char *exe)
{
    char app[300];
    unsigned n = fw_len(exe), i;
    if (!rule || !exe || !n || n + 7 >= sizeof(app))
        return 0;
    app[0] = '|'; app[1] = 'A'; app[2] = 'p'; app[3] = 'p'; app[4] = '=';
    for (i = 0; i < n; i++)
        app[5 + i] = exe[i];
    app[5 + n] = '|';
    app[6 + n] = 0;
    return fw_contains(rule, "|Action=Allow|") &&
           fw_contains(rule, "|Active=TRUE|") &&
           fw_contains(rule, "|Dir=In|") &&
           fw_contains(rule, app);
}

#endif /* RETRO_FWPLAN_H */
