#!/usr/bin/env python3
"""Is the DOSGAME.EXE the fleet actually runs the one this repo can rebuild?

WHY THIS EXISTS
===============
On 2026-08-30, publishing a rebuilt DOSGAME.EXE was stopped one command short by
a size mismatch, and the mismatch turned out to be this:

    git HEAD, rebuilt        111,170 bytes   (byte-exact reproduction)
    share, dated 2026-08-26  113,012 bytes

The share's binary is **1,842 bytes of code that exists in no commit, on no
branch, in no worktree and in no file on this host**. It carries four log
strings that appear nowhere in the repository's entire history:

    pick:   %s is a self-extracting archive, not the game
    pick:   %s -> %s (self-extracting archive; needs setup run)
    pick:   %s -> %s (skip-listed, but it is the only thing that runs here)
    registry: DROP %s - launcher "%s" is a self-extracting archive, not the
              game; re-deriving

i.e. a real refinement to the launcher choice and a registry-repair rule, built
and published to the fleet and never committed. Searched exhaustively:
`git log --all -S` over the whole history with no path filter, `git grep` across
every reachable commit, and a filesystem sweep of every `dosgame.c` on the host.

**So the fleet is running a DOSGAME the repo cannot reproduce, and overwriting
it would delete that work permanently.** That is why the DOSGAME.TXT support
added on 2026-08-30 was committed but NOT published: publishing is a trade
(a staged-library fix for a shareware-install fix) that a person has to make.

This script makes the divergence loud instead of leaving it to be rediscovered
by the next person who compares two file sizes. It reports; it never writes.

    python3 scripts/dosgames/check-published.py
    python3 scripts/dosgames/check-published.py --strict   # exit 1 on divergence

It exits 0 by default even when the share diverges, deliberately: the divergence
is a known, recorded, unresolved fact, and a check that fails the whole suite
today would train everyone to ignore it. Use --strict once it is resolved, and
wire that into tests/run_all.sh at the same time.
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

# Strings the PUBLISHED binary carries that no committed source has ever
# produced. If a future build reproduces these, the lost source has been
# recovered and this whole file can go.
LOST_MARKERS = [
    b"is a self-extracting archive, not the game",
    b"skip-listed, but it is the only thing that runs here",
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

        lost = [m for m in LOST_MARKERS if m in theirs and m not in mine]
        lines = ["THE PUBLISHED DOSGAME.EXE IS NOT THIS REPO'S BUILD"]
        if lost:
            lines += [
                "",
                "It carries %d string(s) that NO COMMIT has ever produced:" % len(lost),
            ]
            lines += ["  " + m.decode() for m in lost]
            lines += [
                "",
                "The source for it is lost - searched all of git history with",
                "-S and no path filter, git grep over every reachable commit,",
                "and every dosgame.c on this host.",
                "",
                "DO NOT PUBLISH OVER IT without deciding to discard that work.",
            ]
        else:
            lines += ["", "The share carries none of the known lost markers, so",
                      "this is a NEW divergence - find out what published it."]
        print()
        banner(lines)
        return 1 if args.strict else 0
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
