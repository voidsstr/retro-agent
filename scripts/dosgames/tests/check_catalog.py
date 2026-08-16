#!/usr/bin/env python3
"""Is data/GAMES.CAT consistent with the generator that is supposed to produce it?

A generated artifact that is checked in can silently outlive the fix to its
generator. This one did: gen_catalog.py's dos83() and survey_share.py's
coms_shallow export were both fixed, but nothing regenerated GAMES.CAT, so the
shipped catalogue -- byte-identical to the one on the fleet's Win98 box -- still
carries the OLD `name.upper()[:12]` output and no .COM launchers at all. Both
fixes are inert on real hardware until `make catalog` runs against the share.

This check needs no share and no network, so it can say "stale" from anywhere.

  exit 0  the catalogue is reproducible by the current generator
  exit 1  it is stale (details printed); run `make catalog` with the share mounted
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))

from gen_catalog import dos83  # noqa: E402  (path set above)


def check(path):
    rows = []
    with open(path, encoding="cp437", errors="replace") as f:
        for n, line in enumerate(f, 1):
            line = line.rstrip("\n")
            if not line or line.startswith("#"):
                continue
            fld = line.split("|")
            if len(fld) >= 4:
                rows.append((n, fld))

    problems = []

    # 1. Every launcher must be a fixed point of the CURRENT dos83(): if it is
    #    not, the row was written by an older generator.
    unreproducible = [(n, f[0][:40], f[3]) for n, f in rows if dos83(f[3]) != f[3]]
    if unreproducible:
        problems.append(
            "%d launcher name(s) the current dos83() could not have produced "
            "(8-char stem truncation from the old name.upper()[:12]):"
            % len(unreproducible))
        for n, title, exe in unreproducible[:10]:
            problems.append("      line %-5d %-40s %r" % (n, title, exe))
        if len(unreproducible) > 10:
            problems.append("      ... and %d more" % (len(unreproducible) - 10))

    # 2. A DOS library of this vintage always has .COM-launched games. Zero of
    #    them means survey_share.py's coms_shallow export never reached this file.
    coms = [f for _, f in rows if f[3].upper().endswith(".COM")]
    if rows and not coms:
        problems.append(
            "no row has a .COM launcher, which is impossible for a pre-1990 DOS "
            "library - survey_share.py's coms_shallow export is not in this file, "
            "so every COM-launched game is missing from the Available tab")

    # 3. Tile names must be collision-free: a shared .PRV means the second game
    #    of the pair can never be rendered (gen_tiles skips existing files) and
    #    shows the first game's screenshot.
    seen = {}
    dupes = 0
    for _, f in rows:
        if len(f) > 5 and f[5]:
            seen.setdefault(f[5].upper(), []).append(f[0])
    for tile, titles in seen.items():
        if len(titles) > 1:
            dupes += len(titles)
    if dupes:
        shared = sum(1 for t in seen.values() if len(t) > 1)
        example = next(t for t in seen.values() if len(t) > 1)
        problems.append(
            "%d rows share %d tile names (e.g. %s and %s) - stem8() truncation "
            "instead of zip_stem()" % (dupes, shared, example[0], example[1]))

    print("%s: %d rows" % (path, len(rows)))
    if not problems:
        print("  OK - reproducible by the current generator")
        return 0
    print("  STALE:")
    for p in problems:
        print("    " + p)
    print("\n  Fix: mount the share and run `make catalog` in scripts/dosgames,")
    print("  then re-publish data/GAMES.CAT to the fleet.")
    return 1


if __name__ == "__main__":
    target = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
        os.path.dirname(HERE), "data", "GAMES.CAT")
    sys.exit(check(target))
