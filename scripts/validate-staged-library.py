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

        # The DISPLAY NAME becomes the .lnk filename, so it must be a legal
        # Windows filename. Redneck Rampage shipped "Redneck Rampage - Setup /
        # Network Config" and that shortcut has NEVER existed on any box: the
        # agent logs "could not create desktop shortcut" and moves on, so a
        # title quietly loses a launcher with nothing on the desktop to show
        # for it. Same family as the parentheses rule - a character that is
        # fine in prose and fatal in a path.
        # NB this applies to the display name ONLY. The target and icon fields
        # are PATHS and legitimately contain backslashes (AGAIN\BUBBA.ICO).
        disp = parts[1].strip() if len(parts) >= 2 else ""
        if disp:
            bad = [c for c in '\\/:*?"<>|' if c in disp]
            if bad:
                fail("launch.txt", "display name %r contains %s, which is "
                                   "illegal in a filename — the .lnk cannot be "
                                   "created and that launcher silently never "
                                   "appears" % (disp, " ".join(repr(c) for c in bad)))

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

    # --- per-box resolution: FLEETRES ---------------------------------------
    #
    # ONE staged tree deploys to EIGHT monitors — four 1920x1080 LCDs and four
    # CRTs, two of them 4:3 tubes being driven at 5:4. A resolution written into
    # a staged config is therefore wrong somewhere BY CONSTRUCTION, and the
    # whole library used to be pinned at 1024x768 (Tiberian Sun at 640x480).
    # The fix is FLEETRES.EXE + FLEETRES.BAT staged in the title, called by its
    # launchers. These checks catch the half-applied version of that fix, which
    # is silent: the game still starts, just at the wrong size on most boxes.
    bats = [f for f in os.listdir(tdir) if f.lower().endswith(".bat")]
    bodies = {b: read_text(os.path.join(tdir, b)) for b in bats}
    has_exe = os.path.isfile(os.path.join(tdir, "FLEETRES.EXE"))
    has_bat = os.path.isfile(os.path.join(tdir, "FLEETRES.BAT"))

    if has_exe != has_bat:
        fail("fleetres", "FLEETRES.%s is staged without FLEETRES.%s — the "
                         "launchers would call a block that is not there, or "
                         "measure a panel nothing reads"
                         % ("EXE" if has_exe else "BAT",
                            "BAT" if has_exe else "EXE"))

    # A launcher that expands FR_* without calling the block gets empty strings
    # — i.e. `-w  -h ` on a command line, silently.
    for b, body in bodies.items():
        if "%FR_" in body and "FLEETRES.BAT" not in body:
            fail("fleetres", "%r uses %%FR_*%% but never calls FLEETRES.BAT, so "
                             "every one of those expands to nothing" % b)

    # DOSBox: `fullresolution=original` changes the WHOLE DESKTOP to the DOS
    # mode. Measured on .145 with DISPLAYCFG: the desktop really does drop to
    # 640x480 and a 4:3 signal is handed to a 16:9 panel, and it is left behind
    # after a crash. `desktop` + `aspect=true` pillarboxes correctly instead —
    # but only on an LCD, so this cannot be a staged constant either way. The
    # launcher has to rewrite it per box.
    sdl_confs = [c for c in confs
                 if re.search(r"^\s*fullresolution\s*=", read_text(os.path.join(tdir, c)),
                              re.I | re.M)]
    if sdl_confs:
        rewritten = set()
        rw = re.compile(r"-ini\s+\"([^\"]+)\"\s+sdl\s+fullresolution\s+"
                        r"%FR_DOSFULLRES%", re.I)
        for body in bodies.values():
            for m in rw.finditer(body):
                # the path is a BATCH EXPRESSION, "%~dp0dosboxD1.conf", which
                # os.path.basename cannot split (there is no separator in it),
                # so match on the conf name appearing in it.
                rewritten.add(m.group(1).lower())
        for c in sdl_confs:
            if not any(c.lower() in r for r in rewritten):
                fail("fleetres-dosbox",
                     "%s sets [sdl] fullresolution but no launcher rewrites it "
                     "with FLEETRES (-ini ... sdl fullresolution "
                     "%%FR_DOSFULLRES%%). Left as a staged constant it is wrong "
                     "on half the fleet: `original` retargets the whole desktop "
                     "on an LCD, `desktop` is wrong on a CRT." % c)

    # id Tech 3: r_mode / r_customwidth / r_customheight / r_fullscreen are
    # CVAR_LATCH — they bite only at renderer init. A `seta r_mode` in the
    # staged autoexec.cfg runs after Com_StartupVariable and before R_Init, so
    # it BEATS the command line; that is exactly why passing +set r_mode on the
    # command line did nothing on .123. The mode has to come from the launcher,
    # and these two setas have to be gone for it to arrive.
    for root, dirs, files in os.walk(tdir):
        dirs[:] = [d for d in dirs if not d.startswith("_")]
        for fn in files:
            if fn.lower() != "autoexec.cfg":
                continue
            path = os.path.join(root, fn)
            rel = os.path.relpath(path, tdir)
            body = read_text(path)
            for n, line in enumerate(body.splitlines(), 1):
                m = re.match(r"\s*seta\s+(r_mode|r_fullscreen)\b", line, re.I)
                if m:
                    fail("idtech3-latch",
                         "%s line %d sets %s, a CVAR_LATCH cvar read at renderer "
                         "init — it silently beats the launcher's +set and pins "
                         "every monitor on the fleet to one resolution. Delete "
                         "it and let the launcher supply the mode: %s"
                         % (rel, n, m.group(1), line.strip()[:60]))

    # ---- engine configs: a double quote must not span a newline -----------
    #
    # Quake II's tokenizer (SiN, Quake 2, SoF, Hexen II ...) and LithTech's
    # (Shogo) let a QUOTED STRING RUN PAST THE END OF THE LINE. So a config
    # line holding an odd number of double quotes swallows the NEXT line into
    # a string. Two ways that bites, both seen in this library:
    #
    #   * a `bind`/`rangebind` with a missing closing quote simply does not
    #     take, and neither does the line after it. Shogo shipped
    #     `rangebind "##keyboard" "##9 0.0 0.0 "Weapon_7"` -- keys 8 and 9
    #     were dead in a staged game and nothing reported it.
    #   * a COMMENT that opens a quote on one line and closes it on the next
    #     puts the second line's `//` inside a string, so the comment marker
    #     stops working and the prose is executed as console commands.
    #
    # Either way the failure is silent and presents as "the config is not
    # taking" -- which sends you looking at the engine instead of the file.
    for root, dirs, files in os.walk(tdir):
        dirs[:] = [d for d in dirs if not d.startswith("_")]
        for fn in files:
            if fn.lower() not in ("autoexec.cfg", "config.cfg", "default.cfg"):
                continue
            path = os.path.join(root, fn)
            rel = os.path.relpath(path, tdir)
            for n, line in enumerate(read_text(path).splitlines(), 1):
                if line.count('"') % 2:
                    fail("config-quotes",
                         "%s line %d has an unmatched double quote, so the "
                         "quote spans into the next line and BOTH are "
                         "silently swallowed: %s"
                         % (rel, n, line.strip()[:70]))

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
