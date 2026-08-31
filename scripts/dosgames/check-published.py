#!/usr/bin/env python3
"""Is the DOSGAME.EXE the fleet actually runs the one this repo can rebuild?

WHY THIS EXISTS
===============
On 2026-08-30, publishing a rebuilt DOSGAME.EXE was stopped one command short
by a size mismatch, and the mismatch turned out to be this:

    git HEAD, rebuilt        111,170 bytes   (byte-exact reproduction)
    share, dated 2026-08-26  113,012 bytes

The share's binary carried **1,842 bytes of code that existed in no commit, on
no branch, in no worktree and in no file on this host** - a self-extracting-
archive fix, built and published to the fleet and never committed. Nothing
anywhere said so, for five days, and a `make` plus a `copy` would have deleted
it permanently.

**That is resolved.** The source was recovered from the binary on 2026-08-31
(see scripts/dosgames/README.md, "how the lost feature was recovered") and the
share now carries a build this repo makes. So this check no longer documents an
unresolved divergence: it EXISTS TO STOP ONE HAPPENING AGAIN, in either
direction, and `--strict` is wired into tests/run_dos_tests.sh.

    python3 scripts/dosgames/check-published.py
    python3 scripts/dosgames/check-published.py --strict   # exit 1 on divergence

It SKIPS (exit 0) when the share is not mounted or Open Watcom is absent - it
cannot answer the question then, and "could not ask" must never render as
"they agree". It reports; it never writes.
"""

import argparse
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "dosgame.c")
TRACKED = os.path.join(HERE, "dosgame.exe")
SHARE = "/mnt/retro-share/Utility/Retro Automation/dosgame/DOSGAME.EXE"
WATCOM = os.environ.get("DOSGAME_WATCOM",
                        os.path.expanduser("~/development/toolchain-dos/watcom"))

# The four log strings of the recovered feature. A published binary WITHOUT
# them is a build made from a tree that never had the fix - the exact silent
# regression this file was created by. Its only symptom on a DOS box is a game
# menu that launches an installer instead of a game.
FEATURE_MARKERS = [
    b"is a self-extracting archive, not the game",
    b"self-extracting archive; needs setup run",
    b"skip-listed, but it is the only thing that runs here",
    b'registry: DROP %s - launcher "%s" is a self-extracting archive',
]


def strings_of(path):
    try:
        with open(path, "rb") as fh:
            return fh.read()
    except OSError:
        return b""


def rebuild(dest_dir):
    """Rebuild dosgame.c exactly as the Makefile does. None if no toolchain."""
    wcl = os.path.join(WATCOM, "binl64", "wcl")
    if not os.access(wcl, os.X_OK):
        return None
    env = dict(os.environ, WATCOM=WATCOM, INCLUDE=os.path.join(WATCOM, "h"),
               PATH=os.path.join(WATCOM, "binl64") + os.pathsep + os.environ["PATH"])
    out = os.path.join(dest_dir, "DOSGAME.EXE")
    r = subprocess.run([wcl, "-bcl=dos", "-ml", "-os", "-q", "-wx", "-k8192",
                        "-fe=" + out, SRC],
                       cwd=dest_dir, env=env, capture_output=True, text=True)
    shutil.copy(SRC, os.path.join(dest_dir, "dosgame.c"))
    if not os.path.isfile(out):
        print(r.stdout + r.stderr, file=sys.stderr)
        return None
    return out


def banner(lines):
    width = max(len(l) for l in lines) + 4
    print("+" + "-" * (width - 2) + "+")
    for l in lines:
        print("| " + l.ljust(width - 4) + " |")
    print("+" + "-" * (width - 2) + "+")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--share", default=SHARE)
    ap.add_argument("--strict", action="store_true",
                    help="exit 1 when the share diverges (see the docstring)")
    args = ap.parse_args()

    if not os.path.isfile(args.share):
        print("SKIP  the share is not mounted at %s - the published binary was "
              "NOT checked" % args.share)
        return 0

    tmp = tempfile.mkdtemp(prefix="dosgame-check-")
    try:
        built = rebuild(tmp)
        if built is None:
            print("SKIP  Open Watcom not at %s - cannot rebuild to compare"
                  % WATCOM)
            return 0
        mine = strings_of(built)
        theirs = strings_of(args.share)

        print("rebuilt from scripts/dosgames/dosgame.c : %7d bytes" % len(mine))
        print("published on the share                  : %7d bytes" % len(theirs))
        if os.path.isfile(TRACKED):
            n = os.path.getsize(TRACKED)
            print("tracked scripts/dosgames/dosgame.exe    : %7d bytes%s"
                  % (n, "" if n == len(mine) else "   <-- STALE, rebuild it"))

        if mine == theirs:
            print("\nok  the fleet runs exactly what this repo builds")
            return 0

        missing_here = [m for m in FEATURE_MARKERS if m not in mine]
        missing_there = [m for m in FEATURE_MARKERS if m not in theirs]
        lines = ["THE PUBLISHED DOSGAME.EXE IS NOT THIS REPO'S BUILD"]
        if missing_there:
            lines += [
                "",
                "and the PUBLISHED one is missing %d string(s) of the"
                % len(missing_there),
                "self-extracting-archive fix:",
            ]
            lines += ["  " + m.decode() for m in missing_there]
            lines += [
                "",
                "So the fleet is running a build made without that fix. On a",
                "DOS box its only symptom is a menu that launches an installer",
                "instead of the game, for ever. Publish this repo's build.",
            ]
        elif missing_here:
            lines += [
                "",
                "and THIS REPO'S BUILD is missing %d string(s) the published"
                % len(missing_here),
                "one has:",
            ]
            lines += ["  " + m.decode() for m in missing_here]
            lines += [
                "",
                "That is the 2026-08-26 shape again: the share carries work the",
                "repo cannot rebuild. DO NOT PUBLISH OVER IT - recover the",
                "source first (README.md records how it was done last time).",
            ]
        else:
            lines += ["", "Both carry the whole feature set, so this is a",
                      "smaller drift - a stale publish, or an uncommitted edit.",
                      "Compare the two and publish from a build you can",
                      "reproduce."]
        print()
        banner(lines)
        return 1 if args.strict else 0
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
