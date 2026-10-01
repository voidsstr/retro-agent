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
import hashlib
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


# An explicit icon that EXISTS can still draw as a blank generic page, and it
# did, on every XP box, for four titles (measured on .123, 2026-09-28):
#   * a Vista-format .ico whose every image is PNG-compressed. XP's icon
#     loader cannot decode a PNG entry, so it has nothing to draw. The GOG
#     icons (Warcraft I/II) and Serious Sam's were all PNG-only;
#   * an .exe with no icon resource. Carmageddon 1's MAINPROG.EXE is a
#     DOS4GW/LE binary - no PE, no resources, so no icon to take.
# Windows 7 draws the PNG entries fine, which is how this hid: the file is
# there, the path resolves, and only the XP desktop shows a blank page.
# Returns None when the icon is drawable, else (severity, reason).
def icon_xp_problem(path):
    try:
        with open(path, "rb") as fh:
            data = fh.read()
    except OSError as exc:
        return ("fail", "cannot be read: %s" % exc)
    low = path.lower()
    if low.endswith(".ico"):
        if len(data) < 6:
            return ("fail", "is too short to be an icon")
        reserved, kind, count = struct.unpack_from("<HHH", data, 0)
        if reserved != 0 or kind != 1 or count == 0:
            head = "a BMP" if data[:2] == b"BM" else "not an icon (no ICONDIR)"
            return ("warn", "is %s renamed .ico - XP happens to draw a BMP, "
                            "but it is not an icon; convert it" % head)
        bitmaps = 0
        for i in range(count):
            ent = 6 + 16 * i
            if ent + 16 > len(data):
                return ("fail", "has a truncated icon directory")
            off = struct.unpack_from("<I", data, ent + 12)[0]
            if data[off:off + 4] != b"\x89PNG":
                bitmaps += 1
        if bitmaps == 0:
            return ("fail", "holds only PNG-compressed images, which XP cannot "
                            "decode - every XP box shows a blank generic icon. "
                            "Re-save it with BMP entries (16/24/32/48)")
        return None
    if low.endswith((".exe", ".dll")):
        return None if pe_has_icon(data) else (
            "fail", "carries no icon resource (a DOS binary or an icon-less "
                    "PE) - the shortcut shows a blank generic icon everywhere")
    return None


# Windows 9x draws an .ico only from an image it can decode, and XP's 32-bit
# alpha images are not among them: an .ico holding nothing else - DXX-Rebirth's
# d1x-rebirth.ico, which every XP box draws - shows the generic MS-DOS icon on
# a 9x desktop. MEASURED on .243 (Win98 SE, a 256-colour desktop), 2026-09-29,
# one shortcut per kind side by side: an 8-bit icon (DESCENT9.ICO) and a
# 24-bit-only one (Redneck Rampage's rampage.ico) both drew; the 32-bit-only
# d1x-rebirth.ico showed the generic icon, as "Descent - DOS" had. A PNG entry
# decodes nowhere before Vista. Only .ico files are judged here: an .exe's icon
# is icon_xp_problem()'s. Returns None when a 9x desktop can draw it, else why.
def icon_9x_problem(path):
    if not path.lower().endswith(".ico"):
        return None
    try:
        with open(path, "rb") as fh:
            data = fh.read()
    except OSError:
        return None
    if len(data) < 6:
        return None
    reserved, kind, count = struct.unpack_from("<HHH", data, 0)
    if reserved != 0 or kind != 1 or count == 0:
        return None                      # icon_xp_problem() reports these
    depths = set()
    for i in range(count):
        ent = 6 + 16 * i
        if ent + 16 > len(data):
            return None
        off = struct.unpack_from("<I", data, ent + 12)[0]
        if data[off:off + 4] == b"\x89PNG":
            depths.add("PNG")
            continue
        if off + 16 > len(data):
            continue
        bpp = struct.unpack_from("<H", data, off + 14)[0]
        if bpp <= 24:
            return None
        depths.add("%d-bit" % bpp)
    return ("holds only %s images - Windows 9x draws none of them (measured on "
            ".243), so a 9x desktop shows the generic MS-DOS icon. Add 8-bit "
            "32x32 and 16x16 BMP images, under a new filename (the shell caches "
            "an icon by path)" % " and ".join(sorted(depths)))


def shortcut_os_range(req, target):
    """(min_os, max_os) for one launch.txt target: its own rule, else the title's."""
    rules = req.get("shortcuts") if isinstance(req.get("shortcuts"), dict) else {}
    sc = next((v for k, v in rules.items() if k.lower() == target.lower()), None)
    sc = sc if isinstance(sc, dict) else {}
    return (sc.get("min_os", req.get("min_os")), sc.get("max_os", req.get("max_os")))


def pe_has_icon(data):
    """True if a PE image has an RT_GROUP_ICON (14) resource."""
    if data[:2] != b"MZ" or len(data) < 0x40:
        return False
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe:pe + 4] != b"PE\0\0":
        return False
    nsec = struct.unpack_from("<H", data, pe + 6)[0]
    optsz = struct.unpack_from("<H", data, pe + 20)[0]
    opt = pe + 24
    magic = struct.unpack_from("<H", data, opt)[0]
    dd = opt + (96 if magic == 0x10B else 112)
    rva, size = struct.unpack_from("<II", data, dd + 2 * 8)
    if not rva or not size:
        return False
    sec = opt + optsz
    for i in range(nsec):
        vsz, va, rawsz, raw = struct.unpack_from("<IIII", data, sec + 40 * i + 8)
        if va <= rva < va + max(vsz, rawsz):
            root = raw + (rva - va)
            named, ids = struct.unpack_from("<HH", data, root + 12)
            for j in range(named + ids):
                name = struct.unpack_from("<I", data, root + 16 + 8 * j)[0]
                if name == 14:
                    return True
            return False
    return False


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


# Unreal Engine 1/2 builds User.ini - and every key and mouse binding, in its
# [Engine.Input] section - from DefUser.ini ONLY when User.ini is missing.
# Epic's installer leaves a User.ini holding just its own wizard's
# [WindowPositions]; a tree captured straight after setup ships that stub and
# the game runs with NOTHING bound. It rendered and joined the fleet server on
# every UT2004 box on 2026-09-29 and ignored keyboard and mouse on all of them.
# Returns None when fine, else the reason.
def ue_userini_problem(sysdir):
    defuser = find_ci(sysdir, "DefUser.ini")
    user = find_ci(sysdir, "User.ini")
    if not defuser or not user:
        return None
    def has_input(path):
        try:
            with open(path, "rb") as fh:
                return b"[engine.input]" in fh.read().lower()
        except OSError:
            return True        # unreadable: not evidence of the stub
    if has_input(defuser) and not has_input(user):
        return ("User.ini has no [Engine.Input] although DefUser.ini does - the "
                "installer stub; the engine only builds User.ini from DefUser.ini "
                "when it is MISSING, so every key and mouse button is unbound")
    return None

def ue_runtime_log_problem(sysdir):
    """An Unreal Engine title must not stage the engine's own <exe>.log.

    The engine writes System\\<ExeName>.log while it runs and holds it open, so
    a staged copy cannot be overwritten on any box where the game is running:
    GAMESYNC ends with failed_files 1 and skips its completion marker. Found
    2026-09-29 - the UT99 trees shipped an empty UNREALTOURNAMENT.log and every
    box syncing during a UT99 game failed on it."""
    try:
        names = os.listdir(sysdir)
    except OSError:
        return None
    low = {n.lower(): n for n in names}
    if "core.u" not in low and "core.dll" not in low:
        return None                      # not an Unreal Engine System dir
    hits = []
    for n in names:
        stem, ext = os.path.splitext(n)
        if ext.lower() == ".exe" and (stem.lower() + ".log") in low:
            hits.append(low[stem.lower() + ".log"])
    if hits:
        return ("System\\%s is the engine's own runtime log - the game rewrites "
                "it and holds it open, so a staged copy fails GAMESYNC on every "
                "box where the game is running; remove it from the library"
                % ", System\\".join(sorted(hits)))
    return None

UE2_INIS = ("UT2004.ini", "UT2003.ini")
# key -> (bad value, why). Each one is a stutter or input-lag cause measured
# against the fleet on 2026-09-30 (.123, Athlon 64 4000+ / HD 3850: "UT2004 and
# UT2003 stutter, every 30 s or so input does not work or hesitates").
_UE2_BAD = {
    "usespeechrecognition": ("true", "UT2004 runs Windows speech recognition on "
                             "the microphone the whole game - a known periodic "
                             "hitch, and nobody on the fleet uses voice commands"),
    "usevoip": ("true", "voice chat keeps the microphone capture open; the fleet "
                "has no microphones"),
    "reducemouselag": ("true", "makes the CPU wait for the GPU every frame - "
                       "input hesitation and stutter, worst on ATI cards"),
    "cachesizemegs": ("32", "the 32 MB default thrashes on these maps; 128 on "
                      "every box UT2003/2004 reaches"),
}
_UE2_DEAD_MASTERS = ("epicgames.com",)


def ue2_config_problems(sysdir):
    """Settings in a staged UT2003/UT2004 ini that make it stutter or keep it
    offline. A WARN, not a FAIL: the title still deploys and runs."""
    out = []
    for name in UE2_INIS:
        path = find_ci(sysdir, name)
        if not path:
            continue
        try:
            with open(path, "rb") as fh:
                text = fh.read().decode("latin-1")
        except OSError:
            continue
        for line in text.splitlines():
            if "=" not in line or line.lstrip().startswith(";"):
                continue
            k, v = line.split("=", 1)
            k, v = k.strip().lower(), v.strip().lower()
            bad = _UE2_BAD.get(k)
            if bad and v == bad[0]:
                out.append("%s %s=%s - %s" % (name, line.split("=", 1)[0].strip(),
                                              line.split("=", 1)[1].strip(), bad[1]))
            if (k.startswith("masterserveraddress") or k == "masterserverlist") and \
                    any(d in v for d in _UE2_DEAD_MASTERS):
                out.append("%s %s - Epic's masters are gone; point it at "
                           "utmaster.openspy.net:28902" % (name, line.strip()))
            if k == "masterserverlist" and ",group=" in v:
                out.append("%s %s - 3369 has no Group member (logs 'Unknown "
                           "member Group in MasterServerList' at every start)"
                           % (name, line.strip()))
    return sorted(set(out))


# --- id-engine configs ------------------------------------------------------
# Quake 1/2/3, SiN, SoF/SoF2, Hexen II, Jedi Academy, RTCW, GoldSrc and DOOM 3
# split a config line into commands at every ';' that is outside double quotes
# BEFORE the tokenizer looks for '//'. So a semicolon in a comment runs the
# rest of the line as a console command. MEASURED on .243's Quake II console at
# every start: `Can't "cmd", not connected` (from "invuse is its action key")
# and `Unknown command "nobody"` (from "nobody wants 1997 walk speed"). Prose
# today; a real command the day a comment starts with one. The rule is scoped
# to titles that carry an id-family engine, by the executable in the tree:
# LithTech (Shogo) and CryEngine (Far Cry) are other parsers.
ID_ENGINE_EXE_RE = re.compile(
    r"^(quake|glquake|winquake|glqwcl|qwcl|quake2|quake3|ioquake3[^/\\]*|sin|sof|"
    r"sof2|sof2mp|glh2|h2|hexen2|jasp|jamp|jk2sp|jk2mp|wolfsp|wolfmp|hl|hlds|"
    r"doom3|kingpin)\.exe$", re.I)
# What an id engine writes at the top of the config it saves on exit. A staged
# copy is one machine's state, and GAMESYNC copies it back over every box's own
# on each sync that walks the title (resolution, sound rate, binds - all of it).
GAME_WRITTEN_CFG_HEADER = "// generated by quake, do not modify"


def is_id_engine_title(tdir):
    """True when the title root (or one level down) carries an id-family exe."""
    for root, dirs, files in os.walk(tdir):
        rel = os.path.relpath(root, tdir)
        depth = 0 if rel == "." else rel.count(os.sep) + 1
        if depth >= 1:
            dirs[:] = []
        if any(ID_ENGINE_EXE_RE.match(f) for f in files):
            return True
    return False


def comment_semicolon(line):
    """True when a ';' outside double quotes follows a '//' outside double
    quotes - Cbuf_Execute splits there and runs the rest. Quote parity counts
    from the start of the line, as the engine counts it. Mirrored by
    scripts/fleet/stage-fleetres.py comment_semicolons(), which fixes it."""
    q = 0
    in_comment = False
    i = 0
    while i < len(line):
        c = line[i]
        if c == '"':
            q += 1
        elif not in_comment and c == "/" and line[i:i + 2] == "//" and q % 2 == 0:
            in_comment = True
            i += 2
            continue
        elif in_comment and c == ";" and q % 2 == 0:
            return True
        i += 1
    return False


def id_engine_cfg_problems(tdir):
    """[(severity, check, detail)] for the .cfg files of an id-engine title."""
    out = []
    if not is_id_engine_title(tdir):
        return out
    for root, dirs, files in os.walk(tdir):
        dirs[:] = [d for d in dirs if not d.startswith("_")]
        for fn in files:
            if not fn.lower().endswith(".cfg"):
                continue
            path = os.path.join(root, fn)
            rel = os.path.relpath(path, tdir)
            text = read_text(path)
            for n, line in enumerate(text.splitlines(), 1):
                if comment_semicolon(line):
                    out.append(("fail", "config-semicolon",
                                "%s line %d has a ';' inside a // comment - the "
                                "engine splits commands there and runs the rest "
                                "of the line: %s" % (rel, n, line.strip()[:70])))
            first = next((l.strip() for l in text.splitlines() if l.strip()), "")
            if first.lower().startswith(GAME_WRITTEN_CFG_HEADER):
                out.append(("warn", "per-box-config",
                            "%s is the engine's own saved config (%r) - one "
                            "machine's state, which GAMESYNC copies back over "
                            "every box's own at each sync. Stop shipping it"
                            % (rel, first[:40])))
    return out


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

    try:
        with open(os.path.join(tdir, "requires.json"), encoding="utf-8") as fh:
            req = json.load(fh)
        req = req if isinstance(req, dict) else {}
    except (OSError, ValueError):
        req = {}
    icons_9x_cannot_draw = set()

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
            else:
                prob = icon_xp_problem(ipath)
                if prob:
                    (fail if prob[0] == "fail" else warn)(
                        "icon", "icon %r %s" % (icon, prob[1]))
                prob9 = icon_9x_problem(ipath)
                min_os, max_os = shortcut_os_range(req, target)
                if prob9 and max_os == "win9x":
                    # A shortcut that exists ONLY for 9x boxes, with an icon
                    # no 9x box can draw: it defeats its own purpose.
                    fail("icon", "%r is a Windows 9x shortcut, and its icon %r "
                                 "%s" % (target, icon, prob9))
                elif prob9 and min_os in (None, "win9x"):
                    icons_9x_cannot_draw.add(icon)

    # One line per title, not per shortcut: today the gate keeps every one of
    # these off the only Win9x box (a CPU floor or an operator override), so
    # this is a latent defect for the next 9x box, not a broken desktop.
    for icon in sorted(icons_9x_cannot_draw):
        warn("icon", "icon %r %s - a Windows 9x box that receives this title "
                     "would show it" % (icon, icon_9x_problem(os.path.join(
                         tdir, icon.replace("\\", os.sep)))))

    # --- Unreal Engine User.ini: an installer stub unbinds every key --------
    for sysname in ("System", "SYSTEM"):
        sysdir = find_ci(tdir, sysname)
        if sysdir:
            why = ue_userini_problem(sysdir)
            if why:
                fail("user.ini", why)
            why = ue_runtime_log_problem(sysdir)
            if why:
                fail("runtime-log", why)
            for why in ue2_config_problems(sysdir):
                warn("ue2-config", why)
            break

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
    #
    # A HELPER .bat INHERITS FR_* FROM THE LAUNCHER THAT CALLED IT, and this
    # check has to know that or it reports a working title as broken.
    # Quake III's FLEETGL.BAT is exactly that: every "Play ....bat" in the tree
    # does `call FLEETRES.BAT` and then `call FLEETGL.BAT`, so FR_W and FR_H are
    # already in the environment by the time the helper reads them. Requiring
    # every file that mentions %FR_* to call the block itself failed the whole
    # library on that one file - and a validator that cries wolf is one people
    # learn to ignore, which is the same argument that demoted the REGEDIT5
    # check to a warning.
    #
    # So the rule is: a file may inherit FR_* if some OTHER .bat in the same
    # tree calls it by name and that caller does satisfy the block. A helper
    # nothing calls is still a failure - it would run with empty variables.
    callers = {}
    for b, body in bodies.items():
        low = body.lower()
        for other in bodies:
            if other == b:
                continue
            if re.search(r"\bcall\b[^\r\n]*" + re.escape(other.lower()), low):
                callers.setdefault(other, []).append(b)

    for b, body in bodies.items():
        # The CALL, not the mention. Testing for the bare name "FLEETRES.BAT"
        # passes a launcher that only names it in a comment — and a good
        # comment DOES name it, to say where the resolution comes from.
        if "%FR_" not in body or FLEETRES_CALL_RE.search(body):
            continue
        inherited = [c for c in callers.get(b, [])
                     if FLEETRES_CALL_RE.search(bodies[c])]
        if inherited:
            continue
        if callers.get(b):
            fail("fleetres", "%r uses %%FR_*%% and is called only by %s, which "
                             "does not call FLEETRES.BAT either — so every one "
                             "of those expands to nothing"
                             % (b, ", ".join(sorted(callers[b]))))
        else:
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
        # the way back may MOVE or COPY: since 2026-10-01 the library ships the
        # wrapper as .nglide and the generated launchers copy it in
        if not re.search(r'(?:move|copy)\s+/y\s+"[^"]*\.nglide"\s+"[^"]*glide2x\.dll"',
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

    # ---- id-engine configs: comment semicolons, staged game-written state --
    for severity, check, detail in id_engine_cfg_problems(tdir):
        (fail if severity == "fail" else warn)(check, detail)

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


def check_icon_names_9x(lib, titles):
    """Windows 98's shell caches an icon by its FILE NAME, not its path.

    Measured on .243 (2026-09-29): 39 Flight-* titles each shipped their own
    ICON.ICO, at 39 different paths, and every one of their desktop icons drew
    F-14 Fleet Defender's art. Uniquely named icons on the same desktop
    (DESCENT9.ICO, hexen2.ico, Q2.ico) drew correctly. So two Windows 9x
    shortcuts whose icons share a file name but differ in content FAIL."""
    seen = {}                      # NAME.ICO -> {md5: [title, ...]}
    for t in titles:
        tdir = os.path.join(lib, t)
        lpath = os.path.join(tdir, "launch.txt")
        if not os.path.isfile(lpath):
            continue
        try:
            with open(os.path.join(tdir, "requires.json"), encoding="utf-8") as fh:
                req = json.load(fh)
            req = req if isinstance(req, dict) else {}
        except (OSError, ValueError):
            req = {}
        for line in read_text(lpath)[:LAUNCH_TXT_READ_LIMIT].splitlines():
            if not line.strip() or line.strip().startswith("#"):
                continue
            parts = line.rstrip("\r\n").split("\t")
            icon = parts[2].strip() if len(parts) >= 3 else ""
            if not icon.lower().endswith(".ico"):
                continue
            if shortcut_os_range(req, parts[0].strip())[1] != "win9x":
                continue
            ipath = os.path.join(tdir, icon.replace("\\", os.sep))
            try:
                with open(ipath, "rb") as fh:
                    digest = hashlib.md5(fh.read()).hexdigest()
            except OSError:
                continue                # check_title() reports a missing icon
            name = os.path.basename(icon.replace("\\", "/")).upper()
            seen.setdefault(name, {}).setdefault(digest, set()).add(t)
    out = []
    for name, by_md5 in sorted(seen.items()):
        if len(by_md5) < 2:
            continue
        owners = sorted(set().union(*by_md5.values()))
        for t in owners:
            out.append(Problem(t, "fail", "icon", (
                "its Windows 9x shortcut's icon is named %s, like %d other title(s)' "
                "different icons (%s) - Windows 98 caches icons by FILE NAME, so "
                "all of them show one picture. Give each title its own icon name"
                % (name, len(owners) - 1, ", ".join(o for o in owners if o != t)[:200]))))
    return out


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
    problems.extend(check_icon_names_9x(lib, titles))

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
