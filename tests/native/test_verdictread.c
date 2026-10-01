/*
 * test_verdictread.c - agent/shared/verdictread.h (agent 1.97.1).
 *
 * Until 1.97.1 gs_gate_init() read the published gate verdicts with gs_slurp(),
 * whose NULL meant "absent" AND "the share could not be read". Measured on the
 * W98BUILD VM 2026-10-01: SMB error 53 at the start of a run, the run fell back
 * to the local rules (which cannot see an operator override) and planned
 * 18,104 MB - every title ejected from that profile included.
 */
#include <stdio.h>
#include "../../agent/shared/verdictread.h"

static int fails;
#define CHECK(c, m) do { if (!(c)) { printf("  [FAIL] %s\n", m); fails++; } else printf("  [ ok ] %s\n", m); } while (0)

int main(void)
{
    const unsigned long cap = 256u * 1024u;

    CHECK(vr_outcome(2, 0, 0, cap) == VR_ABSENT, "file not found = not published (fail-open, as designed)");
    CHECK(vr_outcome(3, 0, 0, cap) == VR_ABSENT, "path not found = not published");
    /* the 1.97.1 fix: network errors are NOT "not published" (old: they were) */
    CHECK(vr_outcome(53, 0, 0, cap) == VR_UNREADABLE, "error 53 (network path not found) = unreadable");
    CHECK(vr_outcome(55, 0, 0, cap) == VR_UNREADABLE, "error 55 (resource no longer available) = unreadable");
    CHECK(vr_outcome(64, 0, 0, cap) == VR_UNREADABLE, "error 64 (network name deleted) = unreadable");
    CHECK(vr_outcome(5, 0, 0, cap) == VR_UNREADABLE, "access denied = unreadable, never absent");
    CHECK(vr_outcome(0, 0, 1, cap) == VR_UNREADABLE, "an empty file (mid-replace) = unreadable");
    CHECK(vr_outcome(0, 1200, 0, cap) == VR_UNREADABLE, "a read that failed = unreadable");
    CHECK(vr_outcome(0, cap + 1, 1, cap) == VR_ABSENT, "an oversize file is a mistake, not an outage");
    CHECK(vr_outcome(0, 13551, 1, cap) == VR_LOADED, "a normal file loads");
    CHECK(VR_TRIES >= 3, "it retries before refusing");
    printf("-- verdictread (unreadable gate verdicts never read as 'not published', agent 1.97.1): %s --\n",
           fails ? "FAILED" : "all passed");
    return fails != 0;
}
