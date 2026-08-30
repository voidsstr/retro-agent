/*
 * hwpub.h - the pure logic behind the fleet inventory publish.
 *
 * The agent writes its own HWPROFILE JSON to
 *   \\192.168.1.122\files\Utility\Retro Automation\fleet-inventory\<host>.json
 * on every startup, so the fleet documentation is measured rather than
 * remembered.  Two small decisions in that job are easy to get subtly wrong and
 * expensive to get wrong, so they live here - header-only and free of Win32, so
 * the code the agent runs is the code tests/native/test_hwpublish.c compiles:
 *
 *   hwpub_safe_name()      the hostname -> filename mapping
 *   hwpub_retry_delay_sec() the bounded retry schedule
 *
 * WHY A SAFE NAME. The destination filename is built from GetComputerNameA,
 * and a NetBIOS name is not a filename.  It is uppercase-ASCII in practice but
 * the API does not promise it, and the value is ultimately whatever somebody
 * typed into the System control panel - which on this fleet has already
 * included a name with a space in it.  Pasted straight into a path, a name
 * containing '\', '/', ':' or ".." does not fail: it writes SOMEWHERE ELSE on
 * the share, silently, under the credentials the box has.  One host would
 * overwrite another host's record, or a file outside the inventory directory
 * entirely.  A record that lands in the wrong place is worse than no record,
 * because the renderer then reports a machine as "never seen" while its data
 * sits somewhere nobody reads.
 *
 * WHY THE RETRY IS BOUNDED. The publish runs at startup, and at startup the
 * share is frequently not reachable yet - the box may still be bringing up its
 * NIC, or the file server may be off.  So it has to retry.  But a cosmetic
 * feature must never cost a box its agent (this project already killed one by
 * copying an 11 MB payload at startup on a 31 MB Pentium 1), and an unbounded
 * retry loop against an absent server is exactly the shape of thing that eats
 * a single-core box forever while reporting nothing.  The schedule is
 * therefore finite and says so: after the last attempt it returns 0 and the
 * thread goes to its long idle refresh, which is a no-op when the share is
 * still gone.
 */
#ifndef RETRO_HWPUB_H
#define RETRO_HWPUB_H

#include <string.h>

/* Where each box drops its own record. One file per host - never a shared file
 * two agents append to, because eight boxes publish concurrently and SMB gives
 * us no lock worth trusting. */
#define HWPUB_DIR_DEFAULT \
    "\\\\192.168.1.122\\files\\Utility\\Retro Automation\\fleet-inventory"

/* Hard cap on the record we will write. The JSON is a couple of KB; anything
 * near this means something has gone wrong upstream and we would rather write
 * nothing than stream garbage onto the share. */
#define HWPUB_MAX_BYTES 65536

/* Attempts at startup before falling back to the slow refresh. */
#define HWPUB_MAX_ATTEMPTS 4

/*
 * Map a computer name onto a leaf filename, into out[0..outsz-1].
 *
 * Keeps [A-Za-z0-9._-] and turns everything else into '_'.  A name that is
 * empty, or that reduces to only dots (".", ".."), becomes "unknown" - the
 * dots case matters because "..json" is harmless but a bare ".." with the
 * extension appended elsewhere is not, and because a file called "." cannot be
 * created at all on some servers, which would look like an unreachable share.
 *
 * Returns 1 when the name passed through unchanged, 0 when it had to be
 * rewritten - the caller logs that, so a box whose name needed mangling says
 * so once rather than being quietly filed under a name nobody recognises.
 */
static int hwpub_safe_name(const char *host, char *out, int outsz)
{
    int i = 0, changed = 0, dots_only = 1;
    const char *p;

    if (!out || outsz < 2)
        return 0;
    out[0] = 0;
    if (!host)
        host = "";

    for (p = host; *p && i < outsz - 1; p++) {
        unsigned char c = (unsigned char)*p;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-') {
            out[i++] = (char)c;
            if (c != '.')
                dots_only = 0;
        } else {
            out[i++] = '_';
            dots_only = 0;
            changed = 1;
        }
    }
    out[i] = 0;
    if (*p)                       /* truncated */
        changed = 1;
    if (i == 0 || dots_only) {
        strncpy(out, "unknown", (size_t)(outsz - 1));
        out[outsz - 1] = 0;
        changed = 1;
    }
    return changed ? 0 : 1;
}

/*
 * Seconds to wait before startup attempt number `attempt` (0-based), or 0 when
 * the schedule is exhausted and the caller must stop retrying.
 *
 * 0 -> 90s   let autoupdate / retrowall / dosstage have the boot window first;
 *            on the slowest box those are still streaming files at T+45s.
 * 1 -> 120s  the NIC and any mapped drive have settled by now.
 * 2 -> 300s
 * 3 -> 900s  last chance, then give up until the periodic refresh.
 */
static int hwpub_retry_delay_sec(int attempt)
{
    static const int schedule[HWPUB_MAX_ATTEMPTS] = { 90, 120, 300, 900 };
    if (attempt < 0 || attempt >= HWPUB_MAX_ATTEMPTS)
        return 0;
    return schedule[attempt];
}

#endif /* RETRO_HWPUB_H */
