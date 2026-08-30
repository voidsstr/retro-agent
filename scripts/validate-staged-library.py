#!/usr/bin/env python3
"""Check every staged title against the staged-game contract.

WHAT THIS IS FOR. "Staged" is a promise: the agent can move a title onto a
freshly imaged machine and it simply works — no installer, no wizard, nobody at
the keyboard. That promise is easy to break by accident, and every way we have
broken it so far was silent: a launch.txt naming a file that is not there, a
line pushed past the agent's 1023-byte read, a launcher whose name contains
parentheses (unlaunchable through the agent but fine from a desktop shortcut),
an icon path with a typo that degrades quietly to the wrong artwork.

None of those show up until a box tries them, and by then the box is wrong. So
this runs on the SHARE, in seconds, and answers one question: **would this
library deploy cleanly to a brand-new machine right now?**

Run it before an imaging run, after any change to the library, and as the last
step of any staging work:

    python3 scripts/validate-staged-library.py            # human-readable
    python3 scripts/validate-staged-library.py --quiet    # only problems
    python3 scripts/validate-staged-library.py --json     # for tooling

Exit status is 0 only when every title passes. Nothing here is a style
preference: each check encodes a defect that actually reached a box.
"""

import argparse
import json
import os
import re
import sys

LIB_DEFAULT = "/mnt/retro-share/Files/Games-Library"

# The agent reads only the first this-many bytes of launch.txt
# (agent/src/gamesync.c: gs_make_game_shortcut reads sizeof(buf)-1).
LAUNCH_TXT_READ_LIMIT = 1023


class Problem:
    __slots__ = ("title", "severity", "check", "detail")

    def __init__(self, title, severity, check, detail):
        self.title = title
        self.severity = severity        # "fail" blocks a deploy, "warn" does not
        self.check = check
        self.detail = detail

    def as_dict(self):
        return {"title": self.title, "severity": self.severity,
                "check": self.check, "detail": self.detail}


def read_text(path):
    """Staged files are Windows-authored: latin1 never raises, and we only ever
    compare ASCII structure, so decoding cannot be the thing that fails."""
    with open(path, "rb") as fh:
        return fh.read().decode("latin1")


def check_title(lib, title):
    """Every check below is a defect that really reached a box."""
    out = []
    tdir = os.path.join(lib, title)

    def fail(check, detail):
        out.append(Problem(title, "fail", check, detail))

    def warn(check, detail):
        out.append(Problem(title, "warn", check, detail))

    # --- launch.txt: without it the title deploys but is unreachable ---------
    lpath = os.path.join(tdir, "launch.txt")
    if not os.path.isfile(lpath):
        fail("launch.txt", "missing — the title would deploy with no shortcut, "
                           "so it lands on the box and nobody can start it")
        return out

    raw = read_text(lpath)

    # The agent stops reading at 1023 bytes. A data line past that is silently
    # lost, which reads as "that game has no shortcut" with nothing to explain
    # it. Comments above the data lines are how this happens in practice.
    data_lines = []
    for idx, line in enumerate(raw.splitlines()):
        s = line.strip()
        if s and not s.startswith("#"):
            data_lines.append((idx, line))

    if not data_lines:
        fail("launch.txt", "no data lines — only comments or blanks")

    # Where does the LAST data line end, in bytes?
    pos, last_end = 0, 0
    for line in raw.splitlines(True):
        s = line.strip()
        if s and not s.startswith("#"):
            last_end = pos + len(line.encode("latin1"))
        pos += len(line.encode("latin1"))
    if last_end > LAUNCH_TXT_READ_LIMIT:
        fail("launch.txt", "a data line ends at byte %d, past the agent's "
                           "%d-byte read — that shortcut is silently lost. "
                           "Move data lines above the comments."
                           % (last_end, LAUNCH_TXT_READ_LIMIT))

    for _, line in data_lines:
        parts = line.rstrip("\r\n").split("\t")
        target = parts[0].strip()
        icon = parts[2].strip() if len(parts) >= 3 else ""

        if not target:
            fail("launch.txt", "a data line has an empty target")
            continue

        # ( and ) survive a desktop double-click and break agent launching,
        # so a broken launcher looks perfect to a human tester.
        if "(" in target or ")" in target:
            fail("filename", "%r contains parentheses — cannot be launched "
                             "through the agent (the double cmd /c loses the "
                             "quoting). Rename with a dash." % target)

        tpath = os.path.join(tdir, target.replace("\\", os.sep))
        if not os.path.isfile(tpath):
            fail("launch.txt", "names %r which is not in the tree — no shortcut "
                               "is made and nothing says why" % target)

        # An icon path that does not resolve degrades silently to auto-
        # resolution, i.e. to exactly the wrong icon the field exists to stop.
        if icon:
            ipath = os.path.join(tdir, icon.replace("\\", os.sep))
            if not os.path.isfile(ipath):
                fail("icon", "launch.txt names icon %r which is not in the "
                             "tree — it degrades silently to the auto-resolved "
                             "icon" % icon)

    # --- install.reg: merged after copying; malformed = silently not merged --
    rpath = os.path.join(tdir, "install.reg")
    if os.path.isfile(rpath):
        reg = read_text(rpath)
        head = reg.lstrip()[:40].upper()
        # Two valid dialects, and the difference only matters off the NT family:
        #   REGEDIT4                              -> merges everywhere, incl. Win9x
        #   Windows Registry Editor Version 5.00  -> XP and later ONLY
        # The current fleet is XP + one Win7, so v5 is fine today. It is still
        # worth flagging: a Win9x box would merge nothing and report nothing,
        # and this project has had Win98 machines. Deliberately a WARN, not a
        # fail - claiming a deploy-breaker that is not one trains people to
        # ignore the tool.
        if head.startswith("REGEDIT4"):
            pass
        elif head.startswith("WINDOWS REGISTRY EDITOR VERSION 5"):
            warn("install.reg", "REGEDIT5 dialect — merges on XP/Win7 (the whole "
                                "current fleet) but silently does nothing on "
                                "Win9x. REGEDIT4 works on both.")
        else:
            fail("install.reg", "starts with neither REGEDIT4 nor 'Windows "
                                "Registry Editor Version 5.00' — regedit will "
                                "refuse it and report nothing")
        # REGEDIT4 is an ANSI, CRLF format. XP's regedit tolerates LF, so this
        # is a warning rather than a failure - but hex(2) values in a REGEDIT4
        # file are byte-per-character, and mixing conventions is how the
        # DevicePath truncation bug happened in the PXE image.
        if "\r\n" not in reg and "\n" in reg:
            warn("install.reg", "LF line endings (regedit expects CRLF; XP "
                                "tolerates it, Win9x is less forgiving)")

    # --- DOSBox titles: fullscreen is a user requirement --------------------
    confs = [f for f in os.listdir(tdir) if f.lower().endswith(".conf")]
    if confs:
        saw_fullscreen = False
        for c in confs:
            body = read_text(os.path.join(tdir, c))
            if re.search(r"^\s*fullscreen\s*=\s*true", body, re.I | re.M):
                saw_fullscreen = True
        if not saw_fullscreen:
            warn("fullscreen", "DOSBox title: no conf sets fullscreen=true "
                               "(all staged games must run fullscreen)")

    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--library", default=LIB_DEFAULT)
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--quiet", action="store_true",
                    help="print only titles with problems")
    args = ap.parse_args()

    lib = args.library
    if not os.path.isdir(lib):
        print("library not found: %s" % lib, file=sys.stderr)
        return 2

    titles = sorted(d for d in os.listdir(lib)
                    if os.path.isdir(os.path.join(lib, d))
                    and not d.startswith("_"))

    problems = []
    for t in titles:
        problems.extend(check_title(lib, t))

    fails = [p for p in problems if p.severity == "fail"]
    warns = [p for p in problems if p.severity == "warn"]

    if args.json:
        print(json.dumps({
            "library": lib,
            "titles": len(titles),
            "deployable": not fails,
            "problems": [p.as_dict() for p in problems],
        }, indent=2))
        return 1 if fails else 0

    bad = {p.title for p in problems}
    print("Staged library: %s" % lib)
    print("%d titles checked\n" % len(titles))

    for t in titles:
        mine = [p for p in problems if p.title == t]
        if not mine:
            if not args.quiet:
                print("  [ ok ] %s" % t)
            continue
        print("  [%s] %s" % ("FAIL" if any(p.severity == "fail" for p in mine)
                             else "warn", t))
        for p in mine:
            print("         %s: %s" % (p.check, p.detail))

    print()
    if fails:
        print("NOT DEPLOYABLE — %d problem(s) across %d title(s) would break a "
              "fresh box." % (len(fails), len({p.title for p in fails})))
    else:
        print("DEPLOYABLE — every title satisfies the staged-game contract.")
    if warns:
        print("%d warning(s); these do not block a deploy." % len(warns))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
