"""The C gate and the Python gate must give the SAME answer, always.

Two implementations of the capability gate exist, and both are load-bearing:

* the AGENT's, in C (`agent/shared/gamegate.h`), because a freshly PXE-imaged
  box runs GAMESYNC before any host tool has ever seen it and must still gate
  itself correctly;
* the HOST's, in Python (`scripts/gamegate/rules.py`), because planning the
  whole fleet must not require waking eight machines.

A mirrored implementation is worth nothing the moment it drifts, and the drift
would be SILENT: the host would publish a verdict file the agent disagrees with,
or a box with no published file would gate itself differently from the plan
someone just read off the screen. Worse, it would look like coverage.

So this does not compare comments or grep for constants. It COMPILES the C
header into a small driver that dumps its answers, and compares them against the
Python ones, case by case:

  * every row of the GPU table, plus the id either side of every boundary
    (which is where an off-by-one lives), plus a sweep of uncatalogued ids;
  * the verdict, limiting factor and marginal-band edges for a grid of real
    fleet profiles against real requirement shapes;
  * the profile hash for each fleet machine, so the cache key itself is pinned.

If the C compiler is missing this test FAILS rather than skips. The two copies
being unverified is exactly the state it exists to prevent, and a green skip
would read as "they match".
"""

import json
import os
import shutil
import subprocess
import sys
import tempfile

import pytest

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(REPO, "scripts"))

from gamegate import rules  # noqa: E402

HEADER = os.path.join(REPO, "agent", "shared", "gamegate.h")


def _cc():
    for cand in (os.environ.get("CC"), "gcc", "cc", "clang"):
        if cand and shutil.which(cand):
            return cand
    return None


DRIVER = r"""
#include <stdio.h>
#include <string.h>
#include "gamegate.h"

/* Dump the C side's answers as one JSON object, so the Python side compares
 * values rather than prose. */
int main(int argc, char **argv)
{
    (void)argc;
    if (!strcmp(argv[1], "gpu")) {
        unsigned ven, dev;
        while (scanf("%x %x", &ven, &dev) == 2)
            printf("%d\n", gg_gpu_level_from_pci(ven, dev));
        return 0;
    }
    if (!strcmp(argv[1], "decide")) {
        /* one case per line:
         * mhz ram vram gpulevel oslevel features caps free <tab> json */
        char line[4096];
        while (fgets(line, sizeof(line), stdin)) {
            gg_profile_t p;
            gg_req_t r;
            gg_decision_t d;
            char *tab;
            unsigned mhz, ram, vram, feats, caps, freemb;
            int gl, ol;

            tab = strchr(line, '\t');
            if (!tab) continue;
            *tab = 0;
            if (sscanf(line, "%u %u %u %d %d %u %u %u",
                       &mhz, &ram, &vram, &gl, &ol, &feats, &caps,
                       &freemb) != 8)
                continue;
            memset(&p, 0, sizeof(p));
            p.cpu_mhz = mhz; p.ram_mb = ram; p.vram_mb = vram;
            p.gpu_level = gl; p.os_level = ol;
            p.cpu_features = feats; p.caps = caps; p.free_mb = freemb;
            {
                char *nl = strchr(tab + 1, '\n');
                if (nl) *nl = 0;
            }
            gg_req_parse(tab + 1, &r);
            gg_decide(&p, &r, &d);
            printf("%d|%s|%u\n", d.verdict, d.limiting, d.missing_caps);
        }
        return 0;
    }
    if (!strcmp(argv[1], "hash")) {
        /* vendor family model stepping mhz count features ram ven dev vram
         * osmajor osminor panelw panelh
         *
         * The whole struct is zeroed each pass. It used to be only partly
         * filled, which was harmless while every hashed field was assigned -
         * but the moment a NEW field entered the hash, the unassigned bytes
         * were stack garbage and the same machine hashed differently run to
         * run. Zero it, don't chase which members the hash happens to read. */
        gg_profile_t p;
        char vendor[32];
        char out[17];
        while (memset(&p, 0, sizeof(p)),
               scanf("%31s %u %u %u %u %u %u %u %x %x %u %u %u %u %u",
                     vendor, &p.cpu_family, &p.cpu_model, &p.cpu_stepping,
                     &p.cpu_mhz, &p.cpu_count, &p.cpu_features, &p.ram_mb,
                     &p.gpu_ven, &p.gpu_dev, &p.vram_mb,
                     &p.os_major, &p.os_minor,
                     &p.panel_w, &p.panel_h) == 15) {
            strncpy(p.cpu_vendor, vendor, sizeof(p.cpu_vendor) - 1);
            gg_profile_hash(&p, out);
            printf("%s\n", out);
        }
        return 0;
    }
    return 2;
}
"""


@pytest.fixture(scope="module")
def cbin():
    cc = _cc()
    assert cc, ("no host C compiler (tried $CC, gcc, cc, clang). The C and "
                "Python gates would go unverified, which is the state this "
                "test exists to prevent - install a compiler rather than "
                "skipping.")
    d = tempfile.mkdtemp(prefix="gamegate-mirror-")
    src = os.path.join(d, "driver.c")
    with open(src, "w") as fh:
        fh.write(DRIVER)
    exe = os.path.join(d, "driver")
    subprocess.run(
        [cc, "-std=c11", "-O0", "-Wall", "-Wextra", "-Werror",
         "-I", os.path.dirname(HEADER), src, "-o", exe],
        check=True, capture_output=True)
    return exe


def _run(exe, mode, stdin):
    r = subprocess.run([exe, mode], input=stdin, capture_output=True,
                       text=True, check=True)
    return r.stdout.strip().splitlines()


# --------------------------------------------------------------------------


def _gpu_probe_ids():
    """Every table boundary, the id either side of it, and a sweep of ids that
    belong to no row. Boundaries are where an off-by-one lives, and an id that
    matches nothing must come back UNKNOWN from BOTH sides - a copy that
    returned 0 (GPU_NONE) there would reject every shader title on an
    uncatalogued card."""
    ids = set()
    for ven, lo, hi, _lvl in rules.GPU_TABLE:
        for dev in (lo - 1, lo, lo + 1, hi - 1, hi, hi + 1):
            if 0 <= dev <= 0xFFFF:
                ids.add((ven, dev))
    for ven in (0x10DE, 0x1002, 0x8086, 0x121A, 0x1AF4, 0x0000, 0xFFFF):
        for dev in range(0, 0x10000, 0x137):      # coprime-ish sweep
            ids.add((ven, dev))
    return sorted(ids)


def test_gpu_table_matches(cbin):
    ids = _gpu_probe_ids()
    stdin = "".join(f"{v:x} {d:x}\n" for v, d in ids)
    got = _run(cbin, "gpu", stdin)
    assert len(got) == len(ids)
    bad = []
    for (ven, dev), c in zip(ids, got):
        py = rules.gpu_level_from_pci(ven, dev)
        if int(c) != py:
            bad.append(f"{ven:04X}:{dev:04X} C={c} py={py}")
    assert not bad, ("GPU table drift between agent/shared/gamegate.h and "
                     "scripts/gamegate/rules.py:\n  " + "\n  ".join(bad[:20]))


# Real fleet machines, measured 2026-08-30. Keeping the actual numbers means a
# drift shows up as "this box would now be told something different", which is
# the only form of the bug anyone cares about.
FLEET = {
    # name:   (mhz, ram, vram, gpu_level, os_level, features, caps, free_mb)
    # free_mb is the volume games land on (C:), measured the same day. .240 is
    # the one genuinely disk-constrained box and is why the disk floor exists.
    ".124 PIII/GeForce2 GTS": (845, 511, 32, rules.GPU_TNL, rules.OS_WINXP,
                               rules.CPU_FPU | rules.CPU_MMX | rules.CPU_CMOV
                               | rules.CPU_SSE, 0, 95968),
    ".143 Athlon K7/6800":    (1000, 511, 128, rules.GPU_SM3, rules.OS_WINXP,
                               # a K7 Athlon has 3DNow! and NO SSE - a title
                               # that executes SSE is #UD on this box
                               rules.CPU_FPU | rules.CPU_MMX | rules.CPU_CMOV
                               | rules.CPU_3DNOW, rules.CAP_DISC_MOUNT,
                               146367),
    ".133 dual PIII/Ti4600":  (701, 255, 128, rules.GPU_SM1, rules.OS_WINXP,
                               rules.CPU_FPU | rules.CPU_MMX | rules.CPU_CMOV
                               | rules.CPU_SSE, rules.CAP_DISC_MOUNT, 897572),
    ".171 P4/Intel 865G":     (2793, 509, 8, rules.GPU_FIXED, rules.OS_WINXP,
                               rules.CPU_FPU | rules.CPU_MMX | rules.CPU_CMOV
                               | rules.CPU_SSE | rules.CPU_SSE2,
                               rules.CAP_DISC_MOUNT, 51509),
    ".123 Athlon64/HD3850":   (2403, 2047, 512, rules.GPU_SM3, rules.OS_WINXP,
                               rules.CPU_FPU | rules.CPU_MMX | rules.CPU_CMOV
                               | rules.CPU_SSE | rules.CPU_SSE2
                               | rules.CPU_SSE3 | rules.CPU_3DNOW, 0, 204494),
    ".240 Athlon64/9800XT":   (2403, 1534, 256, rules.GPU_SM2, rules.OS_WINXP,
                               rules.CPU_FPU | rules.CPU_MMX | rules.CPU_CMOV
                               | rules.CPU_SSE | rules.CPU_SSE2
                               | rules.CPU_3DNOW, rules.CAP_DISC_MOUNT,
                               15471),
    ".145 i5-2400/8400GS":    (3093, 2047, 512, rules.GPU_SM3, rules.OS_WINXP,
                               0xFF, rules.CAP_DISC_MOUNT, 141095),
    ".246 i5-2400/HD5450":    (3093, 2047, 512, rules.GPU_SM3, rules.OS_WIN7,
                               0xFF, 0, 143400),
    "P1 Deskpro/S3":          (166, 32, 2, rules.GPU_FIXED, rules.OS_WIN9X,
                               rules.CPU_FPU | rules.CPU_MMX, 0, 900),
    # A 2D-ONLY adapter - the level that had no way to be reached until the
    # Deskpro arrived. Kept as its own row because GPU_NONE now takes a
    # DIFFERENT branch from "one level short" and the two must not drift.
    "2D-only VGA / no MMX":   (166, 31, 2, rules.GPU_NONE, rules.OS_WIN9X,
                               rules.CPU_FPU, 0, 900),
    # free space unmeasurable (0) must FAIL OPEN, never refuse.
    "unknown free space":     (3093, 2047, 512, rules.GPU_SM3, rules.OS_WINXP,
                               0xFF, 0, 0),
}

REQS = [
    "{}",
    '{"requirements_version":1}',
    '{"min_cpu_mhz":300}',
    '{"min_cpu_mhz":700}',
    '{"min_cpu_mhz":845}',
    '{"min_cpu_mhz":1000}',
    '{"min_cpu_mhz":1126}',
    '{"min_cpu_mhz":1127}',
    '{"min_cpu_mhz":1500}',
    '{"min_ram_mb":128}',
    '{"min_ram_mb":256}',
    '{"min_ram_mb":384}',
    '{"min_ram_mb":640}',
    '{"min_ram_mb":1024}',
    '{"min_vram_mb":8}',
    '{"min_vram_mb":16}',
    '{"min_vram_mb":32}',
    '{"min_vram_mb":64}',
    '{"gpu_feature_level":"fixed"}',
    '{"gpu_feature_level":"tnl"}',
    '{"gpu_feature_level":"sm1.x"}',
    '{"gpu_feature_level":"sm2.0"}',
    '{"gpu_feature_level":"sm3.0"}',
    '{"disk_mb":500}',
    '{"disk_mb":15471}',
    '{"disk_mb":15472}',
    '{"disk_mb":3700}',
    '{"disk_mb":2000000}',
    '{"min_cpu_mhz":1000,"disk_mb":2000000}',
    '{"cpu_features":["sse"]}',
    '{"cpu_features":["sse2"]}',
    '{"cpu_features":["mmx","sse"]}',
    '{"cpu_features":["3dnow"]}',
    '{"min_os":"win9x"}',
    '{"min_os":"winxp"}',
    '{"min_os":"win7"}',
    '{"max_os":"winxp"}',
    '{"max_os":"win9x"}',
    '{"requires_capabilities":["disc_mount"]}',
    '{"min_cpu_mhz":500,"min_ram_mb":128,"min_vram_mb":32,'
    '"gpu_feature_level":"tnl","requires_capabilities":["disc_mount"]}',
    '{"min_cpu_mhz":1500,"min_ram_mb":384,"min_vram_mb":64,'
    '"gpu_feature_level":"sm2.0","cpu_features":["sse"],"min_os":"winxp"}',
    '{"min_cpu_mhz":1000,"min_ram_mb":128,"min_vram_mb":32,'
    '"gpu_feature_level":"tnl"}',
    # nonsense that must be tolerated identically on both sides
    '{"gpu_feature_level":"raytracing"}',
    '{"cpu_features":["avx512"]}',
    '{"min_os":"plan9"}',
    '{"notes":"no floor"}',
]


def test_decisions_match(cbin):
    cases = []
    for name, (mhz, ram, vram, gl, ol, feats, caps, free) in FLEET.items():
        for js in REQS:
            cases.append((name, mhz, ram, vram, gl, ol, feats, caps, free, js))
    stdin = "".join(
        f"{mhz} {ram} {vram} {gl} {ol} {feats} {caps} {free}\t{js}\n"
        for _n, mhz, ram, vram, gl, ol, feats, caps, free, js in cases)
    got = _run(cbin, "decide", stdin)
    assert len(got) == len(cases), "C driver dropped a case"

    bad = []
    for (name, mhz, ram, vram, gl, ol, feats, caps, free,
         js), line in zip(cases, got):
        cv, climit, ccaps = line.split("|")
        p = rules.Profile(cpu_mhz=mhz, ram_mb=ram, vram_mb=vram, gpu_level=gl,
                          os_level=ol, cpu_features=feats, caps=caps,
                          free_mb=free)
        r = rules.parse_requirements(json.loads(js), "T")
        d = rules.decide(p, r)
        if (int(cv) != d.verdict or climit != d.limiting
                or int(ccaps) != d.missing_caps):
            bad.append(f"{name} {js}: C=({cv},{climit},{ccaps}) "
                       f"py=({d.verdict},{d.limiting},{d.missing_caps})")
    assert not bad, ("verdict drift between the C and Python gates:\n  "
                     + "\n  ".join(bad[:25]))


def test_marginal_band_is_where_both_think_it_is(cbin):
    """Sweep the whole shortfall range on one machine and demand the C and
    Python band boundaries land on the same MHz. A band that quietly widened on
    one side only would turn arithmetic into model calls, or the reverse."""
    mhz = 845
    needs = list(range(400, 2000, 1))
    stdin = "".join(
        f"{mhz} 1024 128 {rules.GPU_SM3} {rules.OS_WINXP} 255 0 0"
        f'\t{{"min_cpu_mhz":{n}}}\n' for n in needs)
    got = _run(cbin, "decide", stdin)
    p = rules.Profile(cpu_mhz=mhz, ram_mb=1024, vram_mb=128,
                      gpu_level=rules.GPU_SM3, os_level=rules.OS_WINXP,
                      cpu_features=255)
    for need, line in zip(needs, got):
        cv = int(line.split("|")[0])
        d = rules.decide(p, rules.parse_requirements(
            {"min_cpu_mhz": need}, "T"))
        assert cv == d.verdict, f"min_cpu_mhz={need}: C={cv} py={d.verdict}"
    # And the boundaries are where the design says: <= 845 run, 846..1126
    # marginal, >= 1127 no.
    def cverdict(n):
        return int(_run(cbin, "decide",
                        f"{mhz} 1024 128 {rules.GPU_SM3} {rules.OS_WINXP} 255 0 0"
                        f'\t{{"min_cpu_mhz":{n}}}\n')[0].split("|")[0])
    assert cverdict(845) == rules.V_RUN
    assert cverdict(846) == rules.V_MARGINAL
    assert cverdict(1126) == rules.V_MARGINAL
    assert cverdict(1127) == rules.V_NO


def test_profile_hash_matches_and_is_stable(cbin):
    """The hash IS the cache key and the published verdict file's name. If the
    C side ever computed a different one from the same machine, the agent would
    look for a file the host never wrote and silently fall back to local rules.
    """
    machines = [
        # ... trailing pair is the PANEL's native mode, which is hashed
        ("GenuineIntel", 6, 8, 3, 845, 1, 15, 511, 0x10DE, 0x0150, 32, 5, 1,
         1024, 768),
        ("AuthenticAMD", 6, 2, 2, 1000, 1, 271, 511, 0x10DE, 0x0041, 128, 5, 1,
         1024, 768),
        ("GenuineIntel", 6, 42, 7, 3093, 4, 255, 2047, 0x10DE, 0x10C3, 512, 5, 1,
         1920, 1080),
        ("GenuineIntel", 6, 42, 7, 3093, 4, 255, 2047, 0x1002, 0x68F9, 1024, 6, 1,
         1920, 1080),
    ]
    stdin = "".join(" ".join(str(x) for x in m) + "\n" for m in machines)
    got = _run(cbin, "hash", stdin)
    assert len(got) == len(machines)
    for h in got:
        assert len(h) == 16 and all(c in "0123456789abcdef" for c in h)
    assert len(set(got)) == len(machines), \
        "two different fleet machines hashed the same - the cache would " \
        "serve one box's verdicts to another"

    # Measurement jitter must NOT move it, or the cache misses on every poll.
    jittered = list(machines[0])
    jittered[4] = 848        # inside the 25 MHz bucket
    jittered[7] = 508        # inside the 16 MB bucket
    again = _run(cbin, "hash", " ".join(str(x) for x in jittered) + "\n")
    assert again[0] == got[0], \
        "clock/RAM measurement jitter changed the profile hash"

    # A real hardware change must move it, or a re-carded box keeps stale
    # verdicts - which is .124's actual history.
    recarded = list(machines[0])
    recarded[9] = 0x0250
    again = _run(cbin, "hash", " ".join(str(x) for x in recarded) + "\n")
    assert again[0] != got[0], "a new GPU must produce a new profile"


def test_the_panel_is_part_of_the_machine(cbin):
    """Two boxes with identical silicon and different panels must not share a
    cache entry.

    1920x1080 is ~2.4x the pixels of 1024x768, which is the difference between
    a 2004 title being comfortable on a Radeon 9800 XT and not - so the panel
    changes the answer and therefore has to change the key. This is a live
    fleet case, not a hypothetical: .123/.145/.240/.246 drive 1080p panels
    while .124/.133/.143/.171 do not, and .123/.240 were additionally found
    sitting at 640x480 from a DOSBox leftover, which is why the profile carries
    the panel's EDID native mode rather than the current desktop mode.
    """
    base = ["GenuineIntel", 6, 42, 7, 3093, 4, 255, 2047,
            0x10DE, 0x10C3, 512, 5, 1, 1024, 768]
    wide = list(base)
    wide[13], wide[14] = 1920, 1080

    got = _run(cbin, "hash",
               " ".join(str(x) for x in base) + "\n"
               + " ".join(str(x) for x in wide) + "\n")
    assert len(got) == 2
    assert got[0] != got[1], (
        "swapping the panel left the profile hash alone - a 1080p box would "
        "be served a 1024x768 box's cached verdicts")


def test_the_hash_is_deterministic(cbin):
    """The same machine must hash the same every time.

    This is only true if every hashed field is assigned. The harness used to
    leave part of gg_profile_t as stack residue, which was harmless only for as
    long as no unassigned member was hashed - exactly the kind of latent fault
    that surfaces as "the cache stopped working" a release later.
    """
    m = ["AuthenticAMD", 15, 39, 1, 2403, 1, 255, 2047,
         0x1002, 0x9515, 512, 5, 1, 1920, 1080]
    line = " ".join(str(x) for x in m) + "\n"
    got = _run(cbin, "hash", line * 3)
    assert len(got) == 3
    assert got[0] == got[1] == got[2], "the profile hash is not deterministic"
