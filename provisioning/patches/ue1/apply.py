#!/usr/bin/env python3
"""ue1 - let the Unreal Engine 1 video menus list 1920x1080 (the 16-entry cut).

WHAT IS WRONG. Unreal Gold 226b, UT99 436 and Deus Ex 1112fm all ship a
DirectX 7 renderer (D3DDrv) whose `GetRes` console command keeps each distinct
16-bpp WxH that IDirectDraw7::EnumDisplayModes reports, in DirectDraw's order,
and then prints only the FIRST 16 (`cmp edi,0x10 / jge` in the print loop).
The menus copy GetRes into a combo the player cannot type into. DirectDraw
lists modes ascending on every 1080p LCD box, so 1920x1080 is the LAST entry:
measured 15 modes on .123 and 16 on .145 (listed), 20 on .195 and 24 on .240
(cut - those menus stop at 1440x900 and 1280x768). The game still STARTS at
1920x1080 (SetRes searches the full DirectDraw list); only the menu is short.

Deus Ex cuts the list a second time in script: MenuChoice_Resolution.
GetScreenResolutions() loops `while (ResNum < ArrayCount(Resolutions))` over
a `local string Resolutions[16]` that is never used, so it compiles to the
constant 16 (IntConstByte 0x10). And because MenuUIWindow.bEscapeSavesSettings
defaults True, Esc on Settings > Display SAVES: with 1080p absent, LoadSetting
selected entry 0 and leaving the screen by Esc, Enter or OK SetRes'd the box
down to 640x480 (.195) / 640x400 (.240). Both bytes fixed => 1920x1080 is in
the list, LoadSetting selects it, and none of those keys issue a SetRes.

THE EDITS (one byte each, file size unchanged):
  UnrealGold/System/D3DDrv.dll           @0x64f5   83 FF 10 -> 83 FF 7F  (required)
  DeusEx/SYSTEM/D3DDRV.DLL               @0x62ce   83 FF 10 -> 83 FF 7F  (required)
  DeusEx/SYSTEM/DEUSEX.U                 @0x4607ce       10 ->       28  (required)
  UnrealTournament436/System/D3DDrv.dll  @0x659f   83 FF 10 -> 83 FF 7F  (optional:
      the staged 436 renderer is OpenGLDrv, which is uncapped; this only helps a
      player who switches the tree to Direct3D)
cmp edi,imm8 sign-extends, so 0x7F (127) is the largest cap one byte can hold;
the loop is also bounded by the real mode count, and the output is a growing
string. 0x28 = 40 = ArrayCount(enumText), the menu choice's real capacity.

Either Deus Ex file alone changes nothing (the other still cuts at 16 in the
same order), so a half-published pair is harmless - but both are needed.

USAGE
    apply.py --check                 # read the staged files (original OR patched), assert everything
    apply.py --build [OUTDIR]        # write patched copies + manifest.json (works before and
                                     # after --publish; refuses an OUTDIR inside a git work tree)
    apply.py --publish [OUTDIR] [--dry-run]   # FUTURE: back up + put, one file at a time
    apply.py --install-server        # installs nothing; verifies deusex-server's DEUSEX.U identity
    options: --titles UnrealGold,DeusEx   --no-optional

Game files are copyrighted: outputs go to ~/.retro-fleet/patch-out/ue1/
(outside git), never into the repo.
"""
import argparse
import datetime
import hashlib
import json
import os
import struct
import subprocess
import sys

KEY = "ue1"
HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.normpath(os.path.join(HERE, "..", "..", ".."))
SHAREWRITE = os.path.join(REPO, "scripts", "fleet", "sharewrite.py")
MNT = os.environ.get("UE1_PATCH_MNT", "/mnt/retro-share")
LIB_REL = "Files/Games-Library"
DEFAULT_OUT = os.path.expanduser("~/.retro-fleet/patch-out/%s" % KEY)
BACKUP_DIR = "originals-2026-09-29"
# The fleet's Deus Ex dedicated server (systemd --user unit deusex-server, :7790).
DX_SERVER_ROOT = os.environ.get("UE1_DX_SERVER", "/home/voidsstr/deusex-server")

# ---------------------------------------------------------------------------
# The patch table. Every original byte, size and md5 was re-read from the
# staged library on 2026-09-29; `context` is the 16 bytes starting 8 before
# the edit, so a mismatch report says what is actually there.
# ---------------------------------------------------------------------------
CAP_CONTEXT = bytes.fromhex("ff8b45cc3bf87d60" "83ff107d5b8d0c7f")  # ...cmp edi,eax; jge; cmp edi,0x10; jge; lea...

PATCHES = [
    {
        "title": "UnrealGold",
        "rel": "UnrealGold/System/D3DDrv.dll",
        "kind": "pe",
        "required": True,
        "size": 208896,
        "md5": "f924e209e7bc23b90b6f0360174683d3",
        "patched_md5": None,
        "edits": [{"offset": 0x64F5, "before": "83ff10", "after": "83ff7f",
                   "context": CAP_CONTEXT.hex(), "va": 0x100064F5,
                   "what": "D3DDrv GetRes print loop: cmp edi,0x10 -> cmp edi,0x7f"}],
    },
    {
        "title": "DeusEx",
        "rel": "DeusEx/SYSTEM/D3DDRV.DLL",
        "kind": "pe",
        "required": True,
        "size": 212992,
        "md5": "6179c83ce1d7516908e5442ebfb4d154",
        "patched_md5": None,
        "edits": [{"offset": 0x62CE, "before": "83ff10", "after": "83ff7f",
                   "context": CAP_CONTEXT.hex(), "va": 0x100062CE,
                   "what": "D3DDrv GetRes print loop: cmp edi,0x10 -> cmp edi,0x7f "
                           "(the file's other 83 ff 10, @0x8d91, is an unrelated jg loop - untouched)"}],
    },
    {
        "title": "DeusEx",
        "rel": "DeusEx/SYSTEM/DEUSEX.U",
        "kind": "ue1pkg",
        "required": True,
        "size": 5386972,
        "md5": "d343da03a6d311ee412dfae4b52ff975",
        "patched_md5": None,
        "guid": "6bcadecaffb7b34e92b50c1f8eccec21",
        "generations": [[21422, 12872]],
        "function": ("MenuChoice_Resolution", "GetScreenResolutions"),
        "edits": [{"offset": 0x4607CE, "before": "10", "after": "28",
                   "pattern": "074b0196004b5f2c1016", "pattern_offset": 0x4607C6,
                   "what": "GetScreenResolutions: JumpIfNot(Less_IntInt(ResNum, IntConstByte 16)) "
                           "-> IntConstByte 40 (= ArrayCount(enumText))"}],
    },
    {
        "title": "UnrealTournament436",
        "rel": "UnrealTournament436/System/D3DDrv.dll",
        "kind": "pe",
        "required": False,
        "size": 217088,
        "md5": "dd6e3692f8ead5e1df88716024bc25d1",
        "patched_md5": None,
        "edits": [{"offset": 0x659F, "before": "83ff10", "after": "83ff7f",
                   "context": CAP_CONTEXT.hex(), "va": 0x1000659F,
                   "what": "D3DDrv GetRes print loop: cmp edi,0x10 -> cmp edi,0x7f "
                           "(optional: staged renderer is OpenGLDrv)"}],
    },
]
# Patched md5s, recorded from the first --build on 2026-09-29 (see PATCHED_MD5
# below - kept separate so the table above stays about the ORIGINALS).
PATCHED_MD5 = {
    "UnrealGold/System/D3DDrv.dll": "c51ba326f6a287f2e29ec3cc7dcb5e01",
    "DeusEx/SYSTEM/D3DDRV.DLL": "deff85e5e043b700567db9377c45b0ee",
    "DeusEx/SYSTEM/DEUSEX.U": "4e18e635d2dc0df2069171c72ec89eff",
    "UnrealTournament436/System/D3DDrv.dll": "35a4f321d06bf6da519b2d023be4eaa2",
}
for _p in PATCHES:
    _p["patched_md5"] = PATCHED_MD5.get(_p["rel"])


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


# ---------------------------------------------------------------------------
# Pure transforms (unit-tested without the share)
# ---------------------------------------------------------------------------
def apply_edits(data, edits):
    """Return data with every edit applied. Refuses unless each offset holds
    exactly the `before` bytes. Never changes the length."""
    out = bytearray(data)
    for e in edits:
        off, before, after = e["offset"], bytes.fromhex(e["before"]), bytes.fromhex(e["after"])
        if len(before) != len(after):
            raise PatchError("edit @0x%x changes length" % off)
        got = bytes(out[off:off + len(before)])
        if got != before:
            raise PatchError("@0x%x: expected %s, found %s" % (off, before.hex(" "), got.hex(" ")))
        out[off:off + len(after)] = after
    if len(out) != len(data):
        raise PatchError("length changed")
    return bytes(out)


def revert_edits(data, edits):
    """The inverse of apply_edits: a PATCHED file back to the original bytes.
    Every edit is a same-length byte swap, so the original is recoverable from
    the published file itself - which is what lets --build run again after
    --publish (the caller still checks the result against the recorded md5)."""
    return apply_edits(data, [dict(e, before=e["after"], after=e["before"]) for e in edits])


def edit_state(data, edits):
    """'original' if every edit site holds its before bytes, 'patched' if every
    one holds its after bytes, else 'unknown'."""
    if all(data[e["offset"]:e["offset"] + len(bytes.fromhex(e["before"]))] == bytes.fromhex(e["before"])
           for e in edits):
        return "original"
    if all(data[e["offset"]:e["offset"] + len(bytes.fromhex(e["after"]))] == bytes.fromhex(e["after"])
           for e in edits):
        return "patched"
    return "unknown"


def diff_offsets(a, b):
    if len(a) != len(b):
        raise PatchError("sizes differ: %d vs %d" % (len(a), len(b)))
    return [i for i in range(len(a)) if a[i] != b[i]]


# --- PE -------------------------------------------------------------------
def pe_info(data):
    """Minimal PE32 parse: image base, CheckSum, security dir size and sections."""
    if data[:2] != b"MZ":
        raise PatchError("not an MZ file")
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe:pe + 4] != b"PE\0\0":
        raise PatchError("no PE signature")
    nsec = struct.unpack_from("<H", data, pe + 6)[0]
    opt_size = struct.unpack_from("<H", data, pe + 20)[0]
    opt = pe + 24
    magic = struct.unpack_from("<H", data, opt)[0]
    if magic != 0x10B:
        raise PatchError("not PE32")
    image_base = struct.unpack_from("<I", data, opt + 28)[0]
    checksum = struct.unpack_from("<I", data, opt + 64)[0]
    security_size = struct.unpack_from("<I", data, opt + 96 + 4 * 8 + 4)[0]
    secs = []
    for i in range(nsec):
        s = opt + opt_size + 40 * i
        name = data[s:s + 8].rstrip(b"\0").decode("latin-1")
        vsize, va, rsize, raw = struct.unpack_from("<IIII", data, s + 8)
        chars = struct.unpack_from("<I", data, s + 36)[0]
        secs.append({"name": name, "va": va, "vsize": vsize, "raw": raw, "rsize": rsize, "chars": chars})
    return {"image_base": image_base, "checksum": checksum, "security_size": security_size, "sections": secs}


def pe_offset_to_va(info, off):
    for s in info["sections"]:
        if s["raw"] <= off < s["raw"] + s["rsize"]:
            return info["image_base"] + s["va"] + (off - s["raw"]), s
    raise PatchError("offset 0x%x is in no section" % off)


# --- UE1 package ------------------------------------------------------------
UE_TAG = 0x9E2A83C1


def read_compact(data, p):
    """Unreal compact index -> (value, new position)."""
    b0 = data[p]
    p += 1
    neg = b0 & 0x80
    v = b0 & 0x3F
    if b0 & 0x40:
        shift = 6
        for _ in range(4):
            b = data[p]
            p += 1
            v |= (b & 0x7F) << shift
            shift += 7
            if not b & 0x80:
                break
    return (-v if neg else v), p


def write_compact(v):
    """Inverse of read_compact (used by tests to build synthetic packages)."""
    neg = v < 0
    v = abs(v)
    b0 = (0x80 if neg else 0) | (v & 0x3F)
    v >>= 6
    out = bytearray()
    if v:
        b0 |= 0x40
    out.append(b0)
    while v:
        b = v & 0x7F
        v >>= 7
        if v:
            b |= 0x80
        out.append(b)
    return bytes(out)


def ue_header(data):
    tag, ver, lic, flags, nc, no, ec, eo, ic, io = struct.unpack_from("<IHHIIIIIII", data, 0)
    if tag != UE_TAG:
        raise PatchError("not an Unreal package")
    h = {"version": ver, "licensee": lic, "flags": flags, "name_count": nc, "name_offset": no,
         "export_count": ec, "export_offset": eo, "import_count": ic, "import_offset": io}
    if ver >= 68:
        h["guid"] = data[36:52].hex()
        gc = struct.unpack_from("<I", data, 52)[0]
        h["generations"] = [list(struct.unpack_from("<II", data, 56 + 8 * i)) for i in range(gc)]
    else:
        h["guid"] = None
        h["generations"] = []
    return h


def ue_tables(data, h=None):
    h = h or ue_header(data)
    names = []
    p = h["name_offset"]
    for _ in range(h["name_count"]):
        if h["version"] >= 64:
            n, p = read_compact(data, p)
            s = data[p:p + n]
            p += n
        else:
            e = data.index(b"\0", p)
            s = data[p:e + 1]
            p = e + 1
        names.append(s.rstrip(b"\0").decode("latin-1"))
        p += 4  # object flags
    imports = []
    p = h["import_offset"]
    for _ in range(h["import_count"]):
        cpkg, p = read_compact(data, p)
        cname, p = read_compact(data, p)
        pkg = struct.unpack_from("<i", data, p)[0]
        p += 4
        oname, p = read_compact(data, p)
        imports.append({"class_package": cpkg, "class_name": cname, "package": pkg, "name": oname})
    exports = []
    p = h["export_offset"]
    for _ in range(h["export_count"]):
        cls, p = read_compact(data, p)
        sup, p = read_compact(data, p)
        pkg = struct.unpack_from("<i", data, p)[0]
        p += 4
        oname, p = read_compact(data, p)
        oflags = struct.unpack_from("<I", data, p)[0]
        p += 4
        ssize, p = read_compact(data, p)
        soff = 0
        if ssize > 0:
            soff, p = read_compact(data, p)
        exports.append({"class": cls, "super": sup, "package": pkg, "name": oname,
                        "flags": oflags, "serial_size": ssize, "serial_offset": soff})
    return names, imports, exports


def ue_find_export(data, outer_name, obj_name):
    """Return (index, export) of the export named obj_name whose outer is an
    export named outer_name. Exactly one must exist."""
    names, imports, exports = ue_tables(data)
    hits = []
    for i, e in enumerate(exports):
        if names[e["name"]].lower() != obj_name.lower():
            continue
        if e["package"] > 0 and names[exports[e["package"] - 1]["name"]].lower() == outer_name.lower():
            hits.append((i, e))
    if len(hits) != 1:
        raise PatchError("%d exports named %s.%s" % (len(hits), outer_name, obj_name))
    return hits[0]


# ---------------------------------------------------------------------------
# Verification of one file's bytes (original or patched)
# ---------------------------------------------------------------------------
def verify_structure(spec, data, report):
    """Structural assertions that do not depend on the whole-file md5."""
    for e in spec["edits"]:
        off = e["offset"]
        if spec["kind"] == "pe":
            info = pe_info(data)
            va, sec = pe_offset_to_va(info, off)
            if not sec["chars"] & 0x20000000:
                raise PatchError("@0x%x lies in non-executable section %s" % (off, sec["name"]))
            if va != e["va"]:
                raise PatchError("@0x%x maps to VA 0x%x, expected 0x%x" % (off, va, e["va"]))
            if info["checksum"] != 0 or info["security_size"] != 0:
                raise PatchError("PE CheckSum/Authenticode present - a byte edit would need them fixed")
            report.append("  0x%x -> VA 0x%x in %s (executable); PE CheckSum 0, no signature" % (off, va, sec["name"]))
            ctx = bytes.fromhex(e["context"])
            got = data[off - 8:off + 8]
            state = edit_state(data, [e])
            want = ctx if state == "original" else ctx[:8] + bytes.fromhex(e["after"]) + ctx[8 + len(bytes.fromhex(e["after"])):]
            if got != want:
                raise PatchError("context @0x%x: expected %s, found %s" % (off - 8, want.hex(" "), got.hex(" ")))
            report.append("  context %s" % got.hex(" "))
        else:
            h = ue_header(data)
            if h["guid"] != spec["guid"] or h["generations"] != spec["generations"]:
                raise PatchError("package GUID/generations changed: %s %s" % (h["guid"], h["generations"]))
            report.append("  package v%d GUID %s generations %s (names %d, exports %d, imports %d)"
                          % (h["version"], h["guid"], h["generations"], h["name_count"],
                             h["export_count"], h["import_count"]))
            idx, ex = ue_find_export(data, *spec["function"])
            lo, hi = ex["serial_offset"], ex["serial_offset"] + ex["serial_size"]
            if not lo <= off < hi:
                raise PatchError("0x%x is outside %s.%s [0x%x,0x%x)" % ((off,) + spec["function"] + (lo, hi)))
            report.append("  %s.%s = export #%d (0-based; object ref %d), serial 0x%x..0x%x (%d B); edit inside it"
                          % (spec["function"] + (idx, idx + 1, lo, hi, ex["serial_size"])))
            state = edit_state(data, [e])
            pat = bytearray(bytes.fromhex(e["pattern"]))
            if state == "patched":
                pat[e["offset"] - e["pattern_offset"]] = bytes.fromhex(e["after"])[0]
            body = data[lo:hi]
            n = body.count(bytes(pat))
            if n != 1 or data.find(bytes(pat), lo, hi) != e["pattern_offset"]:
                raise PatchError("loop-test pattern %s found %d times in the function" % (bytes(pat).hex(" "), n))
            report.append("  loop test %s once, at 0x%x" % (bytes(pat).hex(" "), e["pattern_offset"]))


def classify(spec, data):
    """'original' | 'patched' | 'unknown' by size + md5 (+ edit sites)."""
    m = md5_bytes(data)
    if len(data) == spec["size"] and m == spec["md5"]:
        return "original"
    if spec.get("patched_md5") and m == spec["patched_md5"]:
        return "patched"
    return "unknown"


def resolve_ci(root, rel):
    """Case-insensitive path walk (the library is a Windows tree)."""
    cur = root
    for part in rel.replace("\\", "/").split("/"):
        if os.path.exists(os.path.join(cur, part)):
            cur = os.path.join(cur, part)
            continue
        try:
            match = [n for n in os.listdir(cur) if n.lower() == part.lower()]
        except OSError:
            match = []
        if len(match) != 1:
            raise PatchError("not found (case-insensitive): %s under %s" % (part, cur))
        cur = os.path.join(cur, match[0])
    return cur


def selected(args):
    titles = None
    if getattr(args, "titles", None):
        titles = {t.strip().lower() for t in args.titles.split(",") if t.strip()}
    out = []
    for p in PATCHES:
        if titles and p["title"].lower() not in titles:
            continue
        if getattr(args, "no_optional", False) and not p["required"]:
            continue
        out.append(p)
    return out


def library_root():
    root = os.path.join(MNT, LIB_REL)
    if not os.path.isdir(root):
        raise SystemExit("LIBRARY NOT MOUNTED: %s is absent - cannot read the staged originals" % root)
    return root


# ---------------------------------------------------------------------------
# --check
# ---------------------------------------------------------------------------
def cmd_check(args):
    root = library_root()
    bad = 0
    for spec in selected(args):
        rep = []
        try:
            path = resolve_ci(root, spec["rel"])
            data = open(path, "rb").read()
            state = classify(spec, data)
            rep.append("%s  [%s]  %s" % (spec["rel"], "required" if spec["required"] else "optional", path))
            rep.append("  size %d  md5 %s  -> %s" % (len(data), md5_bytes(data), state.upper()))
            if state == "unknown":
                raise PatchError("neither the recorded original (%s, %d B) nor the patched md5"
                                 % (spec["md5"], spec["size"]))
            for e in spec["edits"]:
                got = data[e["offset"]:e["offset"] + len(bytes.fromhex(e["before"]))]
                rep.append("  @0x%x holds %s (original %s, patched %s): %s"
                           % (e["offset"], got.hex(" "), e["before"], e["after"], e["what"]))
            verify_structure(spec, data, rep)
            if state == "original":
                patched = apply_edits(data, spec["edits"])
                verify_structure(spec, patched, [])
                pm = md5_bytes(patched)
                if not spec.get("patched_md5"):
                    raise PatchError("no patched md5 recorded for this file")
                if spec["patched_md5"] != pm:
                    raise PatchError("patched build %s differs from the recorded %s" % (pm, spec["patched_md5"]))
                rep.append("  would become md5 %s (= recorded patched md5)" % pm)
            else:
                orig = revert_edits(data, spec["edits"])
                if len(orig) != spec["size"] or md5_bytes(orig) != spec["md5"]:
                    raise PatchError("reverting the edit does not give the recorded original %s" % spec["md5"])
                rep.append("  already PATCHED; reverting the edit gives the recorded original %s" % spec["md5"])
            rep.append("  OK")
        except (PatchError, OSError) as ex:
            bad += 1
            rep.append("  FAIL: %s" % ex)
        print("\n".join(rep))
    print("\ncheck: %s" % ("FAILED (%d)" % bad if bad else "all files verified"))
    return 1 if bad else 0


# ---------------------------------------------------------------------------
# --build
# ---------------------------------------------------------------------------
def notes_text(outputs):
    lines = ["ue1 patch (retro-agent provisioning/patches/ue1/apply.py), built %s" % outputs[0]["built"],
             "Why: the D3DDrv GetRes 16-entry cut hides 1920x1080 from the Video/Display menu on",
             ".195 and .240 (Deus Ex also cut 16 tokens in DEUSEX.U script). Originals here are",
             "byte-exact copies of the staged files before the edit.", ""]
    for o in outputs:
        lines.append("%s" % o["share_path"])
        lines.append("  original md5 %s  size %d" % (o["original_md5"], o["size"]))
        lines.append("  patched  md5 %s  size %d" % (o["md5"], o["size"]))
        for e in o["edits"]:
            lines.append("  @0x%x %s -> %s  %s" % (e["offset"], e["before"], e["after"], e["what"]))
    lines.append("")
    lines.append("Rollback: put the file from this directory back over the staged path, then")
    lines.append("GAMESYNC RESET + GAMESYNC START on each box.")
    return "\r\n".join(lines) + "\r\n"


def git_toplevel(path):
    """The git work tree `path` (or its nearest existing parent) lies in, else None.
    Patched and original game binaries are copyrighted and must never land in
    one: the evidence folders under the main repo's .claude/ are untracked but
    NOT ignored, so a later `git add -A` would sweep them straight in."""
    p = os.path.abspath(path)
    while not os.path.isdir(p):
        parent = os.path.dirname(p)
        if parent == p:
            return None
        p = parent
    try:
        r = subprocess.run(["git", "-C", p, "rev-parse", "--show-toplevel"],
                           capture_output=True, text=True)
    except OSError:
        return None
    top = r.stdout.strip()
    return top if r.returncode == 0 and top else None


def cmd_build(args):
    root = library_root()
    out = os.path.abspath(args.outdir or DEFAULT_OUT)
    top = git_toplevel(out)
    if top:
        raise SystemExit("REFUSING to build into %s: it is inside the git work tree %s, and these outputs "
                         "are copyrighted game binaries. Use the default %s (outside git)." % (out, top, DEFAULT_OUT))
    built = datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    outputs = []
    for spec in selected(args):
        path = resolve_ci(root, spec["rel"])
        data = open(path, "rb").read()
        state = classify(spec, data)
        if state == "patched":
            # Already published: rebuild the original from it, so --build stays
            # reproducible for the whole life of the patch, not just before --publish.
            data = revert_edits(data, spec["edits"])
            if len(data) != spec["size"] or md5_bytes(data) != spec["md5"]:
                raise SystemExit("%s: staged file is PATCHED but reverting the edit does not give the "
                                 "recorded original %s - refusing to build" % (spec["rel"], spec["md5"]))
            print("%s: staged file is already PATCHED - rebuilt the original from it (md5 %s verified)"
                  % (spec["rel"], spec["md5"]))
        elif state != "original":
            raise SystemExit("%s: staged file is %s, not the recorded original - refusing to build"
                             % (spec["rel"], state))
        verify_structure(spec, data, [])
        patched = apply_edits(data, spec["edits"])
        verify_structure(spec, patched, [])
        diffs = diff_offsets(data, patched)
        want = [e["offset"] + i for e in spec["edits"]
                for i, (a, b) in enumerate(zip(bytes.fromhex(e["before"]), bytes.fromhex(e["after"]))) if a != b]
        if diffs != want:
            raise SystemExit("%s: unexpected byte differences %s" % (spec["rel"], [hex(d) for d in diffs]))
        if spec["kind"] == "ue1pkg":
            a, b = ue_header(data), ue_header(patched)
            if a != b:
                raise SystemExit("%s: package header changed" % spec["rel"])
        local = os.path.join(out, "patched", spec["rel"])
        orig_local = os.path.join(out, "originals", spec["rel"])
        for p, b in ((local, patched), (orig_local, data)):
            os.makedirs(os.path.dirname(p), exist_ok=True)
            with open(p, "wb") as f:
                f.write(b)
            if md5_file(p) != md5_bytes(b):
                raise SystemExit("write verify failed: %s" % p)
        title_rel = spec["rel"].split("/", 1)[1]
        outputs.append({
            "title": spec["title"],
            "share_path": "%s/%s" % (LIB_REL, spec["rel"]),
            "local_path": local,
            "md5": md5_bytes(patched),
            "size": len(patched),
            "original_md5": spec["md5"],
            "original_size": spec["size"],
            "original_local_path": orig_local,
            "backup_share_path": "%s/_patches/%s/%s/%s" % (LIB_REL, spec["title"], BACKUP_DIR, title_rel),
            "required": spec["required"],
            "edits": [{k: e[k] for k in ("offset", "before", "after", "what")} for e in spec["edits"]],
            "built": built,
        })
        if spec["kind"] == "ue1pkg":
            outputs[-1]["package_guid"] = spec["guid"]
            outputs[-1]["package_generations"] = spec["generations"]
        print("%-40s %s -> %s  (%d B, %d byte(s) changed)" % (spec["rel"], spec["md5"], md5_bytes(patched),
                                                               len(patched), len(diffs)))
    notes = []
    for title in sorted({o["title"] for o in outputs}):
        mine = [o for o in outputs if o["title"] == title]
        p = os.path.join(out, "notes", title, "PATCH-NOTES-ue1.txt")
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, "w", newline="") as f:
            f.write(notes_text(mine))
        notes.append({"title": title, "local_path": p, "md5": md5_file(p),
                      "share_path": "%s/_patches/%s/%s/PATCH-NOTES-ue1.txt" % (LIB_REL, title, BACKUP_DIR)})
    manifest = {"key": KEY, "built": built, "library": os.path.join(MNT, LIB_REL),
                "outputs": outputs, "notes": notes,
                "deploy": "publish, then GAMESYNC RESET + GAMESYNC START on every box; "
                          "verify by DOWNLOAD + md5 on each box (same-size edits)"}
    with open(os.path.join(out, "manifest.json"), "w") as f:
        json.dump(manifest, f, indent=1)
    print("manifest: %s" % os.path.join(out, "manifest.json"))
    return 0


# ---------------------------------------------------------------------------
# --publish (FUTURE use - not run in the build phase)
# ---------------------------------------------------------------------------
def share_md5(share_path):
    try:
        p = resolve_ci(MNT, share_path)
    except PatchError:
        return None
    return md5_file(p) if os.path.isfile(p) else None


def put(local, share_path, dry):
    cmd = [sys.executable, SHAREWRITE, "put", local, share_path] + (["--dry-run"] if dry else [])
    print("  $ sharewrite.py put %s %s%s" % (local, share_path, " --dry-run" if dry else ""))
    r = subprocess.run(cmd)
    return r.returncode


def cmd_publish(args):
    out = os.path.abspath(args.outdir or DEFAULT_OUT)
    manifest = json.load(open(os.path.join(out, "manifest.json")))
    library_root()
    want_titles = {p["title"] for p in selected(args)}
    want_rels = {"%s/%s" % (LIB_REL, p["rel"]) for p in selected(args)}
    outputs = [o for o in manifest["outputs"] if o["share_path"] in want_rels]
    # A manifest from a narrower --build would otherwise publish a SUBSET and
    # still end with "publish: done" - refuse before anything is written.
    missing = sorted(want_rels - {o["share_path"] for o in outputs})
    if missing:
        print("FAIL: %s has no build for %s - run --build with the same --titles/--no-optional first"
              % (os.path.join(out, "manifest.json"), ", ".join(missing)))
        return 2
    for o in outputs:
        print("%s" % o["share_path"])
        if md5_file(o["local_path"]) != o["md5"] or md5_file(o["original_local_path"]) != o["original_md5"]:
            print("  FAIL: local build output does not match the manifest - rebuild")
            return 2
        cur = share_md5(o["share_path"])
        if cur not in (o["original_md5"], o["md5"]):
            print("  FAIL: share copy is %s, neither original %s nor patched %s - stopping"
                  % (cur, o["original_md5"], o["md5"]))
            print("  If this is the debris of a failed put, the verified original is %s (backup %s);"
                  " put it back with scripts/fleet/sharewrite.py and re-run." % (o["original_local_path"],
                                                                                  o["backup_share_path"]))
            return 3
        # 1. back up the original FIRST - also when the staged file is already
        #    patched, so every published file has its original beside it.
        if share_md5(o["backup_share_path"]) == o["original_md5"]:
            print("  backup already present: %s" % o["backup_share_path"])
        else:
            rc = put(o["original_local_path"], o["backup_share_path"], args.dry_run)
            if rc != 0:
                print("  FAIL: backup put rc=%d - stopping before touching the staged file" % rc)
                return 4
            if not args.dry_run and share_md5(o["backup_share_path"]) != o["original_md5"]:
                print("  FAIL: backup does not read back as the original - stopping")
                return 4
        if cur == o["md5"]:
            print("  already patched on the share - skip")
            continue
        # 2. the patched file
        rc = put(o["local_path"], o["share_path"], args.dry_run)
        if rc != 0:
            print("  FAIL: put rc=%d - stopping" % rc)
            return 5
        if not args.dry_run:
            got = share_md5(o["share_path"])
            if got != o["md5"]:
                print("  FAIL: share reads back %s, want %s - stopping" % (got, o["md5"]))
                return 5
            print("  published and verified: %s" % got)
    for n in manifest.get("notes", []):
        if n["title"] not in want_titles:
            continue
        if share_md5(n["share_path"]) == n["md5"]:
            continue
        rc = put(n["local_path"], n["share_path"], args.dry_run)
        if rc != 0:
            print("  FAIL: notes put rc=%d" % rc)
            return 6
    print("publish: done%s. NEXT: python3 scripts/validate-staged-library.py --quiet (after any library "
          "write); then GAMESYNC RESET + GAMESYNC START on every box, then DOWNLOAD each file and compare "
          "md5 (the edits keep file size, and a provisioned box's boot sync is idle)."
          % (" - dry run" if args.dry_run else ""))
    return 0


def cmd_install_server(args):
    """Nothing is installed (read-only): verify the fleet server still has the
    package identity the patched clients will present at join."""
    spec = [p for p in PATCHES if p["kind"] == "ue1pkg"][0]
    print("ue1: no fleet game server needs a change - nothing is installed, nothing to restart.\n"
          "  - D3DDrv.dll is a client renderer; a dedicated server (ucc, or DeusEx.exe -server) never loads it.\n"
          "  - DEUSEX.U: the Deus Ex join handshake is 'USES GUID= PKG= FLAGS= SIZE= GEN=' (Engine.dll);\n"
          "    SIZE only sizes a download. The patched client keeps GUID %s and\n"
          "    generations %s. The edited function is client menu code. Leave the server's copy alone."
          % (spec["guid"], spec["generations"]))
    try:
        path = resolve_ci(DX_SERVER_ROOT, "System/DEUSEX.U")
    except PatchError:
        print("  NOT VERIFIED: no System/DEUSEX.U under %s (case-insensitive) - check the server host by hand."
              % DX_SERVER_ROOT)
        return 0
    data = open(path, "rb").read()
    h = ue_header(data)
    m = md5_bytes(data)
    kind = {spec["md5"]: "the recorded ORIGINAL", spec.get("patched_md5"): "the PATCHED client build"}.get(m, "neither")
    print("  deusex-server %s: md5 %s (%s), GUID %s, generations %s" % (path, m, kind, h["guid"], h["generations"]))
    if h["guid"] != spec["guid"] or h["generations"] != spec["generations"]:
        print("  FAIL: the server's package identity differs from the clients' - a join would be refused "
              "('version mismatch'), whatever this patch does")
        return 1
    print("  OK: same GUID and generations as the patched client - the package map will match at join")
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--check", action="store_true")
    g.add_argument("--build", nargs="?", const="", metavar="OUTDIR")
    g.add_argument("--publish", nargs="?", const="", metavar="OUTDIR")
    g.add_argument("--install-server", action="store_true")
    ap.add_argument("--titles", help="comma list: UnrealGold,DeusEx,UnrealTournament436")
    ap.add_argument("--no-optional", action="store_true", help="skip the optional UT436 D3DDrv edit")
    ap.add_argument("--dry-run", action="store_true", help="with --publish: pass --dry-run to sharewrite")
    a = ap.parse_args(argv)
    try:
        if a.check:
            return cmd_check(a)
        if a.build is not None:
            a.outdir = a.build or None
            return cmd_build(a)
        if a.publish is not None:
            a.outdir = a.publish or None
            return cmd_publish(a)
        return cmd_install_server(a)
    except PatchError as ex:
        print("FAIL: %s" % ex)
        return 1


if __name__ == "__main__":
    sys.exit(main())
