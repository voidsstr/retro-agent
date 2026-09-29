#!/usr/bin/env python3
"""id Tech 2 titles: make 1920x1080 selectable IN GAME (vid_modes[9] 1600x1200 -> 1920x1080).

WHAT THIS DOES, and why it is a byte patch at all.
Quake II 3.20, SiN 1.11 and Soldier of Fortune share id Tech 2's FIXED video
mode table: ten {desc, width, height, mode} entries compiled into the exe,
'Mode 0: 320x240' .. 'Mode 9: 1600x1200', no custom mode, no 16:9 entry. The
in-game video menu is an index into that table and gl_mode IS that index, so
no config value can reach 1920x1080. The only in-tree route is to rewrite the
LAST entry (1600x1200, which no fleet box resolves to today) as 1920x1080 and
relabel it wherever a menu shows it:

  Quake2Complete  quake2.exe  vid_modes[9] w/h + 'Mode 9: ...' + the Video
                              menu's own label array entry '[1600 1200]'
  SiNGold         sin.exe     vid_modes[9] w/h + 'Mode 9: ...'
                  base\\menus\\main.mnu  NEW LOOSE FILE = pak5.sin's copy,
                              ' 1600 x 1200' -> ' 1920 x 1080'
                  2015\\menus\\main.mnu  NEW LOOSE FILE = 2015\\pak0.sin's copy,
                              same relabel and numitems 9 -> 10 (Wages of SiN
                              listed ten strings but declared nine, so its menu
                              stopped at 1280 x 960)
                  ctf\\menus\\main.mnu   NEW LOOSE FILE = ctf\\pak1.sin's copy,
                              same relabel (a client joining a ds_sinctf.bat
                              server is switched into ctf\\ and would otherwise
                              read pak1's ' 1600 x 1200' for mode 9)
                  (SiN's FS_AddGameDirectory @0x441ef0 prepends the LOOSE
                   directory after the paks, so a loose file wins.)
  SoldierOfFortune SoF.exe    vid_modes[9] w/h + 'Mode 9: ...'
                  base\\pak2.pak  NEW PAK holding only menus/m_video.rmf =
                              pak0.pak's copy with '1600x1200' -> '1920x1080'
                  (SoF prepends pak0-9 AFTER the loose directory, so a loose
                   file would be shadowed by pak0.pak: it must be a pak.)

Quake2Win9x ships the identical quake2.exe and is DELIBERATELY LEFT STOCK: it
serves the Win9x/Voodoo lane at 640x480, and the OS gate keeps it off every
1080p LCD.

Nothing here selects mode 9. Publishing these files is inert until the
launchers and GAMERES hand these three titles FR_Q2WIDE / %Q2WIDE% (see
README.md) - which is what makes the exe half safe to ship first.

Modes (exactly one):
  --check            read the staged originals, assert size/md5/structure and
                     the exact original bytes at every offset; report
  --build [OUTDIR]   write the patched copies + manifest.json
                     (default ~/.retro-fleet/patch-out/idtech2)
  --publish [OUTDIR] FUTURE USE - back up each original to
                     Files/Games-Library/_patches/<Title>/originals-2026-09-29/
                     and put each output with scripts/fleet/sharewrite.py,
                     one file at a time, stopping on the first failure;
                     a file whose share copy already has the patched md5 is
                     skipped (idempotent), and a destination an earlier run
                     left MISSING or TRUNCATED is put again (the exe's
                     original then comes from its verified backup). Refused
                     unless --library is the real share mount.
  --install-server   no server component: prints why and exits 0

Game binaries and data are copyrighted: NOTHING this script writes may be
committed. Outputs live outside git.
"""
import argparse
import hashlib
import io
import json
import os
import struct
import subprocess
import sys

KEY = "idtech2"
SHARE_PREFIX = "Files/Games-Library"
# the read-only mount sharewrite.py verifies every write through
SHARE_LIBRARY = "/mnt/retro-share/" + SHARE_PREFIX
LIBRARY = os.environ.get("RETRO_LIBRARY", SHARE_LIBRARY)
DEFAULT_OUT = os.path.expanduser("~/.retro-fleet/patch-out/%s" % KEY)
BACKUP_TAG = "originals-2026-09-29"
HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.normpath(os.path.join(HERE, "..", "..", ".."))
SHAREWRITE = os.path.join(REPO, "scripts", "fleet", "sharewrite.py")


class PatchError(Exception):
    pass


# ---------------------------------------------------------------------------
# pure logic
# ---------------------------------------------------------------------------

def md5(b):
    return hashlib.md5(b).hexdigest()


def u32(v):
    return struct.pack("<I", v)


def cstr(s):
    return s.encode("ascii") + b"\0"


def apply_edits(data, edits):
    """Return data with every (offset, old, new, what) applied.

    Each edit must be same-length and the ORIGINAL bytes must be exactly
    `old` - a patch applied to a file it was not measured on is refused, never
    guessed at."""
    buf = bytearray(data)
    for off, old, new, what in edits:
        if len(old) != len(new):
            raise PatchError("%s: length change %d -> %d" % (what, len(old), len(new)))
        got = bytes(buf[off:off + len(old)])
        if got != old:
            raise PatchError("%s @0x%x: expected %s, found %s"
                             % (what, off, old.hex(), got.hex()))
        buf[off:off + len(new)] = new
    return bytes(buf)


def edits_applied(data, edits):
    """True when every edit's NEW bytes are in place (idempotency check)."""
    return all(data[off:off + len(new)] == new for off, _o, new, _w in edits)


def vid_mode_edits(table_off, str_off, old=(1600, 1200), new=(1920, 1080)):
    """The three edits to vid_modes[9] of an id Tech 2 exe.

    Entry layout {char *desc; int width; int height; int mode} = 16 bytes, so
    entry 9 sits at table_off + 0x90: width at +4, height at +8."""
    e9 = table_off + 9 * 16
    return [
        (e9 + 4, u32(old[0]), u32(new[0]), "vid_modes[9].width"),
        (e9 + 8, u32(old[1]), u32(new[1]), "vid_modes[9].height"),
        (str_off, cstr("Mode 9: %dx%d" % old), cstr("Mode 9: %dx%d" % new),
         "vid_modes[9].desc"),
    ]


def read_vid_table(data, table_off, va_to_off, n=10):
    """Decode n {desc,w,h,mode} entries; desc resolved to (file offset, text)."""
    out = []
    for i in range(n):
        va, w, h, mode = struct.unpack_from("<IiiI", data, table_off + 16 * i)
        off = va_to_off(va)
        text = None
        if off is not None and 0 <= off < len(data):
            text = data[off:data.index(b"\0", off)].decode("latin1")
        out.append({"i": i, "desc_off": off, "desc": text, "w": w, "h": h, "mode": mode})
    return out


def read_ptr_array(data, off, va_to_off, n):
    """n pointers at off, each resolved to (file offset, string) or None for NULL."""
    out = []
    for i in range(n):
        (va,) = struct.unpack_from("<I", data, off + 4 * i)
        if va == 0:
            out.append(None)
            continue
        fo = va_to_off(va)
        out.append((fo, data[fo:data.index(b"\0", fo)].decode("latin1")))
    return out


# --- SiN menu (text) -------------------------------------------------------

SIN_OLD_ITEM = b'" 1600 x 1200"'
SIN_NEW_ITEM = b'" 1920 x 1080"'


def sin_mnu_relabel(text, numitems_before):
    """Relabel the con_vidmode list's 10th entry and make numitems 10.

    Operates on bytes, touches ONLY the con_vidmode listitem block, preserves
    CRLF and any trailing blanks (2015's lines carry one). Refuses when the
    block is not exactly as measured."""
    start = text.find(b"listitem con_vidmode ")
    if start < 0:
        raise PatchError("main.mnu: no 'listitem con_vidmode' block")
    if text.find(b"listitem con_vidmode ", start + 1) >= 0:
        raise PatchError("main.mnu: more than one con_vidmode block")
    end = text.find(b"enditem", start)
    if end < 0:
        raise PatchError("main.mnu: con_vidmode block has no enditem")
    block = text[start:end]
    want_n = b"numitems %d" % numitems_before
    if block.count(want_n + b"\r\n") != 1:
        raise PatchError("main.mnu: expected '%s' in the con_vidmode block" % want_n.decode())
    if block.count(SIN_OLD_ITEM) != 1:
        raise PatchError("main.mnu: expected exactly one %s in the block" % SIN_OLD_ITEM.decode())
    # the list must be the ten id Tech 2 modes in table order
    items = [l.strip() for l in block.split(b"\r\n")
             if l.strip().startswith(b'"') and l.strip().endswith(b'"')]
    expect = [b'" %d x %d"' % wh for wh in ((320, 240), (400, 300), (512, 384), (640, 480),
                                           (800, 600), (960, 720), (1024, 768), (1152, 864),
                                           (1280, 960), (1600, 1200))]
    if items != expect:
        raise PatchError("main.mnu: con_vidmode items are not the id Tech 2 table: %r" % items)
    block = block.replace(want_n + b"\r\n", b"numitems 10\r\n", 1)
    block = block.replace(SIN_OLD_ITEM, SIN_NEW_ITEM, 1)
    return text[:start] + block + text[end:]


# --- SoF menu (RMF text) ----------------------------------------------------

SOF_OLD_LIST = (b'<list "640x480,800x600,960x720,1024x768,1152x864,1280x960,1600x1200" '
                b'match "3,4,5,6,7,8,9"')
SOF_NEW_LIST = (b'<list "640x480,800x600,960x720,1024x768,1152x864,1280x960,1920x1080" '
                b'match "3,4,5,6,7,8,9"')


def sof_rmf_relabel(text):
    if text.count(SOF_OLD_LIST) != 1:
        raise PatchError("m_video.rmf: resolution list not found exactly once")
    return text.replace(SOF_OLD_LIST, SOF_NEW_LIST, 1)


# --- paks ---------------------------------------------------------------------

PAK_FORMATS = {b"PACK": 64, b"SPAK": 128}     # Quake II / SoF, SiN


def pak_dir(data_or_file, size=None):
    """{lower-case name: (name, filepos, filelen)} of a PACK/SPAK archive.

    Takes bytes or a binary file object (so a 700 MB pak is not read whole)."""
    f = io.BytesIO(data_or_file) if isinstance(data_or_file, (bytes, bytearray)) else data_or_file
    f.seek(0)
    hdr = f.read(12)
    magic = hdr[:4]
    if magic not in PAK_FORMATS:
        raise PatchError("not a pak: magic %r" % magic)
    esz = PAK_FORMATS[magic]
    dofs, dlen = struct.unpack("<ii", hdr[4:12])
    if dlen % esz:
        raise PatchError("pak directory length %d not a multiple of %d" % (dlen, esz))
    f.seek(dofs)
    d = f.read(dlen)
    out = {}
    for i in range(dlen // esz):
        e = d[i * esz:(i + 1) * esz]
        name = e[:esz - 8].split(b"\0")[0].decode("latin1")
        pos, ln = struct.unpack("<ii", e[esz - 8:])
        out[name.lower()] = (name, pos, ln)
    return magic, out


def pak_read(f, name):
    _m, d = pak_dir(f)
    if name.lower() not in d:
        raise PatchError("pak has no %s" % name)
    _n, pos, ln = d[name.lower()]
    f.seek(pos)
    b = f.read(ln)
    if len(b) != ln:
        raise PatchError("short read of %s" % name)
    return b


def build_pak(files):
    """A Quake II PACK: 12-byte header, file data, then 64-byte entries."""
    body = bytearray()
    entries = []
    for name, data in files:
        nb = name.encode("ascii")
        if len(nb) >= 56:
            raise PatchError("pak name too long: %s" % name)
        entries.append((nb, 12 + len(body), len(data)))
        body += data
    dofs = 12 + len(body)
    d = b"".join(nb.ljust(56, b"\0") + struct.pack("<ii", pos, ln) for nb, pos, ln in entries)
    return b"PACK" + struct.pack("<ii", dofs, len(d)) + bytes(body) + d


# --- the launcher/GAMERES value these patched exes need ---------------------

PATCHED_MODE9 = (1920, 1080)

# id Tech 2's STOCK table - fleetres.c q2tab[] / gameres.h gr_q2tab[].
Q2TAB = [(320, 240), (400, 300), (512, 384), (640, 480), (800, 600),
         (960, 720), (1024, 768), (1152, 864), (1280, 960), (1600, 1200)]


def aspect_class_mode(r):
    """fleetres.c aspect_class_mode() / gameres.h gr_aspect_mode(), verbatim bands."""
    if 1.320 < r < 1.348:
        return 43
    if 1.240 < r < 1.260:
        return 54
    if 1.580 < r < 1.620:
        return 1610
    if 1.760 < r < 1.790:
        return 169
    return 0


def q2_mode_for(w, h, offered):
    """fleetres.c q2_mode_for() / gameres.h gr_q2_mode_for(), same logic: the
    largest table entry that fits in w x h AND is offered; 640x480 floor; when
    nothing above the floor is offered, the largest that merely fits if that
    one is not offered either. FR_Q2MODE = q2_mode_for(FR_W43, FR_H43)."""
    best = best_fit = 3
    for i, (mw, mh) in enumerate(Q2TAB):
        if mw > w or mh > h:
            continue
        best_fit = i
        if offered(mw, mh):
            best = i
    if best > 3:
        return best
    return best if offered(*Q2TAB[best_fit]) else best_fit


def fr_q2wide(tgt_w, tgt_h, t43_w, t43_h, offered):
    """REFERENCE for FR_Q2WIDE (fleetres.c) == t->q2wide / %Q2WIDE% (gameres.h).

    The gl_mode for the PATCHED id Tech 2 exes, whose mode 9 is 1920x1080:
      9   when the box's target (after ResCap) is 16:9, at least 1920x1080,
          and the driver offers 1920x1080;
      q2_mode_for(min(t43_w, 1280), min(t43_h, 960))   otherwise - the SAME
          selector FR_Q2MODE uses, with the table capped at entry 8. That is
          FR_Q2MODE exactly whenever FR_Q2MODE <= 8 (every box today), and it
          can never be 9, because on a patched exe 9 no longer means 1600x1200.
          (A bare min(FR_Q2MODE, 8) would also never be 9, but on a 1600x1200
          tube whose driver does not list 1280x960 it would ask for a mode the
          driver refuses, and id Tech 2 then drops to a WINDOW.)
    `offered(w, h)` is the writer's own mode_offered()/gr_mode_offered()."""
    if tgt_h > 0 and aspect_class_mode(float(tgt_w) / tgt_h) == 169 \
            and tgt_w >= PATCHED_MODE9[0] and tgt_h >= PATCHED_MODE9[1] \
            and offered(*PATCHED_MODE9):
        return 9
    return q2_mode_for(min(t43_w, Q2TAB[8][0]), min(t43_h, Q2TAB[8][1]), offered)


def q2wide_res(q2wide):
    """The resolution a PATCHED exe sets for gl_mode `q2wide` - what any
    per-mode refresh for these three titles has to be computed at."""
    return PATCHED_MODE9 if q2wide == 9 else Q2TAB[q2wide]


# ---------------------------------------------------------------------------
# the measured specification
# ---------------------------------------------------------------------------

EXES = [
    {
        "title": "Quake2Complete", "rel": "quake2.exe",
        "size": 362496, "md5": "57dd2cf4ba176f3e6ae72fccc94c38b4",
        "table": 0x543d0, "desc9": 0x54470,
        # the Video menu's spinner labels: 10 pointers then NULL
        "labels": 0x54678, "label9": 0x54720,
        "extra": [(0x54720, cstr("[1600 1200]"), cstr("[1920 1080]"), "menu label[9]")],
        "desc0": "Mode 0: 320x240",
    },
    {
        "title": "SiNGold", "rel": "sin.exe",
        "size": 667648, "md5": "8d320d7fc3ea3a226ec98dafef3e81ba",
        "table": 0x94450, "desc9": 0x944f0,
        "labels": None, "label9": None, "extra": [],
        "desc0": "Mode 0: 320x240",
    },
    {
        "title": "SoldierOfFortune", "rel": "SoF.exe",
        "size": 2052096, "md5": "2f4db3771d51d8b585bbea695041e40c",
        "table": 0x1c6e08, "desc9": 0x1c6ea8,
        "labels": None, "label9": None, "extra": [],
        "desc0": "Mode 0: Unsupported",
    },
]

# Same-size files the exe patch must NOT touch: Quake2Win9x keeps the stock exe.
KEEP_STOCK = [("Quake2Win9x", "quake2.exe", "57dd2cf4ba176f3e6ae72fccc94c38b4")]

# "pak" must be the copy the engine LOADS today: both FS_AddGameDirectory
# variants prepend every pak as it is opened, so among a game dir's paks the
# HIGHEST-numbered one carrying the entry wins (base\ has main.mnu in pak2,
# pak4 AND pak5; ctf\ in pak0 and pak1). load_originals() re-derives the winner
# from the share on every run and refuses when it is not this pak.
# "paks" = the engine's own loop: SiN pak0..31.sin (sin.exe 0x441f72
# cmp edi,0x20), SoF pak0..9.pak (SoF.exe 0x200282a7 cmp edi,0xa).
DERIVED = [
    {
        "title": "SiNGold", "rel": "base/menus/main.mnu", "kind": "sin-mnu",
        "pak": "base/pak5.sin", "pak_size": 1239753, "entry": "menus/main.mnu",
        "entry_size": 90780, "entry_md5": "d038864bdf6921ad4181ab96ac1300a4",
        "numitems_before": 10, "paks": (32, "sin"),
        # SiN searches the LOOSE directory before that directory's paks, so
        # this one loose file beats pak2/pak4/pak5 alike.
    },
    {
        "title": "SiNGold", "rel": "2015/menus/main.mnu", "kind": "sin-mnu",
        "pak": "2015/pak0.sin", "pak_size": 269730473, "entry": "menus/main.mnu",
        "entry_size": 112319, "entry_md5": "3de83c90bf6e20f4ea250ef143648739",
        "numitems_before": 9, "paks": (32, "sin"),
    },
    {
        # ctf\ has no desktop shortcut, but it IS reached: a client that joins
        # a ds_sinctf.bat server is switched to the server's game dir, and the
        # patched sin.exe's mode 9 is 1920x1080 there too. ctf\ is added above
        # base\, so without this file its pak1 copy (' 1600 x 1200') wins over
        # base\menus\main.mnu and mislabels mode 9. (ctf\locale\en\menus\ is
        # NOT read: no SiN 1.11 binary or pak contains the string "locale".)
        "title": "SiNGold", "rel": "ctf/menus/main.mnu", "kind": "sin-mnu",
        "pak": "ctf/pak1.sin", "pak_size": 8752518, "entry": "menus/main.mnu",
        "entry_size": 91704, "entry_md5": "e55c5dd6b26f90bc7590446e98d70ac8",
        "numitems_before": 10, "paks": (32, "sin"),
    },
    {
        "title": "SoldierOfFortune", "rel": "base/pak2.pak", "kind": "sof-pak",
        "pak": "base/pak0.pak", "pak_size": 705825871, "entry": "menus/m_video.rmf",
        "entry_size": 3080, "entry_md5": "bc6bab97e0a55ca6b42316d210093ee7",
        "paks": (10, "pak"),
    },
]


def exe_edits(spec):
    return vid_mode_edits(spec["table"], spec["desc9"]) + spec["extra"]


def derive(spec, entry_bytes):
    if spec["kind"] == "sin-mnu":
        return sin_mnu_relabel(entry_bytes, spec["numitems_before"])
    if spec["kind"] == "sof-pak":
        return build_pak([(spec["entry"], sof_rmf_relabel(entry_bytes))])
    raise PatchError("unknown kind %s" % spec["kind"])


# ---------------------------------------------------------------------------
# share access (read-only)
# ---------------------------------------------------------------------------

def ci_path(root, rel):
    """Resolve rel under root CASE-INSENSITIVELY; None when a component is absent."""
    cur = root
    for part in rel.replace("\\", "/").split("/"):
        try:
            names = os.listdir(cur)
        except OSError:
            return None
        hit = [n for n in names if n.lower() == part.lower()]
        if not hit:
            return None
        cur = os.path.join(cur, hit[0])
    return cur


def pe_mapper(data):
    import pefile
    pe = pefile.PE(data=data, fast_load=True)
    base = pe.OPTIONAL_HEADER.ImageBase

    def va_to_off(va):
        try:
            return pe.get_offset_from_rva(va - base)
        except Exception:
            return None
    return va_to_off


def check_exe_structure(spec, data, patched=False):
    """Assert the table/labels decode as measured. Returns notes."""
    v2o = pe_mapper(data)
    tab = read_vid_table(data, spec["table"], v2o)
    notes = []
    for e in tab:
        if e["mode"] != e["i"]:
            raise PatchError("%s: vid_modes[%d].mode = %d" % (spec["title"], e["i"], e["mode"]))
    if tab[0]["desc"] != spec["desc0"]:
        raise PatchError("%s: vid_modes[0].desc = %r" % (spec["title"], tab[0]["desc"]))
    w9, h9 = (1920, 1080) if patched else (1600, 1200)
    e9 = tab[9]
    if (e9["w"], e9["h"]) != (w9, h9) or e9["desc_off"] != spec["desc9"] \
            or e9["desc"] != "Mode 9: %dx%d" % (w9, h9):
        raise PatchError("%s: vid_modes[9] = %r" % (spec["title"], e9))
    if (tab[8]["w"], tab[8]["h"]) != (1280, 960):
        raise PatchError("%s: vid_modes[8] is not 1280x960" % spec["title"])
    notes.append("vid_modes[9] = %dx%d '%s' (desc @0x%x)" % (w9, h9, e9["desc"], e9["desc_off"]))
    if spec["labels"] is not None:
        arr = read_ptr_array(data, spec["labels"], v2o, 11)
        want9 = "[1920 1080]" if patched else "[1600 1200]"
        if arr[10] is not None or arr[9] is None or arr[9][0] != spec["label9"] \
                or arr[9][1] != want9 or arr[0][1] != "[320 240  ]":
            raise PatchError("%s: menu label array = %r" % (spec["title"], arr))
        notes.append("menu labels[9] = '%s' (@0x%x), labels[10] = NULL" % (arr[9][1], arr[9][0]))
    return notes


def backup_rel_of(title, rel):
    """Library-relative path of an original's backup (see --publish)."""
    return "_patches/%s/%s/%s" % (title, BACKUP_TAG, rel)


def classify_exe(spec, live):
    """('stock', original) | ('patched', original rebuilt by reversing OUR
    edits) | (None, None) when `live` is neither."""
    if live is None or len(live) != spec["size"]:
        return None, None
    if md5(live) == spec["md5"]:
        return "stock", live
    edits = exe_edits(spec)
    try:
        orig = apply_edits(live, [(o, n, ol, w) for o, ol, n, w in edits])
    except PatchError:
        return None, None
    if md5(orig) == spec["md5"]:
        return "patched", orig
    return None, None


def is_partial_copy(cur, *wholes):
    """True when `cur` is a strict prefix (possibly empty) of one of the files
    we know - what a put that died half way leaves behind (a failed write to
    this NAS leaves the destination missing or truncated)."""
    return any(len(cur) < len(w) and w.startswith(cur) for w in wholes)


def pak_winner(gamedir, entry, npaks, ext, skip=()):
    """The pak<i>.<ext> (i < npaks) the engine loads `entry` from: the
    highest-numbered one carrying it, because every pak is PREPENDED to the
    search path as it is opened. None when no pak carries it."""
    try:
        names = {n.lower(): n for n in os.listdir(gamedir)}   # ONE listing, case-insensitive
    except (OSError, TypeError):
        return None
    win = None
    for i in range(npaks):
        n = names.get("pak%d.%s" % (i, ext))
        if i in skip or n is None:
            continue
        with open(os.path.join(gamedir, n), "rb") as f:
            if entry.lower() in pak_dir(f)[1]:
                win = n
    return win


def load_originals(lib):
    """Read and verify every staged original. Returns a list of work items.

    A work item marked "damaged" is the ONE share state this recovers from on
    its own: a live copy that is MISSING or a TRUNCATED prefix of a file we
    know, i.e. an earlier --publish whose put died half way. It is recovered
    only from the verified backup that --publish wrote first. Anything else
    that is not exactly what was measured is refused, never overwritten."""
    work = []
    for spec in EXES:
        edits = exe_edits(spec)
        p = ci_path(lib, "%s/%s" % (spec["title"], spec["rel"]))
        live = open(p, "rb").read() if p else None
        share_md5 = md5(live) if live is not None else None
        damaged = None
        kind, data = classify_exe(spec, live)
        if kind == "stock":
            state = "stock (original)"
        elif kind == "patched":
            # the original is rebuilt by reversing OUR edits and must hash to
            # the pinned md5 (classify_exe); the backup --publish wrote first is
            # what a damaged share would be recovered from, so it is verified
            # too - a backup that is not the original is refused, not trusted
            b = ci_path(lib, backup_rel_of(spec["title"], spec["rel"]))
            if b is None:
                state = ("ALREADY PATCHED on the share; NO backup at %s"
                         % backup_rel_of(spec["title"], spec["rel"]))
            else:
                bdata = open(b, "rb").read()
                if bdata != data:
                    raise PatchError("%s/%s: ALREADY PATCHED on the share, but the backup %s is "
                                     "md5 %s (%d B), not the original %s - re-measure"
                                     % (spec["title"], spec["rel"],
                                        backup_rel_of(spec["title"], spec["rel"]),
                                        md5(bdata), len(bdata), spec["md5"]))
                state = "ALREADY PATCHED on the share; backup verified"
        else:
            b = ci_path(lib, backup_rel_of(spec["title"], spec["rel"]))
            bdata = open(b, "rb").read() if b else None
            if bdata is None or len(bdata) != spec["size"] or md5(bdata) != spec["md5"]:
                raise PatchError("%s/%s: %s, and there is no verified backup at %s"
                                 % (spec["title"], spec["rel"],
                                    "not found (case-insensitive)" if live is None else
                                    "md5 %s (%d B) is neither the measured original nor our patch"
                                    % (share_md5, len(live)),
                                    backup_rel_of(spec["title"], spec["rel"])))
            if live is not None and not is_partial_copy(live, bdata, apply_edits(bdata, edits)):
                raise PatchError("%s/%s: md5 %s (%d B) is neither the measured original, our "
                                 "patch, nor a truncated copy of either - somebody changed it; "
                                 "re-measure, never overwrite" % (spec["title"], spec["rel"],
                                                                   share_md5, len(live)))
            damaged = ("MISSING from the share" if live is None else
                       "TRUNCATED on the share (%d of %d B)" % (len(live), spec["size"]))
            data = bdata
            state = "DAMAGED - %s; original taken from the verified backup" % damaged
        notes = check_exe_structure(spec, data)
        patched = apply_edits(data, edits)
        notes += check_exe_structure(spec, patched, patched=True)
        # nothing outside the edited bytes may differ
        diff = [i for i in range(len(data)) if data[i] != patched[i]]
        allowed = set()
        for off, old, _n, _w in edits:
            allowed.update(range(off, off + len(old)))
        if not set(diff) <= allowed:
            raise PatchError("%s: bytes changed outside the edits" % spec["title"])
        work.append({
            "title": spec["title"], "rel": spec["rel"], "kind": "exe",
            # where --publish copies the ORIGINAL from when it makes the backup
            "orig_path": p if kind == "stock" else None,
            "orig_md5": spec["md5"], "orig_size": spec["size"],
            "share_md5": share_md5, "state": state, "data": patched, "damaged": damaged,
            "edits": [{"offset": "0x%x" % o, "what": w, "old": ol.hex(), "new": n.hex()}
                      for o, ol, n, w in edits],
            "bytes_changed": len(diff), "notes": notes,
        })
    for title, rel, want in KEEP_STOCK:
        p = ci_path(lib, "%s/%s" % (title, rel))
        if p is not None and md5(open(p, "rb").read()) != want:
            raise PatchError("%s/%s is no longer the stock exe - it must stay stock" % (title, rel))
    for spec in DERIVED:
        pp = ci_path(lib, "%s/%s" % (spec["title"], spec["pak"]))
        if pp is None:
            raise PatchError("%s/%s: not found (case-insensitive)" % (spec["title"], spec["pak"]))
        if os.path.getsize(pp) != spec["pak_size"]:
            raise PatchError("%s: size %d, measured %d" % (pp, os.path.getsize(pp), spec["pak_size"]))
        with open(pp, "rb") as f:
            entry = pak_read(f, spec["entry"])
        if len(entry) != spec["entry_size"] or md5(entry) != spec["entry_md5"]:
            raise PatchError("%s:%s md5 %s size %d, measured %s %d" % (
                pp, spec["entry"], md5(entry), len(entry), spec["entry_md5"], spec["entry_size"]))
        out = derive(spec, entry)
        notes = []
        gamedir = spec["rel"].split("/")[0]
        gd = ci_path(lib, "%s/%s" % (spec["title"], gamedir))
        # The source must be the copy the engine LOADS today, or the relabel
        # would silently revert whatever a newer pak changed. For SoF our own
        # pak2 is left out of the question (it is the override, not a source).
        npaks, ext = spec["paks"]
        ours = int(os.path.basename(spec["rel"])[3:-4]) if spec["kind"] == "sof-pak" else None
        win = pak_winner(gd, spec["entry"], npaks, ext, skip=(ours,) if ours is not None else ())
        if win is None or win.lower() != os.path.basename(spec["pak"]).lower():
            raise PatchError("%s/%s: the engine loads %s from %s, not %s - re-measure"
                             % (spec["title"], gamedir, spec["entry"], win, spec["pak"]))
        notes.append("source %s is the pak the engine loads %s from (highest of pak0-%d.%s)"
                     % (spec["pak"], spec["entry"], npaks - 1, ext))
        if spec["kind"] == "sof-pak":
            _m, d = pak_dir(out)
            with io.BytesIO(out) as f:
                inner = pak_read(f, spec["entry"])
            if list(d) != [spec["entry"]] or SOF_NEW_LIST not in inner:
                raise PatchError("built pak2 does not hold the relabelled menu")
            notes.append("pak2.pak = PACK with 1 entry %s (%d B, md5 %s)"
                         % (spec["entry"], len(inner), md5(inner)))
        else:
            notes.append("loose %s: numitems %d -> 10, ' 1600 x 1200' -> ' 1920 x 1080'"
                         % (spec["rel"], spec["numitems_before"]))
            if len(out) - len(entry) != (1 if spec["numitems_before"] == 9 else 0):
                raise PatchError("unexpected size change in %s" % spec["rel"])
        dest = ci_path(lib, "%s/%s" % (spec["title"], spec["rel"]))
        damaged = None
        if dest is None:
            state, share_md5 = "absent (new file)", None
        else:
            cur = open(dest, "rb").read()
            share_md5 = md5(cur)
            if cur == out:
                state = "ALREADY PUBLISHED on the share"
            elif is_partial_copy(cur, out):
                damaged = "TRUNCATED on the share (%d of %d B of our output)" % (len(cur), len(out))
                state = "DAMAGED - %s" % damaged
            else:
                raise PatchError("%s exists on the share and is not our output (md5 %s)"
                                 % (dest, share_md5))
        work.append({
            "title": spec["title"], "rel": spec["rel"], "kind": spec["kind"],
            "orig_path": None, "orig_md5": None, "orig_size": None,
            "source": {"pak": "%s/%s/%s" % (SHARE_PREFIX, spec["title"], spec["pak"]),
                       "pak_size": spec["pak_size"], "entry": spec["entry"],
                       "entry_md5": spec["entry_md5"], "entry_size": spec["entry_size"]},
            "share_md5": share_md5, "state": state, "data": out, "notes": notes,
            "damaged": damaged,
        })
    return work


def share_rel(item):
    return "%s/%s/%s" % (SHARE_PREFIX, item["title"], item["rel"])


def backup_rel(item):
    return "%s/_patches/%s/%s/%s" % (SHARE_PREFIX, item["title"], BACKUP_TAG, item["rel"])


# ---------------------------------------------------------------------------
# modes
# ---------------------------------------------------------------------------

def cmd_check(lib):
    work = load_originals(lib)
    for w in work:
        print("%-17s %-22s %-6s %s" % (w["title"], w["rel"], w["kind"], w["state"]))
        if w["orig_md5"]:
            print("    original md5 %s (%d B)" % (w["orig_md5"], w["orig_size"]))
        else:
            s = w["source"]
            print("    source %s:%s md5 %s (%d B)" % (s["pak"], s["entry"], s["entry_md5"], s["entry_size"]))
        for e in w.get("edits", []):
            print("    @%-9s %-20s %s -> %s" % (e["offset"], e["what"], e["old"], e["new"]))
        for n in w["notes"]:
            print("    %s" % n)
        print("    patched md5 %s (%d B)" % (md5(w["data"]), len(w["data"])))
    bad = [w for w in work if w.get("damaged")]
    if bad:
        # recoverable, but a broken share is never "CHECK OK"
        print("*" * 72)
        print("* SHARE DAMAGED - an earlier --publish died half way:")
        for w in bad:
            print("*   %s/%s: %s" % (w["title"], w["rel"], w["damaged"]))
        print("* The originals are verified from the backup; re-run --build then")
        print("* --publish to put the outputs back. Boxes may have synced the damage.")
        print("*" * 72)
        return 2
    print("CHECK OK: %d outputs, every original verified byte-for-byte" % len(work))
    return 0


def cmd_build(lib, outdir):
    work = load_originals(lib)
    manifest = {"key": KEY, "library": lib, "backup_tag": BACKUP_TAG, "outputs": []}
    for w in work:
        lp = os.path.join(outdir, w["title"], *w["rel"].split("/"))
        os.makedirs(os.path.dirname(lp), exist_ok=True)
        with open(lp, "wb") as f:
            f.write(w["data"])
        got = md5(open(lp, "rb").read())
        if got != md5(w["data"]):
            raise PatchError("%s did not write back correctly" % lp)
        manifest["outputs"].append({
            "title": w["title"], "share_path": share_rel(w), "local_path": lp,
            "md5": got, "size": len(w["data"]), "kind": w["kind"],
            "original_md5": w["orig_md5"], "original_size": w["orig_size"],
            "original_state": "absent - new file" if w["orig_md5"] is None else "replaced",
            "source": w.get("source"), "backup_share_path": backup_rel(w) if w["orig_md5"] else None,
            "edits": w.get("edits"), "notes": w["notes"],
        })
        print("wrote %s  md5 %s  %d B" % (lp, got, len(w["data"])))
        if w.get("damaged"):
            print("  WARNING: the share copy of %s/%s is %s - --publish will put it back"
                  % (w["title"], w["rel"], w["damaged"]))
    mp = os.path.join(outdir, "manifest.json")
    with open(mp, "w") as f:
        json.dump(manifest, f, indent=1)
    print("manifest %s" % mp)
    return 0


def sharewrite(local, dest, dry):
    cmd = [sys.executable, SHAREWRITE, "put", local, dest] + (["--dry-run"] if dry else [])
    print("$ %s" % " ".join(cmd))
    return subprocess.run(cmd).returncode


def cmd_publish(lib, outdir, dry):
    """FUTURE USE. Order: every exe first (inert until a launcher asks for
    mode 9), then the menus. One file at a time; stop on the first failure.
    Re-running after a failure resumes: a destination the dead put left
    missing or truncated is recognised by load_originals() and put again."""
    mp = os.path.join(outdir, "manifest.json")
    manifest = json.load(open(mp))
    work = {(w["title"], w["rel"]): w for w in load_originals(lib)}
    if sorted(work) != sorted((o["title"], o["share_path"][len(SHARE_PREFIX) + 1:].split("/", 1)[1])
                              for o in manifest["outputs"]):
        raise PatchError("%s does not list this script's outputs - rebuild first" % mp)
    for o in manifest["outputs"]:
        rel = o["share_path"][len(SHARE_PREFIX) + 1:].split("/", 1)[1]
        w = work[(o["title"], rel)]
        if md5(open(o["local_path"], "rb").read()) != o["md5"] or md5(w["data"]) != o["md5"]:
            raise PatchError("%s: local output is stale - rebuild first" % o["local_path"])
        if w["share_md5"] == o["md5"]:
            print("skip %s: share already has md5 %s" % (o["share_path"], o["md5"]))
            continue
        if w.get("damaged"):
            print("REPAIR %s: %s" % (o["share_path"], w["damaged"]))
        if o["original_md5"]:
            bpath = ci_path(lib, backup_rel_of(o["title"], rel))
            if bpath and md5(open(bpath, "rb").read()) == o["original_md5"]:
                print("backup present: %s" % o["backup_share_path"])
            else:
                if bpath:
                    raise PatchError("%s exists with another md5 - refusing to overwrite a backup"
                                     % o["backup_share_path"])
                if not w["orig_path"]:
                    raise PatchError("%s: no verified original to back up" % o["share_path"])
                if sharewrite(w["orig_path"], o["backup_share_path"], dry) != 0:
                    print("STOP: backup of %s failed" % o["share_path"])
                    return 2
        if sharewrite(o["local_path"], o["share_path"], dry) != 0:
            print("STOP: publishing %s failed" % o["share_path"])
            return 2
    print("publish %s" % ("dry-run complete" if dry else "complete - now run "
                          "scripts/validate-staged-library.py and bump _deploy_generation.txt"))
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--check", action="store_true")
    g.add_argument("--build", nargs="?", const=DEFAULT_OUT, metavar="OUTDIR")
    g.add_argument("--publish", nargs="?", const=DEFAULT_OUT, metavar="OUTDIR")
    g.add_argument("--install-server", action="store_true")
    ap.add_argument("--library", default=LIBRARY)
    ap.add_argument("--dry-run", action="store_true", help="with --publish: pass --dry-run to sharewrite")
    a = ap.parse_args(argv)
    if a.install_server:
        print("not applicable: the Quake II server on 192.168.1.132:27910 is Yamagi 8.60 and "
              "protocol 34 has no exe or menu check; SiN servers are box-to-box ds_*.bat "
              "(sin.exe from the same tree); SoF has no fleet server. Nothing to install.")
        return 0
    if a.publish and os.path.realpath(a.library) != os.path.realpath(SHARE_LIBRARY):
        # sharewrite.py always writes \\192.168.1.122\files and verifies through
        # /mnt/retro-share: originals read from anywhere else would be backed up
        # and "verified" against a library that is not the one being written.
        print("REFUSED: --publish writes the real share, so --library must be %s (got %s)"
              % (SHARE_LIBRARY, a.library))
        return 2
    if not os.path.isdir(a.library):
        print("library not mounted: %s" % a.library)
        return 3
    try:
        if a.check:
            return cmd_check(a.library)
        if a.build:
            return cmd_build(a.library, a.build)
        return cmd_publish(a.library, a.publish, a.dry_run)
    except PatchError as e:
        print("FAILED: %s" % e)
        return 2


if __name__ == "__main__":
    sys.exit(main())
