#!/usr/bin/env python3
"""DOOM 3 1.3 - make the monitor's own mode SELECTABLE in the in-game menu.

THE PROBLEM (verified 2026-09-29, .claude/evidence-1080p/_results/titles-verified.json
"Doom3"). The fleet launcher already starts DOOM 3 at the panel's mode -
`+set r_mode -1 +set r_customWidth %FR_W% +set r_customHeight %FR_H%`, and id
Tech 4 re-applies +set after DoomConfig.cfg - but the menu cannot express it:

    System > Screen Size   choiceDef OS2Primary
        choices "#str_04222"   = 640x480;800x600;1024x768;1152x864;1280x1024;1600x1200
        values  "3;4;5;6;7;8"  cvar r_mode   choiceType 1

With r_mode -1 no value matches, so the control shows its index 0
("640x480"), and touching it moves the game off the panel's mode with no way
back short of relaunching. There is no 1920x1080 entry in the engine's
r_vidModes table (Mode 0..8 end at 1600x1200) - r_mode -1 is the only route.

THE FIX. A new pk4, base/zz_fleetvideo.pk4, holding copies of the two stock
GUIs that carry that choiceDef, each with exactly TWO lines changed:

    choices  ...;1600x1200;Native      values  ...;8;-1

  * guis/mainmenu.gui  (from pak007.pk4) - the main menu, System page.
  * guis/mpmain.gui    (from pak007.pk4) - the in-game multiplayer ESC menu,
    which has its own identical Screen Size control. The diagnosis named only
    mainmenu.gui; without mpmain the LAN game's menu still shows "640x480".

"Native" rather than "1920x1080": r_customWidth/Height are written per box by
FLEETRES (1920x1080 on the four LCDs, other modes on the CRTs), so the label
must not promise one number.

The choices are written LITERALLY (the stock text of #str_04222 plus
";Native") because a #str_ reference cannot be extended; stock GUIs use
literal choices beside #str ones ("No;Yes", "best;oss;alsa"). A negative value
is also a stock pattern: the invert-mouse choiceDef is
`values "0.022;-0.022"` on cvar m_pitch.

WHY zz_ (read in id's GPL source, neo/framework/FileSystem.cpp, not recalled):
AddGameDirectory() lists base/*.pk4, sorts them with idStrList::Sort (whose
comparator is idStr::Icmp - CASE-INSENSITIVE) and inserts each pak IMMEDIATELY
AFTER its directory, so the last name in sorted order is searched first ("sort
them so that later alphabetic matches override earlier ones"). pak000/005/006/
007 each ship mainmenu.gui and pak007's is the one the game shows, which is that
rule observed. `zz_fleetvideo.pk4` sorts after every stock name in base/.

WHY A LITERAL LIST SHOWS THE SAME LABELS (ui/RegExp.cpp, ui/ChoiceWindow.cpp):
`choices` is a registered STRING var, so idRegisterList::AddReg translates the
WHOLE token with GetLanguageDict()->GetString() at parse time - "#str_04222"
becomes "640x480;...;1600x1200" before idChoiceWindow lexes it. A literal equal
to that text (plus ";Native") is lexed identically. `values` is not translated;
UpdateChoicesAndVals' negNum branch turns the tokens "-" "1" into "-1", and
UpdateChoice (choiceType 1) Icmp-matches the cvar string "-1" -> index 6,
"Native". Stock: no match, so index 0, "640x480". engine_values() and
engine_current_choice() mirror both, and --check prints the result.

MULTIPLAYER / PURE (FileSystem.cpp pureExclusions + GetPackStatus; the retail
1.3 DOOM3.exe carries the same table - ".pda" ".gui" ".pd" "sound/VO" sit
together in its .rdata): every *.gui is pure-EXCLUDED, and a pak whose members
are all excluded is PURE_NEVER. UpdatePureServerChecksums skips PURE_NEVER paks
and OpenFileReadFlags never filters one out in pure mode. So this pk4 is never
on any pure list, stays active while connected to a pure server, and a pure
listen server cannot require it from joiners. That matters here: "Host DOOM 3 -
LAN.bat" never sets si_pure, whose default is 1, so every fleet LAN game is a
PURE server. THE PK4 MUST STAY GUI-ONLY: one non-excluded member (a readme.txt
is enough) makes it PURE_NEUTRAL, and because idMultiplayerGame::Reset reads
guis/mpmain.gui during map load the pak would be referenced and join the host's
pure list, locking every box without the identical pk4 out of the game.
--build refuses that (require_pure_never).

NOT DONE, deliberately: an r_aspectRatio control. There is no free row in the
Video box (Screen Size 125, Fullscreen 150, Brightness 175, box ends at 203),
so it would mean re-laying out stock windows; and the launcher sets
r_aspectRatio on every start from the measured panel, which id Tech 4
re-applies after the config, so a menu value would not survive a relaunch.

PREMISE: "Native" means r_mode -1, i.e. whatever r_customWidth/Height hold.
Both staged launchers pass +set r_mode -1 +set r_customWidth %FR_W% +set
r_customHeight %FR_H%; --check fails if either stops doing so.

Modes (only --check and --build are meant to be run in the build phase):
    --check                 verify the staged originals and every anchor byte
    --build [OUTDIR]        write zz_fleetvideo.pk4 + diffs + manifest.json
    --publish [OUTDIR]      FUTURE: put the pk4 on the share via sharewrite.py
    --install-server        NOT NEEDED for a PURE_NEVER pk4 (says so and copies
                            nothing); copies into ~/doom3-server/base only if a
                            future pk4 ever carries non-GUI content

Game data is copyrighted: the built pk4 goes to ~/.retro-fleet/patch-out/doom3,
never into git. This script regenerates it from the staged pak007.pk4.
"""
import argparse
import difflib
import hashlib
import io
import json
import os
import re
import shutil
import subprocess
import sys
import zipfile

KEY = "doom3"
TITLE = "Doom3"
LIB_REL = "Files/Games-Library"
MNT = "/mnt/retro-share"
LIBRARY = os.path.join(MNT, LIB_REL)
DEFAULT_OUT = os.path.expanduser("~/.retro-fleet/patch-out/%s" % KEY)
BACKUP_DIR = "originals-2026-09-29"
HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
SHAREWRITE = os.path.join(REPO, "scripts", "fleet", "sharewrite.py")
SERVER_BASE = os.path.expanduser("~/doom3-server/base")
SERVER_UNIT = "doom3-server"

PK4_NAME = "zz_fleetvideo.pk4"
PK4_REL = "base/" + PK4_NAME            # relative to the title directory

# The staged original the GUIs are taken from.
SOURCE_PAK = {
    "rel": "base/pak007.pk4",
    "md5": "6319f086f930ec1618ab09b4c20c268c",
    "size": 192031,
}

# Fixed zip timestamp so a rebuild is byte-identical (the md5 of the pk4
# depends only on the inputs and zlib).
ZIP_DATE = (2026, 9, 29, 0, 0, 0)

NATIVE_LABEL = "Native"
STOCK_MODES = "640x480;800x600;1024x768;1152x864;1280x1024;1600x1200"  # #str_04222

# One entry per member. Every edit is an exact byte replacement that must occur
# EXACTLY ONCE in the member, on the named 1-based line, inside the
# `choiceDef OS2Primary` block that binds cvar "r_mode".
MEMBERS = [
    {
        "name": "guis/mainmenu.gui",
        "size": 696570,
        "crc": 0xB34AA79A,
        "md5": "e90e5f71de393bc74622c82ec7c57e60",
        "eol": b"\n",
        "edits": [
            {"line": 4889,
             "old": b'\t\t\t\tchoices\t\t"#str_04222"//320x240;400x300;512x384;',
             "new": b'\t\t\t\tchoices\t\t"' + STOCK_MODES.encode() + b";" + NATIVE_LABEL.encode()
                    + b'"//320x240;400x300;512x384;'},
            {"line": 4890,
             "old": b'\t\t\t\tvalues\t\t"3;4;5;6;7;8"//0;1;2;',
             "new": b'\t\t\t\tvalues\t\t"3;4;5;6;7;8;-1"//0;1;2;'},
        ],
    },
    {
        "name": "guis/mpmain.gui",
        "size": 142478,
        "crc": 0x9E451C89,
        "md5": "c7adb0744ee918263132201c55b3d890",
        "eol": b"\r\n",
        "edits": [
            {"line": 4446,
             "old": b'\t\t\t\t\tchoices\t"#str_04222"',
             "new": b'\t\t\t\t\tchoices\t"' + STOCK_MODES.encode() + b";" + NATIVE_LABEL.encode() + b'"'},
            {"line": 4447,
             "old": b'\t\t\t\t\tvalues\t"3;4;5;6;7;8"',
             "new": b'\t\t\t\t\tvalues\t"3;4;5;6;7;8;-1"'},
        ],
    },
]


class PatchError(Exception):
    pass


def md5_bytes(b):
    return hashlib.md5(b).hexdigest()


def md5_file(path):
    h = hashlib.md5()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


# ---------------------------------------------------------------- pure logic

def line_offsets(data):
    """1-based line number -> byte offset of that line's first byte."""
    offs = {1: 0}
    n = 1
    i = data.find(b"\n")
    while i != -1:
        n += 1
        offs[n] = i + 1
        i = data.find(b"\n", i + 1)
    return offs


def choice_block(data, anchor_off):
    """Return (start, end) of the `choiceDef <name> {...}` block holding anchor_off.

    Walks back to the nearest 'choiceDef' and forward by brace depth. Used to
    prove an edit sits in the Screen Size control and nowhere else."""
    start = data.rfind(b"choiceDef", 0, anchor_off)
    if start == -1:
        raise PatchError("no choiceDef before offset %d" % anchor_off)
    brace = data.find(b"{", start)
    if brace == -1 or brace > anchor_off:
        raise PatchError("choiceDef at %d has no opening brace before the anchor" % start)
    depth = 0
    i = brace
    while i < len(data):
        c = data[i:i + 1]
        if c == b"{":
            depth += 1
        elif c == b"}":
            depth -= 1
            if depth == 0:
                return start, i + 1
        i += 1
    raise PatchError("unterminated choiceDef at %d" % start)


def locate_edits(data, spec):
    """Verify every anchor of `spec` in `data`; return [(edit, byte_offset)]."""
    offs = line_offsets(data)
    found = []
    for e in spec["edits"]:
        n = data.count(e["old"])
        if n != 1:
            raise PatchError("%s: anchor for line %d occurs %d times (want 1): %r"
                             % (spec["name"], e["line"], n, e["old"]))
        at = data.find(e["old"])
        if offs.get(e["line"]) != at:
            raise PatchError("%s: anchor found at byte %d, expected line %d (byte %s)"
                             % (spec["name"], at, e["line"], offs.get(e["line"])))
        end = at + len(e["old"])
        if data[end:end + len(spec["eol"])] != spec["eol"]:
            raise PatchError("%s: line %d does not end with %r" % (spec["name"], e["line"], spec["eol"]))
        s, t = choice_block(data, at)
        block = data[s:t]
        if not block.startswith(b"choiceDef OS2Primary") or b'"r_mode"' not in block:
            raise PatchError("%s: line %d is not inside the r_mode choiceDef OS2Primary"
                             % (spec["name"], e["line"]))
        found.append((e, at))
    return found


def patch_member(data, spec):
    """Apply spec's edits to data; return the new bytes (anchors verified first)."""
    found = locate_edits(data, spec)
    out = data
    # apply back to front so earlier offsets stay valid
    for e, at in sorted(found, key=lambda x: -x[1]):
        out = out[:at] + e["new"] + out[at + len(e["old"]):]
    return out


def changed_lines(old, new):
    """Line numbers (1-based, in old) that differ; asserts no lines added/removed."""
    a = old.split(b"\n")
    b = new.split(b"\n")
    if len(a) != len(b):
        raise PatchError("line count changed: %d -> %d" % (len(a), len(b)))
    return [i + 1 for i, (x, y) in enumerate(zip(a, b)) if x != y]


def verify_patched(old, new, spec):
    """The patched member differs from the original on exactly the edited lines,
    and each of those lines now carries the intended bytes."""
    want = sorted(e["line"] for e in spec["edits"])
    got = changed_lines(old, new)
    if got != want:
        raise PatchError("%s: changed lines %s, expected exactly %s" % (spec["name"], got, want))
    offs = line_offsets(new)
    for e in spec["edits"]:
        at = offs[e["line"]]
        if new[at:at + len(e["new"])] != e["new"]:
            raise PatchError("%s: line %d does not carry the new bytes" % (spec["name"], e["line"]))
    return got


def parse_choice_list(s):
    """Split a choiceDef choices/values string on ';' the way the menu lists it."""
    return [p for p in s.split(";") if p != ""]


def choice_pairs(new_line_choices, new_line_values):
    """(label, value) pairs from the two patched lines - for the tests and --check."""
    def quoted(b):
        q1 = b.index(b'"')
        q2 = b.index(b'"', q1 + 1)
        return b[q1 + 1:q2].decode()
    labels = parse_choice_list(quoted(new_line_choices))
    values = parse_choice_list(quoted(new_line_values))
    if len(labels) != len(values):
        raise PatchError("choices/values length mismatch: %d vs %d" % (len(labels), len(values)))
    return list(zip(labels, values))


def pk4_sort_key(name):
    """DOOM 3 sorts base/*.pk4 names before prepending them, with idStr::Icmp
    (case-insensitive - idlib/containers/StrList.h). overrides_all() still
    demands the same answer both ways, so a wrong reading of that comparator
    could not flip the result. This key is only used to PRINT the order."""
    return name.lower()


def overrides_all(pk4, names):
    """True when `pk4` sorts after every other name BOTH case-sensitively and
    case-insensitively, i.e. its files win regardless of the comparison used."""
    others = [n for n in names if n.lower() != pk4.lower()]
    return all(pk4 > n for n in others) and all(pk4.lower() > n.lower() for n in others)


# ------------------------------------------ id Tech 4 rules, mirrored from GPL
# Each mirror cites the function it copies. They exist so the tests can pin the
# two properties this patch depends on: the pk4 is PURE_NEVER, and the menu
# selects "Native" for r_mode -1.

# FileSystem.cpp pureExclusions - the unconditional entries (excludeExtension
# for "/", "\\", ".pda", ".gui", ".pd", ".lang"; excludePathPrefixAndExtension
# for sound/VO *.ogg/*.wav). The DOOM3_PURE_SPECIAL_CASES block adds only more
# exclusions, so leaving it out can only under-report PURE_NEVER, never over-.
PURE_EXCLUDED_EXT = ("/", "\\", ".pda", ".gui", ".pd", ".lang")
PURE_EXCLUDED_PREFIX_EXT = (("sound/vo", ".ogg"), ("sound/vo", ".wav"))


def pure_excluded(name):
    """True when id's pure rules ignore this pak member (excludeExtension /
    excludePathPrefixAndExtension; names are lower-cased with / separators at
    load, LoadZipFile)."""
    n = name.replace("\\", "/").lower()
    for ext in PURE_EXCLUDED_EXT:
        if len(n) > len(ext) and n.endswith(ext):
            return True
    for prefix, ext in PURE_EXCLUDED_PREFIX_EXT:
        if len(n) > len(prefix) and n.endswith(ext) and n.startswith(prefix):
            return True
    return False


def pak_pure_status(pak_name, member_names):
    """Mirror of idFileSystemLocal::GetPackStatus: 'never' when EVERY member is
    excluded, else 'always' for a pak*-named file, else 'neutral'."""
    if all(pure_excluded(n) for n in member_names):
        return "never"
    if os.path.basename(pak_name).lower().startswith("pak"):
        return "always"
    return "neutral"


def searched_on_pure_server(status, on_server_list):
    """Mirror of the OpenFileReadFlags filter while serverPaks is set: a pak is
    skipped only if it is not PURE_NEVER and not on the server's list."""
    return status == "never" or on_server_list


def enters_pure_list(status, referenced):
    """Mirror of UpdatePureServerChecksums: PURE_NEVER never; PURE_NEUTRAL only
    when referenced during the map load; PURE_ALWAYS always."""
    if status == "never":
        return False
    if status == "neutral" and not referenced:
        return False
    return True


def require_pure_never(pak_name, member_names):
    """Refuse a pk4 that a pure server could put on its list (see the module
    docstring, MULTIPLAYER / PURE)."""
    st = pak_pure_status(pak_name, member_names)
    if st != "never":
        raise PatchError(
            "%s would be PURE_%s because of %s. Host DOOM 3 - LAN.bat runs a PURE listen "
            "server (si_pure defaults to 1) and reads guis/mpmain.gui at map load, so this "
            "pak would join its pure list and lock out every box without it. Keep the pk4 "
            "GUI-only." % (pak_name, st.upper(), [n for n in member_names if not pure_excluded(n)]))
    return st


def _lex_values(s):
    """The idLexer tokens a choiceDef `values` string produces - only the subset
    these menus use (integers/decimals, ';', '-'). Anything else raises rather
    than being modelled wrongly."""
    toks = []
    i = 0
    while i < len(s):
        c = s[i]
        if c in " \t\r\n":
            i += 1
        elif c.isdigit() or (c == "." and i + 1 < len(s) and s[i + 1].isdigit()):
            j = i
            while j < len(s) and (s[j].isdigit() or s[j] == "."):
                j += 1
            if j < len(s) and (s[j].isalpha() or s[j] == "_"):
                raise PatchError("unmodelled number/name run in values: %r" % s)
            toks.append(s[i:j])
            i = j
        elif c == ";":
            toks.append(c)
            i += 1
        elif c == "-":
            if s[i + 1:i + 2] in ("-", "=", ">"):
                raise PatchError("unmodelled punctuation in values: %r" % s)
            toks.append(c)
            i += 1
        else:
            raise PatchError("unmodelled character %r in values: %r" % (c, s))
    return toks


def engine_values(s):
    """Mirror of idChoiceWindow::UpdateChoicesAndVals, values branch (negNum)."""
    values = []
    cur = ""
    neg = False
    for t in _lex_values(s):
        if t == "-":
            neg = True
            continue
        if t == ";":
            if cur:
                values.append(cur.rstrip())
                cur = ""
            continue
        if neg:
            cur += "-"
            neg = False
        cur += t + " "
    if cur:
        values.append(cur.rstrip())
    return values


def engine_current_choice(cvar_string, values):
    """Mirror of idChoiceWindow::UpdateChoice, choiceType 1: the first value
    that Icmp-matches the cvar's string, else index 0."""
    for i, v in enumerate(values):
        if v.lower() == cvar_string.lower():
            return i
    return 0


def quoted_field(line):
    """The first "..." field of a GUI line (bytes -> str)."""
    q1 = line.index(b'"')
    return line[q1 + 1:line.index(b'"', q1 + 1)].decode()


# The launchers whose command line "Native" (r_mode -1) depends on.
LAUNCHERS = ("Play DOOM 3.bat", "Host DOOM 3 - LAN.bat")
LAUNCHER_NEEDS = ("+set r_mode -1", "+set r_customWidth %FR_W%", "+set r_customHeight %FR_H%")


def launcher_premise(bat_bytes):
    """The +set tokens LAUNCHER_NEEDS that a launcher is missing (case-insensitive)."""
    low = bat_bytes.decode("latin-1").lower()
    return [t for t in LAUNCHER_NEEDS if t.lower() not in low]


def build_pk4(members):
    """members: [(name, bytes)] -> deterministic zip bytes."""
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for name, data in members:
            zi = zipfile.ZipInfo(name, date_time=ZIP_DATE)
            zi.compress_type = zipfile.ZIP_DEFLATED
            zi.create_system = 0          # MS-DOS, like id's own pk4s
            zi.external_attr = 0x20       # FILE_ATTRIBUTE_ARCHIVE
            z.writestr(zi, data)
    return buf.getvalue()


# ----------------------------------------------------------- share-side work

def title_dir(library):
    return os.path.join(library, TITLE)


def read_source(library):
    """Open the staged pak007.pk4, check md5/size, return {member: bytes}."""
    path = os.path.join(title_dir(library), SOURCE_PAK["rel"])
    if not os.path.isfile(path):
        raise PatchError("staged original missing: %s" % path)
    size = os.path.getsize(path)
    md5 = md5_file(path)
    if size != SOURCE_PAK["size"] or md5 != SOURCE_PAK["md5"]:
        raise PatchError("%s: size %d md5 %s, expected %d %s"
                         % (path, size, md5, SOURCE_PAK["size"], SOURCE_PAK["md5"]))
    out = {}
    with zipfile.ZipFile(path) as z:
        for spec in MEMBERS:
            info = z.getinfo(spec["name"])
            if info.file_size != spec["size"] or info.CRC != spec["crc"]:
                raise PatchError("%s:%s size %d crc %08x, expected %d %08x"
                                 % (path, spec["name"], info.file_size, info.CRC, spec["size"], spec["crc"]))
            data = z.read(spec["name"])
            if md5_bytes(data) != spec["md5"]:
                raise PatchError("%s:%s md5 %s, expected %s" % (path, spec["name"], md5_bytes(data), spec["md5"]))
            out[spec["name"]] = data
    return path, md5, size, out


def _ci_path(directory, name):
    """Resolve `name` in `directory` ignoring case (Windows names; CLAUDE.md)."""
    p = os.path.join(directory, name)
    if os.path.exists(p):
        return p
    for n in os.listdir(directory):
        if n.lower() == name.lower():
            return os.path.join(directory, n)
    raise PatchError("%s not found in %s (case-insensitive)" % (name, directory))


def _lang_string(lang_text, key):
    """The value of `"<key>" "<value>"` in a Doom 3 .lang file, or None."""
    m = re.search(r'"%s"\s+"([^"]*)"' % re.escape(key), lang_text)
    return m.group(1) if m else None


def base_pk4s(library):
    d = os.path.join(title_dir(library), "base")
    return sorted(n for n in os.listdir(d) if n.lower().endswith(".pk4"))


def gui_owners(library, member):
    """Every base/*.pk4 that carries `member` (case-insensitive), in engine order."""
    d = os.path.join(title_dir(library), "base")
    owners = []
    for n in sorted(base_pk4s(library), key=pk4_sort_key):
        if n.lower() == PK4_NAME.lower():
            continue
        try:
            with zipfile.ZipFile(os.path.join(d, n)) as z:
                if any(i.filename.lower() == member.lower() for i in z.infolist()):
                    owners.append(n)
        except zipfile.BadZipFile:
            owners.append(n + " (UNREADABLE)")
    return owners


def cmd_check(library):
    print("DOOM 3 zz_fleetvideo - check against %s" % title_dir(library))
    path, md5, size, members = read_source(library)
    print("original  %s  %d B  md5 %s  OK" % (SOURCE_PAK["rel"], size, md5))
    for spec in MEMBERS:
        data = members[spec["name"]]
        print("  member %s  %d B  crc %08x  md5 %s  OK"
              % (spec["name"], len(data), spec["crc"], spec["md5"]))
        for e, at in locate_edits(data, spec):
            print("    line %d @ byte %d (0x%x): %r  OK (inside choiceDef OS2Primary / r_mode)"
                  % (e["line"], at, at, e["old"].decode()))
        new = patch_member(data, spec)
        verify_patched(data, new, spec)
        c = [e for e in spec["edits"] if b"choices" in e["old"]][0]
        v = [e for e in spec["edits"] if b"values" in e["old"]][0]
        print("    patched menu: %s" % ", ".join("%s=%s" % p for p in choice_pairs(c["new"], v["new"])))
        labels = parse_choice_list(quoted_field(c["new"]))
        stock_labels = parse_choice_list(STOCK_MODES)
        now = engine_current_choice("-1", engine_values(quoted_field(v["new"])))
        was = engine_current_choice("-1", engine_values(quoted_field(v["old"])))
        print("    engine (ChoiceWindow mirror): r_mode -1 -> choice %d %r; stock menu showed choice %d %r"
              % (now, labels[now], was, stock_labels[was]))
        if labels[now] != NATIVE_LABEL:
            raise PatchError("%s: r_mode -1 would not select %r" % (spec["name"], NATIVE_LABEL))
    # the labels must be exactly what the stock control shows today
    with zipfile.ZipFile(path) as z:
        lang = z.read("strings/english.lang").decode("latin-1")
    stock = _lang_string(lang, "#str_04222")
    if stock != STOCK_MODES:
        raise PatchError("#str_04222 in pak007 strings/english.lang is %r, not %r" % (stock, STOCK_MODES))
    print("  #str_04222 (pak007 strings/english.lang) == the literal's first six labels  OK")
    st = require_pure_never(PK4_NAME, [s["name"] for s in MEMBERS])
    print("  pure: %s is PURE_%s (every member is *.gui) - never on a pure list, active on pure "
          "servers, so a Host LAN game (si_pure defaults to 1) cannot require it  OK" % (PK4_NAME, st.upper()))
    for bat in LAUNCHERS:
        p = _ci_path(title_dir(library), bat)
        with open(p, "rb") as f:
            missing = launcher_premise(f.read())
        if missing:
            raise PatchError("%s no longer passes %s - 'Native' (r_mode -1) would not mean the "
                             "panel's mode" % (bat, missing))
        print("  launcher %s passes %s  OK" % (bat, " ".join(LAUNCHER_NEEDS)))
    names = base_pk4s(library)
    print("base/*.pk4 (engine order, last wins): %s" % ", ".join(sorted(names, key=pk4_sort_key)))
    if not overrides_all(PK4_NAME, names):
        raise PatchError("%s would NOT sort after every pk4 in base/" % PK4_NAME)
    print("precedence: %s sorts after all %d stock paks (case-sensitive and -insensitive)  OK"
          % (PK4_NAME, len([n for n in names if n.lower() != PK4_NAME.lower()])))
    for spec in MEMBERS:
        owners = gui_owners(library, spec["name"])
        print("  %s is shipped by %s -> effective today: %s; after fix: %s"
              % (spec["name"], ", ".join(owners) or "(none)", owners[-1] if owners else "-", PK4_NAME))
        if not owners or owners[-1] != os.path.basename(SOURCE_PAK["rel"]):
            raise PatchError("%s: the effective stock copy is %s, not pak007 - rebase the patch"
                             % (spec["name"], owners[-1] if owners else None))
    target = os.path.join(title_dir(library), PK4_REL)
    if os.path.exists(target):
        print("share already has %s: md5 %s" % (PK4_REL, md5_file(target)))
    else:
        print("share has no %s yet (new file; nothing to back up)" % PK4_REL)
    return 0


def cmd_build(library, outdir):
    path, md5, size, members = read_source(library)
    outdir = os.path.abspath(os.path.expanduser(outdir))
    built = []
    diffs = []
    for spec in MEMBERS:
        old = members[spec["name"]]
        new = patch_member(old, spec)
        lines = verify_patched(old, new, spec)
        built.append((spec["name"], new))
        d = difflib.unified_diff(
            old.decode("latin-1").splitlines(True), new.decode("latin-1").splitlines(True),
            "pak007.pk4/" + spec["name"], PK4_NAME + "/" + spec["name"], n=6)
        diffs.append((spec["name"], "".join(d), lines, md5_bytes(new)))
    blob = build_pk4(built)
    # read the archive back: members intact, exactly these names, CRCs good
    with zipfile.ZipFile(io.BytesIO(blob)) as z:
        if z.testzip() is not None:
            raise PatchError("built pk4 fails its own CRC test")
        if sorted(z.namelist()) != sorted(n for n, _ in built):
            raise PatchError("built pk4 members %s" % z.namelist())
        for n, data in built:
            if z.read(n) != data:
                raise PatchError("built pk4 member %s does not read back" % n)
        pure = require_pure_never(PK4_NAME, z.namelist())
    local = os.path.join(outdir, TITLE, PK4_REL)
    os.makedirs(os.path.dirname(local), exist_ok=True)
    with open(local, "wb") as f:
        f.write(blob)
    if md5_file(local) != md5_bytes(blob):
        raise PatchError("write of %s did not read back" % local)
    ddir = os.path.join(outdir, "diffs")
    os.makedirs(ddir, exist_ok=True)
    for name, text, lines, _ in diffs:
        with open(os.path.join(ddir, os.path.basename(name) + ".diff"), "w") as f:
            f.write(text)
    manifest = {
        "key": KEY,
        "title": TITLE,
        "note": "new file; derived from pak007.pk4's GUIs with only the Screen Size choiceDef edited",
        "originals": [{"share_path": "%s/%s/%s" % (LIB_REL, TITLE, SOURCE_PAK["rel"]),
                       "md5": md5, "size": size}],
        "outputs": [{
            "share_path": "%s/%s/%s" % (LIB_REL, TITLE, PK4_REL),
            "local_path": local,
            "md5": md5_bytes(blob),
            "size": len(blob),
            "original_md5": None,          # the share has no such file today
            "derived_from": {"share_path": "%s/%s/%s" % (LIB_REL, TITLE, SOURCE_PAK["rel"]),
                             "md5": md5},
            "members": [{"name": n, "changed_lines": l, "md5": m,
                         "stock_md5": [s for s in MEMBERS if s["name"] == n][0]["md5"]}
                        for n, _, l, m in diffs],
            "pure_status": "PURE_%s" % pure.upper(),
        }],
        "server": {"path": os.path.join(SERVER_BASE, PK4_NAME), "unit": SERVER_UNIT,
                   "needed": "no - the pk4 is PURE_NEVER (GUI-only): it never enters a pure list and a "
                             "client's copy is used even on a pure server; the dedicated server draws no "
                             "menu. (~/doom3-server also runs +set si_pure 0.)"},
    }
    with open(os.path.join(outdir, "manifest.json"), "w") as f:
        json.dump(manifest, f, indent=2)
    print("built %s  %d B  md5 %s" % (local, len(blob), md5_bytes(blob)))
    print("  pure status: PURE_%s (GUI-only - never on a pure list)" % pure.upper())
    for name, _, lines, m in diffs:
        print("  %s  changed lines %s  md5 %s" % (name, lines, m))
    print("diffs    %s" % ddir)
    print("manifest %s" % os.path.join(outdir, "manifest.json"))
    return 0


def _share_abs(share_path):
    return os.path.join(MNT, share_path)


def _share_md5(share_path):
    p = _share_abs(share_path)
    return md5_file(p) if os.path.isfile(p) else None


def _put(local, share_path, dry_run):
    cmd = [sys.executable, SHAREWRITE, "put", local, share_path]
    if dry_run:
        cmd.append("--dry-run")
    print("+ " + " ".join('"%s"' % c if " " in c else c for c in cmd))
    return subprocess.call(cmd)


def load_manifest(outdir):
    outdir = os.path.abspath(os.path.expanduser(outdir))
    with open(os.path.join(outdir, "manifest.json")) as f:
        m = json.load(f)
    for o in m["outputs"]:
        if md5_file(o["local_path"]) != o["md5"]:
            raise PatchError("%s no longer matches its manifest md5 - rebuild" % o["local_path"])
    return m


def cmd_publish(library, outdir, dry_run):
    """FUTURE USE. One file at a time; stop on the first failure; idempotent."""
    read_source(library)                      # the staged original must still be the one we built from
    m = load_manifest(outdir)
    for o in m["outputs"]:
        live = _share_md5(o["share_path"])
        if live == o["md5"]:
            print("skip %s: share already has the patched md5" % o["share_path"])
            continue
        if live is not None:
            # something else sits at that name: keep it before replacing it
            rel = o["share_path"][len(LIB_REL) + 1:]          # Doom3/base/zz_fleetvideo.pk4
            sub = rel.split("/", 1)[1]
            backup = "%s/_patches/%s/%s/%s" % (LIB_REL, TITLE, BACKUP_DIR, sub)
            if _share_md5(backup) is None:
                print("backing up the existing %s (md5 %s) to %s" % (o["share_path"], live, backup))
                if _put(_share_abs(o["share_path"]), backup, dry_run) != 0:
                    print("FAILED: backup of %s - stopping" % o["share_path"])
                    return 1
        else:
            print("%s is new on the share - no original to back up" % o["share_path"])
        if _put(o["local_path"], o["share_path"], dry_run) != 0:
            print("FAILED: %s - stopping; nothing after it was attempted" % o["share_path"])
            return 1
        if not dry_run and _share_md5(o["share_path"]) != o["md5"]:
            print("FAILED: %s did not read back with md5 %s through /mnt" % (o["share_path"], o["md5"]))
            return 1
    if dry_run:
        print("dry-run: nothing was written to the share.")
        return 0
    print("published. Run: python3 scripts/validate-staged-library.py --quiet, then GAMESYNC the boxes.")
    return 0


def cmd_install_server(outdir, dry_run):
    """FUTURE USE, and NOT NEEDED for this pk4. A GUI-only pk4 is PURE_NEVER: it
    never enters a pure server's list (so a server cannot require it), a client's
    copy stays active even on a pure server, and a dedicated server draws no
    menu. So for PURE_NEVER this reports that and copies nothing. Only if a
    future revision carried non-GUI content (PURE_NEUTRAL - which --build now
    refuses) would a pure server need the file, and then this copies it."""
    m = load_manifest(outdir)
    o = m["outputs"][0]
    with zipfile.ZipFile(o["local_path"]) as z:
        status = pak_pure_status(PK4_NAME, z.namelist())
    if status == "never":
        print("NOT NEEDED - nothing copied, nothing to restart: %s is PURE_NEVER (only *.gui "
              "members). It never enters a pure list, a client's copy is used even on a pure "
              "server, and the dedicated server draws no menu. (~/doom3-server also runs "
              "+set si_pure 0.)" % PK4_NAME)
        return 0
    dst = os.path.join(SERVER_BASE, PK4_NAME)
    if not os.path.isdir(SERVER_BASE):
        print("no server tree at %s" % SERVER_BASE)
        return 1
    if os.path.isfile(dst) and md5_file(dst) == o["md5"]:
        print("server already has %s (md5 %s)" % (dst, o["md5"]))
    elif dry_run:
        print("would copy %s -> %s" % (o["local_path"], dst))
        return 0
    else:
        tmp = dst + ".tmp"
        shutil.copyfile(o["local_path"], tmp)
        os.replace(tmp, dst)
        if md5_file(dst) != o["md5"]:
            print("FAILED: %s md5 mismatch after copy" % dst)
            return 1
        print("installed %s (md5 %s)" % (dst, o["md5"]))
    print("restart to load it:  systemctl --user restart %s" % SERVER_UNIT)
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--check", action="store_true")
    g.add_argument("--build", nargs="?", const=DEFAULT_OUT, metavar="OUTDIR")
    g.add_argument("--publish", nargs="?", const=DEFAULT_OUT, metavar="OUTDIR")
    g.add_argument("--install-server", nargs="?", const=DEFAULT_OUT, metavar="OUTDIR")
    ap.add_argument("--library", default=LIBRARY)
    ap.add_argument("--dry-run", action="store_true", help="with --publish/--install-server: change nothing")
    a = ap.parse_args(argv)
    try:
        if a.check:
            return cmd_check(a.library)
        if a.build is not None:
            return cmd_build(a.library, a.build)
        if a.publish is not None:
            return cmd_publish(a.library, a.publish, a.dry_run)
        return cmd_install_server(a.install_server, a.dry_run)
    except (PatchError, OSError, KeyError, zipfile.BadZipFile) as e:
        print("FAILED: %s" % e, file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
