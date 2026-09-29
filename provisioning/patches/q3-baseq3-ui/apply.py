#!/usr/bin/env python3
"""Quake III Arena (baseq3) - list the monitor's own mode (1920x1080 on the
fleet LCDs) in the in-game SETUP > SYSTEM "Video Mode" menu, for BOTH engines
in the staged tree (ioquake3.x86.exe 1.36 and retail quake3.exe 1.32c).

THE DEFECT (measured 2026-09-29 on .123 .145 .195 .240, see
.claude/evidence-1080p/_results/titles-verified.json "Quake3-TeamArena - ..."):
both engines load id's 1.32 UI, baseq3/pak8.pk3 vm/ui.qvm. Its Video Mode
list is a fixed table of twelve modes (r_mode 0..11, 320x240 .. 2048x1536 +
856x480). The launcher runs every box at the panel's own size through
r_mode -1 + r_customwidth/r_customheight, so the menu shows "640x480" while
the game runs 1920x1080, and ANY ACCEPT writes r_mode 3 - the session drops
to 640x480.

THE FIX: a replacement UI module, baseq3/zz-fleetres-ui.pk3 (vm/ui.qvm only).
"zz-" sorts after pak8.pk3 in both engines' case-insensitive pk3 order, so
its vm/ui.qvm wins. It is id's own q3_ui, rebuilt from the GPL source with
ONE change, to ui_video.c (ui_video-fleetres.patch + ui_fleetres.h):
  - the list is r_availableModes when the engine publishes it (ioquake3),
    else id's table (retail publishes nothing);
  - the running mode (r_mode -1 = r_customwidth x r_customheight) is always
    in the list and selected - appended as "WxH" when absent;
  - ACCEPT on it writes the running mode back unchanged (r_mode -1 stays -1
    with the same size); other entries write id's r_mode for id's sizes and
    r_mode -1 + r_customwidth/r_customheight otherwise.

THE PROOF THAT NOTHING ELSE CHANGES - built into --build, not assumed:
  1. REPRODUCTION GATE. The unmodified source is compiled first and must
     reproduce the staged pak8.pk3 vm/ui.qvm BYTE FOR BYTE (md5
     3e6b8fadb2a970c2b754e48e63a054d7). That pins the source (id GPL
     release), the compiler (ioq3's q3lcc), the assembler (ioq3's q3asm
     -vq3) and the link order (id's 1.32 Linux cons order, plus
     ui_loadconfig/ui_saveconfig, which pak8 contains). Two facts it
     encodes: pak8 was built on Linux from a tr_types.h WITHOUT the later
     "defined(Q3_VM)" guard, so its ACCEPT writes r_glDriver "libGL.so.1"
     (and "libMesaVoodooGL.so" for Voodoo) - kept as-is, see
     RELEASE_132_TR_TYPES; and q3_ui.q3asm in the GPL tree is NOT the list
     pak8 was linked from.
  2. DRIFT GATE. The patched build's .asm files must equal the stock ones
     except ui_video.asm, and every function outside ui_video.c must decode
     to the same instruction stream (opcodes, locals, call targets).
  3. FORMAT GATE. The QVM carries VM_MAGIC 0x12721444 with a 32-byte header
     and no jump-table section (q3asm -vq3). A VER2 QVM is ERR_FATAL
     "vm/ui.qvm has bad header" in retail 1.32c - it would stop quake3.exe
     starting at all, the Voodoo boxes' only engine included.
  4. BEHAVIOUR GATE. qvmsim.py (a Q3 VM interpreter with a stub UI engine)
     boots both builds and walks Main > SETUP > SYSTEM > Video Mode by
     keystrokes, changes Texture Filter and presses ACCEPT - the hardware
     test, in software. The stock build must show what the fleet measured
     ("640x480", ACCEPT writes r_mode 3); the patched build must show the
     running mode and write it back; both must write every OTHER cvar
     identically (see BEHAVIOUR).

    apply.py --check                 staged originals, engines, server: assert and report
    apply.py --build [OUTDIR]        sources -> tools -> QVM (all gates) -> pk3 + manifest.json
    apply.py --publish [OUTDIR] [--dry-run]
                                     FUTURE: put the source bundle, then the pk3, on the
                                     share with scripts/fleet/sharewrite.py - one file at a
                                     time, stop on the first failure, skip what is current
    apply.py --install-server [OUTDIR] [--dry-run] [--force]
                                     FUTURE: copy the pk3 into quake3-server's baseq3 (its
                                     pure list). ONLY after every fleet box has it: a client
                                     without it is dropped as unpure unless it downloads it
                                     (this server's ioq3ded touches vm/ui.qvm when pure, so
                                     the pk3 IS offered - but only to cl_allowDownload 1).
                                     Refuses until the library carries it AND
                                     retro-autodeploy has recorded a sync of every box since.

Nothing here writes game data into git. Sources and tools are cached in
~/.retro-fleet/patch-src/q3-baseq3-ui/, outputs go to
~/.retro-fleet/patch-out/q3-baseq3-ui/. The QVM is GPL-2 (id's GPL release +
our patch), so --publish also puts its source next to the patch record.
"""
import argparse
import hashlib
import io
import json
import os
import shutil
import struct
import subprocess
import sys
import tarfile
import time
import zipfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import qvmsim  # noqa: E402  (beside this file: the VM interpreter for the behaviour gate)

KEY = "q3-baseq3-ui"
TITLE = "Quake3-TeamArena"
MNT = "/mnt/retro-share"                 # read-only CIFS mount of the NAS share
LIBRARY = MNT + "/Files/Games-Library"
SHARE_LIB_REL = "Files/Games-Library"
BACKUP_REL = SHARE_LIB_REL + "/_patches/" + TITLE + "/originals-2026-09-29"
SOURCE_REL = SHARE_LIB_REL + "/_patches/" + TITLE + "/zz-fleetres-ui-source"
DEFAULT_OUT = os.path.expanduser("~/.retro-fleet/patch-out/" + KEY)
SRC_CACHE = os.path.expanduser("~/.retro-fleet/patch-src/" + KEY)
HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
SHAREWRITE = os.path.join(REPO, "scripts", "fleet", "sharewrite.py")
PATCH_FILE = os.path.join(HERE, "ui_video-fleetres.patch")
HEADER_FILE = os.path.join(HERE, "ui_fleetres.h")

# ---- the output ------------------------------------------------------------
PK3_NAME = "zz-fleetres-ui.pk3"      # no spaces, no parentheses; sorts after pak8
PK3_REL = "baseq3/" + PK3_NAME        # relative to the title tree
PK3_MEMBER = "vm/ui.qvm"              # the ONLY member
PK3_DATE = (2026, 9, 29, 0, 0, 0)     # fixed, so the pk3 is byte-reproducible

# ---- the fleet Quake III server (dev host) ---------------------------------
# Its pk3s live in the HOMEPATH, not the basepath: the journal's "current
# search path" lists ~/q3a-server/.q3a/baseq3/pak8..pak0 and then
# /usr/lib/ioquake3/baseq3, which holds only qagame.so (read 2026-09-29).
SERVER_UNIT = "quake3-server"
SERVER_UNIT_FILE = os.path.expanduser("~/.config/systemd/user/%s.service" % SERVER_UNIT)
SERVER_BASEQ3 = "/home/voidsstr/q3a-server/.q3a/baseq3"   # fallback when the unit is unreadable
SERVER_PORT = 27961
# How a published library file reaches the boxes: retro-autodeploy re-syncs a
# box (GAMESYNC RESET + START) when this one-line file changes, and records each
# sync that met its post-condition, keyed by hostname (scripts/fleet/autodeploy.py).
DEPLOY_GENERATION_FILE = "_deploy_generation.txt"
AUTODEPLOY_STATE = os.path.expanduser("~/.retro-fleet/autodeploy.json")


def server_baseq3():
    """<fs_homepath>/baseq3 as the unit's ExecStart sets it - where the server's
    pure paks really are (its basepath, /usr/lib/ioquake3/baseq3, holds only
    qagame.so). Falls back to SERVER_BASEQ3."""
    try:
        for line in open(SERVER_UNIT_FILE):
            if line.startswith("ExecStart=") and "fs_homepath" in line:
                toks = line.split()
                i = toks.index("fs_homepath")
                return os.path.join(toks[i + 1], "baseq3")
    except (OSError, ValueError, IndexError):
        pass
    return SERVER_BASEQ3

# ---- pinned sources ---------------------------------------------------------
ID_REPO = "https://github.com/id-Software/Quake-III-Arena.git"
ID_COMMIT = "dbe4ddb10315479fc00086f08e25d968b4b43c49"   # GPL release, 2005-08-20
IOQ3_REPO = "https://github.com/ioquake/ioq3.git"
IOQ3_COMMIT = "83a776283bdb958f82db25554b5ed0966aaf6e49"  # tools only: q3lcc/q3cpp/q3rcc/q3asm
ID_PATHS = ["code/q3_ui", "code/game", "code/cgame", "code/ui"]
IOQ3_PATHS = ["code/tools", "code/qcommon/q_platform.h", "code/qcommon/qfiles.h"]

# ---- the staged originals (read-only, asserted by --check) -----------------
PAK8_REL = "baseq3/pak8.pk3"
PAK8_MD5 = "d8b96d429ca4a9c289071cb7e77e14d2"
PAK8_SIZE = 454478
PAK8_UI_MD5 = "3e6b8fadb2a970c2b754e48e63a054d7"
PAK8_UI_SIZE = 278308
PAK8_UI_CRC = 0x9e5dd0a3
# vmHeader_t: magic, instructionCount, codeOffset, codeLength, dataOffset,
#             dataLength, litLength, bssLength
PAK8_UI_HEADER = (0x12721444, 79994, 32, 244976, 245008, 11764, 21536, 520196)
# the exact original bytes the defect lives in: id's table and its label
PAK8_UI_STRINGS = {
    0x43c2a: "Video Mode:",
    0x43cb9: "856x480 wide screen",
    0x43ccd: "2048x1536",
    0x43cd7: "1600x1200",
    0x43ce1: "1280x1024",
    0x43ceb: "1152x864",
    0x43cf4: "1024x768",
    0x43cfd: "960x720",
    0x43d05: "800x600",
    0x43d0d: "640x480",
    0x43d15: "512x384",
    0x43d1d: "400x300",
    0x43d25: "320x240",
    0x43e1b: "r_glDriver",
    0x43e33: "r_mode",
    0x43e63: "libMesaVoodooGL.so",
    0x43e76: "libGL.so.1",
}
ENGINES = {
    # retail id 1.32c: rejects a VER2 QVM, publishes no mode list
    "quake3.exe": {
        "md5": "b5cf3dd55e045aac6096ff97379d0cab", "size": 872448,
        "strings": {0xba0a4: "%s has bad header", 0xb2820: "Q3 1.32c",
                    0xbc824: "r_customwidth", 0xbc80c: "r_customheight"},
        "absent": ["r_availableModes"],
    },
    # ioquake3 1.36: publishes r_availableModes (the driver's mode list)
    "ioquake3.x86.exe": {
        "md5": "12f99bc0f9ba416ee477a4f13de8feaa", "size": 1363456,
        "strings": {0x135308: "r_availableModes", 0x12b90f: "r_customwidth"},
        "absent": [],
    },
}

# ---- the build recipe -------------------------------------------------------
VM_MAGIC = 0x12721444
VM_MAGIC_VER2 = 0x12721445
# 1.32 was compiled from a tr_types.h that picked the GL driver names with
# "#ifdef _WIN32" alone; the GPL tree added "defined(Q3_VM) ||" later. pak8 -
# every Windows player's UI for 20 years - carries the non-Windows names, and
# the reproduction gate proves it. Kept: the task is "nothing but the video
# mode changes". (Harmless on the fleet: GLW_StartOpenGL falls back when the
# name does not load; ioquake3 ignores r_glDriver.)
RELEASE_132_TR_TYPES = ("#if defined(Q3_VM) || defined(_WIN32)\n", "#if defined(_WIN32)\n")
# id's 1.32 link order: code/q3_ui/Conscript (the Linux cons build that made
# the release QVMs) + ui_loadconfig/ui_saveconfig, which pak8 contains and
# that Conscript omits. Proven by the reproduction gate - not q3_ui.q3asm.
Q3ASM_ORDER = [
    "ui_main", "bg_misc", "q_math", "q_shared",
    "ui_addbots", "ui_atoms", "ui_cdkey", "ui_cinematics", "ui_confirm",
    "ui_connect", "ui_controls2", "ui_credits", "ui_demo2", "ui_display",
    "ui_gameinfo", "ui_ingame", "ui_loadconfig", "ui_menu", "ui_mfield",
    "ui_mods", "ui_network", "ui_options", "ui_playermodel", "ui_players",
    "ui_playersettings", "ui_preferences", "ui_qmenu", "ui_removebots",
    "ui_saveconfig", "ui_serverinfo", "ui_servers2", "ui_setup", "ui_sound",
    "ui_sparena", "ui_specifyserver", "ui_splevel", "ui_sppostgame",
    "ui_spskill", "ui_startserver", "ui_team", "ui_teamorders", "ui_video",
    "bg_lib", "../../ui/ui_syscalls",
]
GAME_SOURCES = {"bg_misc", "q_math", "q_shared", "bg_lib"}
LCC_FLAGS = ["-DQ3_VM", "-S", "-Wf-target=bytecode", "-Wf-g",
             "-I../../cgame", "-I../../game", "-I../../q3_ui"]
HOST_CFLAGS = ["-O2", "-fno-strict-aliasing", "-Wno-unused-result",
               "-Wno-pointer-to-int-cast", "-Wno-int-to-pointer-cast", "-w"]
Q3RCC_SOURCES = ["alloc", "bind", "bytecode", "dag", "decl", "enode", "error",
                 "event", "expr", "gen", "init", "inits", "input", "lex", "list",
                 "main", "null", "output", "prof", "profio", "simp", "stmt",
                 "string", "sym", "symbolic", "trace", "tree", "types"]
# ---- the behaviour gate: Main > Setup > System by keystrokes, in qvmsim ------
# Each scenario drives BOTH builds through the menu path the hardware check
# uses, changes Texture Filter so ACCEPT appears, presses it, and compares
# what the menu showed and what ACCEPT wrote. The stock rows are what was
# MEASURED on hardware (label "640x480", "...setting mode 3: 640 480").
_BASE_CVARS = {"r_fullscreen": "1", "r_glDriver": "opengl32", "r_picmip": "1",
               "r_textureMode": "GL_LINEAR_MIPMAP_NEAREST", "r_colorbits": "32",
               "r_texturebits": "0", "r_allowExtensions": "1", "r_vertexLight": "0",
               "r_lodBias": "0", "r_subdivisions": "4"}
_LCD = {"r_mode": "-1", "r_customwidth": "1920", "r_customheight": "1080"}
# the shape ioquake3 publishes: largest first, then by closeness to the desktop aspect
IOQ3_MODES = "1920x1080 1680x1050 1600x1200 1400x1050 1280x1024 1280x960 1024x768 800x600 640x480"
K_L, K_R = qvmsim.K_LEFTARROW, qvmsim.K_RIGHTARROW
BEHAVIOUR = [
    {"name": "retail 1.32c, LCD, ACCEPT with Video Mode untouched",
     "cvars": dict(_LCD), "vid": (1920, 1080), "keys": [],
     "stock": {"label": "640x480", "r_mode": "3"},
     "patched": {"label": "1920x1080", "count": 13, "r_mode": "-1",
                 "r_customwidth": "1920", "r_customheight": "1080"}},
    {"name": "ioquake3 1.36, LCD, ACCEPT with Video Mode untouched",
     "cvars": dict(_LCD, r_availableModes=IOQ3_MODES), "vid": (1920, 1080), "keys": [],
     "stock": {"label": "640x480", "r_mode": "3"},
     "patched": {"label": "1920x1080", "index": 0, "count": 9, "r_mode": "-1",
                 "r_customwidth": "1920", "r_customheight": "1080"}},
    {"name": "ioquake3 1.36, LCD, pick the next driver mode",
     "cvars": dict(_LCD, r_availableModes=IOQ3_MODES), "vid": (1920, 1080), "keys": [K_R],
     "patched": {"chosen": "1680x1050", "r_mode": "-1",
                 "r_customwidth": "1680", "r_customheight": "1050"}},
    {"name": "ioquake3 1.36, LCD, pick a driver mode that is one of id's",
     "cvars": dict(_LCD, r_availableModes=IOQ3_MODES), "vid": (1920, 1080), "keys": [K_R] * 6,
     "patched": {"chosen": "1024x768", "r_mode": "6"}},
    {"name": "retail 1.32c, LCD, pick id's wide-screen entry",
     "cvars": dict(_LCD), "vid": (1920, 1080), "keys": [K_L],
     "patched": {"chosen": "856x480 wide screen", "r_mode": "11"}},
    {"name": "retail 1.32c, LCD, RIGHT at the end of the list does nothing",
     "cvars": dict(_LCD), "vid": (1920, 1080), "keys": [K_R],
     "patched": {"chosen": "1920x1080", "r_mode": "-1",
                 "r_customwidth": "1920", "r_customheight": "1080"}},
    {"name": "retail 1.32c, 4:3 CRT at r_mode -1 1024x768 (one of id's sizes)",
     "cvars": {"r_mode": "-1", "r_customwidth": "1024", "r_customheight": "768"},
     "vid": (1024, 768), "keys": [],
     "stock": {"label": "640x480", "r_mode": "3"},
     "patched": {"label": "1024x768", "index": 6, "count": 12, "r_mode": "-1",
                 "r_customwidth": "1024", "r_customheight": "768"}},
    {"name": "retail 1.32c, r_mode 6 (a plain id mode) - unchanged from id",
     "cvars": {"r_mode": "6"}, "vid": (1024, 768), "keys": [],
     "stock": {"label": "1024x768", "r_mode": "6"},
     "patched": {"label": "1024x768", "index": 6, "count": 12, "r_mode": "6"}},
]
_MODE_CVARS = ("r_mode", "r_customwidth", "r_customheight")

# strings the patched UI must carry (and pak8 does not)
NEW_STRINGS = ["r_availableModes", "r_customwidth", "r_customheight"]
PATCHED_FILE = "ui_video"             # the only translation unit the patch touches


class GateError(RuntimeError):
    """A build gate failed - the output must not be used."""


# ============================================================================
# small helpers
# ============================================================================
def md5_bytes(b):
    return hashlib.md5(b).hexdigest()


def md5_file(path):
    h = hashlib.md5()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def find_ci(parent, name):
    """The entry of 'parent' whose name equals 'name' case-insensitively (Windows tree)."""
    try:
        for e in os.listdir(parent):
            if e.lower() == name.lower():
                return os.path.join(parent, e)
    except OSError:
        pass
    return None


def tree_path(root, rel):
    """Resolve a tree-relative path one component at a time, case-insensitively."""
    cur = root
    for part in rel.replace("\\", "/").split("/"):
        cur = find_ci(cur, part)
        if cur is None:
            return None
    return cur


def pk3_order_key(name):
    """The order both engines load a game directory's pk3s in, as a sort key;
    the LAST name in this order is searched FIRST.

    id 1.32 AND ioquake3 1.36 qsort the names with paksort() = FS_PathCmp()
    (code/qcommon/files.c, read in both trees): it UPPER-cases a-z, maps '\\'
    and ':' to '/', and compares everything else as signed C chars up to and
    including the terminating NUL. It is NOT a lower-case compare - the six
    characters [ \\ ] ^ _ ` sit between 'Z' and 'a', so they sort AFTER every
    letter here and BEFORE them in lower case. (Review 2026-09-29: this used
    name.lower(), which reported "zz-fleetres-u_.pk3" as harmless while the
    engines would load it over ours.)"""
    key = []
    for ch in name.encode("latin-1", "replace"):
        c = ch - 256 if ch >= 0x80 else ch     # char is signed on x86
        if ord("a") <= c <= ord("z"):
            c -= ord("a") - ord("A")
        if c in (ord("\\"), ord(":")):
            c = ord("/")
        key.append(c)
    key.append(0)                              # the NUL: a prefix sorts first
    return tuple(key)


def host_cc():
    for cand in [os.environ.get("CC"), "gcc", "cc", "clang"]:
        if cand and shutil.which(cand):
            return shutil.which(cand)
    return None


def run(cmd, cwd=None, check=True):
    r = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True)
    if check and r.returncode != 0:
        raise GateError("command failed (%d): %s\n%s%s" % (
            r.returncode, " ".join(cmd), r.stdout[-2000:], r.stderr[-2000:]))
    return r


# ============================================================================
# QVM reading
# ============================================================================
# operand bytes per opcode (qcommon/vm_local.h opcode_t): ENTER LEAVE CONST
# LOCAL, the 16 conditional branches and BLOCK_COPY carry 4; ARG carries 1.
OP_NAMES = ["UNDEF", "IGNORE", "BREAK", "ENTER", "LEAVE", "CALL", "PUSH", "POP",
            "CONST", "LOCAL", "JUMP", "EQ", "NE", "LTI", "LEI", "GTI", "GEI",
            "LTU", "LEU", "GTU", "GEU", "EQF", "NEF", "LTF", "LEF", "GTF", "GEF",
            "LOAD1", "LOAD2", "LOAD4", "STORE1", "STORE2", "STORE4", "ARG",
            "BLOCK_COPY", "SEX8", "SEX16", "NEGI", "ADD", "SUB", "DIVI", "DIVU",
            "MODI", "MODU", "MULI", "MULU", "BAND", "BOR", "BXOR", "BCOM", "LSH",
            "RSHI", "RSHU", "NEGF", "ADDF", "SUBF", "DIVF", "MULF", "CVIF", "CVFI"]
OP = {n: i for i, n in enumerate(OP_NAMES)}
OPERAND4 = {OP["ENTER"], OP["LEAVE"], OP["CONST"], OP["LOCAL"], OP["BLOCK_COPY"]} | \
    set(range(OP["EQ"], OP["GEF"] + 1))
OPERAND1 = {OP["ARG"]}
BRANCHES = set(range(OP["EQ"], OP["GEF"] + 1))
HEADER_FIELDS = ("vmMagic", "instructionCount", "codeOffset", "codeLength",
                 "dataOffset", "dataLength", "litLength", "bssLength")


def qvm_header(data):
    if len(data) < 32:
        raise ValueError("QVM shorter than its header (%d B)" % len(data))
    return dict(zip(HEADER_FIELDS, struct.unpack_from("<8i", data, 0)))


def qvm_format_problems(data):
    """Every reason the retail 1.32c loader (or a sane reader) would refuse
    this image. Empty list = original-format QVM, loadable by both engines."""
    probs = []
    try:
        h = qvm_header(data)
    except ValueError as e:
        return [str(e)]
    magic = h["vmMagic"] & 0xffffffff
    if magic == VM_MAGIC_VER2:
        probs.append("VM_MAGIC_VER2 (0x%08x): retail 1.32c ERR_FATALs '%%s has bad header'"
                     " - assemble with q3asm -vq3" % magic)
    elif magic != VM_MAGIC:
        probs.append("bad magic 0x%08x" % magic)
    if h["codeOffset"] != 32:
        probs.append("codeOffset %d, want 32 (an original header has no jtrgLength)"
                     % h["codeOffset"])
    if h["codeLength"] <= 0 or h["instructionCount"] <= 0:
        probs.append("empty code segment")
    for f in ("dataLength", "litLength", "bssLength"):
        if h[f] < 0:
            probs.append("negative %s" % f)
    if h["dataOffset"] != h["codeOffset"] + h["codeLength"]:
        probs.append("dataOffset %d != codeOffset+codeLength %d"
                     % (h["dataOffset"], h["codeOffset"] + h["codeLength"]))
    want = h["dataOffset"] + h["dataLength"] + h["litLength"]
    if len(data) != want:
        probs.append("file is %d B, header accounts for %d (a trailing jump-table"
                     " section means a VER2 image)" % (len(data), want))
    return probs


def qvm_decode(data):
    """[(opcode, operand)] for every instruction; raises on a malformed stream."""
    h = qvm_header(data)
    code = data[h["codeOffset"]:h["codeOffset"] + h["codeLength"]]
    out = []
    pc = 0
    while pc < len(code):
        op = code[pc]
        pc += 1
        if op >= len(OP_NAMES):
            raise ValueError("bad opcode %d at code byte %d" % (op, pc - 1))
        if op in OPERAND4:
            (arg,) = struct.unpack_from("<i", code, pc)
            pc += 4
        elif op in OPERAND1:
            arg = code[pc]
            pc += 1
        else:
            arg = None
        out.append((op, arg))
    if pc != len(code):
        raise ValueError("code segment ends mid-instruction")
    if len(out) != h["instructionCount"]:
        raise ValueError("decoded %d instructions, header says %d"
                         % (len(out), h["instructionCount"]))
    return out


def qvm_lit_strings(data):
    h = qvm_header(data)
    lit = data[h["dataOffset"] + h["dataLength"]:
               h["dataOffset"] + h["dataLength"] + h["litLength"]]
    return [s.decode("latin-1") for s in lit.split(b"\0")]


def parse_map(text):
    """q3asm -m map: '<segment> <hex address> <name>' -> {segment: {name: address}}."""
    segs = {}
    for line in text.splitlines():
        parts = line.split()
        if len(parts) != 3:
            continue
        seg, addr, name = int(parts[0]), int(parts[1], 16), parts[2]
        if addr >= 0x80000000:
            addr -= 0x100000000
        segs.setdefault(seg, {})[name] = addr
    return segs


def procs_in_asm(asm_text):
    return [l.split()[1] for l in asm_text.splitlines() if l.startswith("proc ")]


def function_streams(insns, code_syms):
    """{name: normalized instruction tuple} for every function in the map.
    Branch/jump targets are made function-relative and call targets become
    symbol names, so a function that only MOVED compares equal; CONST
    operands that address data are left out (the data segment relocates)."""
    # q3asm also files _stackStart/_stackEnd (bss addresses) under segment 0:
    # only an address inside the code is a function
    by_addr = sorted((a, n) for n, a in code_syms.items() if 0 <= a < len(insns))
    addr_name = {a: n for a, n in by_addr}
    out = {}
    for k, (start, name) in enumerate(by_addr):
        end = by_addr[k + 1][0] if k + 1 < len(by_addr) else len(insns)
        seq = []
        for i in range(start, end):
            op, arg = insns[i]
            nxt = insns[i + 1][0] if i + 1 < len(insns) else None
            if op == OP["CONST"] and nxt == OP["CALL"]:
                seq.append((op, "call", addr_name.get(arg, arg if arg < 0 else "?")))
            elif op == OP["CONST"] and nxt == OP["JUMP"]:
                seq.append((op, "jump", arg - start))
            elif op == OP["CONST"]:
                seq.append((op, "const"))
            elif op in BRANCHES:
                seq.append((op, arg - start))
            else:
                seq.append((op, arg))
        out[name] = tuple(seq)
    return out


# ============================================================================
# pk3 (zip) writing - deterministic
# ============================================================================
def make_pk3(qvm_bytes):
    """A pk3 holding vm/ui.qvm and nothing else, STORED, with fixed metadata:
    the same QVM always gives the same pk3 bytes (and the same pure checksum,
    which both engines derive from the member CRCs)."""
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w", compression=zipfile.ZIP_STORED) as z:
        zi = zipfile.ZipInfo(PK3_MEMBER, date_time=PK3_DATE)
        zi.compress_type = zipfile.ZIP_STORED
        zi.create_system = 0          # MS-DOS, as id's own pk3s
        zi.external_attr = 0
        z.writestr(zi, qvm_bytes)
    return buf.getvalue()


def pk3_problems(pk3_bytes):
    probs = []
    try:
        z = zipfile.ZipFile(io.BytesIO(pk3_bytes))
    except zipfile.BadZipFile as e:
        return ["not a zip: %s" % e]
    names = z.namelist()
    if names != [PK3_MEMBER]:
        probs.append("members %r, want exactly [%r]" % (names, PK3_MEMBER))
        return probs
    zi = z.getinfo(PK3_MEMBER)
    if zi.compress_type not in (zipfile.ZIP_STORED, zipfile.ZIP_DEFLATED):
        probs.append("compression %d: id's unzip reads only stored/deflate" % zi.compress_type)
    if z.testzip() is not None:
        probs.append("CRC mismatch in %s" % PK3_MEMBER)
    probs += ["member: " + p for p in qvm_format_problems(z.read(PK3_MEMBER))]
    return probs


# ============================================================================
# --check
# ============================================================================
def check(library=LIBRARY, verbose=True):
    """Read the staged originals and the engines, assert every recorded fact.
    Returns (ok, report). Raises nothing: every failure is IN the report."""
    rep = {"library": library, "title": TITLE, "problems": [], "facts": {}}
    probs = rep["problems"]

    def say(msg):
        if verbose:
            print(msg)

    tree = find_ci(library, TITLE)
    if not tree:
        probs.append("title tree %s/%s not found (case-insensitive)" % (library, TITLE))
        return False, rep
    rep["tree"] = tree

    # pak8.pk3 and its vm/ui.qvm - the module this patch overrides, not modifies
    pak8 = tree_path(tree, PAK8_REL)
    if not pak8:
        probs.append("%s not found (case-insensitive)" % PAK8_REL)
    else:
        size, md5 = os.path.getsize(pak8), md5_file(pak8)
        rep["facts"]["pak8"] = {"path": pak8, "size": size, "md5": md5}
        if (size, md5) != (PAK8_SIZE, PAK8_MD5):
            probs.append("pak8.pk3 is %d B md5 %s, recorded %d B md5 %s"
                         % (size, md5, PAK8_SIZE, PAK8_MD5))
        z = zipfile.ZipFile(pak8)
        member = next((n for n in z.namelist() if n.lower() == PK3_MEMBER), None)
        if member is None:
            probs.append("pak8.pk3 has no %s" % PK3_MEMBER)
        else:
            zi = z.getinfo(member)
            ui = z.read(member)
            rep["facts"]["pak8_ui"] = {"size": len(ui), "md5": md5_bytes(ui), "crc": "%08x" % zi.CRC}
            if (len(ui), md5_bytes(ui), zi.CRC) != (PAK8_UI_SIZE, PAK8_UI_MD5, PAK8_UI_CRC):
                probs.append("pak8 vm/ui.qvm is %d B md5 %s crc %08x, recorded %d B md5 %s crc %08x"
                             % (len(ui), md5_bytes(ui), zi.CRC, PAK8_UI_SIZE, PAK8_UI_MD5, PAK8_UI_CRC))
            hdr = struct.unpack_from("<8i", ui, 0)
            if (hdr[0] & 0xffffffff,) + tuple(hdr[1:]) != PAK8_UI_HEADER:
                probs.append("pak8 vm/ui.qvm header %r, recorded %r" % (hdr, PAK8_UI_HEADER))
            for off, s in sorted(PAK8_UI_STRINGS.items()):
                got = ui[off:off + len(s) + 1]
                if got != s.encode() + b"\0" or ui[off - 1:off] != b"\0":
                    probs.append("pak8 vm/ui.qvm @0x%x: want %r, found %r" % (off, s, got))
            fp = qvm_format_problems(ui)
            if fp:
                probs.append("pak8 vm/ui.qvm format: %s" % "; ".join(fp))
        say("pak8.pk3        %s  %d B  (vm/ui.qvm %s)" % (rep["facts"].get("pak8", {}).get("md5"),
            rep["facts"].get("pak8", {}).get("size", 0), rep["facts"].get("pak8_ui", {}).get("md5")))

    # the engines: what each publishes and accepts
    for exe, want in ENGINES.items():
        p = find_ci(tree, exe)
        if not p:
            probs.append("%s not found (case-insensitive)" % exe)
            continue
        data = open(p, "rb").read()
        rep["facts"][exe] = {"size": len(data), "md5": md5_bytes(data)}
        if (len(data), md5_bytes(data)) != (want["size"], want["md5"]):
            probs.append("%s is %d B md5 %s, recorded %d B md5 %s"
                         % (exe, len(data), md5_bytes(data), want["size"], want["md5"]))
        for off, s in want["strings"].items():
            if data[off:off + len(s)] != s.encode():
                probs.append("%s @0x%x: want %r, found %r" % (exe, off, s, data[off:off + len(s)]))
        low = data.lower()
        for s in want["absent"]:
            if s.lower().encode() in low:
                probs.append("%s now contains %r (case-insensitive) - the retail fallback"
                             " assumption no longer holds" % (exe, s))
        say("%-15s %s  %d B" % (exe, md5_bytes(data), len(data)))

    # baseq3: does anything sort after our pk3, is ours there already?
    baseq3 = find_ci(tree, "baseq3")
    pk3s = sorted((e for e in os.listdir(baseq3) if e.lower().endswith(".pk3")),
                  key=pk3_order_key) if baseq3 else []
    rep["facts"]["baseq3_pk3s"] = pk3s
    later = [p for p in pk3s if pk3_order_key(p) > pk3_order_key(PK3_NAME)]
    if later:
        probs.append("baseq3 pk3(s) sort AFTER %s and would shadow its vm/ui.qvm: %s"
                     % (PK3_NAME, ", ".join(later)))
    ours = find_ci(baseq3, PK3_NAME) if baseq3 else None
    rep["facts"]["share_pk3"] = ({"path": ours, "size": os.path.getsize(ours), "md5": md5_file(ours)}
                                 if ours else None)
    say("baseq3 pk3s     %s" % ", ".join(pk3s))
    say("%-15s %s" % (PK3_NAME, ("present, md5 " + rep["facts"]["share_pk3"]["md5"]) if ours
                      else "not on the share yet"))

    # the fleet server: read-only; its pure list is what a joining client must match
    sdir = server_baseq3()
    srv = {"baseq3": sdir, "unit_file": SERVER_UNIT_FILE, "exists": os.path.isdir(sdir)}
    if srv["exists"]:
        srv["pk3s"] = sorted((e for e in os.listdir(sdir) if e.lower().endswith(".pk3")),
                             key=pk3_order_key)
        sp8 = find_ci(sdir, "pak8.pk3")
        srv["pak8_md5"] = md5_file(sp8) if sp8 else None
        if srv["pak8_md5"] and srv["pak8_md5"] != PAK8_MD5:
            probs.append("server pak8.pk3 md5 %s differs from the library's %s"
                         % (srv["pak8_md5"], PAK8_MD5))
        so = find_ci(sdir, PK3_NAME)
        srv["pk3"] = md5_file(so) if so else None
    rep["facts"]["server"] = srv
    say("server baseq3   %s: %s" % (sdir, ", ".join(srv.get("pk3s", [])) or "absent"))

    ok = not probs
    for p in probs:
        say("PROBLEM: " + p)
    say("check: %s" % ("OK - every recorded original matches" if ok else "FAILED (%d problem(s))" % len(probs)))
    return ok, rep


# ============================================================================
# sources and toolchain
# ============================================================================
def ensure_repo(path, url, commit):
    if not os.path.isdir(os.path.join(path, ".git")):
        os.makedirs(os.path.dirname(path), exist_ok=True)
        print("cloning %s -> %s" % (url, path))
        run(["git", "clone", "--quiet", url, path])
    if run(["git", "-C", path, "cat-file", "-e", commit + "^{commit}"], check=False).returncode:
        run(["git", "-C", path, "fetch", "--quiet", "origin"])
        run(["git", "-C", path, "cat-file", "-e", commit + "^{commit}"])
    return path


def export_tree(repo, commit, paths, dest):
    """Extract 'paths' at 'commit' into dest with git archive - never touches the
    clone's checkout, so a cached clone on another branch cannot leak in."""
    os.makedirs(dest, exist_ok=True)
    r = subprocess.run(["git", "-C", repo, "archive", "--format=tar", commit] + paths,
                       capture_output=True)
    if r.returncode:
        raise GateError("git archive %s %s failed: %s" % (repo, commit, r.stderr.decode()[-500:]))
    with tarfile.open(fileobj=io.BytesIO(r.stdout)) as t:
        if hasattr(tarfile, "data_filter"):
            t.extractall(dest, filter="data")
        else:
            t.extractall(dest)


def build_tools(ioq3_dir, bindir):
    """q3lcc + q3cpp + q3rcc (+lburg) and q3asm from ioquake3's tree, host gcc."""
    cc = host_cc()
    if not cc:
        raise GateError("no host C compiler (tried $CC, gcc, cc, clang)")
    t = os.path.join(ioq3_dir, "code", "tools")
    os.makedirs(bindir, exist_ok=True)
    lcc = os.path.join(t, "lcc")
    run([cc] + HOST_CFLAGS + ["-o", os.path.join(bindir, "lburg"),
        os.path.join(lcc, "lburg", "lburg.c"), os.path.join(lcc, "lburg", "gram.c")])
    dag = os.path.join(bindir, "dagcheck.c")
    run([os.path.join(bindir, "lburg"), os.path.join(lcc, "src", "dagcheck.md"), dag])
    run([cc] + HOST_CFLAGS + ["-I" + os.path.join(lcc, "src"), "-o", os.path.join(bindir, "q3rcc")] +
        [os.path.join(lcc, "src", s + ".c") for s in Q3RCC_SOURCES] + [dag])
    cpp_dir = os.path.join(lcc, "cpp")
    run([cc] + HOST_CFLAGS + ["-o", os.path.join(bindir, "q3cpp")] +
        sorted(os.path.join(cpp_dir, f) for f in os.listdir(cpp_dir) if f.endswith(".c")))
    run([cc] + HOST_CFLAGS + ["-o", os.path.join(bindir, "q3lcc"),
        os.path.join(lcc, "etc", "lcc.c"), os.path.join(lcc, "etc", "bytecode.c")])
    run([cc] + HOST_CFLAGS + ["-o", os.path.join(bindir, "q3asm"),
        os.path.join(t, "asm", "q3asm.c"), os.path.join(t, "asm", "cmdlib.c")])
    cc_ver = run([cc, "--version"]).stdout.splitlines()[0]
    return {"cc": cc_ver, **{n: md5_file(os.path.join(bindir, n))
                            for n in ("q3lcc", "q3cpp", "q3rcc", "q3asm")}}


def apply_release_tr_types(code_dir):
    p = os.path.join(code_dir, "cgame", "tr_types.h")
    s = open(p).read()
    old, new = RELEASE_132_TR_TYPES
    if s.count(old) != 1:
        raise GateError("tr_types.h: the 1.32 driver-name guard %r occurs %d times, want 1"
                        % (old.strip(), s.count(old)))
    open(p, "w").write(s.replace(old, new))


def apply_fleetres_patch(code_dir):
    """git apply the ui_video.c patch (exact context, fails on any drift) and
    add ui_fleetres.h beside it."""
    root = os.path.dirname(code_dir)
    run(["git", "apply", "--check", "-p1", PATCH_FILE], cwd=root)
    run(["git", "apply", "-p1", PATCH_FILE], cwd=root)
    shutil.copyfile(HEADER_FILE, os.path.join(code_dir, "q3_ui", "ui_fleetres.h"))


def compile_ui_qvm(code_dir, bindir):
    """Compile every translation unit, assemble in id's 1.32 order, -vq3.
    Returns (qvm bytes, map text, {unit: asm text})."""
    vm = os.path.join(code_dir, "q3_ui", "vm")
    if os.path.isdir(vm):
        shutil.rmtree(vm)
    os.makedirs(vm)
    q3lcc = os.path.join(bindir, "q3lcc")
    asm = {}
    for unit in Q3ASM_ORDER:
        if unit.endswith("ui_syscalls"):
            continue
        src = ("../../game/%s.c" if unit in GAME_SOURCES else "../%s.c") % unit
        r = run([q3lcc] + LCC_FLAGS + [src], cwd=vm, check=False)
        out = os.path.join(vm, unit + ".asm")
        if r.returncode or not os.path.isfile(out):
            raise GateError("q3lcc %s failed (%d):\n%s%s" % (src, r.returncode, r.stdout, r.stderr))
        asm[unit] = open(out).read()
    with open(os.path.join(vm, "build.q3asm"), "w") as f:
        f.write("-o ui\n" + "\n".join(Q3ASM_ORDER) + "\n")
    # q3asm is silent without -v, exits with its error count and writes no
    # image when that is non-zero; CodeError() prints to stderr
    r = run([os.path.join(bindir, "q3asm"), "-vq3", "-m", "-f", "build"], cwd=vm, check=False)
    qvm_path = os.path.join(vm, "ui.qvm")
    if r.returncode or r.stderr.strip() or not os.path.isfile(qvm_path):
        raise GateError("q3asm failed (%d):\n%s%s" % (r.returncode, r.stdout[-3000:], r.stderr[-3000:]))
    return (open(qvm_path, "rb").read(), open(os.path.join(vm, "ui.map")).read(), asm)


# ============================================================================
# --build
# ============================================================================
def drift_gate(stock, patched):
    """Everything outside ui_video.c must be the stock (= pak8) code."""
    (s_qvm, s_map, s_asm), (p_qvm, p_map, p_asm) = stock, patched
    changed = sorted(u for u in s_asm if s_asm[u] != p_asm.get(u))
    if changed != [PATCHED_FILE]:
        raise GateError("the patch changed the compiled output of %r, want only %r"
                        % (changed, [PATCHED_FILE]))
    video_procs = set(procs_in_asm(s_asm[PATCHED_FILE])) | set(procs_in_asm(p_asm[PATCHED_FILE]))
    s_fn = function_streams(qvm_decode(s_qvm), parse_map(s_map).get(0, {}))
    p_fn = function_streams(qvm_decode(p_qvm), parse_map(p_map).get(0, {}))
    compared, bad = 0, []
    for name, seq in s_fn.items():
        if name in video_procs:
            continue
        compared += 1
        if p_fn.get(name) != seq:
            bad.append(name)
    if bad:
        raise GateError("functions outside ui_video.c differ after linking: %s" % ", ".join(bad[:20]))
    s_lit, p_lit = qvm_lit_strings(s_qvm), qvm_lit_strings(p_qvm)
    missing = sorted(set(s_lit) - set(p_lit))
    if missing:
        raise GateError("strings lost from the UI: %r" % missing[:20])
    return {
        "asm_changed": changed,
        "functions_compared": compared,
        "functions_identical": compared,
        "ui_video_functions_stock": sorted(procs_in_asm(s_asm[PATCHED_FILE])),
        "ui_video_functions_patched": sorted(procs_in_asm(p_asm[PATCHED_FILE])),
        "strings_added": sorted(set(p_lit) - set(s_lit)),
    }


def behaviour_gate(stock, patched):
    """Drive both builds through BEHAVIOUR in qvmsim; every expectation must
    hold, and apart from r_mode/r_customwidth/r_customheight both builds'
    ACCEPT must write the same cvars with the same values in the same order."""
    results = []
    for sc in BEHAVIOUR:
        row = {"name": sc["name"]}
        runs = {}
        for kind, (qvm, mapt, _asm) in (("stock", stock), ("patched", patched)):
            cv = dict(_BASE_CVARS, **sc["cvars"])
            try:
                r = qvmsim.system_menu_session(qvm, mapt, cv, sc["vid"], sc["keys"])
            except qvmsim.VMError as e:
                raise GateError("BEHAVIOUR GATE: %s build, %r: %s" % (kind, sc["name"], e))
            runs[kind] = r
            want = sc.get(kind)
            got = {"label": r["shown"]["label"], "index": r["shown"]["index"],
                   "count": r["shown"]["count"], "chosen": r["chosen"]["label"]}
            got.update({k: r["accept"].get(k) for k in _MODE_CVARS})
            row[kind] = got
            if want is None:
                continue
            bad = {k: (v, got.get(k)) for k, v in want.items() if got.get(k) != v}
            if bad:
                raise GateError("BEHAVIOUR GATE: %s build, %r: want/got %r"
                                % (kind, sc["name"], bad))
            if "vid_restart\n" not in r["commands"]:
                raise GateError("BEHAVIOUR GATE: %s build, %r: ACCEPT ran no vid_restart"
                                % (kind, sc["name"]))
        rest = {k: [(n, v) for n, v in runs[k]["accept_writes"] if n not in _MODE_CVARS]
                for k in runs}
        if rest["stock"] != rest["patched"]:
            raise GateError("BEHAVIOUR GATE: %r: ACCEPT's other writes differ:\n stock   %r\n patched %r"
                            % (sc["name"], rest["stock"], rest["patched"]))
        row["other_writes_identical"] = len(rest["stock"])
        results.append(row)
    return results


def source_txt(manifest):
    q = manifest["qvm"]
    return "\r\n".join([
        "zz-fleetres-ui.pk3 - Quake III Arena baseq3 UI (vm/ui.qvm) for the retro-agent fleet",
        "",
        "This QVM is id Software's q3_ui (GPL-2.0-or-later) rebuilt from the GPL",
        "source with one change, to code/q3_ui/ui_video.c: the Video Mode list",
        "shows the mode the game is running in (1920x1080 on the fleet LCDs) and",
        "ACCEPT keeps it. ui_video-fleetres.patch + ui_fleetres.h here are that",
        "change; apply.py builds it and proves nothing else changed.",
        "",
        "source:    %s @ %s" % (ID_REPO, ID_COMMIT),
        "tools:     %s @ %s (q3lcc/q3cpp/q3rcc/q3asm -vq3)" % (IOQ3_REPO, IOQ3_COMMIT),
        "rebuild:   python3 provisioning/patches/%s/apply.py --build OUTDIR" % KEY,
        "           (retro-agent repo; the unmodified source must first reproduce",
        "           pak8.pk3 vm/ui.qvm md5 %s byte for byte)" % PAK8_UI_MD5,
        "",
        "vm/ui.qvm: %d B md5 %s, VM_MAGIC 0x%08x, %d instructions"
        % (q["size"], q["md5"], VM_MAGIC, q["header"]["instructionCount"]),
        "pk3:       %d B md5 %s" % (manifest["pk3"]["size"], manifest["pk3"]["md5"]),
        "",
        "Remove it: delete baseq3\\%s (GAMESYNC never deletes - add a del line" % PK3_NAME,
        "to the launchers or clear it per box), and from the server's baseq3.",
        "",
    ])


def build(outdir):
    t0 = time.time()
    outdir = os.path.abspath(outdir)
    work = os.path.join(outdir, "_build")
    if os.path.isdir(work):
        shutil.rmtree(work)
    os.makedirs(work)

    id_dir = ensure_repo(os.path.join(SRC_CACHE, "id-q3a"), ID_REPO, ID_COMMIT)
    ioq3_dir = ensure_repo(os.path.join(SRC_CACHE, "ioq3"), IOQ3_REPO, IOQ3_COMMIT)
    tools_src = os.path.join(work, "ioq3")
    export_tree(ioq3_dir, IOQ3_COMMIT, IOQ3_PATHS, tools_src)
    bindir = os.path.join(work, "bin")
    print("building q3lcc/q3cpp/q3rcc/q3asm from ioq3 %s" % IOQ3_COMMIT[:10])
    toolchain = build_tools(tools_src, bindir)

    builds = {}
    for kind in ("stock", "patched"):
        root = os.path.join(work, kind)
        export_tree(id_dir, ID_COMMIT, ID_PATHS, root)
        code = os.path.join(root, "code")
        apply_release_tr_types(code)
        if kind == "patched":
            apply_fleetres_patch(code)
        print("compiling %s q3_ui ..." % kind)
        builds[kind] = compile_ui_qvm(code, bindir)

    # GATE 1: the unmodified source reproduces the staged pak8 vm/ui.qvm exactly
    stock_qvm = builds["stock"][0]
    if md5_bytes(stock_qvm) != PAK8_UI_MD5 or len(stock_qvm) != PAK8_UI_SIZE:
        raise GateError("REPRODUCTION GATE: stock build is %d B md5 %s, pak8 vm/ui.qvm is %d B md5 %s"
                        " - the toolchain or the source drifted; do not ship"
                        % (len(stock_qvm), md5_bytes(stock_qvm), PAK8_UI_SIZE, PAK8_UI_MD5))
    print("GATE reproduction: stock build == pak8 vm/ui.qvm (md5 %s)" % PAK8_UI_MD5)

    # GATE 2: nothing outside ui_video.c changed
    drift = drift_gate(builds["stock"], builds["patched"])
    print("GATE drift: only %s.asm changed; %d functions outside it identical; strings added %r"
          % (PATCHED_FILE, drift["functions_compared"], drift["strings_added"]))

    # GATE 3: original QVM format, and the new code is really in it
    qvm = builds["patched"][0]
    fmt = qvm_format_problems(qvm)
    if fmt:
        raise GateError("FORMAT GATE: %s" % "; ".join(fmt))
    qvm_decode(qvm)
    lit = set(qvm_lit_strings(qvm))
    absent = [s for s in NEW_STRINGS + list(PAK8_UI_STRINGS.values()) if s not in lit]
    if absent:
        raise GateError("patched QVM lacks %r" % absent)
    hdr = qvm_header(qvm)
    print("GATE format: VM_MAGIC 0x%08x, header 32 B, no jump table, %d instructions"
          % (hdr["vmMagic"] & 0xffffffff, hdr["instructionCount"]))

    # GATE 4: the menu itself, driven by keystrokes in qvmsim (both builds)
    behaviour = behaviour_gate(builds["stock"], builds["patched"])
    for row in behaviour:
        print("GATE behaviour: %-62s stock %-10s patched %-20s ACCEPT r_mode %s %sx%s" % (
            row["name"], (row["stock"]["label"] + "->" + str(row["stock"]["r_mode"])),
            row["patched"]["chosen"], row["patched"]["r_mode"],
            row["patched"]["r_customwidth"] or "-", row["patched"]["r_customheight"] or "-"))

    # the pk3, byte-reproducible; built twice to prove it
    pk3 = make_pk3(qvm)
    if make_pk3(qvm) != pk3:
        raise GateError("pk3 writer is not deterministic")
    pp = pk3_problems(pk3)
    if pp:
        raise GateError("pk3: %s" % "; ".join(pp))

    pk3_local = os.path.join(outdir, PK3_REL.replace("/", os.sep))
    os.makedirs(os.path.dirname(pk3_local), exist_ok=True)
    with open(pk3_local, "wb") as f:
        f.write(pk3)
    qvm_local = os.path.join(outdir, "ui.qvm")
    with open(qvm_local, "wb") as f:
        f.write(qvm)

    manifest = {
        "key": KEY,
        "title": TITLE,
        "built_at": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
        "build_seconds": round(time.time() - t0, 1),
        "sources": {
            "id": {"repo": ID_REPO, "commit": ID_COMMIT, "paths": ID_PATHS},
            "ioq3_tools": {"repo": IOQ3_REPO, "commit": IOQ3_COMMIT, "paths": IOQ3_PATHS},
            "patch": {"file": os.path.relpath(PATCH_FILE, REPO), "md5": md5_file(PATCH_FILE)},
            "header": {"file": os.path.relpath(HEADER_FILE, REPO), "md5": md5_file(HEADER_FILE)},
            "release_132_tr_types": list(RELEASE_132_TR_TYPES),
            "q3asm_order": Q3ASM_ORDER,
        },
        "toolchain": dict(toolchain, q3asm_flags="-vq3 -m", lcc_flags=LCC_FLAGS),
        "gates": {
            "reproduction": {"stock_md5": md5_bytes(stock_qvm), "pak8_ui_md5": PAK8_UI_MD5,
                             "identical": True},
            "drift": drift,
            "format": {"vm_magic": "0x%08x" % (hdr["vmMagic"] & 0xffffffff), "header_bytes": 32,
                       "jump_table": False, "problems": []},
            "behaviour": behaviour,
        },
        "qvm": {"size": len(qvm), "md5": md5_bytes(qvm), "header": hdr, "local_path": qvm_local},
        "pk3": {"name": PK3_NAME, "member": PK3_MEMBER, "size": len(pk3), "md5": md5_bytes(pk3),
                "stored": True, "date_time": list(PK3_DATE)},
        "outputs": [],
        "server_install": {
            "unit": SERVER_UNIT, "port": SERVER_PORT,
            "path": os.path.join(server_baseq3(), PK3_NAME),
            "note": "install ONLY after every fleet box has the pk3 (GAMESYNC); the pure "
                    "server then lists it and a client without it is dropped as unpure "
                    "(or downloads it: sv_allowDownload 1 + cl_allowDownload 1)",
        },
    }

    # the GPL source bundle, published beside the patch record (not synced to boxes:
    # GAMESYNC skips the library's _-prefixed directories)
    srcdir = os.path.join(outdir, "source")
    os.makedirs(srcdir, exist_ok=True)
    bundle = []
    for src in (PATCH_FILE, HEADER_FILE, os.path.abspath(__file__),
                os.path.join(HERE, "qvmsim.py")):
        dst = os.path.join(srcdir, os.path.basename(src))
        shutil.copyfile(src, dst)
        bundle.append(dst)
    manifest["pk3"]["local_path"] = pk3_local
    txt = os.path.join(srcdir, "SOURCE.txt")
    with open(txt, "w", newline="") as f:
        f.write(source_txt(manifest))
    bundle.append(txt)

    for path in bundle:
        manifest["outputs"].append({
            "share_path": SOURCE_REL + "/" + os.path.basename(path),
            "local_path": path, "md5": md5_file(path), "size": os.path.getsize(path),
            "original_md5": None, "kind": "source",
        })
    # the pk3 LAST: it is what the boxes pick up, so it is the last thing published
    manifest["outputs"].append({
        "share_path": SHARE_LIB_REL + "/" + TITLE + "/" + PK3_REL,
        "local_path": pk3_local, "md5": md5_bytes(pk3), "size": len(pk3),
        "original_md5": None, "kind": "pk3",
        "original_note": "new file; pak8.pk3 is NOT modified - it is overridden. "
                         "Rollback = sharewrite.py rm this path, then delete it on each box.",
        "overrides": {"share_path": SHARE_LIB_REL + "/" + TITLE + "/" + PAK8_REL,
                      "md5": PAK8_MD5, "member": PK3_MEMBER, "member_md5": PAK8_UI_MD5},
    })
    with open(os.path.join(outdir, "manifest.json"), "w") as f:
        json.dump(manifest, f, indent=1, sort_keys=False)
        f.write("\n")
    print("pk3      %s  %d B  md5 %s" % (pk3_local, len(pk3), md5_bytes(pk3)))
    print("ui.qvm   %d B  md5 %s" % (len(qvm), md5_bytes(qvm)))
    print("manifest %s" % os.path.join(outdir, "manifest.json"))
    return manifest


# ============================================================================
# --publish  (FUTURE: not run in the build phase)
# ============================================================================
def load_manifest(outdir):
    p = os.path.join(outdir, "manifest.json")
    if not os.path.isfile(p):
        raise SystemExit("no %s - run --build first" % p)
    m = json.load(open(p))
    for o in m["outputs"]:
        if md5_file(o["local_path"]) != o["md5"]:
            raise SystemExit("%s changed since the build (md5) - rebuild" % o["local_path"])
    return m


def mnt_path(share_rel):
    """'Files/Games-Library/X' -> '/mnt/retro-share/Files/Games-Library/X' (read side)."""
    return os.path.join(MNT, *share_rel.split("/"))


def backup_rel(dest):
    """Where --publish keeps the copy it is about to replace: the same path
    relative to the title tree (or to the title's _patches record)."""
    tree = SHARE_LIB_REL + "/" + TITLE + "/"
    record = SHARE_LIB_REL + "/_patches/" + TITLE + "/"
    if dest.startswith(tree):
        return BACKUP_REL + "/" + dest[len(tree):]
    if dest.startswith(record):
        return BACKUP_REL + "/" + dest[len(record):]
    raise ValueError("not a path this patch publishes: %s" % dest)


def sharewrite(local, dest, dry_run):
    cmd = [sys.executable, SHAREWRITE, "put", local, dest] + (["--dry-run"] if dry_run else [])
    print("  $ " + " ".join(cmd[1:]))
    return subprocess.run(cmd).returncode


def publish(outdir, dry_run):
    ok, _ = check(verbose=False)
    if not ok:
        raise SystemExit("--check fails: the staged originals are not what this patch was built on")
    m = load_manifest(outdir)
    for o in m["outputs"]:
        dest = o["share_path"]
        cur = mnt_path(dest)
        if os.path.isfile(cur):
            have = md5_file(cur)
            if have == o["md5"]:
                print("current  %s (md5 %s) - skipped" % (dest, have))
                continue
            # an older copy: back it up first, same relative path
            bdest = backup_rel(dest)
            bcur = mnt_path(bdest)
            if os.path.isfile(bcur) and md5_file(bcur) != have:
                # NEVER overwrite a backup: the first one there may be the only
                # copy of an original. Keep this replaced copy beside it.
                bdest = "%s.%s" % (bdest, have[:8])
                bcur = mnt_path(bdest)
            if not (os.path.isfile(bcur) and md5_file(bcur) == have):
                print("backup   %s -> %s" % (dest, bdest))
                if not dry_run:
                    tmp = os.path.join(outdir, "_backup", os.path.basename(dest))
                    os.makedirs(os.path.dirname(tmp), exist_ok=True)
                    shutil.copyfile(cur, tmp)
                    if md5_file(tmp) != have:
                        raise SystemExit("backup read of %s did not hold still - stopping" % cur)
                    if sharewrite(tmp, bdest, False) != 0:
                        raise SystemExit("backup of %s FAILED - stopping before replacing it" % dest)
        print("put      %s (md5 %s)" % (dest, o["md5"]))
        if sharewrite(o["local_path"], dest, dry_run) != 0:
            raise SystemExit("put %s FAILED - stopping (nothing after it was written)" % dest)
    if dry_run:
        print("\ndry-run: nothing was written")
        return
    print("\npublished. Next:")
    print("  1. python3 scripts/validate-staged-library.py --quiet")
    print("  2. carry it to every box: a provisioned box's GAMESYNC idles until RESET +")
    print("     START. Bump the one line of Files/Games-Library/%s (shared by" % DEPLOY_GENERATION_FILE)
    print("     every title's fix - bump it ONCE after all of the night's publishes) and")
    print("     retro-autodeploy re-syncs each box as it comes online, the ones that are")
    print("     off now included; or GAMESYNC RESET + START a box by hand. Then check")
    print("     <GamesDir>\\%s\\baseq3\\%s is %d B (GamesDir is C:\\Games unless the"
          % (TITLE, PK3_NAME, m["pk3"]["size"]))
    print("     box sets HKLM\\Software\\RetroAgent\\GamesDir)")
    print("  3. only then: apply.py --install-server (the pure server). It reads")
    print("     retro-autodeploy's records and refuses while a box has not synced since")
    print("     the pk3 reached the share.")


# ============================================================================
# --install-server  (FUTURE: not run in the build phase)
# ============================================================================
def fleet_readiness(published_at, state_path=None):
    """Which boxes have completed a GAMESYNC since the pk3 reached the share, by
    retro-autodeploy's own records (AUTODEPLOY_STATE: one record per box, keyed
    by hostname, written only after a sync met its post-condition; "at" is the
    host's local time). Returns (rows, error), rows = [(box, ip, at, ready)].

    A legacy address-keyed record (no hostname) whose address a hostname record
    now holds is left out: that box was re-keyed, not lost. Anything else that
    cannot be shown ready is NOT ready - a box autodeploy never reached, or one
    synced only by hand, reads as not ready, never as ready. Evidence, not
    proof: spot-check a box with DIRLIST before trusting a borderline row."""
    path = state_path or AUTODEPLOY_STATE
    try:
        with open(path) as f:
            st = json.load(f)
    except (OSError, ValueError) as e:
        return [], "cannot read %s (%s)" % (path, e)
    if not isinstance(st, dict):
        return [], "%s is not a map of box records" % path
    named_ips = {r.get("ip") for r in st.values() if isinstance(r, dict) and r.get("hostname")}
    rows = []
    for key in sorted(st):
        rec = st[key]
        if not isinstance(rec, dict):
            continue
        ip = rec.get("ip") or key
        if not rec.get("hostname") and ip in named_ips:
            continue
        at = rec.get("at")
        try:
            t = time.mktime(time.strptime(at, "%Y-%m-%d %H:%M:%S"))
        except (TypeError, ValueError, OverflowError):
            t = None
        rows.append((key, ip, at or "?", t is not None and t >= published_at))
    return rows, None


def install_server(outdir, dry_run, force):
    m = load_manifest(outdir)
    pk3 = m["pk3"]["local_path"]
    want = m["pk3"]["md5"]
    sdir = server_baseq3()
    if not os.path.isdir(sdir):
        raise SystemExit("server baseq3 %s not found" % sdir)
    sp8 = find_ci(sdir, "pak8.pk3")
    if not sp8 or md5_file(sp8) != PAK8_MD5:
        raise SystemExit("server pak8.pk3 is not the library's (md5 %s) - refusing" % PAK8_MD5)
    share = mnt_path(SHARE_LIB_REL + "/" + TITLE + "/" + PK3_REL)
    if not (os.path.isfile(share) and md5_file(share) == want):
        msg = ("the library does not carry this pk3 yet (%s). A pure server that lists it drops "
               "every client that lacks it - publish and GAMESYNC the fleet first" % share)
        if not force:
            raise SystemExit("REFUSED: " + msg + " (or --force)")
        print("WARNING: " + msg)
    else:
        # the library has it; has every BOX had a sync since? A box without the
        # pk3 is dropped by the pure server ("Unpure Client ... set
        # cl_allowDownload 1") - the fleet's clients leave that at its default 0.
        published_at = os.path.getmtime(share)
        rows, err = fleet_readiness(published_at)
        print("fleet readiness - GAMESYNC since the pk3 reached the share (%s), by %s:"
              % (time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(published_at)), AUTODEPLOY_STATE))
        for box, ip, at, ready in rows:
            print("  %-6s %-18s %-15s last sync %s" % ("ready" if ready else "NOT", box, ip, at))
        late = [r for r in rows if not r[3]]
        if err:
            why = err
        elif late:
            why = "%d box(es) have not synced since the pk3 was published: %s" % (
                len(late), ", ".join(r[0] for r in late))
        elif not rows:
            why = "retro-autodeploy has no box records at all"
        else:
            why = None
        if why:
            msg = ("%s - bump %s and let retro-autodeploy catch them up; a listed box "
                   "that does not carry %s at all is fine (--force)"
                   % (why, DEPLOY_GENERATION_FILE, TITLE))
            if not force:
                raise SystemExit("REFUSED: " + msg)
            print("WARNING: " + msg)
    dest = os.path.join(sdir, PK3_NAME)
    if os.path.isfile(dest) and md5_file(dest) == want:
        print("current  %s (md5 %s) - nothing to do" % (dest, want))
    else:
        print("install  %s -> %s" % (pk3, dest))
        if dry_run:
            print("dry-run: nothing was written")
            return
        tmp = dest + ".tmp"            # not *.pk3: ioq3ded never loads a half-copied file
        try:
            shutil.copyfile(pk3, tmp)
            os.replace(tmp, dest)
        finally:
            if os.path.exists(tmp):
                os.remove(tmp)
        if md5_file(dest) != want:
            os.remove(dest)
            raise SystemExit("installed copy did not verify - removed it again")
        print("verified %s md5 %s" % (dest, want))
    print("\nThen restart the server so its pure list carries the pk3:")
    print("  systemctl --user restart %s" % SERVER_UNIT)
    print("  journalctl --user -u %s -n 60 | grep -i %s     # in its search path"
          % (SERVER_UNIT, PK3_NAME))
    print("(ioq3ded also rescans at the next map load: SV_SpawnServer restarts the filesystem.)")
    print("Rollback: rm %s && systemctl --user restart %s" % (dest, SERVER_UNIT))


# ============================================================================
def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--check", action="store_true")
    g.add_argument("--build", nargs="?", const=DEFAULT_OUT, metavar="OUTDIR")
    g.add_argument("--publish", nargs="?", const=DEFAULT_OUT, metavar="OUTDIR")
    g.add_argument("--install-server", nargs="?", const=DEFAULT_OUT, metavar="OUTDIR")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--force", action="store_true",
                    help="--install-server although the library lacks the pk3, or a box has "
                         "not synced since it was published (read the list it prints first)")
    ap.add_argument("--library", default=LIBRARY)
    a = ap.parse_args(argv)
    if a.check:
        ok, _ = check(a.library)
        return 0 if ok else 1
    if a.build:
        ok, _ = check(a.library)
        if not ok:
            print("refusing to build: --check failed")
            return 1
        try:
            build(a.build)
        except GateError as e:
            print("BUILD FAILED: %s" % e)
            return 2
        return 0
    if a.publish:
        publish(a.publish, a.dry_run)
        return 0
    install_server(a.install_server, a.dry_run, a.force)
    return 0


if __name__ == "__main__":
    sys.exit(main())
