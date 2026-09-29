#!/usr/bin/env python3
"""id Tech 3 kin (Jedi Academy, Soldier of Fortune II, RTCW): put the panel's
own mode INTO THE IN-GAME VIDEO MENU.

WHAT WAS VERIFIED BEFORE THIS WAS WRITTEN (2026-09-29,
.claude/evidence-1080p/_results/titles-verified.json and
.claude/evidence-1080p/review-jka-sof2-rtcw/):

  * All three engines render 1920x1080 through `r_mode -1` + r_customwidth /
    r_customheight - jasp/jamp as staged, WolfSP/WolfMP as staged (measured on
    .240, R1/R2/F2), SoF2.exe as staged (P1) and sof2mp.exe ONLY after a
    two-byte patch of r_mode's registered MINIMUM (S1 stock -> 640x480, S2
    patched -> 1920x1080).
  * None of their video menus can express it. Every menu lists fixed 4:3
    indices, and with r_mode -1 the Video Mode line shows an EMPTY value
    (Item_Multi_Setting returns "" when nothing matches), and touching it moves
    the game off the panel with no way back short of relaunching.

WHAT THIS BUILDS

  JediAcademy   base/zz_fleet_video.pk3
                  ui/setup.menu, ui/ingamesetup.menu          (from assets1)
                  ui/jamp/setup.menu                          (from assets3 - the 1.01 copy)
                  ui/jamp/ingame_setup.menu                   (from assets1)
  SoldierOfFortune2
                sof2mp.exe  file 0xBA61A..0xBA61D  00 00 40 40 -> 00 00 80 BF
                  (r_mode's registered minimum 3.0f -> -1.0f; only 0xBA61C/D change)
                base/zz_fleet_video.pk3
                  ui/setup_display.menu                       (from update101 - MP)
                  menus/m_video.rmf                           (from therest - SP)
                  Strip/menu_video.sp                         (from update101 - SP labels)
  ReturnToCastleWolfenstein
                Main/zz_fleet_video.pk3
                  ui/system.menu, ui/ingame_system.menu       (from sp_pak3 - SP)
                  ui_mp/system.menu, ui_mp/ingame_system.menu (from mp_pak1 - MP)

Each Q3-style menu gets THREE edits, nothing else:

  1. the Video Mode cvarFloatList gains a `Custom -1` entry (JKA: the stock,
     localised @MENUS_CUSTOM string);
  2. three read-only text items drawn on the SAME row, right after the value,
     showing `<r_customwidth> x <r_customheight>` - visible only while the list
     is on -1 (cvarTest/showCvar "-1"), zero-width rects so they can never take
     the mouse, `decoration`. So the line reads `Video Mode: Custom 1920 x 1080`
     on an LCD and `Custom 1024 x 768` on a CRT: the label must not promise one
     number, because FLEETRES writes a different size on every box;
  3. after each graphics-QUALITY preset (`uiScript update "ui_glCustom"` /
     `"ui_r_glCustom"`) the mode is set back to -1. The review concluded a menu
     cannot stop the presets writing r_mode 4/3 - it can: the presets' writes
     are immediate trap_Cvar_SetValue calls (disassembled: uix86 0x4000B747..,
     ui_mp_x86 0x4000D2A6.., sof2mp_ui.qvm syscall -7, JKA uix86 0x4000B706..),
     and the menu script runs its next command after the uiScript returns.

  The RMF menu of SoF2 single player gets `match "...,-1"` and its label list
  (Strip VIDMODES) gains `,Custom`; RMF has no cvar-text overlay.

WHY zz_: every one of these engines lists <gamedir>/*.pk3, sorts the names
with FS_PathCmp and lets the LATER name win (Q3 paksort; observed here:
assets3 overrides assets1, update101 overrides mp.pk3, sp_pak3 overrides
sp_pak1). FS_PathCmp UPPERCASES a-z before comparing - read in all six
binaries (review 2026-09-29): jamp.exe 0x43A60B, jasp.exe 0x41CD39,
sof2mp.exe 0x44DC22, SoF2.exe 0x1003A478, WolfMP.exe 0x4234DB,
WolfSP.exe 0x4200A6 - so '_' (0x5F) sorts AFTER every letter ('Z' is 0x5A).
A lower-case sort key gets that backwards: `z_mod.pk3` would beat
`zz_fleet_video.pk3` in the engine while a lower() sort says the opposite.
pk3_sort_key() models FS_PathCmp exactly. `zz_fleet_video.pk3` sorts after
every stock name in all three directories, and --check proves the copy each
menu AND each font is taken from is the one that currently wins.

FONTS: the overlay is positioned from the width of the value text, so the
font must be the one the engine DRAWS the value with. JKA font 4 = small2Font
"arialnb"; SoF2 textfont "hud"; RTCW picks per item (uix86.dll 0x40002AD0,
ui_mp_x86.dll 0x40002D3E): textfont 1 (UI_FONT_NORMAL) = the 16-pt textFont,
textfont 0 = by scale - <= ui_smallFont (0.25) the 12-pt smallFont, > 0.4
the bigFont. RTCW's renderer loads fonts/fontImage_<points>.dat whatever the
name, so the SP menus (no textfont, textscale .22/.23) draw with
fontImage_12.dat and the MP menus (UI_FONT_NORMAL) with fontImage_16.dat.
The first build measured the SP menus with the 16-pt data: "Custom" ended
exactly where the width began and a 4-digit width ran over the "x".

NO SERVER NEEDS THE PK3: jka-server, sof2-server and rtcw-server all run
sv_pure 0 (read from their server.cfg). --install-server re-reads that and
refuses to copy anything unless a server has become pure (or --force).

DEPLOY ORDER (the pk3s must not reach a box before their launcher change),
ENFORCED by --publish through PUBLISH_GATES, which reads the share itself:
  * sof2mp.exe first - it is safe alone: the old launchers still pass an
    index (7/6), and lowering the minimum does not touch an index.
  * RTCW/SoF2 launchers still pass a mode INDEX. A menu that sets r_mode -1
    with r_customwidth unset renders the engine's default custom size, so the
    RTCW and SoF2 pk3s wait for the launcher move to r_mode -1 +
    r_customwidth/height (launcher_changes in manifest.json).
  * SoF2's pk3 without the patched sof2mp.exe would offer MP a -1 that the
    stock exe clamps to 640x480 (S1) while the menu says "Custom" - its gate
    also requires the share's sof2mp.exe to BE the patched md5.
  * JKA's launchers already run r_mode -1; its pk3 is independent.

Modes (only --check and --build are meant to be run in the build phase):
    --check                 verify every staged original and every anchor
    --check --full          ... plus the whole-file md5 of every source pk3
    --build [OUTDIR]        write patched files + manifest.json + diffs
    --publish [OUTDIR]      FUTURE: back up originals, then sharewrite.py put
    --install-server        FUTURE: copy a pk3 into a PURE fleet server

Game data is copyrighted: outputs go to ~/.retro-fleet/patch-out/idtech3-kin,
never into git. They are regenerated from the staged originals.
"""
import argparse
import difflib
import hashlib
import io
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import zipfile

KEY = "idtech3-kin"
HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
SHARE_ROOT = "/mnt/retro-share"
LIB_REL = "Files/Games-Library"
LIB = os.path.join(SHARE_ROOT, LIB_REL)
DEFAULT_OUT = os.path.expanduser("~/.retro-fleet/patch-out/%s" % KEY)
SHAREWRITE = os.path.join(REPO, "scripts", "fleet", "sharewrite.py")
BACKUP_TAG = "originals-2026-09-29"
PK3_NAME = "zz_fleet_video.pk3"
# Fixed zip timestamp so a rebuild from the same originals is byte-identical.
ZIP_DATE = (2026, 9, 29, 0, 0, 0)

# --------------------------------------------------------------------------
# The staged originals, pinned. A pk3 is pinned by SIZE plus the md5 of every
# MEMBER this script reads (hashing a 650 MB pk3 over CIFS on every --check is
# pointless); --check --full also hashes the whole file.
# --------------------------------------------------------------------------
ORIGINALS = {
    # rel path under Games-Library: (size, whole-file md5)
    "JediAcademy/base/assets1.pk3": (652804510, "3f47006dde61f171769666f31065a34c"),
    "JediAcademy/base/assets3.pk3": (9282026, "71c2edf5a30135d4aa921bf6259946ee"),
    "SoldierOfFortune2/sof2mp.exe": (1572917, "0263922b67e3474f9e77e769f217f53a"),
    "SoldierOfFortune2/base/update101.pk3": (9668867, "9d9ee621e26ffafb8fddc1619ac3b7ee"),
    "SoldierOfFortune2/base/therest.pk3": (69463128, "4cffdaeead89e89ed03c4ebb24ae3ae8"),
    "ReturnToCastleWolfenstein/Main/sp_pak3.pk3": (152544, "cf25d6731ed29c80303febbb177aa585"),
    "ReturnToCastleWolfenstein/Main/mp_pak1.pk3": (4449596, "22b972974f499a237c96a2200b0d019e"),
    "ReturnToCastleWolfenstein/Main/pak0.pk3": (315823656, "ce92b11df889cb0a045762bb5fd7cde5"),
}

# md5 of each pk3 member read, as staged on 2026-09-29.
MEMBERS = {
    ("JediAcademy/base/assets1.pk3", "ui/setup.menu"): "fceabe412ebe7f25e05a10a64747699a",
    ("JediAcademy/base/assets1.pk3", "ui/ingamesetup.menu"): "5c95ff28f03e909b729c96e7bc8a7ba7",
    ("JediAcademy/base/assets3.pk3", "ui/jamp/setup.menu"): "032a4a9c916e3ba67b247e9b9a984003",
    ("JediAcademy/base/assets1.pk3", "ui/jamp/ingame_setup.menu"): "ae0fbda9d3662658aed0cd70c56ce828",
    ("JediAcademy/base/assets1.pk3", "fonts/arialnb.fontdat"): "e4f927c1d9841faa82cf05bc98a06d83",
    ("SoldierOfFortune2/base/update101.pk3", "ui/setup_display.menu"): "9c16721f1d78654b23a0751468a9b310",
    ("SoldierOfFortune2/base/therest.pk3", "menus/m_video.rmf"): "d0bbf978a97fc1af4480a2396b4d21a7",
    ("SoldierOfFortune2/base/update101.pk3", "Strip/menu_video.sp"): "d03e523a4328782d7cc0a97f47a9ccf5",
    ("SoldierOfFortune2/base/therest.pk3", "fonts/hud.fontdat"): "cbab2a0a22545eabdf80eb40892ea8ea",
    ("ReturnToCastleWolfenstein/Main/sp_pak3.pk3", "ui/system.menu"): "2e726bd333bf8555d1668e1e066a1a37",
    ("ReturnToCastleWolfenstein/Main/sp_pak3.pk3", "ui/ingame_system.menu"): "766326c734772034d5935e49a89a45a2",
    ("ReturnToCastleWolfenstein/Main/mp_pak1.pk3", "ui_mp/system.menu"): "71ca9d89046cb285b04076ec5b1b2f21",
    ("ReturnToCastleWolfenstein/Main/mp_pak1.pk3", "ui_mp/ingame_system.menu"): "8b154829e7e7257d4e4320cca62065c6",
    ("ReturnToCastleWolfenstein/Main/pak0.pk3", "fonts/fontImage_16.dat"): "32a2100f57bde8a4fc4c5f3776a772d5",
    ("ReturnToCastleWolfenstein/Main/pak0.pk3", "fonts/fontImage_12.dat"): "0f755299ce9114e7bd7aad722c54abdf",
}

# The outputs of the build reviewed on 2026-09-29 (deterministic: fixed zip
# timestamps, fixed member order). A rebuild that differs is not wrong by
# itself - a deliberate edit to this script changes them - but it is no longer
# the build that was reviewed, and --build says so.
# RTCW's pk3 was b88dbdce... in the first build: its SP overlay was placed with
# the 16-pt font's metrics while the engine draws those menus with the 12-pt
# smallFont, so the width touched "Custom" and ran over the "x". The review
# rebuilt it (d220e773...); the other three outputs are byte-identical.
REVIEWED_BUILD = {
    "JediAcademy/base/zz_fleet_video.pk3": "3c1b3ca3afd41fe5e342243bfce8aab7",
    "SoldierOfFortune2/base/zz_fleet_video.pk3": "d894ac0f376dd59986f0b589d8fac7ed",
    "SoldierOfFortune2/sof2mp.exe": "30f099da5ab0b9a5c29658edf07c7c2e",
    "ReturnToCastleWolfenstein/Main/zz_fleet_video.pk3": "d220e7735caeca3a3f9939e398fb455c",
}

# --------------------------------------------------------------------------
# SoF2 MP: r_mode's registration in R_Register. Cvar_Get takes min/max only
# from the FIRST registration carrying flag 0x2000 (0x44B2E5), which is this
# one, so nothing later can re-impose 3.0. Verified on .240 (S2).
#   file 0xBA614: 68 00 00 20 41   push 10.0f  (max)
#                 68 00 00 40 40   push  3.0f  (min)   <- patched to -1.0f
#                 68 21 20 00 00   push 0x2021 (flags)
# --------------------------------------------------------------------------
SOF2MP = "SoldierOfFortune2/sof2mp.exe"
SOF2MP_CONTEXT = (0xBA614, bytes.fromhex("680000204168000040406821200000"))
SOF2MP_PATCH = [(0xBA61A, bytes.fromhex("00004040"), bytes.fromhex("000080bf"))]
SOF2MP_PATCHED_MD5 = "30f099da5ab0b9a5c29658edf07c7c2e"  # the .240 test copy, S2

# --------------------------------------------------------------------------
# Font metrics: the per-glyph horizontal advance, from the game's own files.
# --------------------------------------------------------------------------


def raven_advances(data):
    """Raven .fontdat (JKA, SoF2): 256 x glyphInfo_t {short width, height,
    horizAdvance, horizOffset; int baseline; float s, t, s2, t2} (28 bytes),
    then the point size. Drawn at textscale x advance."""
    if len(data) < 256 * 28:
        raise ValueError("fontdat too short: %d" % len(data))
    return [struct.unpack_from("<hhhhiffff", data, i * 28)[2] * 1.0 for i in range(256)]


def q3_advances(data):
    """Quake III Team Arena fontImage_<pt>.dat (RTCW): 256 x glyphInfo_t {int
    height, top, bottom, pitch, xSkip, imageWidth, imageHeight; float s, t, s2,
    t2; qhandle_t glyph; char shaderName[32]} (80 bytes), then float glyphScale.
    Text_Width = sum(xSkip) * textscale * glyphScale."""
    if len(data) < 256 * 80 + 4:
        raise ValueError("font .dat too short: %d" % len(data))
    gs = struct.unpack_from("<f", data, 256 * 80)[0]
    return [struct.unpack_from("<7i", data, i * 80)[4] * gs for i in range(256)]


def text_width(s, adv, scale):
    return sum(adv[ord(c) & 0xFF] for c in s) * scale


# RTCW's Text_Width font choice, read out of both UI DLLs (uix86.dll
# 0x40002AD0, ui_mp_x86.dll 0x40002D3E). ui_smallFont/ui_bigFont defaults are
# 0.25/0.4 and every fleet wolfconfig*.cfg read (.145, .240) carries exactly
# those. The point sizes are the assetGlobalDef's (ui/main.menu,
# ui_mp/main.menu: font 16, smallFont 12). Only the two branches these four
# menus reach are allowed; a menu that lands anywhere else (bigFont is 24 in
# one assetGlobalDef and 20 in another) is refused rather than guessed.
RTCW_UI_FONT = {"UI_FONT_DEFAULT": 0, "UI_FONT_NORMAL": 1, "UI_FONT_BIG": 2,
                "UI_FONT_SMALL": 3, "UI_FONT_HANDWRITING": 4}
RTCW_SMALLFONT_MAX, RTCW_BIGFONT_MIN = 0.25, 0.4
RTCW_FONT_POINTS = {"small": 12, "normal": 16}


def rtcw_font_points(textfont, textscale):
    """Point size of the font RTCW draws an item's text with."""
    tf = (textfont or "0").strip().strip('"')
    font = RTCW_UI_FONT[tf.upper()] if tf.upper() in RTCW_UI_FONT else int(tf)
    if font == 0:
        if textscale <= RTCW_SMALLFONT_MAX:
            kind = "small"
        elif textscale >= RTCW_BIGFONT_MIN:
            # exactly 0.4 is bigFont in MP (fcomp; test ah,1) and textFont in
            # SP (test ah,0x41) - refused along with every bigFont case
            kind = "big"
        else:
            kind = "normal"
    else:
        kind = {1: "normal", 2: "big", 3: "small", 4: "handwriting"}.get(font, "normal")
    if kind not in RTCW_FONT_POINTS:
        raise ValueError("RTCW item draws with the %s font (textfont %r, scale %s) - "
                         "not measured, refusing to guess its metrics" % (kind, textfont, textscale))
    return RTCW_FONT_POINTS[kind]


def resolve_font(spec, text):
    """(pk3 rel, member, kind) of the font the engine draws THIS menu's mode
    value with. A fixed font for JKA/SoF2; per menu for RTCW."""
    if spec.get("font"):
        return spec["font"]
    if spec.get("font_rule") == "rtcw":
        _, _, body = find_mode_item(text, spec["mode_cvar"])
        pts = rtcw_font_points(attr(body, "textfont"), float(attr(body, "textscale") or 1.0))
        return ("ReturnToCastleWolfenstein/Main/pak0.pk3", "fonts/fontImage_%d.dat" % pts, "q3")
    return None


# --------------------------------------------------------------------------
# Menu-file helpers. Menus are Latin-1 text with CRLF; every edit is done on
# the decoded string and re-encoded, and the line ending of the file is kept.
# --------------------------------------------------------------------------


def newline_of(text):
    return "\r\n" if "\r\n" in text else "\n"


def _skip_block(text, open_idx):
    """Index just past the '}' matching text[open_idx] == '{', ignoring braces
    inside "strings" and // comments."""
    depth, i, n = 0, open_idx, len(text)
    while i < n:
        c = text[i]
        if c == '"':
            j = text.find('"', i + 1)
            if j < 0:
                raise ValueError("unterminated string at %d" % i)
            i = j + 1
            continue
        if text.startswith("//", i):
            j = text.find("\n", i)
            i = n if j < 0 else j + 1
            continue
        if text.startswith("/*", i):
            j = text.find("*/", i + 2)
            i = n if j < 0 else j + 2
            continue
        if c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                return i + 1
        i += 1
    raise ValueError("unbalanced braces from %d" % open_idx)


def item_blocks(text):
    """[(start, end, body)] for every `itemDef { ... }` in the file."""
    out, i = [], 0
    while True:
        m = re.compile(r"\bitemDef\b", re.I).search(text, i)
        if not m:
            return out
        brace = text.find("{", m.end())
        end = _skip_block(text, brace)
        out.append((m.start(), end, text[m.start():end]))
        i = end


def strip_comments(s):
    s = re.sub(r"//[^\n]*", "", s)
    return re.sub(r"/\*.*?\*/", "", s, flags=re.S)


def attr(body, key):
    """The raw value of a one-line `key value` attribute of an item (the first
    one at item level), or None. Comments are ignored."""
    m = re.search(r"(?im)^[ \t]*%s[ \t]+([^\r\n]*?)[ \t]*$" % re.escape(key),
                  strip_comments(body.replace("\r", "")))
    return m.group(1) if m else None


def check_balanced(text):
    """Raise if braces or quotes do not balance at top level."""
    i, depth, n = 0, 0, len(text)
    while i < n:
        c = text[i]
        if c == '"':
            j = text.find('"', i + 1)
            if j < 0:
                raise ValueError("unterminated string at offset %d" % i)
            i = j + 1
            continue
        if text.startswith("//", i):
            j = text.find("\n", i)
            i = n if j < 0 else j + 1
            continue
        if c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth < 0:
                raise ValueError("unbalanced '}' at offset %d" % i)
        i += 1
    if depth != 0:
        raise ValueError("unbalanced braces: depth %d at end" % depth)


def find_mode_item(text, mode_cvar):
    """The ONE itemDef whose cvar is the video-mode cvar and that carries a
    cvarFloatList. Anything else is a staged tree we did not measure."""
    hits = []
    for s, e, body in item_blocks(text):
        c = attr(body, "cvar")
        if c and c.strip('"').lower() == mode_cvar.lower() and \
                re.search(r"\bcvarFloatList\b", body, re.I):
            hits.append((s, e, body))
    if len(hits) != 1:
        raise ValueError("expected exactly one %s cvarFloatList item, found %d"
                         % (mode_cvar, len(hits)))
    return hits[0]


def float_list_span(body):
    """(open, close) indices of the cvarFloatList braces inside an item body."""
    m = re.search(r"\bcvarFloatList\b", body, re.I)
    if not m:
        raise ValueError("no cvarFloatList")
    o = body.find("{", m.end())
    c = _skip_block(body, o) - 1
    return o, c


def list_values(body):
    """The numeric values of a cvarFloatList, in order (labels skipped)."""
    o, c = float_list_span(body)
    inner = strip_comments(body[o + 1:c])
    toks = re.findall(r'"[^"]*"|\S+', inner)
    vals = []
    for label, val in zip(toks[0::2], toks[1::2]):
        vals.append(float(val))
    return vals


def add_custom_entry(text, mode_cvar, label):
    """Append `<label> -1` to the video-mode cvarFloatList. Refuses a list that
    already has -1 (a second run would add it twice)."""
    s, e, body = find_mode_item(text, mode_cvar)
    if -1.0 in list_values(body):
        raise ValueError("%s list already carries -1" % mode_cvar)
    o, c = float_list_span(body)
    before = body[:c].rstrip(" \t")      # up to the last entry (or its newline)
    closing = body[len(before):]         # indent of the '}' line + '}' + rest
    if before.endswith("\n"):
        # multi-line list: the entry goes on its own line, indented like the
        # last entry, and the closing '}' line is left exactly as it was
        prev = before.rstrip("\r\n").split("\n")[-1]
        indent = re.match(r"[ \t]*", prev).group(0)
        nl = "\r\n" if before.endswith("\r\n") else "\n"
        new_body = before + indent + "%s -1" % label + nl + closing
    else:
        new_body = before + " %s -1 " % label + body[c:]
    return text[:s] + new_body + text[e:]


def keep_custom_after_presets(text, preset_cvar, mode_cvar):
    """After every `uiScript update "<preset_cvar>"` put the mode back to -1.
    Returns (text, count)."""
    pat = re.compile(r'(uiScript[ \t]+update[ \t]+"%s")' % re.escape(preset_cvar), re.I)
    tail = ' ; setcvar %s "-1"' % mode_cvar
    if tail.strip() in text:
        raise ValueError("preset re-assert already present")
    new, n = pat.subn(lambda m: m.group(1) + tail, text)
    return new, n


def overlay_positions(body, adv, scale, custom_text, gap=None):
    """x of the width value, the 'x' and the height value, drawn after the
    list's value on the same row. The value of a right-aligned multi item is
    drawn at rect.x + textalignx + 8 (Item_Multi_Paint: textRect.x + textRect.w
    + 8, and a right-aligned textRect ends at rect.x + textalignx)."""
    rect = [float(v) for v in attr(body, "rect").split()[:4]]
    tax = float(attr(body, "textalignx") or 0)
    align = (attr(body, "textalign") or "").strip()
    if align not in ("ITEM_ALIGN_RIGHT", "2"):
        raise ValueError("mode item is not right-aligned (%r)" % align)
    value_x = rect[0] + tax + 8
    sp = gap if gap is not None else max(4.0, text_width("  ", adv, scale))
    # widest 4-digit value: four of the widest digit (a proportional font
    # need not make '8' the widest - fontImage_12 has 32-unit '1' and '5')
    wide = 4 * max(adv[ord(d)] for d in "0123456789") * scale
    x_w = value_x + text_width(custom_text, adv, scale) + sp
    x_x = x_w + wide + sp / 2
    x_h = x_x + text_width("x", adv, scale) + sp / 2
    return [round(x_w), round(x_x), round(x_h)], rect


def add_size_overlay(text, mode_cvar, test_cvar, adv, scale, custom_text):
    """Insert three read-only text items after the mode item:
    r_customwidth, "x", r_customheight - shown only while test_cvar is -1."""
    s, e, body = find_mode_item(text, mode_cvar)
    xs, rect = overlay_positions(body, adv, scale, custom_text)
    nl = newline_of(text)
    # copy the look of the row (font, scale, colour, vertical alignment) and
    # its show/hide handle (name + group), nothing that paints a background
    keep = []
    for k in ("name", "group"):
        v = attr(body, k)
        if v:
            keep.append((k, v))
    look = []
    for k in ("font", "textfont", "textscale", "textstyle", "forecolor",
              "textaligny", "visible"):
        v = attr(body, k)
        if v is not None:
            look.append((k, v))
    ind = re.match(r"[ \t]*", text[text.rfind("\n", 0, s) + 1:s]).group(0) or "\t\t"
    fi = ind + "\t"
    items = []
    for x, what in zip(xs, [("cvar", '"r_customwidth"'), ("text", '"x"'),
                            ("cvar", '"r_customheight"')]):
        lines = [ind + "itemDef", ind + "{",
                 fi + "// fleet: the size r_mode -1 means on THIS box (FLEETRES)"]
        lines += ["%s%s\t%s" % (fi, k, v) for k, v in keep]
        lines += [fi + "type\tITEM_TYPE_TEXT",
                  "%s%s\t%s" % (fi, what[0], what[1]),
                  '%scvarTest\t"%s"' % (fi, test_cvar),
                  '%sshowCvar\t{ "-1" }' % fi,
                  "%srect\t%d %d 0 %d" % (fi, x, int(rect[1]), int(rect[3])),
                  fi + "textalignx\t0"]
        lines += ["%s%s\t%s" % (fi, k, v) for k, v in look]
        lines += [fi + "decoration", ind + "}"]
        items.append(nl.join(lines))
    ins = nl + nl + (nl + nl).join(items) + nl
    return text[:e] + ins + text[e:]


def patch_q3_menu(text, spec, adv):
    """All three edits on one Q3-family menu. Returns (new_text, report)."""
    before_items = len(item_blocks(text))
    _, _, body = find_mode_item(text, spec["mode_cvar"])
    scale = float(attr(body, "textscale") or 1.0)
    t = add_custom_entry(text, spec["mode_cvar"], spec["label"])
    t = add_size_overlay(t, spec["mode_cvar"], spec["mode_cvar"], adv,
                         scale * spec.get("font_scale", 1.0), spec["label_text"])
    t, npre = keep_custom_after_presets(t, spec["preset_cvar"], spec["mode_cvar"])
    check_balanced(t)
    after_items = len(item_blocks(t))
    if after_items != before_items + 3:
        raise ValueError("item count %d -> %d, expected +3" % (before_items, after_items))
    _, _, nb = find_mode_item(t, spec["mode_cvar"])
    vals = list_values(nb)
    if vals[-1] != -1.0 or vals.count(-1.0) != 1:
        raise ValueError("custom entry not last/unique: %r" % vals)
    if npre < spec.get("min_presets", 1):
        raise ValueError("expected >= %d preset actions, found %d"
                         % (spec.get("min_presets", 1), npre))
    return t, {"items_added": 3, "presets_reasserted": npre,
               "list_values": vals}


def patch_rmf_match(text):
    """SoF2 SP m_video.rmf: the resolution <list> matches "3,4,6,7,8,9"."""
    old = 'cvar menu_mode match "3,4,6,7,8,9"'
    if text.count(old) != 1:
        raise ValueError("m_video.rmf: expected one %r, found %d" % (old, text.count(old)))
    return text.replace(old, 'cvar menu_mode match "3,4,6,7,8,9,-1"')


def patch_strip_vidmodes(text):
    """SoF2 SP Strip/menu_video.sp: VIDMODES labels, one per match value."""
    m = re.search(r"REFERENCE VIDMODES\b(.*?)\n\}", text, re.S)
    if not m:
        raise ValueError("no VIDMODES entry")
    blk = m.group(1)
    langs = re.findall(r"TEXT_LANGUAGE(\d+)", blk)
    if langs != ["1"]:
        raise ValueError("VIDMODES carries languages %r; only 1 is handled" % langs)
    old = 'TEXT_LANGUAGE1 "640x480,800x600,1024x768,1152x864,1280x1024,1600x1200"'
    if blk.count(old) != 1:
        raise ValueError("VIDMODES text is not the measured one")
    new_blk = blk.replace(old, old[:-1] + ',Custom"')
    return text[:m.start(1)] + new_blk + text[m.end(1):]


# --------------------------------------------------------------------------
# The per-title plan
# --------------------------------------------------------------------------
JKA_MENU = dict(kind="q3menu", mode_cvar="ui_r_mode", preset_cvar="ui_r_glCustom",
                label="@MENUS_CUSTOM", label_text="Custom",
                font=("JediAcademy/base/assets1.pk3", "fonts/arialnb.fontdat", "raven"))
# RTCW: the font depends on the menu - see rtcw_font_points(). The SP menus
# (textscale .22/.23, no textfont) draw with the 12-pt smallFont, the MP ones
# (textfont UI_FONT_NORMAL) with the 16-pt textFont.
RTCW_MENU = dict(kind="q3menu", mode_cvar="r_mode", preset_cvar="ui_glCustom",
                 label='"Custom"', label_text="Custom", font_rule="rtcw")
SOF2_MENU = dict(kind="q3menu", mode_cvar="r_mode", preset_cvar="ui_glCustom",
                 label='"Custom"', label_text="Custom",
                 font=("SoldierOfFortune2/base/therest.pk3", "fonts/hud.fontdat", "raven"))

PLAN = {
    "JediAcademy": {
        "gamedir": "JediAcademy/base",
        "pk3": [
            ("ui/setup.menu", "JediAcademy/base/assets1.pk3", JKA_MENU),
            ("ui/ingamesetup.menu", "JediAcademy/base/assets1.pk3", JKA_MENU),
            ("ui/jamp/setup.menu", "JediAcademy/base/assets3.pk3", JKA_MENU),
            ("ui/jamp/ingame_setup.menu", "JediAcademy/base/assets1.pk3", JKA_MENU),
        ],
        "server": ("jka-server", "~/jka-server/base", "~/jka-server/base/server.cfg"),
    },
    "SoldierOfFortune2": {
        "gamedir": "SoldierOfFortune2/base",
        "pk3": [
            ("ui/setup_display.menu", "SoldierOfFortune2/base/update101.pk3",
             dict(SOF2_MENU, min_presets=1)),
            ("menus/m_video.rmf", "SoldierOfFortune2/base/therest.pk3", dict(kind="rmf")),
            ("Strip/menu_video.sp", "SoldierOfFortune2/base/update101.pk3",
             dict(kind="strip")),
        ],
        "exe": SOF2MP,
        "server": ("sof2-server", "~/sof2-server/base", "~/sof2-server/base/server.cfg"),
    },
    "ReturnToCastleWolfenstein": {
        "gamedir": "ReturnToCastleWolfenstein/Main",
        "pk3": [
            ("ui/system.menu", "ReturnToCastleWolfenstein/Main/sp_pak3.pk3", RTCW_MENU),
            ("ui/ingame_system.menu", "ReturnToCastleWolfenstein/Main/sp_pak3.pk3", RTCW_MENU),
            ("ui_mp/system.menu", "ReturnToCastleWolfenstein/Main/mp_pak1.pk3", RTCW_MENU),
            ("ui_mp/ingame_system.menu", "ReturnToCastleWolfenstein/Main/mp_pak1.pk3", RTCW_MENU),
        ],
        "server": ("rtcw-server", "~/rtcw-server/main", "~/rtcw-server/main/server.cfg"),
    },
}

# The launcher / GAMERES changes this patch depends on - handed to whoever owns
# stage-fleetres.py, gameres.h and the discmount specs. Also written into
# manifest.json so the publish step can refuse to run ahead of them.
LAUNCHER_CHANGES = [
    # 1 - the set that pinned both titles to an index
    "scripts/fleet/stage-fleetres.py: IDTECH3_NO_CUSTOM_MODE = set() - drop "
    "ReturnToCastleWolfenstein AND SoldierOfFortune2, and rewrite the comment "
    "above it and the TITLES comments that say either has 'NO r_mode -1 "
    "BRANCH'. Both render 1920x1080 through r_mode -1 on hardware (.240: R1/R2/"
    "F2 WolfMP/WolfSP, P1 SoF2.exe, S2 the patched sof2mp.exe). RTCW's .246 "
    "640x480 was the FIRST-RUN Com_SetRecommended pass (F1: no "
    "wolfconfig_mp.cfg -> highVidhighCPU.cfg -> setting mode 3); sof2mp's was "
    "r_mode's registered MINIMUM 3.0f (file 0xBA614), which this patch lowers "
    "to -1.0f.",
    # 2 - RTCW, the two generated launchers
    "stage-fleetres.py: idtech3_cfg(mod, wide=True, fov=True) - append the "
    "cg_fov line only when `wide and fov` (every existing caller keeps its "
    "bytes). In new_launcher() add, BEFORE the `elif title in "
    "IDTECH3_NO_CUSTOM_MODE` branch: `elif title == "
    "\"ReturnToCastleWolfenstein\": block = \"\\n\".join([CALL] + "
    "idtech3_cfg(mod, fov=False)); text = NEW_IDTECH3.format(title=disp, "
    "exe=exe, block=block, args=\"+set com_recommendedSet 1 \" + "
    "idtech3_args(fov=False))`. 'Play Return to Castle Wolfenstein.bat' and "
    "'Play RTCW Multiplayer.bat' are 'new' recipes, so put() rewrites them "
    "WHOLE: Main\\fleetres.cfg gets seta r_mode \"-1\", r_customwidth "
    "\"%FR_W%\", r_customheight \"%FR_H%\", r_customaspect \"1\", "
    "r_customPixelAspect \"1\", r_fullscreen \"1\", r_displayRefresh "
    "\"%FR_HZ%\" - no cg_fov - and the start line is `start \"\" WolfSP.exe` "
    "or `WolfMP.exe` then `+set com_recommendedSet 1 +set r_mode -1 +set "
    "r_customwidth %FR_W% +set r_customheight %FR_H% +set r_customaspect 1 "
    "+set r_customPixelAspect 1 +set r_fullscreen 1`. Their _REFRESH pairs "
    "(_refresh_fix(\"Main\", \"\")) stay satisfied because NEW_IDTECH3 puts a "
    "blank line after the block - confirm with --check.",
    # 3 - RTCW, the two hand-written LAN launchers
    "RTCW LAN pair - 'Host RTCW - LAN.bat', 'Join RTCW - LAN.bat' - is "
    "hand-written and already CALLs FLEETRES, so no recipe ever rewrites it: "
    "move it with repair pairs. Add TITLES['ReturnToCastleWolfenstein']['fix'] "
    "entries for both names (the _REFRESH loop extends them, it does not "
    "replace). Pair 1 (CRLF, exact): OLD = the three lines "
    "`>>\"%~dp0Main\\fleetres.cfg\" echo // r_mode -1 DOES NOT EXIST IN THIS "
    "ENGINE - a plain index,` + `>>\"%~dp0Main\\fleetres.cfg\" echo // and "
    "FR_Q3MODE not FR_Q2MODE: idTech3 mode 8 is 1280x1024.` + "
    "`>>\"%~dp0Main\\fleetres.cfg\" echo seta r_mode \"%FR_Q3MODE%\"`, each "
    "ending \\r\\n; NEW = five lines `>>\"%~dp0Main\\fleetres.cfg\" echo seta "
    "r_mode \"-1\"`, `... echo seta r_customwidth \"%FR_W%\"`, `... echo seta "
    "r_customheight \"%FR_H%\"`, `... echo seta r_customaspect \"1\"`, `... "
    "echo seta r_customPixelAspect \"1\"`, each ending \\r\\n. Pair 2: OLD "
    "`+set r_mode %FR_Q3MODE% +set r_fullscreen 1` -> NEW `+set "
    "com_recommendedSet 1 +set r_mode -1 +set r_customwidth %FR_W% +set "
    "r_customheight %FR_H% +set r_customaspect 1 +set r_customPixelAspect 1 "
    "+set r_fullscreen 1` (once per file). Pair 3, rem text only: Host's "
    "block from `rem RESOLUTION: a plain r_mode INDEX, not r_mode -1. Measured "
    "on .246` through `rem would widen an already-correct picture.` and "
    "Join's line `rem RESOLUTION: a plain r_mode INDEX - see \"Host RTCW - "
    "LAN.bat\".` -> a true note: r_mode -1 at the panel size; the .246 640x480 "
    "was the first-run preset pass that com_recommendedSet 1 skips; cg_fov "
    "stays 90. No parentheses in any new text.",
    # 4 - GAMERES, RTCW
    "agent/shared/gameres.h RTCW: rule arg1 \"idtech3-index-nofov\" -> a new "
    "body \"idtech3-custom-nofov\" = the NULL body minus cg_fov: `// written by "
    "GAMESYNC ...`, seta r_mode \"-1\", seta r_customwidth \"%W%\", seta "
    "r_customheight \"%H%\", seta r_customaspect \"1\", seta "
    "r_customPixelAspect \"1\", seta r_fullscreen \"1\", seta r_displayRefresh "
    "\"%FRHZ%\". gr_w_cfg checks each setting line for presence, so it must "
    "be a subset of what ALL FOUR RTCW launchers write into "
    "Main\\fleetres.cfg - items 2 and 3 give exactly these.",
    # 5 - GAMERES, SoF2 + the agent release
    "agent/shared/gameres.h SoF2: rule arg1 \"idtech3-index\" -> NULL (the "
    "standard body, cg_fov included - identical to idtech3_cfg('base')), so "
    "both SoF2 launchers and GAMERES write the SAME base\\fleetres.cfg (today "
    "SP and MP write two different bodies). Keep the \"idtech3-index\" body or "
    "delete it together with tests/native/test_gameres.c lines 436-441, which "
    "expand gr_cfg_body(\"idtech3-index\") - better, repoint that check at "
    "\"idtech3-custom-nofov\" (r_mode -1, r_customwidth 1920, NO cg_fov). "
    "Rewrite the comment above the two rules. gameres.h compiles into "
    "retro_agent.exe, so this is an AGENT RELEASE (tag bump, build, publish "
    "per CLAUDE.md): until a box runs it, its GAMERES writes the index body at "
    "every sync while the launcher writes -1 at every start - rendering follows "
    "the launcher, but that box reports non-zero 'value(s) changed' for "
    "these titles until it updates.",
    # 6 - SoF2 MP
    "SoF2 MP 'Play Soldier of Fortune II - Multiplayer.bat' is hand-written and "
    "already CALLs FLEETRES, so editing its 'launchers' recipe alone changes "
    "NOTHING on the share (patch_launcher skips any launcher carrying MARK). "
    "(a) TITLES['SoldierOfFortune2']['launchers'], BOTH entries: "
    "idtech3_modecfg(\"base\") -> idtech3_cfg(\"base\"), idtech3_modeargs() -> "
    "idtech3_args() - keeps a fresh launcher and the tests' _pre() truthful. "
    "(b) REPLACE the whole literal TITLES['SoldierOfFortune2']['fix'] dict with "
    "MP-only repair pairs - the _REFRESH loop re-adds the refresh pair for "
    "both names: M1 OLD `>>\"%~dp0base\\fleetres.cfg\" echo seta r_mode "
    "\"%FR_Q3MODE%\"\\r\\n` -> NEW the five lines `>>\"%~dp0base\\fleetres.cfg\" "
    "echo seta r_mode \"-1\"`, `... r_customwidth \"%FR_W%\"`, `... "
    "r_customheight \"%FR_H%\"`, `... r_customaspect \"1\"`, `... "
    "r_customPixelAspect \"1\"`, each ending \\r\\n; M2 OLD `start \"\" "
    "\"%~dp0sof2mp.exe\" +set r_mode %FR_Q3MODE% +set r_fullscreen 1 +set "
    "cg_fov %FR_FOV%` -> NEW `start \"\" \"%~dp0sof2mp.exe\" +set r_mode -1 "
    "+set r_customwidth %FR_W% +set r_customheight %FR_H% +set r_customaspect "
    "1 +set r_customPixelAspect 1 +set r_fullscreen 1 +set cg_fov %FR_FOV%`; "
    "M3 the rem block, staged lines 51-77, from `rem  RESOLUTION: r_mode -1 "
    "DOES NOT WORK IN THIS ENGINE. Measured on .123,` through `rem  1.32 and "
    "does not carry the custom-resolution code that was added there.` -> a "
    "true note: r_mode's registered minimum 3.0 at file 0xBA614, not a "
    "missing branch, made -1 render 640x480; the staged exe carries the "
    "0xBA61A patch; a STOCK exe with this launcher renders 640x480. The five "
    "existing 'fix' pairs MUST go: the two FR_Q2MODE->FR_Q3MODE MP pairs "
    "become 'neither old nor new present' and repair() fails the run, and the "
    "three SP pairs would revert the regenerated SP launcher - one replaces "
    "its r_customwidth/r_customheight echo lines with rem lines, leaving -1 "
    "with no size.",
    # 7 - SoF2 SP, the disc-mount spec
    "SoF2 SP: provisioning/discmount/specs/SoldierOfFortune2.json "
    "fleetres_block = \"\\r\\n\".join([CALL] + idtech3_cfg(\"base\")) and "
    "vars.GAMEARGS = idtech3_args() - exactly what JediAcademy.json carries "
    "(verified equal to those two expressions); regenerate 'Play Soldier of "
    "Fortune II.bat' with make-mount-launcher.py. "
    "test_shipped_launcher_matches_its_spec checks the share copy after "
    "publish.",
    # 8 - the tests that pin the old behaviour
    "Tests that pin the old behaviour, change them in the same commit: "
    "tests/python/test_fleetres_staging.py::"
    "test_sof2_uses_a_mode_index_and_the_right_table (asserts SoF2 never gets "
    "-1 - rewrite it to assert -1 + the custom size, citing S2/P1 and the exe "
    "patch); ::test_the_engines_that_do_have_the_minus_one_branch_keep_it (add "
    "SoldierOfFortune2 and ReturnToCastleWolfenstein); the docstring of "
    "::test_command_line_uses_custom_mode_not_a_mode_index; "
    "tests/native/test_gameres.c 436-441 (item 5). "
    "tests/python/test_gameres_mirror.py::"
    "test_a_shared_cfg_is_written_identically_by_both_writers reads the "
    "SHARE's launchers, so it is red between landing gameres.h and publishing "
    "the regenerated launchers - land and publish them in one checkpoint.",
    # 9 - FOV
    "SoF2 cg_fov: keep %FR_FOV% (106 on 16:9) - harmless; the MP cgame "
    "registers cg_fov 80..100 with flags 0x2001, so MP plays at 100. RTCW "
    "stays without cg_fov, as in the verified F2 run; the default 90 is vert- "
    "at 16:9 and a cg_fov value there is an untested, optional follow-up.",
    # 10 - the order, which --publish enforces
    "Deploy order, ENFORCED by apply.py --publish (PUBLISH_GATES): 1) "
    "sof2mp.exe - safe alone, the old launchers still pass index 7/6; 2) items "
    "1-8 staged and published, and the agent release; 3) only then the SoF2 "
    "and RTCW pk3s - --publish refuses each until the share's launchers carry "
    "the -1 idiom and, for SoF2, the share's sof2mp.exe has the patched md5. "
    "The JKA pk3 is independent. Every box then needs GAMESYNC RESET + START "
    "- a provisioned box's boot sync does nothing.",
    # 11 - docs
    "Docs after deploy: CLAUDE.md 'What cannot reach 1080p' drops SoF2 and "
    "RTCW; provisioning/fleetres/PER-TITLE-STATUS.md rows for both.",
]


# --------------------------------------------------------------------------
# I/O
# --------------------------------------------------------------------------


def share_path(rel):
    return os.path.join(LIB, rel.replace("\\", "/"))


def md5_bytes(b):
    return hashlib.md5(b).hexdigest()


def md5_file(p):
    h = hashlib.md5()
    with open(p, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


_ZIPS = {}


def open_pk3(rel):
    if rel not in _ZIPS:
        _ZIPS[rel] = zipfile.ZipFile(share_path(rel))
    return _ZIPS[rel]


def read_member(rel, name):
    z = open_pk3(rel)
    for i in z.infolist():
        if i.filename.lower() == name.lower():
            return z.read(i)
    raise KeyError("%s: no member %s (case-insensitive)" % (rel, name))


def pk3_sort_key(name):
    """FS_PathCmp's order: a-z UPPERCASED (so '_' sorts after every letter),
    '\\' and ':' read as '/', bytes compared as MSVC's signed char. See the
    module docstring for where this was read in each binary."""
    key = []
    for c in name:
        o = ord(c)
        if 0x61 <= o <= 0x7A:
            o -= 0x20
        if c in "\\:":
            o = 0x2F
        if o >= 0x80:
            o -= 0x100
        key.append(o)
    return key


def winning_pk3(pk3_names, member_lists, member):
    """Which pk3 serves `member`: of the pk3s that contain it, the one whose
    name sorts LAST (case-insensitively). Pure logic - tested without a share."""
    have = [p for p in pk3_names if member.lower() in
            {m.lower() for m in member_lists[p]}]
    if not have:
        return None
    return sorted(have, key=pk3_sort_key)[-1]


def gamedir_pk3s(gamedir_rel):
    d = share_path(gamedir_rel)
    return sorted([f for f in os.listdir(d) if f.lower().endswith(".pk3")],
                  key=pk3_sort_key)


def load_font(spec_font):
    rel, member, kind = spec_font
    data = read_member(rel, member)
    return raven_advances(data) if kind == "raven" else q3_advances(data)


def transform(kind, text, spec, adv):
    if kind == "q3menu":
        return patch_q3_menu(text, spec, adv)
    if kind == "rmf":
        return patch_rmf_match(text), {"match": "3,4,6,7,8,9,-1"}
    if kind == "strip":
        return patch_strip_vidmodes(text), {"vidmodes": "+Custom"}
    raise ValueError(kind)


def patch_exe_bytes(data):
    """sof2mp.exe: verify the context + original bytes, return patched bytes."""
    off, ctx = SOF2MP_CONTEXT
    if data[off:off + len(ctx)] != ctx:
        raise ValueError("sof2mp.exe: R_Register r_mode push sequence not at 0x%X "
                         "(found %s)" % (off, data[off:off + len(ctx)].hex()))
    out = bytearray(data)
    for o, old, new in SOF2MP_PATCH:
        if bytes(out[o:o + len(old)]) != old:
            raise ValueError("sof2mp.exe 0x%X: expected %s found %s"
                             % (o, old.hex(), bytes(out[o:o + len(old)]).hex()))
        out[o:o + len(new)] = new
    return bytes(out)


def build_pk3(members):
    """Deterministic pk3: members in the given order, fixed timestamp, deflate."""
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w", zipfile.ZIP_DEFLATED) as z:
        for name, data in members:
            zi = zipfile.ZipInfo(name, ZIP_DATE)
            zi.compress_type = zipfile.ZIP_DEFLATED
            zi.external_attr = 0o644 << 16
            z.writestr(zi, data)
    return buf.getvalue()


# --------------------------------------------------------------------------
# check / build
# --------------------------------------------------------------------------


class Report:
    def __init__(self):
        self.lines, self.failed = [], 0

    def ok(self, msg):
        self.lines.append("  ok    " + msg)

    def fail(self, msg):
        self.failed += 1
        self.lines.append("  FAIL  " + msg)

    def info(self, msg):
        self.lines.append("        " + msg)


def require_share():
    if not os.path.isdir(LIB):
        raise SystemExit("share not mounted: %s is absent - cannot read the "
                         "staged originals (nothing was checked)" % LIB)


def check(full=False, titles=None, rep=None):
    require_share()
    rep = rep or Report()
    for rel, (size, md5) in sorted(ORIGINALS.items()):
        if titles and rel.split("/")[0] not in titles:
            continue
        p = share_path(rel)
        if not os.path.isfile(p):
            rep.fail("%s: missing" % rel)
            continue
        st = os.path.getsize(p)
        if st != size:
            rep.fail("%s: size %d, pinned %d" % (rel, st, size))
            continue
        if full or not rel.lower().endswith(".pk3"):
            got = md5_file(p)
            if got != md5:
                rep.fail("%s: md5 %s, pinned %s" % (rel, got, md5))
                continue
            rep.ok("%s  %d  %s" % (rel, st, got))
        else:
            rep.ok("%s  size %d" % (rel, st))
    for (rel, member), md5 in sorted(MEMBERS.items()):
        if titles and rel.split("/")[0] not in titles:
            continue
        try:
            got = md5_bytes(read_member(rel, member))
        except Exception as e:
            rep.fail("%s!%s: %s" % (rel, member, e))
            continue
        if got != md5:
            rep.fail("%s!%s: md5 %s, pinned %s" % (rel, member, got, md5))
        else:
            rep.ok("%s!%s  %s" % (rel, member, got))
    for title, plan in PLAN.items():
        if titles and title not in titles:
            continue
        names = gamedir_pk3s(plan["gamedir"])
        lists = {n: open_pk3("%s/%s" % (plan["gamedir"], n)).namelist() for n in names}
        if sorted(names + [PK3_NAME], key=pk3_sort_key)[-1] != PK3_NAME:
            rep.fail("%s: %s would NOT sort last among %r" % (title, PK3_NAME, names))
        else:
            rep.ok("%s: %s sorts after %s" % (title, PK3_NAME, names[-1]))
        if PK3_NAME.lower() in [n.lower() for n in names]:
            rep.info("%s: %s is already staged" % (title, PK3_NAME))
        for member, src, spec in plan["pk3"]:
            win = winning_pk3([n for n in names if n.lower() != PK3_NAME.lower()],
                              lists, member)
            want = src.split("/")[-1]
            if win is None or win.lower() != want.lower():
                rep.fail("%s: %s is served by %s, the patch copies %s"
                         % (title, member, win, want))
            else:
                rep.ok("%s: %s is served by %s (the copy patched)" % (title, member, win))
            try:
                text = read_member(src, member).decode("latin-1")
                font = resolve_font(spec, text)
                if font:
                    # the metrics must come from the font file the ENGINE
                    # reads: pinned, and served by the pk3 we read it from
                    if (font[0], font[1]) not in MEMBERS:
                        rep.fail("%s: %s: font %s!%s is not pinned in MEMBERS"
                                 % (title, member, font[0], font[1]))
                    fwin = winning_pk3([n for n in names if n.lower() != PK3_NAME.lower()],
                                       lists, font[1])
                    if fwin is None or fwin.lower() != font[0].split("/")[-1].lower():
                        rep.fail("%s: %s: font %s is served by %s, metrics read from %s"
                                 % (title, member, font[1], fwin, font[0]))
                adv = load_font(font) if font else None
                _, info = transform(spec["kind"], text, spec, adv)
                if font:
                    info["font"] = "%s!%s" % (font[0].split("/")[-1], font[1])
                rep.ok("%s: %s transforms cleanly %s" % (title, member, info))
            except Exception as e:
                rep.fail("%s: %s: %s" % (title, member, e))
        if plan.get("exe"):
            try:
                data = open(share_path(plan["exe"]), "rb").read()
                new = patch_exe_bytes(data)
                got = md5_bytes(new)
                if got != SOF2MP_PATCHED_MD5:
                    rep.fail("%s: patched md5 %s, the hardware-tested copy was %s"
                             % (plan["exe"], got, SOF2MP_PATCHED_MD5))
                else:
                    rep.ok("%s: anchors match; patched md5 %s = the copy tested on .240"
                           % (plan["exe"], got))
            except Exception as e:
                rep.fail("%s: %s" % (plan["exe"], e))
        sv = server_purity(plan["server"])
        rep.info("%s: fleet server %s sv_pure=%s -> pk3 %s on the server"
                 % (title, plan["server"][0], sv,
                    "NOT needed" if sv == "0" else "REQUIRED"))
    return rep


def server_purity(server):
    unit, _dir, cfg = server
    p = os.path.expanduser(cfg)
    try:
        txt = open(p, encoding="latin-1").read()
    except OSError:
        return "unknown (no %s)" % cfg
    m = re.findall(r'(?im)^\s*set[as]?\s+sv_pure\s+"?(\d)"?', txt)
    return m[-1] if m else "unset (engine default 1)"


def build(outdir, titles=None):
    require_share()
    rep = check(titles=titles)
    if rep.failed:
        print("\n".join(rep.lines))
        raise SystemExit("check failed (%d) - nothing built" % rep.failed)
    os.makedirs(outdir, exist_ok=True)
    manifest = {"key": KEY, "outputs": [], "originals": [], "launcher_changes": LAUNCHER_CHANGES,
                "deploy_rules": [
                    "sof2mp.exe: safe on its own - the old launchers still pass "
                    "an index, which the lowered minimum does not touch.",
                    "JediAcademy pk3: independent (its launchers already run r_mode -1).",
                    "SoF2 pk3: only after the SoF2 launchers + spec + gameres "
                    "move to r_mode -1 AND the share's sof2mp.exe is patched.",
                    "RTCW pk3: only after all four RTCW launchers + gameres move "
                    "to r_mode -1 with com_recommendedSet 1.",
                    "--publish enforces the three rules above (PUBLISH_GATES); "
                    "--force overrides."]}
    diffs = []
    for title, plan in PLAN.items():
        if titles and title not in titles:
            continue
        members, sources = [], []
        for member, src, spec in plan["pk3"]:
            raw = read_member(src, member)
            text = raw.decode("latin-1")
            font = resolve_font(spec, text)
            adv = load_font(font) if font else None
            new, info = transform(spec["kind"], text, spec, adv)
            if font:
                info["font"] = "%s!%s" % (font[0].split("/")[-1], font[1])
            data = new.encode("latin-1")
            members.append((member, data))
            sources.append({"member": member, "from": "%s/%s" % (LIB_REL, src),
                            "member_md5": md5_bytes(raw), "patched_md5": md5_bytes(data),
                            "info": info})
            diffs.append("".join(difflib.unified_diff(
                text.replace("\r\n", "\n").splitlines(True),
                new.replace("\r\n", "\n").splitlines(True),
                "a/%s!%s" % (src, member), "b/%s/%s!%s" % (plan["gamedir"], PK3_NAME, member))))
        pk3 = build_pk3(members)
        rel = "%s/%s" % (plan["gamedir"], PK3_NAME)
        lp = os.path.join(outdir, rel)
        os.makedirs(os.path.dirname(lp), exist_ok=True)
        open(lp, "wb").write(pk3)
        pk3_row = {
            "title": title, "share_path": "%s/%s" % (LIB_REL, rel), "local_path": lp,
            "md5": md5_bytes(pk3), "size": len(pk3), "original_md5": None,
            "new_file": True, "members": sources,
            "publish_gate": PUBLISH_GATES.get(rel, {})}
        if plan.get("exe"):
            orig = open(share_path(plan["exe"]), "rb").read()
            new = patch_exe_bytes(orig)
            lp = os.path.join(outdir, plan["exe"])
            os.makedirs(os.path.dirname(lp), exist_ok=True)
            open(lp, "wb").write(new)
            manifest["outputs"].append({
                "title": title, "share_path": "%s/%s" % (LIB_REL, plan["exe"]),
                "local_path": lp, "md5": md5_bytes(new), "size": len(new),
                "original_md5": md5_bytes(orig), "new_file": False,
                "backup_share_path": "%s/_patches/%s/%s/%s" % (
                    LIB_REL, title, BACKUP_TAG, plan["exe"].split("/", 1)[1]),
                "patch": [{"offset": "0x%X" % o, "old": a.hex(), "new": b.hex()}
                          for o, a, b in SOF2MP_PATCH]})
        # the pk3 AFTER the binary it depends on: --publish replaces binaries
        # first and gates the pk3 on them (PUBLISH_GATES)
        manifest["outputs"].append(pk3_row)
    for rel, (size, md5) in sorted(ORIGINALS.items()):
        if titles and rel.split("/")[0] not in titles:
            continue
        manifest["originals"].append({"share_path": "%s/%s" % (LIB_REL, rel),
                                      "size": size, "md5": md5})
    with open(os.path.join(outdir, "manifest.json"), "w") as f:
        json.dump(manifest, f, indent=1)
    with open(os.path.join(outdir, "menus.diff"), "w", encoding="latin-1") as f:
        f.write("\n".join(diffs))
    return manifest, rep


# --------------------------------------------------------------------------
# FUTURE: publish / install-server. Implemented, NOT run in the build phase.
# --------------------------------------------------------------------------


def sharewrite_put(local, dest):
    r = subprocess.run([sys.executable, SHAREWRITE, "put", local, dest])
    return r.returncode == 0


# THE DEPLOY ORDER, ENFORCED rather than documented. A pk3 adds a "Custom"
# (-1) entry to the menu; published ahead of its launcher it offers -1 with
# r_customwidth/height at whatever the box last saved, and SoF2 MP's STOCK
# exe clamps -1 to 640x480 (S1 on .240) while the menu says "Custom". So each
# pk3 waits until the share's own launchers (read through /mnt, never taken
# from a report) carry the -1 idiom - and SoF2's until the share's sof2mp.exe
# IS the patched one. The exe itself has no gate: with the old launchers it
# still receives an index (7/6), which the widened minimum does not touch.
_MINUS1 = ["+set r_mode -1 ", "+set r_customwidth %FR_W%", "+set r_customheight %FR_H%",
           'echo seta r_mode "-1"']
_INDEX_FORMS = ["r_mode %FR_Q3MODE%", 'r_mode "%FR_Q3MODE%"',
                "r_mode %FR_Q2MODE%", 'r_mode "%FR_Q2MODE%"']
PUBLISH_GATES = {
    "JediAcademy/base/zz_fleet_video.pk3": {
        "launchers": {"Play Jedi Academy.bat": _MINUS1,
                      "Play Jedi Academy - Multiplayer.bat": _MINUS1}},
    "SoldierOfFortune2/base/zz_fleet_video.pk3": {
        "launchers": {"Play Soldier of Fortune II - Multiplayer.bat": _MINUS1,
                      "Play Soldier of Fortune II.bat": _MINUS1},
        "exe_md5": (SOF2MP, SOF2MP_PATCHED_MD5)},
    "ReturnToCastleWolfenstein/Main/zz_fleet_video.pk3": {
        "launchers": {n: _MINUS1 + ["+set com_recommendedSet 1"]
                      for n in ("Play Return to Castle Wolfenstein.bat",
                                "Play RTCW Multiplayer.bat",
                                "Host RTCW - LAN.bat", "Join RTCW - LAN.bat")}},
}


def publish_gate(rel, landed=None):
    """[] when the output at Games-Library/<rel> may be published now, else
    every reason it may not. Pure reads of the share; `landed` (dry runs
    only) maps a path this same run would already have written to its md5."""
    g = PUBLISH_GATES.get(rel)
    if not g:
        return []
    why = []
    title = rel.split("/")[0]
    for name, need in sorted(g.get("launchers", {}).items()):
        p = share_path("%s/%s" % (title, name))
        try:
            body = open(p, encoding="latin-1").read()
        except OSError:
            why.append("%s/%s: missing on the share" % (title, name))
            continue
        why += ["%s/%s: lacks %r" % (title, name, t) for t in need if t not in body]
        why += ["%s/%s: still passes a mode index %r" % (title, name, t)
                for t in _INDEX_FORMS if t in body]
    if g.get("exe_md5"):
        rel_exe, want = g["exe_md5"]
        p = share_path(rel_exe)
        if landed and landed.get(rel_exe) == want:
            got = want
        else:
            got = md5_file(p) if os.path.isfile(p) else None
        if got != want:
            why.append("%s on the share is %s, not the patched %s" % (rel_exe, got, want))
    return why


def publish(outdir, titles=None, dry_run=False, force=False):
    require_share()
    man = json.load(open(os.path.join(outdir, "manifest.json")))
    # replaced binaries first: SoF2's pk3 is gated on the patched exe
    outs = sorted(man["outputs"], key=lambda o: 1 if o["new_file"] else 0)
    refused, landed = [], {}
    for o in outs:
        if titles and o["title"] not in titles:
            continue
        if md5_file(o["local_path"]) != o["md5"]:
            raise SystemExit("%s: local output changed since --build" % o["local_path"])
        rel = o["share_path"][len(LIB_REL) + 1:]
        cur = os.path.join(SHARE_ROOT, o["share_path"])
        if os.path.isfile(cur) and md5_file(cur) == o["md5"]:
            print("skip  %s (already the patched md5)" % o["share_path"])
            landed[rel] = o["md5"]
            continue
        why = publish_gate(rel, landed if dry_run else None)
        if why:
            print("REFUSED %s - its deploy precondition does not hold on the share:\n    %s"
                  % (o["share_path"], "\n    ".join(why)))
            if not force:
                refused.append(o["share_path"])
                continue
            print("  --force: publishing anyway")
        if o["new_file"] and os.path.isfile(cur):
            print("note  %s replaces an earlier build (md5 %s) - not backed up; it is "
                  "regenerated by the apply.py that built it" % (o["share_path"], md5_file(cur)))
        if not o["new_file"]:
            if not os.path.isfile(cur):
                raise SystemExit("%s: the original is missing on the share" % o["share_path"])
            got = md5_file(cur)
            if got != o["original_md5"]:
                raise SystemExit("%s: share md5 %s is neither the original %s nor the "
                                 "patched %s - stop" % (o["share_path"], got,
                                                         o["original_md5"], o["md5"]))
            bk = o["backup_share_path"]
            bkp = os.path.join(SHARE_ROOT, bk)
            if os.path.isfile(bkp) and md5_file(bkp) == o["original_md5"]:
                print("have  backup %s" % bk)
            else:
                print("backup %s -> %s" % (o["share_path"], bk))
                if not dry_run and not sharewrite_put(cur, bk):
                    raise SystemExit("backup FAILED: %s - stopping" % bk)
        print("put   %s" % o["share_path"])
        if not dry_run and not sharewrite_put(o["local_path"], o["share_path"]):
            raise SystemExit("put FAILED: %s - stopping" % o["share_path"])
        landed[rel] = o["md5"]
    if refused:
        raise SystemExit("NOT PUBLISHED (deploy order): %s - land the launcher/gameres "
                         "changes in manifest.json launcher_changes first, or --force"
                         % ", ".join(refused))


def install_server(outdir, titles=None, force=False):
    man = json.load(open(os.path.join(outdir, "manifest.json")))
    for title, plan in PLAN.items():
        if titles and title not in titles:
            continue
        unit, sdir, _ = plan["server"]
        pure = server_purity(plan["server"])
        if pure == "0" and not force:
            print("%s: %s runs sv_pure 0 - the client pk3 is not needed there; "
                  "nothing copied (--force to copy anyway)" % (title, unit))
            continue
        out = [o for o in man["outputs"] if o["title"] == title
               and o["share_path"].lower().endswith(".pk3")]
        for o in out:
            dst = os.path.join(os.path.expanduser(sdir), PK3_NAME)
            shutil.copyfile(o["local_path"], dst)
            if md5_file(dst) != o["md5"]:
                raise SystemExit("%s: copy did not verify" % dst)
            print("%s: installed %s (md5 %s); restart with: systemctl --user "
                  "restart %s" % (title, dst, o["md5"], unit))


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--check", action="store_true")
    g.add_argument("--build", nargs="?", const=DEFAULT_OUT, metavar="OUTDIR")
    g.add_argument("--publish", nargs="?", const=DEFAULT_OUT, metavar="OUTDIR")
    g.add_argument("--install-server", nargs="?", const=DEFAULT_OUT, metavar="OUTDIR")
    ap.add_argument("--full", action="store_true", help="--check: whole-file md5s")
    ap.add_argument("--title", action="append", choices=sorted(PLAN))
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--force", action="store_true")
    a = ap.parse_args(argv)
    if a.check:
        rep = check(full=a.full, titles=a.title)
        print("\n".join(rep.lines))
        print("%s: %d failure(s)" % (KEY, rep.failed))
        return 1 if rep.failed else 0
    if a.build:
        man, rep = build(a.build, titles=a.title)
        print("\n".join(rep.lines))
        for o in man["outputs"]:
            rel = o["share_path"][len(LIB_REL) + 1:]
            same = REVIEWED_BUILD.get(rel) == o["md5"]
            print("built %-60s %s %d  %s" % (o["share_path"], o["md5"], o["size"],
                                            "= reviewed build" if same else
                                            "DIFFERS FROM THE REVIEWED BUILD"))
        print("manifest: %s" % os.path.join(a.build, "manifest.json"))
        return 0
    if a.publish:
        publish(a.publish, titles=a.title, dry_run=a.dry_run, force=a.force)
        return 0
    if a.install_server:
        install_server(a.install_server, titles=a.title, force=a.force)
        return 0


if __name__ == "__main__":
    sys.exit(main())
