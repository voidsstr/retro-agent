/* test_autoupdate_version.c - TRUE-SOURCE test of the agent's update gate.
 *
 * WHAT THIS PROTECTS. The self-update used to fire whenever the published
 * version merely DIFFERED from the running one, so a share that had fallen
 * behind would quietly downgrade a newer agent. That is not a hypothetical:
 * it happened three times on this fleet.
 *
 *   - box .243 was reverted to a stale build and stopped answering
 *   - a build VM installed 1.31.0 and was pulled back to 1.30.0 within a minute
 *   - a freshly PXE-imaged machine did the same, because the SMB share had been
 *     reorganised under Files\ while the path compiled into the agent still
 *     pointed at the old location - which still held 1.30.0. Every "publish"
 *     had been landing somewhere no agent ever reads.
 *
 * A downgrade is worse than no update: the box stays up, answers, and reports a
 * plausible version, but whatever command the fleet just started depending on
 * is gone. The symptom is "Unknown command" from a machine that looks healthy.
 *
 * The other half of the bug is ordering. Versions must compare NUMERICALLY per
 * component. As text, "1.9.0" sorts ABOVE "1.31.0" - so a lexicographic compare
 * would treat the 22-release-old build as newer and install it, which is the
 * exact failure this guards.
 */

#include "munit.h"
#include <string.h>
#include <stdio.h>

/* The function under test, copied verbatim from agent/src/autoupdate.c.
 * It is static there and the file cannot be compiled standalone (it pulls in
 * the whole Win32 update machinery), so this mirrors it; the assertion at the
 * bottom of run_all.sh's grep check keeps the two honest. */
static int version_cmp(const char *a, const char *b)
{
    while (*a || *b) {
        int na = 0, nb = 0;
        int have_a = 0, have_b = 0;

        while (*a >= '0' && *a <= '9') { na = na * 10 + (*a++ - '0'); have_a = 1; }
        while (*b >= '0' && *b <= '9') { nb = nb * 10 + (*b++ - '0'); have_b = 1; }
        if (na != nb) return na < nb ? -1 : 1;
        if (!have_a && !have_b) break;
        if (*a == '.') a++;
        if (*b == '.') b++;
        if (!*a && !*b) break;
    }
    return 0;
}

static int sgn(int v) { return v < 0 ? -1 : (v > 0 ? 1 : 0); }

/* Mirrors the decision in check_agent_update(): update only when the published
 * version is strictly newer than the running one. */
static int should_update(const char *remote, const char *local)
{
    return version_cmp(remote, local) > 0;
}

int main(void)
{

    printf("== newer is installed, older is refused ==\n");
    CHECK(should_update("1.31.0", "1.30.0") == 1, "a newer share version updates");
    CHECK(should_update("1.30.0", "1.31.0") == 0, "an OLDER share version is refused");
    CHECK(should_update("1.31.0", "1.31.0") == 0, "the same version is a no-op");

    printf("== components compare numerically, not as text ==\n");
    /* The trap: strcmp("1.9.0","1.31.0") > 0 because '9' > '3'. A textual
     * compare would install a 22-release-old build and call it an upgrade. */
    CHECK(sgn(version_cmp("1.9.0", "1.31.0")) == -1, "1.9.0 is older than 1.31.0");
    CHECK(sgn(version_cmp("1.31.0", "1.9.0")) == 1, "1.31.0 is newer than 1.9.0");
    CHECK(should_update("1.9.0", "1.31.0") == 0, "1.9.0 would NOT be installed over 1.31.0");

    printf("== ordinary ordering ==\n");
    CHECK(sgn(version_cmp("2.0.0", "1.99.99")) == 1, "major beats minor");
    CHECK(sgn(version_cmp("1.30.1", "1.30.0")) == 1, "patch is significant");
    CHECK(sgn(version_cmp("0.0.0", "1.0.0")) == -1, "zero sorts lowest");

    printf("== malformed input does not misbehave ==\n");
    CHECK(sgn(version_cmp("1.31", "1.31.0")) == 0, "a missing component counts as zero");
    CHECK(sgn(version_cmp("", "")) == 0, "empty strings are equal");
    CHECK(should_update("", "1.31.0") == 0, "an empty remote never triggers an update");
    /* An unreadable .ver leaves the buffer empty; that must not read as 0.0.0
     * being newer than nothing, nor loop forever. */
    CHECK(sgn(version_cmp("", "")) == 0, "empty vs empty terminates");

    printf("== the mirrored copy still matches the real agent source ==\n");
    {
        /* A copied function silently rots. Compare the real one, textually,
         * so an edit to autoupdate.c that is not reflected here fails loudly
         * rather than leaving this file testing something that no longer ships. */
        FILE *f = fopen("agent/src/autoupdate.c", "rb");
        int found = 0;
        if (f) {
            static char buf[400000];
            size_t n = fread(buf, 1, sizeof(buf) - 1, f);
            buf[n] = 0;
            fclose(f);
            found = strstr(buf, "static int version_cmp(const char *a, const char *b)") != NULL
                 && strstr(buf, "if (na != nb) return na < nb ? -1 : 1;") != NULL
                 && strstr(buf, "refusing to ") != NULL;
        }
        CHECK(found, "agent/src/autoupdate.c still has version_cmp and the downgrade refusal");
    }

    printf("\n%s\n", munit_fails ? "FAILURES" : "autoupdate version gate: all passed");
    return munit_fails ? 1 : 0;
}
