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
import tempfile
import time
import re
import struct
import sys

# `call "%~dp0FLEETRES.BAT"`, optionally with -cap args. cmd.exe is
# case-insensitive, so this must be too.
FLEETRES_CALL_RE = re.compile(r'call\s+"%~dp0FLEETRES\.BAT"', re.I)

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


# The XP loader refuses a PE whose MajorSubsystemVersion is 6 or higher BEFORE
# a single instruction runs - it is a load-time check, not a runtime one, so
# there is no error dialog from the program and nothing in its own log. The
# whole fleet is XP (5.1), so anything >= 6.0 is a Vista-and-later binary that
# simply cannot start. GOG and re-release repacks are the usual offenders --
# SiN Gold shipped one and was unloadable on every box.
#
# Found by this check on 2026-08-30: UnrealTournament\System\magick.exe, a
# 39 MB ImageMagick 7 binary referenced by no launcher, left in the tree by
# whoever generated the icons. Dead weight on every box AND unloadable.
# `scripts/fleet/pe-audit.py` is the richer standalone sweep of the same
# territory - it also flags an impossible TimeDateStamp (a scene watermark).
# This is deliberately a SEPARATE, dependency-free parse rather than an import:
# the validator is the pre-imaging GATE and has to run on the share with
# nothing but the standard library. Keep the two in agreement on this rule.
MAX_SUBSYSTEM_MAJOR = 5          # 5.x = Win2000/XP. 6.0 = Vista.


def pe_subsystem_version(path):
    """(major, minor) from a PE optional header, or None if it is not a PE.

    Deliberately hand-rolled: the validator must run on the share with nothing
    installed but the standard library. Verified byte-for-byte against
    `objdump -p` on magick.exe (6.0), UnrealTournament.exe (5.1) and Tiberian
    Sun's GAME.EXE (4.0).
    """
    try:
        with open(path, "rb") as fh:
            head = fh.read(1024)
            if head[:2] != b"MZ":
                return None
            pe_off = struct.unpack_from("<I", head, 0x3C)[0]
            # The optional header can sit past our first read on a fat DOS stub.
            if pe_off + 0x50 > len(head):
                fh.seek(0)
                head = fh.read(pe_off + 0x100)
            if head[pe_off:pe_off + 4] != b"PE\0\0":
                return None
            magic = struct.unpack_from("<H", head, pe_off + 24)[0]
            if magic not in (0x10B, 0x20B):      # PE32 / PE32+
                return None
            # MajorSubsystemVersion is at optional-header offset 48 in both.
            major = struct.unpack_from("<H", head, pe_off + 24 + 48)[0]
            minor = struct.unpack_from("<H", head, pe_off + 24 + 50)[0]
            return (major, minor)
    except (OSError, struct.error):
        return None


def mz_kind(path):
    """'PE' | 'NE' | 'LE' | 'LX' | 'MZ' for an MZ image, else None.

    The distinction the DOSGAME.TXT check needs is exactly the one a filename
    cannot make: QUAKE.EXE and GLQUAKE.EXE sit in the same directory and only
    one of them is a DOS program.
    """
    try:
        with open(path, "rb") as fh:
            head = fh.read(0x40)
            if len(head) < 0x40 or head[:2] not in (b"MZ", b"ZM"):
                return None
            off = struct.unpack_from("<I", head, 0x3C)[0]
            if off < 0x40:
                return "MZ"
            fh.seek(off)
            sig = fh.read(2)
            return {b"PE": "PE", b"NE": "NE", b"LE": "LE",
                    b"LX": "LX"}.get(sig, "MZ")
    except (OSError, struct.error):
        return None


def find_ci(directory, name):
    """Case-insensitive lookup. We are a Linux host reading a Windows tree, and
    a case-sensitive miss here would report a staged file as absent."""
    try:
        for entry in os.listdir(directory):
            if entry.lower() == name.lower():
                return os.path.join(directory, entry)
    except OSError:
        pass
    return None


def find_ci_path(base, relpath):
    """Case-insensitive lookup of a MULTI-COMPONENT relative path.

    find_ci() takes one name in one directory. Handing it "_disc\\image.iso"
    silently returns None for every title that has one - which is how the first
    version of the disc-mount check "found" that eleven working launchers all
    pointed at missing images. The measurement was the broken thing, not the
    library, and that is the exact shape CLAUDE.md warns about.
    """
    cur = base
    for part in relpath.replace("\\", "/").split("/"):
        if not part or part == ".":
            continue
        cur = find_ci(cur, part)
        if cur is None:
            return None
    return cur


def _bat_var(text, name):
    """Value of a `set "NAME=value"` line in a .bat, or ''."""
    m = re.search(r'(?mi)^\s*set\s+"%s=(.*?)"\s*$' % re.escape(name), text)
    return m.group(1) if m else ""


def _cue_binary(text):
    """The FILE named by a cue sheet's first FILE line, or ''."""
    m = re.search(r'(?mi)^\s*FILE\s+"([^"]+)"', text)
    if m:
        return m.group(1)
    m = re.search(r'(?mi)^\s*FILE\s+(\S+)', text)
    return m.group(1) if m else ""


def iso_volume_label(path):
    """The ISO9660 volume label, read from the image itself.

    Handles the three sector layouts that actually turn up on this share:
    2048 (a plain .iso), 2352 (MODE1/2352 raw .bin) and 2448 (2352 plus 96
    bytes of subchannel, which is what a SafeDisc-capable dump has to carry).
    Reading the PVD at a flat offset 32768 gets ZEROS on the latter two - that
    arithmetic slip has already cost this project a wrong conclusion about the
    Generals images being malformed when they were not.

    Returns None when no PVD can be found, which is a "could not check", not a
    failure - the caller must render those differently.
    """
    try:
        size = os.path.getsize(path)
        with open(path, "rb") as f:
            for sector, offset in ((2048, 0), (2352, 16), (2448, 16)):
                if size % sector:
                    continue
                f.seek(16 * sector + offset)
                pvd = f.read(2048)
                if len(pvd) >= 72 and pvd[1:6] == b"CD001":
                    return pvd[40:72].decode("latin1").rstrip()
    except OSError:
        return None
    return None


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

    # --- DOSGAME.TXT: the title's REAL-DOS launcher --------------------------
    #
    # A DOS title's Windows shortcut starts the DOSBox staged beside it, and
    # DOSBox needs roughly a gigahertz of host CPU to emulate a 486. On the
    # fleet's Pentium 1 that is refused - while the DOS binary the emulator is
    # running is native to the machine. DOSGAME.TXT is how the tree tells the
    # real-mode DOS menu (DOSGAME.EXE, which already scans C:\GAMES) which file
    # to start, because no ranking of 8.3 names can pick the DOS build out of a
    # Windows-built tree. Generated by scripts/fleet/stage-dosnative.py.
    #
    # Every check here is a way the declaration could be WORSE than the guess
    # it replaces, which is the only way this feature can hurt.
    decl = find_ci(tdir, "DOSGAME.TXT")
    if decl:
        if os.path.basename(decl) != "DOSGAME.TXT":
            warn("dosgame.txt", "named %r; real DOS is case-blind so this "
                                "still resolves, but the library should be "
                                "consistent" % os.path.basename(decl))
        dlines = [l for l in read_text(decl).splitlines()
                  if l.strip() and not l.strip().startswith(("#", ";"))]
        if not dlines:
            fail("dosgame.txt", "no data line — DOSGAME.EXE falls back to "
                                "guessing, which on a staged tree picks a "
                                "Windows binary")
        else:
            # Only the FIRST data line is read, exactly like launch.txt.
            launcher = dlines[0].split("\t")[0].strip()
            if "(" in launcher or ")" in launcher:
                fail("dosgame.txt", "launcher %r contains a parenthesis" % launcher)
            stem, _, ext = launcher.partition(".")
            if len(stem) > 8 or len(ext) > 3 or not launcher:
                fail("dosgame.txt", "launcher %r is not an 8.3 name — real DOS "
                                    "would see a mangled 8.3 alias instead and "
                                    "the declaration would never match" % launcher)
            target = find_ci(tdir, launcher)
            if target is None:
                fail("dosgame.txt", "names %r, which is not in the tree "
                                    "(case-insensitive) — DOSGAME.EXE ignores "
                                    "the declaration and guesses" % launcher)
            elif launcher.lower().endswith((".exe", ".com")):
                kind = mz_kind(target)
                if kind in ("PE", "NE"):
                    fail("dosgame.txt", "names %r, which is a %s (Windows) "
                                        "binary — started from real DOS that is "
                                        "'This program cannot be run in DOS "
                                        "mode', not a game" % (launcher, kind))
                elif kind is None:
                    fail("dosgame.txt", "names %r, which is not an executable "
                                        "image at all" % launcher)

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
        # The CALL, not the mention. Testing for the bare name "FLEETRES.BAT"
        # passes a launcher that only names it in a comment — and a good
        # comment DOES name it, to say where the resolution comes from.
        if "%FR_" in body and not FLEETRES_CALL_RE.search(body):
            fail("fleetres", "%r uses %%FR_*%% but never calls FLEETRES.BAT, so "
                             "every one of those expands to nothing" % b)

    # `%%` is an escape ONLY inside a for loop. Anywhere else cmd.exe reduces
    # `"%%FR_GLIDE%%"` to the literal text `%FR_GLIDE%`, so a comparison against
    # it can never be true and the whole block is a silent no-op that reads
    # perfectly in review. This really shipped once, in the per-box render-device
    # block, and it is the same shape as every other defect this repo has paid
    # for: the tool reported success.
    # The distinction that makes this precise rather than noisy: a FOR loop
    # variable is ONE character (%%A, %%~K, %%D) and is never closed with a
    # second %%, while an environment variable is %%NAME%%. So match only a
    # doubled pair around a multi-character identifier, and skip any line that
    # carries a `for` anyway. Without both, this check fires on every mount
    # launcher in the library and trains people to ignore it.
    dbl = re.compile(r"%%[A-Za-z_][A-Za-z0-9_]+%%")
    for b, body in bodies.items():
        for line in body.splitlines():
            st = line.strip().lower()
            if st.startswith("rem") or st.startswith("::"):
                continue
            if re.search(r"(^|[\s(&|])for\s", st):
                continue
            m = dbl.search(line)
            if m:
                fail("fleetres-percent",
                     "%r contains %s outside a for loop — cmd.exe compares the "
                     "literal text, so this line silently does nothing"
                     % (b, m.group(0)))

    # A launcher that swaps the game-local nGlide wrapper must be able to swap
    # it BACK. A one-way rename strands the wrapper aside the moment the 3dfx
    # card comes out, and the six boxes with no Glide silicon depend on it.
    for b, body in bodies.items():
        if ".nglide" not in body.lower():
            continue
        if not re.search(r'move\s+/y\s+"[^"]*glide2x\.dll"\s+"[^"]*\.nglide"',
                         body, re.I):
            fail("fleetres-glide",
                 "%r mentions .nglide but never moves glide2x.dll aside" % b)
        if not re.search(r'move\s+/y\s+"[^"]*\.nglide"\s+"[^"]*glide2x\.dll"',
                         body, re.I):
            fail("fleetres-glide",
                 "%r moves the nGlide wrapper aside but never restores it — a "
                 "box that loses its 3dfx card would be left with no Glide "
                 "path at all" % b)

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

    # ---- PE subsystem version: XP refuses a Vista-only binary outright -----
    #
    # This is a LOAD-TIME refusal, so the failure is maximally silent: no
    # window, no dialog from the program, nothing in its own log, and the
    # launcher's `start ""` throws the exit code away. It presents as "the
    # game does nothing when you double-click it".
    for root, dirs, files in os.walk(tdir):
        dirs[:] = [d for d in dirs if not d.startswith("_")]
        for fn in files:
            if not fn.lower().endswith((".exe", ".dll")):
                continue
            path = os.path.join(root, fn)
            ver = pe_subsystem_version(path)
            if ver and ver[0] > MAX_SUBSYSTEM_MAJOR:
                fail("pe-subsystem",
                     "%s is PE subsystem %d.%d — XP's loader refuses anything "
                     "from 6.0 (Vista) before a single instruction runs, with "
                     "no dialog and nothing in any log. Restage a build "
                     "targeting 5.x, or drop the file if nothing launches it."
                     % (os.path.relpath(path, tdir), ver[0], ver[1]))

    # --- disc-mount launchers: the image must exist AND the label must match -
    #
    # A mount launcher decides "is the disc already there?" by comparing VOLID
    # against the drive's ISO9660 volume label. Get that string wrong by one
    # character and NOTHING says so: the mount succeeds, :finddisc never
    # matches, :waitdisc spins out its ~30 seconds, and the launcher falls
    # through to "a mounter was found but no drive appeared" - which reads as a
    # broken mounter on the box rather than a typo in the library. The same
    # silence covers an IMAGE path that points at a file GAMESYNC never
    # deployed, because _disc\ is easy to forget when a title is copied.
    #
    # Both are checkable from here, against the image itself, so they are.
    for fn in sorted(os.listdir(tdir)):
        if not fn.lower().endswith(".bat"):
            continue
        text = read_text(os.path.join(tdir, fn))
        if 'set "VOLID=' not in text or 'set "IMAGE=' not in text:
            continue                      # not a mount launcher
        volid = _bat_var(text, "VOLID")
        image = _bat_var(text, "IMAGE")
        if not image.startswith("%~dp0"):
            fail("disc-mount", "%s: IMAGE=%r is not relative to %%~dp0, so the "
                               "title does not relocate" % (fn, image))
            continue
        rel = image[len("%~dp0"):]
        real = find_ci_path(tdir, rel)
        if real is None:
            fail("disc-mount", "%s: the disc image it mounts is not in the tree "
                               "(%s). The launcher will report NO DISC MOUNTER / "
                               "no drive appeared, which reads as a broken box "
                               "rather than a missing file." % (fn, image))
            continue
        ipath = real
        # A .cue names its own data file; check that too, and probe the image.
        probe = ipath
        if ipath.lower().endswith(".cue"):
            binname = _cue_binary(read_text(ipath))
            binpath = find_ci(os.path.dirname(ipath), binname) if binname else None
            if binpath is None:
                fail("disc-mount", "%s: the cue sheet %s names %r and that file "
                                   "is not beside it - Daemon Tools cannot mount "
                                   "a cue whose FILE line does not resolve"
                                   % (fn, os.path.basename(ipath), binname))
                continue
            probe = binpath
        label = iso_volume_label(probe)
        if label is None:
            warn("disc-mount", "%s: could not read an ISO9660 volume label out of "
                               "%s, so its VOLID could not be checked"
                               % (fn, os.path.basename(probe)))
        elif volid.upper() not in label.upper():
            fail("disc-mount", "%s: VOLID=%r but the image's real volume label is "
                               "%r. :finddisc uses a substring match on the label, "
                               "so this launcher will mount the disc correctly and "
                               "then never find it - reported on the box as \"a "
                               "mounter was found but no drive appeared\"."
                               % (fn, volid, label))

    return out


# ---------------------------------------------------------------------------
# ONE VALIDATOR AT A TIME.
#
# This walks ~40 GB of staged tree over SMB, and today several agents ran it
# concurrently: 15 processes at once, 9 of them stuck in uninterruptible IO,
# the oldest 19 minutes in, none able to finish. Three separate agents read
# their own stall as a test failure and one nearly reported a pass that had
# actually been SIGTERMed at its timeout.
#
# A slow check is tolerable. A check that cannot finish, and whose stall looks
# exactly like a failure, is worse than no check - so serialise it. A waiter
# says what it is waiting for rather than sitting mute, and --no-wait exists
# for a caller that would rather be told than queue.
# ---------------------------------------------------------------------------
_LOCK_PATH = os.path.join(tempfile.gettempdir(), "validate-staged-library.lock")


def _acquire_lock(wait_s, quiet=False):
    """Return the held lock file, or None if we gave up waiting.

    Deliberately advisory and best-effort: on a platform without fcntl, or if
    anything about locking fails, the validator still RUNS. Refusing to check
    the library because a lock could not be taken would be a worse failure
    than the contention it guards against.
    """
    try:
        import fcntl
    except ImportError:
        return "unsupported"
    try:
        fh = open(_LOCK_PATH, "a+")
    except OSError:
        return "unsupported"
    deadline = time.time() + max(0, wait_s)
    announced = False
    while True:
        try:
            fcntl.flock(fh, fcntl.LOCK_EX | fcntl.LOCK_NB)
            return fh
        except OSError:
            pass
        if time.time() >= deadline:
            fh.close()
            return None
        if not announced and not quiet:
            print("waiting: another validate-staged-library.py holds the lock "
                  "(%s). This walks the whole share, so they are serialised.\n"
                  "  --no-wait          fail fast (exit 75) instead of queuing\n"
                  "  --library <path>   a SECOND TRANSPORT to the same server is\n"
                  "                     a real way past a contended mount: the\n"
                  "                     gvfs path is uncontended when the CIFS\n"
                  "                     mount is in IO wait, and a run that got\n"
                  "                     no timeslice in 25 minutes on /mnt\n"
                  "                     completed there. Same files, same server."
                  % _LOCK_PATH, file=sys.stderr)
            announced = True
        time.sleep(2.0)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--library", default=LIB_DEFAULT)
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--quiet", action="store_true",
                    help="print only titles with problems")
    ap.add_argument("--no-wait", action="store_true",
                    help="exit 75 rather than queue behind another run")
    ap.add_argument("--lock-wait", type=int, default=1800, metavar="SECONDS",
                    help="how long to wait for another run (default 1800)")
    args = ap.parse_args()

    # Serialise: see _acquire_lock. Held for the whole run.
    _lock = _acquire_lock(0 if args.no_wait else args.lock_wait, args.quiet)
    if _lock is None:
        print("another validate-staged-library.py is already running; "
              "not queuing (--no-wait)", file=sys.stderr)
        return 75          # EX_TEMPFAIL - distinct from 1 (problems found)

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
