#!/usr/bin/env python3
"""Jedi Knight DF2 / Mysteries of the Sith - JKMODE.EXE, the per-box display-mode helper.

THE DEFECT (.claude/evidence-1080p/_results/titles-verified.json "JediKnightDF2" /
"JediKnightMotS"): both Sith-engine titles start at 640x480 8-bpp software on
every 1080p box, because they keep their mode in HKLM as `displayMode` - an
INDEX into a DirectDraw mode list they build themselves (64 entries at most) -
plus `b3DAccel` and two device GUIDs, and nothing has ever stored them (the game
writes them only at a clean Quit; the boxes have only ever force-killed it).
The list is built in enumeration order, capped at 64, and then SORTED by
bpp/width/height (JK 0x422797 -> the CRT qsort at 0x513060, comparator
0x4249d0) - displayMode indexes the SORTED copy. MotS also drops every mode
with width >= 1400 and lPitch == width (its pass-2 callback, 0x428f9e) and runs
no ALLOWMODEX pass. So the index differs per box, per driver and per game: the
game's own code, run under emulation on the four measured enumeration
sequences, puts DF2's 1920x1080x16 at 31 on .123, 33 on .145 and 39 on .195
(MotS: 26 / 27 / 33 when lPitch = width * bpp / 8), and .240's X800 enumerates
it at position 76, past the cap. No staged constant can be right, and the fix
is a helper the launcher runs before every start.

THIS PATCHES NO GAME FILE. JKMODE.EXE is our own code (jkmode.c + jklogic.h,
built by build.sh); the staged JK.EXE / JKM.EXE stay byte-identical. What --check
asserts about them is that they are the binaries the helper was written
against: md5, size, and the exact bytes of every instruction and constant whose
behaviour jkmode.c reproduces (OFFSETS below - the coop flags per pass, the
64-mode / 16-device / 4-D3D-device caps, the 320-wide filter, MotS's
high-resolution 8-bpp filter and DF2's lack of one, the sort call, the
comparator and the qsort cutoff/pivot, IID_IDirect3D, the REG_BINARY writers,
the registry key and its "0.1" Version, the index used with no range check).

    apply.py --check                 assert the staged originals, report the title state
    apply.py --build [OUTDIR]        build JKMODE.EXE, verify its imports, write
                                     manifest.json (+ the reference launchers)
    apply.py --publish [--dry-run]   FUTURE: put JKMODE.EXE into both staged trees
                                     with scripts/fleet/sharewrite.py, one file at a
                                     time, backing up any existing copy first; stops
                                     on the first failure; skips a file already current
    apply.py --install-server        not applicable: the Sith engine has no server

Outputs go to ~/.retro-fleet/patch-out/jk-helper/ (outside git).
"""
import argparse
import hashlib
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile

KEY = "jk-helper"
LIBRARY = "/mnt/retro-share/Files/Games-Library"
SHARE_LIB_REL = "Files/Games-Library"
BACKUP_STAMP = "originals-2026-09-29"
DEFAULT_OUT = os.path.expanduser("~/.retro-fleet/patch-out/" + KEY)
HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
SHAREWRITE = os.path.join(REPO, "scripts", "fleet", "sharewrite.py")
BUILD_SH = os.path.join(HERE, "build.sh")
HELPER = "JKMODE.EXE"
ALLOWED_IMPORTS = {"KERNEL32.DLL", "USER32.DLL", "ADVAPI32.DLL"}

# ---------------------------------------------------------------------------
# What the helper was written against. (va, file offset, original bytes, meaning)
# Read from the staged originals 2026-09-29; every row is re-asserted by --check.
# ---------------------------------------------------------------------------
JK_OFFSETS = [
    (0x425b97, 0x024f97, "6a51", "push 0x51: pass-1 SetCooperativeLevel flags (ALLOWMODEX|EXCLUSIVE|FULLSCREEN)"),
    (0x425bb7, 0x024fb7, "6830604200", "push 0x426030: pass-1 callback (320-wide modes only)"),
    (0x425bd5, 0x024fd5, "6a11", "push 0x11: pass-2 SetCooperativeLevel flags"),
    (0x425beb, 0x024feb, "68305e4200", "push 0x425e30: pass-2 callback (every mode)"),
    (0x425c04, 0x025004, "a108b455006a0856508b08", "SetCooperativeLevel(8) after pass 2"),
    (0x425e37, 0x025237, "83ff40", "cmp edi,0x40: 64-mode cap, pass-2 callback"),
    (0x426036, 0x025436, "83f940", "cmp ecx,0x40: 64-mode cap, pass-1 callback"),
    (0x426059, 0x025459, "b840010000", "mov eax,0x140: pass 1 keeps only 320-wide modes"),
    (0x425c50, 0x025050, "83f810", "cmp eax,0x10: 16-entry device cap (DirectDrawEnumerate callback)"),
    (0x425d33, 0x025133, "6a11", "push 0x11: SetCooperativeLevel in the DirectDrawEnumerate callback"),
    (0x425d58, 0x025158, "ba6c010000", "mov edx,0x16c: DDCAPS size passed to GetCaps"),
    (0x425dac, 0x0251ac, "f644241801", "test [caps.dwCaps],1: DDCAPS_3D"),
    (0x42bd56, 0x02b156, "83f904", "cmp ecx,4: 4-entry Direct3D device cap"),
    (0x429616, 0x028a16, "6808285200", "push 0x522808: the IID QueryInterface asks for"),
    (0x522808, 0x120e08, "8000ba3b2124cf11a31a00aa00b93356", "IID_IDirect3D {3BBA0080-2421-11CF-A31A-00AA00B93356}"),
    (0x522bf8, 0x1211f8, "801a0587fc13d11197c000a024293005", "no-3D GUID {87051A80-13FC-11D1-97C0-00A024293005}"),
    (0x522210, 0x120810, "009fa692fa13d11197c000a024293005", "window device GUID {92A69F00-13FA-11D1-97C0-00A024293005}"),
    (0x414ac1, 0x013ec1, "396804742b3968087426f64024027420f6402810741a39",
     "3D device test: hw, persp, zbuf, RGB colour model, 16-bpp render"),
    (0x414ad7, 0x013ed7, "396810750a3968147505396818", "first-loop extra test: alpha / r14 / r18"),
    (0x414f87, 0x014387, "8b0da46555008b15a8655500c7052c05860001000000890d0005860089150805",
     "3D Acceleration ticked: device/3D device := the recommended pair, b3DAccel := 1"),
    (0x414ba6, 0x013fa6, "a104058600", "mov eax,[displayMode]: used as an index with no range check"),
    (0x50f5d7, 0x10e9d7, "6a03", "push 3: REG_BINARY (GUID writer)"),
    (0x50f335, 0x10e735, "6a04506a036a", "push 4 / push eax / push 3: REG_BINARY 4 bytes (displayMode writer)"),
    (0x50f4fc, 0x10e8fc, "6a04516a036a", "push 4 / push ecx / push 3: REG_BINARY 4 bytes (b3DAccel writer)"),
    (0x5250dc, 0x1228dc, "536f6674776172655c4c756361734172747320456e7465727461696e6d656e7420436f6d70616e"
                         "795c4a6564694b6e696768745c76312e3000", "registry key path"),
    (0x5250d8, 0x1228d8, "302e3100", '"0.1": the Version the game keeps (any other value: RegDeleteKey)'),
    (0x425e6e, 0x02526e, "8b46083d400100007512817e0cc80000007509c746040000403feb07",
     "pass-2 callback: only the 320x200 aspect case here - DF2 has NO high-res 8-bpp filter (MotS 0x428f9e does)"),
    (0x422797, 0x021b97, "8b15fcb3550068d04942006a545268c06c8600e8b1080f00",
     "qsort(0x866cc0, count, 0x54, 0x4249d0): the capped list is SORTED before the device record copies it"),
    (0x4249d0, 0x023dd0, "8b5424048b4c2408568b028b3185c0741585f675158b42088b51082bc2f7d81bc024fe405ec385f674"
                         "1685c075128b41088b4a082bc1f7d81bc083e002485ec38b42208b71203bc674042bc65ec38b42088b"
                         "71083bc674042bc65ec38b420c8b510c2bc25ec3",
     "the sort comparator: ModeX-vs-other by width only, else bpp, width, height (jk_mode_cmp)"),
    (0x5130bd, 0x1124bd, "83f808", "qsort CUTOFF 8: shortsort below 9 elements (jk_sort_modes)"),
    (0x513118, 0x112518, "d1e80fafc3", "qsort pivot: lo + (size/2)*width (jk_sort_modes)"),
]

MOTS_OFFSETS = [
    (0x428cc7, 0x0280c7, "6a11", "push 0x11: pass-1 SetCooperativeLevel flags (no ALLOWMODEX in MotS)"),
    (0x428ce7, 0x0280e7, "6860914200", "push 0x429160: pass-1 callback (320-wide modes only)"),
    (0x428d05, 0x028105, "6a11", "push 0x11: pass-2 SetCooperativeLevel flags"),
    (0x428d1b, 0x02811b, "68608f4200", "push 0x428f60: pass-2 callback (every mode)"),
    (0x428f67, 0x028367, "83ff40", "cmp edi,0x40: 64-mode cap, pass-2 callback"),
    (0x429166, 0x028566, "83f940", "cmp ecx,0x40: 64-mode cap, pass-1 callback"),
    (0x429189, 0x028589, "b840010000", "mov eax,0x140: pass 1 keeps only 320-wide modes"),
    (0x428d80, 0x028180, "83f810", "cmp eax,0x10: 16-entry device cap"),
    (0x428e63, 0x028263, "6a11", "push 0x11: SetCooperativeLevel in the DirectDrawEnumerate callback"),
    (0x428e88, 0x028288, "ba6c010000", "mov edx,0x16c: DDCAPS size passed to GetCaps"),
    (0x428edc, 0x0282dc, "f644241801", "test [caps.dwCaps],1: DDCAPS_3D"),
    (0x42ef16, 0x02e316, "83f904", "cmp ecx,4: 4-entry Direct3D device cap"),
    (0x42c7d6, 0x02bbd6, "6820a85700", "push 0x57a820: the IID QueryInterface asks for"),
    (0x57a820, 0x179020, "8000ba3b2124cf11a31a00aa00b93356", "IID_IDirect3D"),
    (0x57ac10, 0x179410, "801a0587fc13d11197c000a024293005", "no-3D GUID"),
    (0x57a228, 0x178a28, "009fa692fa13d11197c000a024293005", "window device GUID"),
    (0x41743a, 0x01683a, "396804742b3968087426f64024027420f6402810741a39",
     "3D device test: hw, persp, zbuf, RGB colour model, 16-bpp render"),
    (0x417450, 0x016850, "396810750a3968147505396818", "first-loop extra test: alpha / r14 / r18"),
    (0x41793a, 0x016d3a, "8b0dac115b008b15b0115b00b801000000890d801b9400a3ac1b94008915881b9400eb1d8b0da811",
     "3D Acceleration ticked: the recommended pair, b3DAccel := 1"),
    (0x41751f, 0x01691f, "a1841b9400", "mov eax,[displayMode]: used as an index with no range check"),
    (0x561ce7, 0x1610e7, "6a03", "push 3: REG_BINARY (GUID writer)"),
    (0x561a45, 0x160e45, "6a04506a036a", "push 4 / push eax / push 3: REG_BINARY 4 bytes (displayMode writer)"),
    (0x561c0c, 0x16100c, "6a04516a036a", "push 4 / push ecx / push 3: REG_BINARY 4 bytes (b3DAccel writer)"),
    (0x57d0dc, 0x17aadc, "536f6674776172655c4c756361734172747320456e7465727461696e6d656e7420436f6d70616e"
                         "79204c4c435c4d7973746572696573206f662074686520536974685c76312e3000", "registry key path"),
    (0x57d0d8, 0x17aad8, "302e3100", '"0.1": the Version the game keeps'),
    (0x428f9e, 0x02839e, "8b46083d780500007c128b5e143bc3750b5b5d5e5fc20800",
     "pass-2 callback: width >= 1400 && lPitch == width -> return without storing or counting (hires8_filter)"),
    (0x4258da, 0x024cda, "8b15ac6b5b0068007b42006a545268608b9400e81e6c1400",
     "qsort(0x948b60, count, 0x54, 0x427b00): the capped list is SORTED before the device record copies it"),
    (0x427b00, 0x026f00, "8b5424048b4c2408568b028b3185c0741585f675158b42088b51082bc2f7d81bc024fe405ec385f674"
                         "1685c075128b41088b4a082bc1f7d81bc083e002485ec38b42208b71203bc674042bc65ec38b42088b"
                         "71083bc674042bc65ec38b420c8b510c2bc25ec3",
     "the sort comparator (byte-identical to DF2's 0x4249d0)"),
    (0x56c56d, 0x16b96d, "83f808", "qsort CUTOFF 8"),
    (0x56c5c8, 0x16b9c8, "d1e80fafc3", "qsort pivot: lo + (size/2)*width"),
]

TITLES = {
    "JediKnightDF2": {
        "game": "df2", "exe": "JK.EXE", "md5": "85f8f883bbb7a167429879c899be1803", "size": 1388544,
        "offsets": JK_OFFSETS,
        "regkey": r"Software\LucasArts Entertainment Company\JediKnight\v1.0",
        "launcher": "Play Jedi Knight.bat",
        "display": "Jedi Knight - Dark Forces II",
        "label": "Jedi Knight: Dark Forces II",
    },
    "JediKnightMotS": {
        "game": "mots", "exe": "JKM.EXE", "md5": "192d2be2bfbe5a1acc9f562657a7d9ea", "size": 1760256,
        "offsets": MOTS_OFFSETS,
        "regkey": r"Software\LucasArts Entertainment Company LLC\Mysteries of the Sith\v1.0",
        "launcher": "Play Mysteries of the Sith.bat",
        "display": "Jedi Knight - Mysteries of the Sith",
        "label": "Jedi Knight: Mysteries of the Sith",
    },
}

# The index of 1920x1080x16 in each game's STORED (capped, sorted) list, for
# the enumeration sequences MEASURED on each box with ddenum.c
# (.claude/evidence-1080p/dark-sith-review/ddenum_*, copied to fixtures/).
# NOT measured as indices: they come from running the GAME'S OWN CODE - both
# EnumDisplayModes callbacks, the cap, MotS's filter, the CRT qsort and the
# comparator - under emulation on those sequences (emu_modelist.py; evidence
# in .claude/evidence-1080p/build-jk-helper/review/). The ddenum-era figures
# 45 / 34 / 58 / 43 were ENUMERATION positions, which the game never indexes.
# MotS depends on the lPitch the runtime reports for 8-bpp modes. XP SP3's
# ddraw.dll reports exactly width * bpp / 8 (its EnumDisplayModes thunk
# 0x73791f69: imul/shr 3, no alignment - review/xp_ddraw_enumdisplaymodes.asm.txt),
# so "mots" is the XP answer; Win7's ddraw (.195) was not examined, so a
# 32-byte-aligned report is kept as the other case. JKMODE reads the real value
# either way. None = cut by the 64 cap (.240: enumerated at position 76).
GAME_CODE_1080P = {
    "df2": {"123": 31, "145": 33, "195": 39, "240": None},
    "mots": {"123": 26, "145": 27, "195": 33, "240": None},            # lPitch = w * bpp / 8 (XP SP3)
    "mots_aligned32": {"123": 27, "145": 28, "195": 36, "240": None},  # lPitch rounded up to 32 bytes
}


class CheckError(ValueError):
    pass


# ---------------------------------------------------------------------------
# pure logic
# ---------------------------------------------------------------------------
def md5_bytes(b):
    return hashlib.md5(b).hexdigest()


def pe_sections(data):
    """[(va_start, va_end, raw_off, raw_size)] of a PE32 image; ImageBase included in va."""
    if data[:2] != b"MZ":
        raise CheckError("not an MZ image")
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe:pe + 4] != b"PE\0\0":
        raise CheckError("no PE header")
    nsec = struct.unpack_from("<H", data, pe + 6)[0]
    opt = struct.unpack_from("<H", data, pe + 20)[0]
    if struct.unpack_from("<H", data, pe + 24)[0] != 0x10B:
        raise CheckError("not PE32")
    base = struct.unpack_from("<I", data, pe + 24 + 28)[0]
    out = []
    sec = pe + 24 + opt
    for i in range(nsec):
        s = sec + 40 * i
        vsize, va, rsize, roff = struct.unpack_from("<IIII", data, s + 8)
        out.append((base + va, base + va + max(vsize, rsize), roff, rsize))
    return out


def va_to_off(data, va):
    for lo, hi, roff, rsize in pe_sections(data):
        if lo <= va < hi and va - lo < rsize:
            return roff + (va - lo)
    raise CheckError("VA 0x%X is in no section's file data" % va)


def verify_offsets(data, table):
    """List of problems (empty = every row holds). Checks the table's own
    file offset against the PE mapping too, so a wrong row cannot pass."""
    bad = []
    for va, off, hexb, why in table:
        want = bytes.fromhex(hexb)
        try:
            mapped = va_to_off(data, va)
        except CheckError as e:
            bad.append("0x%06X %s: %s" % (va, why, e))
            continue
        if mapped != off:
            bad.append("0x%06X %s: table says file offset 0x%X, the PE maps it to 0x%X" % (va, why, off, mapped))
            continue
        got = data[off:off + len(want)]
        if got != want:
            bad.append("0x%06X %s: bytes %s, expected %s" % (va, why, got.hex(), hexb))
    return bad


def check_exe(data, spec):
    problems = []
    if len(data) != spec["size"]:
        problems.append("size %d, expected %d" % (len(data), spec["size"]))
    if md5_bytes(data) != spec["md5"]:
        problems.append("md5 %s, expected %s" % (md5_bytes(data), spec["md5"]))
    problems += verify_offsets(data, spec["offsets"])
    return problems


def launcher_text(title):
    """The Play .bat for a title (CRLF). The block this adds - the JKMODE line -
    is what stage-fleetres.py must generate; see launcher_changes() below."""
    t = TITLES[title]
    lines = [
        "@echo off",
        "rem %s - fleet launcher." % t["display"],
        "rem",
        "rem This .bat exists because launch.txt pointed straight at %s, and a" % t["exe"],
        "rem desktop shortcut cannot carry arguments - so nothing could give the game",
        "rem this box's resolution and every box started at 640x480 8-bit software.",
        "rem",
        "rem The Sith engine stores its mode as an INDEX into a DirectDraw mode list it",
        "rem builds itself - 64 entries at most - not as a width and height, so no",
        "rem staged value can be right: the index differs per box, per driver and per",
        "rem game. JKMODE.EXE repeats the game's own enumeration, cap and sort and",
        "rem writes the index of FR_W x FR_H at 16 bpp, b3DAccel=1 and the two device",
        "rem GUIDs the game would store, then reads them back. Where the game cannot",
        "rem select that mode - an X800 enumerates 1920x1080 only 77th, past the 64",
        "rem modes the game keeps - it REFUSES, and the game keeps what it has stored;",
        "rem the one thing it then removes is a stored index past the END of the list,",
        "rem which the game would read beyond. Its report is JKMODE.LOG.",
        'cd /d "%~dp0"',
        "",
        'call "%~dp0FLEETRES.BAT"',
        "",
        'if exist "%%~dp0JKMODE.EXE" "%%~dp0JKMODE.EXE" %s %%FR_W%% %%FR_H%% > "%%~dp0JKMODE.LOG" 2>&1' % t["game"],
        "",
        'start "" %s' % t["exe"],
        "",
        "rem Close this console - a launcher that lingers stacks a window per launch and",
        "rem makes every later screenshot ambiguous.",
        "exit",
        "",
    ]
    return "\r\n".join(lines)


def launch_txt_line(title):
    t = TITLES[title]
    return "%s\t%s\t%s" % (t["launcher"], t["display"], t["exe"])


def launcher_problems(title):
    """Rules this project has paid for, applied to what we generate."""
    t = TITLES[title]
    txt = launcher_text(title)
    bad = []
    if "(" in t["launcher"] or ")" in t["launcher"]:
        bad.append("parenthesis in the launcher filename")
    if re.search(r'[\\/:*?"<>|]', t["display"]):
        bad.append("illegal character in the display name (it becomes the .lnk filename)")
    if "\r\n" not in txt or "\n" in txt.replace("\r\n", ""):
        bad.append("not CRLF throughout")
    order = [txt.find('cd /d "%~dp0"'), txt.find('call "%~dp0FLEETRES.BAT"'),
             txt.find('"%~dp0JKMODE.EXE" ' + t["game"] + " %FR_W% %FR_H%"), txt.find('start "" ' + t["exe"])]
    if min(order) < 0 or order != sorted(order):
        bad.append("expected cd -> FLEETRES -> JKMODE -> start, in that order: %r" % order)
    if len(launch_txt_line(title).encode()) > 1023:
        bad.append("launch.txt line over the agent's 1023-byte read")
    return bad


def pe_imports_objdump(path):
    """(set of imported DLL names upper-cased, has .rsrc) via objdump -p/-h."""
    p = subprocess.run(["objdump", "-p", path], capture_output=True, text=True, check=True).stdout
    dlls = {m.upper() for m in re.findall(r"DLL Name:\s*(\S+)", p)}
    h = subprocess.run(["objdump", "-h", path], capture_output=True, text=True, check=True).stdout
    return dlls, (".rsrc" in h)


def helper_problems(path):
    bad = []
    dlls, rsrc = pe_imports_objdump(path)
    if not dlls <= ALLOWED_IMPORTS:
        bad.append("imports beyond KERNEL32/USER32/ADVAPI32: %s" % sorted(dlls - ALLOWED_IMPORTS))
    if "DDRAW.DLL" in dlls:
        bad.append("ddraw.dll imported statically - it must be LoadLibrary'd")
    if rsrc:
        bad.append("has a .rsrc section (a manifest would stop UAC virtualization matching the game's)")
    data = open(path, "rb").read()
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    subsys_major = struct.unpack_from("<H", data, pe + 24 + 48)[0]
    if subsys_major > 5:
        bad.append("SubsystemVersion %d: XP's loader refuses >= 6" % subsys_major)
    return bad


# ---------------------------------------------------------------------------
# share side
# ---------------------------------------------------------------------------
def find_ci(parent, name):
    """The entry of `parent` whose name matches case-insensitively, or None."""
    try:
        for e in os.listdir(parent):
            if e.lower() == name.lower():
                return os.path.join(parent, e)
    except FileNotFoundError:
        return None
    return None


def reg_key_in_install_reg(text):
    return [m.strip() for m in re.findall(r"^\[HKEY_LOCAL_MACHINE\\(.+?)\]\s*$", text, re.M | re.I)]


def cmd_check(library):
    if not os.path.isdir(library):
        print("FAIL: the library is not mounted at %s" % library)
        return 2
    rc = 0
    for title, t in TITLES.items():
        tree = find_ci(library, title)
        print("== %s" % title)
        if not tree:
            print("FAIL %s: not found (case-insensitive) in %s" % (title, library))
            rc = 1
            continue
        exe = find_ci(tree, t["exe"])
        if not exe:
            print("FAIL %s: %s not found (case-insensitive)" % (title, t["exe"]))
            rc = 1
            continue
        data = open(exe, "rb").read()
        probs = check_exe(data, t)
        if probs:
            rc = 1
            for p in probs:
                print("FAIL %s/%s: %s" % (title, t["exe"], p))
        else:
            print("ok   %s md5 %s size %d; %d offsets hold" % (t["exe"], md5_bytes(data), len(data), len(t["offsets"])))
        reg = find_ci(tree, "install.reg")
        keys = reg_key_in_install_reg(open(reg, "rb").read().decode("latin-1")) if reg else []
        if not any(k.lower() == t["regkey"].lower() for k in keys):
            print("FAIL %s: install.reg has no [HKEY_LOCAL_MACHINE\\%s] - JKMODE writes there" % (title, t["regkey"]))
            rc = 1
        else:
            print("ok   install.reg creates HKLM\\%s (the key JKMODE writes)" % t["regkey"])
        cur = find_ci(tree, HELPER)
        print("     %s staged: %s" % (HELPER, ("yes, md5 %s" % md5_bytes(open(cur, "rb").read())) if cur else "no"))
        lt = find_ci(tree, "launch.txt")
        first = open(lt, "rb").read().decode("latin-1").splitlines()[0] if lt else ""
        print("     launch.txt line 1: %r" % first)
        print("     launcher %r present: %s" % (t["launcher"], "yes" if find_ci(tree, t["launcher"]) else "no"))
        print("     FLEETRES.EXE staged: %s" % ("yes" if find_ci(tree, "FLEETRES.EXE") else "no"))
    for title in TITLES:
        for p in launcher_problems(title):
            print("FAIL %s launcher: %s" % (title, p))
            rc = 1
    print("RESULT %s" % ("OK" if rc == 0 else "FAILED"))
    return rc


def cmd_build(outdir, library):
    os.makedirs(outdir, exist_ok=True)
    subprocess.run(["bash", BUILD_SH, outdir], check=True, stdout=subprocess.DEVNULL)
    exe = os.path.join(outdir, HELPER)
    probs = helper_problems(exe)
    for p in probs:
        print("FAIL %s: %s" % (HELPER, p))
    if probs:
        return 1
    blob = open(exe, "rb").read()
    dlls, _ = pe_imports_objdump(exe)
    print("built %s  %d bytes  md5 %s  imports %s" % (exe, len(blob), md5_bytes(blob), ", ".join(sorted(dlls))))
    outputs, refs = [], []
    for title, t in TITLES.items():
        share_dir = SHARE_LIB_REL + "/" + title
        prior = None
        tree = find_ci(library, title) if os.path.isdir(library) else None
        if tree:
            cur = find_ci(tree, HELPER)
            prior = md5_bytes(open(cur, "rb").read()) if cur else None
            game_md5 = md5_bytes(open(find_ci(tree, t["exe"]), "rb").read())
        else:
            game_md5 = None
        outputs.append({
            "share_path": share_dir + "/" + HELPER, "local_path": exe,
            "md5": md5_bytes(blob), "size": len(blob),
            "original_md5": prior,     # None: the file does not exist on the share yet
            "written_for": {"file": t["exe"], "md5": t["md5"], "staged_md5_now": game_md5},
        })
        tdir = os.path.join(outdir, title)
        os.makedirs(tdir, exist_ok=True)
        lp = os.path.join(tdir, t["launcher"])
        with open(lp, "wb") as fh:
            fh.write(launcher_text(title).encode("latin-1"))
        refs.append({"share_path": share_dir + "/" + t["launcher"], "local_path": lp,
                     "md5": md5_bytes(open(lp, "rb").read()),
                     "note": "REFERENCE ONLY - stage-fleetres.py must generate this; not published by apply.py",
                     "launch_txt_line_0": launch_txt_line(title)})
    manifest = {"key": KEY, "helper": HELPER, "outputs": outputs, "reference_launchers": refs}
    with open(os.path.join(outdir, "manifest.json"), "w") as fh:
        json.dump(manifest, fh, indent=2)
    print("manifest %s" % os.path.join(outdir, "manifest.json"))
    return 0


def cmd_publish(outdir, library, dry):
    """FUTURE USE. One file at a time; back up anything it would replace;
    skip what is already current; stop at the first failure."""
    mpath = os.path.join(outdir, "manifest.json")
    if not os.path.isfile(mpath):
        print("FAIL: no %s - run --build first" % mpath)
        return 2
    m = json.load(open(mpath))
    for o in m["outputs"]:
        local = o["local_path"]
        if not os.path.isfile(local) or md5_bytes(open(local, "rb").read()) != o["md5"]:
            print("FAIL %s: the built file no longer matches the manifest - rebuild" % local)
            return 1
        title = o["share_path"].split("/")[2]
        spec = TITLES[title]
        tree = find_ci(library, title)
        if not tree:
            print("FAIL %s: not in the library" % title)
            return 1
        game = find_ci(tree, spec["exe"])
        if not game or md5_bytes(open(game, "rb").read()) != spec["md5"]:
            print("FAIL %s: the staged %s is not the binary JKMODE was written against - not publishing"
                  % (title, spec["exe"]))
            return 1
        cur = find_ci(tree, HELPER)
        cur_md5 = md5_bytes(open(cur, "rb").read()) if cur else None
        if cur_md5 == o["md5"]:
            print("skip %s: already current" % o["share_path"])
            continue
        if cur_md5 is not None:
            backup_rel = "%s/_patches/%s/%s/%s" % (SHARE_LIB_REL, title, BACKUP_STAMP, HELPER)
            with tempfile.NamedTemporaryFile(delete=False, suffix=".bak") as tf:
                shutil.copyfile(cur, tf.name)
                bak = tf.name
            cmd = [sys.executable, SHAREWRITE, "put", bak, backup_rel] + (["--dry-run"] if dry else [])
            print("backup %s -> %s" % (o["share_path"], backup_rel))
            r = subprocess.run(cmd)
            os.unlink(bak)
            if r.returncode != 0:
                print("FAIL backup of %s (sharewrite exit %d) - stopping" % (o["share_path"], r.returncode))
                return 1
        cmd = [sys.executable, SHAREWRITE, "put", local, o["share_path"]] + (["--dry-run"] if dry else [])
        print("put %s" % o["share_path"])
        r = subprocess.run(cmd)
        if r.returncode != 0:
            print("FAIL put %s (sharewrite exit %d) - stopping" % (o["share_path"], r.returncode))
            return 1
    print("RESULT %s" % ("dry run" if dry else "published"))
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--check", action="store_true")
    g.add_argument("--build", nargs="?", const=DEFAULT_OUT, metavar="OUTDIR")
    g.add_argument("--publish", action="store_true")
    g.add_argument("--install-server", action="store_true")
    ap.add_argument("--out", default=DEFAULT_OUT, help="build output dir for --publish")
    ap.add_argument("--library", default=LIBRARY)
    ap.add_argument("--dry-run", action="store_true")
    a = ap.parse_args(argv)
    if a.check:
        return cmd_check(a.library)
    if a.build:
        return cmd_build(a.build, a.library)
    if a.publish:
        return cmd_publish(a.out, a.library, a.dry_run)
    print("not applicable: the Sith engine has no dedicated server (peer-hosted DirectPlay only)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
