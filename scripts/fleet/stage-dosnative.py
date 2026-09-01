#!/usr/bin/env python3
"""Declare each staged title's REAL-DOS launcher, for the boxes that need one.

WHY THIS EXISTS
===============
Five staged titles ship the game's own DOS executable — Descent 1, Descent 2,
Quake, Carmageddon 1 and Redneck Rampage — and every one of them is reached, on
every box, through a `Play <Game>.bat` that starts the DOSBox staged beside it.
That is the right default on the fleet's Pentium III and later machines. It is
useless on a **Pentium 1**: DOSBox needs roughly a gigahertz of host CPU to
emulate a 486 at playable cycles, so the capability gate refuses all four DOSBox
titles on 192.168.1.243 (a 1997 Compaq Deskpro 2000, Windows 98 SE) — while the
binaries those emulators are running are **native to that machine** and would go
at full speed.

Descent 1's own FAQ, staged in the tree, states the game's requirement: "486 or
Pentium processor, 8 MB RAM". A P5 is not marginal for it; it is above spec.

WHAT REACHES THE BOX
====================
Nothing new is executed. The DOS lane already exists — `DOSGAME.EXE`, the
real-mode menu the agent stages onto Win9x boxes (agent/src/dosstage.c), whose
default scan roots are `C:\\GAMES;C:\\` — and `C:\\GAMES\\<Title>` is exactly
where GAMESYNC deploys a staged tree. So the game is already in front of the
DOS menu. What was missing is which file to start.

`DOSGAME.EXE` infers that from 8.3 names, and for a staged tree the inference is
wrong in a way that matters. MEASURED against the real trees, in DOSBox, by
scripts/dosgames/tests/test_pick_outcomes.sh:

    C:\\GAMES\\QUAKE1    -> GLQUAKE.EXE    a Win32 PE. In real DOS that is
                                          "This program cannot be run in DOS
                                          mode", not a game.
    C:\\GAMES\\DESCENT1  -> DESCENT1.BAT   a cmd.exe batch, opening with
                                          "cd /d" — a switch COMMAND.COM does
                                          not have.

That is not a bug in the heuristic; no ranking of 8.3 names can know which of
two real executables is the DOS one. So the tree says it, in one line, in the
same shape as the library's own launch.txt:

    DESCENTR.EXE<TAB>Descent

THE FILE NAME IS THE CONSTRAINT. Real DOS sees 8.3 only, so `dosnative.txt`
would arrive as `DOSNAT~1.TXT` — a mangled name that depends on what else is in
the directory. `DOSGAME.TXT` is 7.3 and is the same string on every box.

Usage:
    python3 scripts/fleet/stage-dosnative.py            # apply / re-apply
    python3 scripts/fleet/stage-dosnative.py --check    # exit 1 if not current
    python3 scripts/fleet/stage-dosnative.py --dry-run

It is idempotent, and it VERIFIES THE POST-CONDITION rather than reporting that
a write returned: a declaration is only emitted once the named file has been
read out of the tree and its PE header confirms it is a DOS image (MZ with no
PE/NE signature, or an LE/LX DOS extender binary). Declaring a Win32 PE would
reproduce the exact bug this exists to remove, and would do it silently.
"""

import argparse
import os
import struct
import sys

LIB_READ = "/mnt/retro-share/Files/Games-Library"
LIB_WRITE = ("/run/user/1000/gvfs/smb-share:server=192.168.1.122,"
             "share=files,user=voidsstr/Files/Games-Library")

DECL_FILE = "DOSGAME.TXT"

# ---------------------------------------------------------------------------
# What each title's REAL-DOS entry point is.
#   title dir -> (launcher, menu title, why this one and not another)
# ---------------------------------------------------------------------------
DECLARE = {
    "Descent1": (
        "DESCENTR.EXE", "Descent",
        "The registered DOS build. NOT DESCENT.BAT, which runs EREGCARD.EXE "
        "(the 1995 registration card) before the game, and not DESCENT1.BAT, "
        "which is a Windows batch. Descent's own FAQ in this tree: '486 or "
        "Pentium processor, 8 MB RAM'."),
    "Descent2": (
        "DESCENT2.EXE", "Descent II",
        "The DOS build. D2VOODOO.EXE beside it is the 3dfx Glide build and "
        "needs a Voodoo; DESCENTW.EXE is the Win95 one. The guess happens to "
        "land on DESCENT2.EXE here (it matches the directory name), so this "
        "declaration is pinning a right answer rather than fixing a wrong one."),
    "Quake1": (
        "QUAKE.EXE", "Quake",
        "The DOS build, with CWSDPMI.EXE staged beside it as its DPMI host. "
        "The undeclared guess is GLQUAKE.EXE, a Win32 PE."),
}

# Titles that carry a DOS binary and are deliberately NOT declared. Recorded
# here rather than omitted, because 'we looked and it cannot work yet' and 'we
# never looked' are different facts and only one of them is finished.
WITHHELD = {
    "Carmageddon1": (
        "MAINPROG.EXE",
        "Its DOSBox conf does `imgmount d \"..\\CARMA.INST\" -t iso` — the "
        "game is started against a mounted CD image. Real DOS has no image "
        "mounter staged, so a bare MAINPROG.EXE would run without D: and this "
        "has not been tried on hardware. Needs a DOS CD-image driver "
        "(SHSUCDX/SHSUCDHD) staged and PROVEN before it can be declared."),
    "Daggerfall": (
        "FALL.EXE",
        "The DOS binary is real - FALL.EXE is a CauseWay-extended LE image, "
        "not a PE - and a Pentium 1 is above the 1996 game's own published "
        "floor, so this one is worth revisiting. TWO things block it today and "
        "neither has been tried on hardware. First, Z.CFG hardcodes "
        "'path C:\\arena2\\' and 'pathCD C:\\arena2\\', which is only "
        "true INSIDE DOSBox, where the conf's 'mount C \"..\"' makes the tree "
        "root C:; on real DOS the tree is at C:\\GAMES\\Daggerfall and the "
        "game would look for a directory that is not there. Second, FALL.EXE "
        "takes its config file as argv[1] - GOG's own autoexec runs "
        "'fall.exe z.cfg' - and DOSGAME.TXT's '<8.3 launcher><TAB><title>' "
        "cannot carry an argument. A second cfg with real-DOS paths plus a "
        "DAGGER.BAT wrapper would probably do it, but neither exists and "
        "neither has been proven, and a declared launcher that starts and then "
        "fails is worse than one that is honestly absent."),
    "RedneckRampage": (
        "RR.EXE",
        "Same shape: `imgmount d \"..\\Redneck.inst\"`, plus FIX.EXE runs "
        "first. Untested without D:."),
}


def dos_image(path):
    """Return the executable kind, or None when the file is not an MZ image.

    'PE' means a Windows binary — the thing that must never be declared.
    """
    try:
        with open(path, "rb") as f:
            head = f.read(0x40)
            if len(head) < 0x40 or head[:2] not in (b"MZ", b"ZM"):
                return None
            off = struct.unpack("<I", head[0x3C:0x40])[0]
            if off < 0x40:
                return "MZ"
            f.seek(off)
            sig = f.read(2)
            return {b"PE": "PE", b"LE": "LE", b"LX": "LX",
                    b"NE": "NE"}.get(sig, "MZ")
    except OSError:
        return None


def find_ci(directory, name):
    """Case-insensitive lookup - we are a Linux host reading a Windows tree."""
    try:
        for entry in os.listdir(directory):
            if entry.lower() == name.lower():
                return os.path.join(directory, entry)
    except OSError:
        pass
    return None


def rendered(launcher, title, why):
    """The staged file. CRLF, because everything DOS reads here is CRLF."""
    lines = [
        "%s\t%s" % (launcher, title),
        "#",
        "# THE REAL-DOS LAUNCHER FOR THIS TITLE. Read by DOSGAME.EXE, the DOS",
        "# menu the agent stages on Win9x boxes, which scans C:\\GAMES - where",
        "# GAMESYNC puts this tree. Without it DOSGAME guesses from 8.3 names",
        "# and picks a Windows binary out of a Windows-built tree.",
        "#",
        "# Generated by retro-agent scripts/fleet/stage-dosnative.py.",
        "# Data line FIRST: only the first data line is read.",
        "#",
    ]
    for chunk in why.split(". "):
        chunk = chunk.strip()
        if not chunk:
            continue
        if not chunk.endswith("."):
            chunk += "."
        while len(chunk) > 72:
            cut = chunk.rfind(" ", 0, 72)
            if cut <= 0:
                break
            lines.append("# " + chunk[:cut])
            chunk = chunk[cut + 1:]
        lines.append("# " + chunk)
    return "\r\n".join(lines) + "\r\n"


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--library", default=None,
                    help="staged library root to WRITE (default: the gvfs mount)")
    ap.add_argument("--check", action="store_true",
                    help="exit 1 if the share is not current; write nothing")
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()

    lib = args.library
    if lib is None:
        lib = LIB_READ if (args.check or args.dry_run) else LIB_WRITE
    if not os.path.isdir(lib):
        print("staged library not mounted at %s" % lib, file=sys.stderr)
        if args.check:
            print("SKIP - cannot verify without the share")
            return 0
        return 2

    problems = 0
    changed = 0
    for title in sorted(DECLARE):
        launcher, menu, why = DECLARE[title]
        tree = find_ci(lib, title)
        if tree is None:
            print("FAIL  %-16s title is not in the library" % title)
            problems += 1
            continue
        exe = find_ci(tree, launcher)
        if exe is None:
            print("FAIL  %-16s declares %s, which is not in the tree "
                  "(case-insensitive)" % (title, launcher))
            problems += 1
            continue
        kind = dos_image(exe)
        if kind in (None, "PE", "NE"):
            print("FAIL  %-16s %s is %s, not a DOS image - refusing to declare"
                  % (title, launcher, kind or "not an MZ executable"))
            problems += 1
            continue

        want = rendered(launcher, menu, why)
        dest = os.path.join(tree, DECL_FILE)
        have = None
        cur = find_ci(tree, DECL_FILE)
        if cur:
            dest = cur
            try:
                with open(cur, "rb") as f:
                    have = f.read().decode("ascii", "replace")
            except OSError:
                have = None
        if have == want:
            print("ok    %-16s %s -> %s (%s)" % (title, DECL_FILE, launcher, kind))
            continue
        changed += 1
        if args.check:
            print("STALE %-16s %s is %s" % (title, DECL_FILE,
                                            "absent" if have is None else "different"))
            continue
        if args.dry_run:
            print("would %-16s write %s -> %s" % (title, DECL_FILE, launcher))
            continue
        with open(dest, "wb") as f:
            f.write(want.encode("ascii"))
        print("WROTE %-16s %s -> %s (%s)" % (title, DECL_FILE, launcher, kind))

    for title, (launcher, why) in sorted(WITHHELD.items()):
        print("held  %-16s carries %s, deliberately NOT declared:" % (title, launcher))
        line = "        "
        for word in why.split():
            if len(line) + len(word) + 1 > 78:
                print(line)
                line = "        "
            line += word + " "
        print(line.rstrip())

    if problems:
        print("\n%d problem(s)" % problems)
        return 1
    if args.check and changed:
        print("\n%d title(s) not current - run without --check" % changed)
        return 1
    print("\n%d declared, %d withheld, %d change(s)"
          % (len(DECLARE), len(WITHHELD), changed))
    return 0


if __name__ == "__main__":
    sys.exit(main())
