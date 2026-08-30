/* test_profile_hash_pin.c - the gate's cache key, PINNED to real fleet values.
 *
 * WHY THIS EXISTS ALONGSIDE test_gamegate.c's hash test. That one asserts the
 * RELATIVE properties: same machine twice hashes the same, two different
 * machines hash differently. Both are necessary and neither catches the
 * failure that nearly happened here.
 *
 * A change that moves EVERY hash UNIFORMLY passes every relative assertion.
 * Add a field to the fold, reorder the twelve, change a bucket size, alter the
 * FNV seed - same-machine stability still holds, different machines still
 * differ, and the existing test stays green while all eight hashes change at
 * once.
 *
 * WHAT THAT COSTS, and it is not a cache miss. The host publishes each box's
 * verdicts to <library>\_gamegate\<profile_hash>.txt - the hash IS THE
 * FILENAME. Move it and all eight boxes stop finding their file simultaneously.
 * The verdict file is what carries the LLM adjudications in the marginal band,
 * the decisions a Pentium III cannot derive for itself, so every one of them
 * silently falls back to the rule the model was called in to overrule. Nothing
 * errors. Every box still deploys games. The artefact looks fine.
 *
 * THE NEAR MISS. Building the fleet inventory (agent 1.74.0) I refactored
 * hwprofile.c - split handle_hwprofile() into a reusable builder, added
 * reported_at, appended video_cards/accelerators/network - and pushed it
 * reasoning that gg_profile_hash() folds only gg_profile_t fields and I had
 * added none. That reasoning was correct and it was still the wrong process:
 * "I did not intend to change it" is not "eight files still resolve", and the
 * check that established it was somebody else's, after the push. This test is
 * so the next person does not need to be lucky or to be checked.
 *
 * THE PINNED VALUES ARE NOT SELF-GENERATED. Each expected hash below is the
 * `profile_hash` that box's OWN agent computed and published to
 * \\192.168.1.122\files\Utility\Retro Automation\fleet-inventory\<host>.json on
 * 2026-08-30 (agent 1.77.1), read back off the share. The inputs are the
 * hashed fields from that same record. So this asserts that the C in this repo
 * reproduces what eight real machines independently computed - not that the
 * code agrees with a snapshot of itself. Seven of these are live filenames in
 * _gamegate/ right now.
 *
 * IF THIS TEST FAILS you have changed the cache key. That may be correct and
 * deliberate - but it is never only a code change: every published verdict
 * file must be regenerated under the new hashes in the same breath, or the
 * fleet quietly loses its LLM verdicts. Update these constants only WITH that
 * republish, never to make the test go green.
 */

#include "munit.h"
#include <string.h>

#include "../../agent/shared/gamegate.h"

typedef struct {
    const char *expect;          /* what the box itself published */
    const char *ip, *host;
    const char *vendor;
    unsigned family, model, stepping, mhz, count;
    unsigned features, ram_mb;
    unsigned gpu_ven, gpu_dev, vram_mb;
    unsigned os_major, os_minor;
    unsigned panel_w, panel_h;
} pinned_t;

/* The fleet, measured 2026-08-30 by agent 1.77.1. */
static const pinned_t g_fleet[] = {
    /* 192.168.1.143  1GHZ
     * AMD Athlon(tm) Processor, 511 MB, NVIDIA GeForce 6800, panel 1024x768 */
    { "ee52d7b186da86b0", "192.168.1.143", "1GHZ",
      "AuthenticAMD", 6, 2, 2, 1000, 1,
      263, 511, 0x10DE, 0x0041, 128, 5, 1,
      1024, 768 },
    /* 192.168.1.246  ADMIN-PC
     * Intel(R) Core(TM) i5-2400 CPU @ 3.10GHz, 3317 MB, AMD Radeon HD 5450, panel 1920x1080 */
    { "7cf5bd34eefa68a7", "192.168.1.246", "ADMIN-PC",
      "GenuineIntel", 6, 42, 7, 3093, 4,
      255, 3317, 0x1002, 0x68F9, 512, 6, 1,
      1920, 1080 },
    /* 192.168.1.145  DELL
     * Intel(R) Core(TM) i5-2400 CPU @ 3.10GHz, 3316 MB, NVIDIA GeForce 8400GS, panel 1920x1080 */
    { "bb7a13d03d67b0b4", "192.168.1.145", "DELL",
      "GenuineIntel", 6, 42, 7, 3093, 4,
      255, 3316, 0x10DE, 0x10C3, 512, 5, 1,
      1920, 1080 },
    /* 192.168.1.171  NSC-5B996B81319
     * Intel(R) Pentium(R) 4 CPU 2.80GHz, 509 MB, Intel(R) 82865G Graphics Controller, panel 1280x1024 */
    { "efb240b3f32bb482", "192.168.1.171", "NSC-5B996B81319",
      "GenuineIntel", 15, 4, 1, 2793, 1,
      63, 509, 0x8086, 0x2572, 96, 5, 1,
      1280, 1024 },
    /* 192.168.1.123  NSC-B20C188E96D
     * AMD Athlon(tm) 64 Processor 4000+, 2047 MB, ATI Radeon HD 3850 AGP, panel 1920x1080 */
    { "e633518d4747e4f2", "192.168.1.123", "NSC-B20C188E96D",
      "AuthenticAMD", 15, 39, 1, 2403, 1,
      319, 2047, 0x1002, 0x9515, 512, 5, 1,
      1920, 1080 },
    /* 192.168.1.124  NSC-CABE14B7486
     * GenuineIntel, 511 MB, NVIDIA GeForce2 GTS/GeForce2 Pro, panel 1024x768 */
    { "881d549a01caf0cb", "192.168.1.124", "NSC-CABE14B7486",
      "GenuineIntel", 6, 8, 3, 845, 1,
      15, 511, 0x10DE, 0x0150, 32, 5, 1,
      1024, 768 },
    /* 192.168.1.133  P3-DUAL
     * GenuineIntel, 255 MB, NVIDIA GeForce4 Ti 4600, panel 1280x1024 */
    { "b65fa1fee4df292c", "192.168.1.133", "P3-DUAL",
      "GenuineIntel", 6, 8, 1, 701, 2,
      15, 255, 0x10DE, 0x0250, 128, 5, 1,
      1280, 1024 },
    /* 192.168.1.240  USER-41EA3B3330
     * AMD Athlon(tm) 64 Processor 3300+, 1534 MB, RADEON 9800 XT, panel 1920x1080 */
    { "bb40bf867d4782d3", "192.168.1.240", "USER-41EA3B3330",
      "AuthenticAMD", 15, 12, 0, 2403, 1,
      287, 1534, 0x1002, 0x4E4A, 256, 5, 1,
      1920, 1080 },};

#define NFLEET ((int)(sizeof(g_fleet) / sizeof(g_fleet[0])))

static gg_profile_t profile_of(const pinned_t *r)
{
    gg_profile_t p;
    memset(&p, 0, sizeof(p));
    /* Only the hashed fields are set. Everything else stays zero on purpose:
     * if a field that is currently NOT folded ever starts being folded, these
     * hashes move and this test says so - which is the point. */
    strncpy(p.cpu_vendor, r->vendor, sizeof(p.cpu_vendor) - 1);
    p.cpu_family   = r->family;
    p.cpu_model    = r->model;
    p.cpu_stepping = r->stepping;
    p.cpu_mhz      = r->mhz;
    p.cpu_count    = r->count;
    p.cpu_features = r->features;
    p.ram_mb       = r->ram_mb;
    p.gpu_ven      = r->gpu_ven;
    p.gpu_dev      = r->gpu_dev;
    p.vram_mb      = r->vram_mb;
    p.os_major     = r->os_major;
    p.os_minor     = r->os_minor;
    p.panel_w      = r->panel_w;
    p.panel_h      = r->panel_h;
    return p;
}

TEST(every_fleet_box_still_hashes_to_its_published_filename)
{
    int i;
    for (i = 0; i < NFLEET; i++) {
        gg_profile_t p = profile_of(&g_fleet[i]);
        char got[17];
        gg_profile_hash(&p, got);
        if (strcmp(got, g_fleet[i].expect) != 0) {
            munit_fails++;
            fprintf(stderr,
                "    FAIL %s:%d: %s (%s) hashes to %s, but published %s\n"
                "         The cache key moved. _gamegate/%s.txt is now\n"
                "         unreachable for that box, and its LLM verdicts with\n"
                "         it. Republish every verdict file before touching\n"
                "         these constants.\n",
                __FILE__, __LINE__, g_fleet[i].host, g_fleet[i].ip,
                got, g_fleet[i].expect, g_fleet[i].expect);
        }
    }
}

TEST(the_eight_boxes_are_eight_distinct_keys)
{
    /* If two machines collided they would share a verdict file and one would
     * be served the other's decisions. Cheap to assert, catastrophic to miss,
     * and it also catches a copy-paste slip in the table above. */
    int i, j;
    for (i = 0; i < NFLEET; i++)
        for (j = i + 1; j < NFLEET; j++)
            CHECK(strcmp(g_fleet[i].expect, g_fleet[j].expect) != 0,
                  "two fleet boxes must not share a profile hash");
}

TEST(a_hash_is_sixteen_lowercase_hex_because_it_is_a_filename)
{
    /* It is used verbatim as a filename on an SMB share. Anything outside
     * [0-9a-f] would be a path problem rather than a hash problem, and the
     * failure would look like a missing verdict file. */
    int i, k;
    for (i = 0; i < NFLEET; i++) {
        gg_profile_t p = profile_of(&g_fleet[i]);
        char got[17];
        gg_profile_hash(&p, got);
        CHECK_EQ_U(strlen(got), 16);
        for (k = 0; k < 16; k++)
            CHECK((got[k] >= '0' && got[k] <= '9') ||
                  (got[k] >= 'a' && got[k] <= 'f'),
                  "a profile hash must be lowercase hex - it is a filename");
    }
}

TEST(adding_an_unfolded_field_must_not_move_a_pinned_hash)
{
    /* The complement of the pin. gg_profile_t carries fields that are
     * deliberately NOT hashed - free_mb above all, because it changes whenever
     * anything is written and would mint a new profile on every poll. Setting
     * them must leave every pinned value untouched.
     *
     * This is the exact guard my hwprofile.c refactor needed: it is what says
     * "you added something to the profile, and the cache key did not move". */
    int i;
    for (i = 0; i < NFLEET; i++) {
        gg_profile_t p = profile_of(&g_fleet[i]);
        char got[17];
        p.free_mb   = 12345;         /* changes constantly */
        p.gpu_level = GG_GPU_SM2;    /* derived from ven/dev, already folded */
        p.dx_major  = 9;             /* derived from os_level */
        p.caps      = 1;             /* software state, fixable, not identity */
        gg_profile_hash(&p, got);
        CHECK(strcmp(got, g_fleet[i].expect) == 0,
              "an unfolded field must never move the cache key");
    }
}

MUNIT_MAIN("gate cache key pinned to the real fleet (agent/shared/gamegate.h)",
    RUN(every_fleet_box_still_hashes_to_its_published_filename);
    RUN(the_eight_boxes_are_eight_distinct_keys);
    RUN(a_hash_is_sixteen_lowercase_hex_because_it_is_a_filename);
    RUN(adding_an_unfolded_field_must_not_move_a_pinned_hash);
)
