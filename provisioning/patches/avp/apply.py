#!/usr/bin/env python3
"""Aliens versus Predator Gold - undo the InstallShield file obfuscation in the staged tree.

THE DEFECT. `Games-Library/AliensVsPredator` is the raw payload of the Gold disc 1,
and 154 of its files still carry the InstallShield 5 file obfuscation that the
retail installer removes as it copies them (the same transform unshield calls
`unshield_deobfuscate`):

    plain[i] = ror8(stored[i] ^ 0xD5, 2) - ((seed + i) % 0x47)     seed = 0 per file

So avp.exe never sees a fast-file: `fastfile\\ffinfo.txt` is noise, every
`Tex*.FFL` starts 9c c8 f4 e8 instead of "RFFL", and the engine falls back to a
loose `Graphics\\` tree that no distribution has. On .123 .145 .195 and .240 the
last LOGFILE.TXT ends with `Menus\\IntroFont.rim ... file not found`,
`AwCreateGraphic(): ERROR: No data medium is specified` and
`ASSERTION FAILED! pSurface alt_tab.cpp 198`, and no log has a single
`Loaded FastFile:` line. The game has never reached its menu on this fleet.

THE FILES (files.tsv, generated from the share on 2026-09-29 and pinned):
    fastfile/Tex1..Tex58.FFL, fastfile/ffinfo.txt   59
    avp_huds/*.rif                                  23
    avp_rifs/*.rif                                  63
    FMVs/*.bik (8) and FMVs/IntroSound.smk           9
The first 145 reproduce, byte for byte, the Gold-install md5 list that
github.com/atsb/NakedAVP publishes (nakedavp-gold-md5.txt). The nine FMVs are in
no reference list, so they are proven by STRUCTURE instead: the Bink frame index
and every frame's audio packet sizes, the Smacker size table and every frame's
chunk sizes - which walks each file end to end, so a wrong transform cannot pass.

LEAVE ALONE (action "keep" in files.tsv): Snd*.FFL, common.ffl, *.dat and
language.txt already match the reference as they are; shape_rifs (REBINFF2),
the 53 message*.smk, the sound sets and everything else are plain. Running the
transform over any of them destroys it.

Every file keeps its size, so a publish MUST give it a fresh write time (GAMESYNC
resumes on size AND mtime) - scripts/fleet/sharewrite.py checks that per file.

    python3 apply.py --check [--full-scan] [-v]    read-only: verify every input md5
    python3 apply.py --build [OUTDIR]              write de-obfuscated copies + manifest.json
    python3 apply.py --publish [--from OUTDIR] [--dry-run]   (FUTURE) back up + put, one file at a time
    python3 apply.py --install-server              not applicable: AvP has no fleet server
    python3 apply.py --video-cfg W H [BPP] [--out FILE]   write an AvP_Video.cfg for a hand test

OUTDIR defaults to ~/.retro-fleet/patch-out/avp (outside git: game data is not ours to commit).
"""
import argparse
import datetime
import hashlib
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", "..", ".."))

SHARE_MNT = "/mnt/retro-share"                 # read-only CIFS mount of \\192.168.1.122\files
TITLE = "AliensVsPredator"
TITLE_REL = "Files/Games-Library/" + TITLE     # share-root relative, the form sharewrite takes
BACKUP_REL = "Files/Games-Library/_patches/" + TITLE + "/originals-2026-09-29"
TABLE_PATH = os.path.join(HERE, "files.tsv")
REFERENCE_PATH = os.path.join(HERE, "nakedavp-gold-md5.txt")
SHAREWRITE = os.path.join(REPO, "scripts", "fleet", "sharewrite.py")
DEFAULT_OUT = os.path.expanduser("~/.retro-fleet/patch-out/avp")

# ------------------------------------------------------------------ transform --

XOR_KEY = 0xD5
PERIOD = 0x47          # 71: the (seed + i) % 0x47 term


def _ror8(x, n):
    return ((x >> n) | (x << (8 - n))) & 0xFF


def _rol8(x, n):
    return ((x << n) | (x >> (8 - n))) & 0xFF


# One 256-byte table per residue class k = (seed + i) % 71, so a whole file is
# 71 C-speed bytes.translate() calls instead of a Python loop per byte.
DEOB_TABLES = tuple(bytes((_ror8(c ^ XOR_KEY, 2) - k) & 0xFF for c in range(256))
                    for k in range(PERIOD))
OBF_TABLES = tuple(bytes(_rol8((p + k) & 0xFF, 2) ^ XOR_KEY for p in range(256))
                   for k in range(PERIOD))


def _apply_tables(data, seed, tables):
    data = bytes(data)
    out = bytearray(len(data))
    for k in range(PERIOD):
        start = (k - seed) % PERIOD        # the positions i with (seed + i) % 71 == k
        out[start::PERIOD] = data[start::PERIOD].translate(tables[k])
    return bytes(out)


def deobfuscate(data, seed=0):
    """InstallShield 5 file de-obfuscation (unshield's unshield_deobfuscate)."""
    return _apply_tables(data, seed, DEOB_TABLES)


def obfuscate(data, seed=0):
    """The inverse - used only to build synthetic test inputs."""
    return _apply_tables(data, seed, OBF_TABLES)


def deobfuscate_reference(data, seed=0):
    """The literal per-byte loop, kept as the oracle the fast path is tested against."""
    out = bytearray(data)
    for i in range(len(out)):
        out[i] = (_ror8(out[i] ^ XOR_KEY, 2) - ((seed + i) % PERIOD)) & 0xFF
    return bytes(out)


def md5_bytes(b):
    return hashlib.md5(b).hexdigest()


def md5_file(path):
    h = hashlib.md5()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


# ------------------------------------------------------------- the file table --

TABLE_FIELDS = ("action", "path", "size", "original_md5", "patched_md5",
                "reference_md5", "verify", "raw_head16")


class Row(object):
    __slots__ = TABLE_FIELDS

    def __init__(self, **kw):
        for f in TABLE_FIELDS:
            setattr(self, f, kw[f])

    @property
    def kind(self):
        return file_kind(self.path)

    def as_dict(self):
        return dict((f, getattr(self, f)) for f in TABLE_FIELDS)


def file_kind(path):
    low = path.lower()
    if low.endswith("/ffinfo.txt") or low == "ffinfo.txt":
        return "ffinfo"
    for ext, kind in ((".rif", "rif"), (".ffl", "rffl"), (".bik", "bink"), (".smk", "smk")):
        if low.endswith(ext):
            return kind
    return "other"


def load_table(path=TABLE_PATH):
    rows = []
    with open(path, encoding="utf-8") as f:
        for n, line in enumerate(f, 1):
            line = line.rstrip("\n")
            if not line or line.startswith("#"):
                continue
            parts = line.split("\t")
            if parts[0] == "action":
                continue                        # header
            if len(parts) != len(TABLE_FIELDS):
                raise ValueError("%s:%d: %d fields, want %d" % (path, n, len(parts), len(TABLE_FIELDS)))
            kw = dict(zip(TABLE_FIELDS, parts))
            kw["size"] = int(kw["size"])
            if kw["action"] not in ("deobfuscate", "keep"):
                raise ValueError("%s:%d: unknown action %r" % (path, n, kw["action"]))
            rows.append(Row(**kw))
    return rows


def write_table(rows, path, header_comment):
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        for c in header_comment.splitlines():
            f.write("# %s\n" % c if c else "#\n")
        f.write("\t".join(TABLE_FIELDS) + "\n")
        for r in rows:
            f.write("\t".join(str(getattr(r, fld)) for fld in TABLE_FIELDS) + "\n")


def load_reference(path=REFERENCE_PATH):
    """{lower-case relative path: md5} from the NakedAVP 'Gold edition MD5s' list."""
    ref = {}
    with open(path, encoding="utf-8") as f:
        for line in f:
            m = re.match(r"MD5 \((.+)\) = ([0-9a-f]{32})\s*$", line.strip())
            if m:
                ref[m.group(1).lower()] = m.group(2)
    return ref


def publish_order(rows):
    """Order for --publish: ffinfo.txt LAST.

    While ffinfo.txt is still obfuscated the engine finds no fast-file list at
    all and fails exactly as it does today. Made plain first, it would point the
    loader at Tex*.FFL files that may still be obfuscated - a new failure mode a
    half-finished publish should never create. So the switch flips last.
    """
    rank = {"bink": 0, "smk": 0, "rif": 1, "rffl": 2, "ffinfo": 9}
    return sorted(rows, key=lambda r: (rank.get(r.kind, 5), r.path.lower()))


# --------------------------------------------------------- structure checks --

MAGIC = {"rif": (b"REBCRIF1",), "rffl": (b"RFFL",), "bink": (b"BIKf", b"BIKg", b"BIKh", b"BIKi"),
         "smk": (b"SMK2", b"SMK4")}


def rffl_entries(data):
    """Parse a Rebellion fast-file (RFFL) index -> [(name, offset, length)].

    Layout (icculus avp ffread.cpp): 'RFFL', le32 version, le32 num_files,
    le32 total_headsize, le32 total_datasize, then num_files index entries of
    le32 offset (from the start of the data block), le32 length, and a
    NUL-terminated name padded to a 4-byte boundary. Raises ValueError on any
    inconsistency, which is what a still-obfuscated or mis-transformed file hits.
    """
    if len(data) < 20 or data[:4] != b"RFFL":
        raise ValueError("no RFFL signature")
    version, nfiles, headsize, datasize = struct.unpack_from("<IIII", data, 4)
    if 20 + headsize + datasize != len(data):
        raise ValueError("header says %d + %d + 20 bytes, file is %d" % (headsize, datasize, len(data)))
    data_base = 20 + headsize
    p, out = 20, []
    for i in range(nfiles):
        if p + 8 > data_base:
            raise ValueError("index entry %d runs past the index" % i)
        off, length = struct.unpack_from("<II", data, p)
        end = data.find(b"\0", p + 8, data_base)
        if end < 0:
            raise ValueError("index entry %d has no terminated name" % i)
        name = data[p + 8:end].decode("latin-1")
        if off + length > datasize:
            raise ValueError("entry %r lies outside the data block" % name)
        out.append((name, data_base + off, length))
        p = (end + 1 + 3) & ~3
    if p != data_base:
        raise ValueError("index ends at %d, header says %d" % (p, data_base))
    return out


def rffl_check(data):
    entries = rffl_entries(data)
    # Every packed file here is an IFF chunk ('LIST'/'FORM' + big-endian size).
    bad = [n for n, o, ln in entries
           if ln < 8 or struct.unpack_from(">I", data, o + 4)[0] + 8 != ln]
    if bad:
        raise ValueError("%d packed file(s) are not whole IFF chunks, first %r" % (len(bad), bad[0]))
    return "RFFL %d files, every one a whole IFF chunk" % len(entries)


def ffinfo_entries(data):
    """fastfile\\ffinfo.txt -> [(directory, fastfile)] e.g. ('graphics\\Menus', 'Tex37.FFL')."""
    text = data.decode("latin-1")
    out = []
    for line in text.splitlines():
        line = line.strip()
        if not line:
            continue
        if ";" not in line:
            raise ValueError("ffinfo line without ';': %r" % line[:40])
        d, ff = line.rsplit(";", 1)
        if not re.match(r"^(Tex|Snd)\d+\.FFL$", ff, re.I):
            raise ValueError("ffinfo names %r, not a Tex/Snd fast-file" % ff[:40])
        out.append((d, ff))
    return out


def bink_check(data):
    """Bink 1 header + frame index + every frame's audio packet sizes."""
    if len(data) < 44 or data[:3] != b"BIK":
        raise ValueError("no BIK signature")
    (size8, frames, largest, _frames2, width, height, fps_n, fps_d, _vflags,
     tracks) = struct.unpack_from("<IIIIIIIIII", data, 4)
    if size8 + 8 != len(data):
        raise ValueError("header size %d != file %d" % (size8 + 8, len(data)))
    if not (0 < frames < 1000000 and 0 < width <= 4096 and 0 < height <= 4096 and tracks <= 256):
        raise ValueError("implausible header: %d frames %dx%d %d tracks" % (frames, width, height, tracks))
    p = 44 + 12 * tracks          # max decoded sizes, (rate, flags), track ids
    table = struct.unpack_from("<%dI" % (frames + 1), data, p)
    pos = [x & ~1 for x in table]
    if pos[0] != p + 4 * (frames + 1):
        raise ValueError("first frame at %d, index ends at %d" % (pos[0], p + 4 * (frames + 1)))
    if pos[-1] != len(data):
        raise ValueError("frame index ends at %d, file is %d" % (pos[-1], len(data)))
    for i in range(frames):
        if not pos[i] < pos[i + 1]:
            raise ValueError("frame index not increasing at %d" % i)
        q = pos[i]
        if pos[i + 1] - pos[i] > largest:
            raise ValueError("frame %d larger than the header's largest frame" % i)
        for _t in range(tracks):
            (asz,) = struct.unpack_from("<I", data, q)
            q += 4 + asz
            if q > pos[i + 1]:
                raise ValueError("frame %d: audio packet runs past the frame" % i)
    return "Bink %dx%d, %d frames at %d/%d fps, %d audio track(s), every frame consistent" % (
        width, height, frames, fps_n, fps_d, tracks)


def smk_check(data):
    """Smacker header + frame size table + every frame's palette/audio chunk sizes."""
    if len(data) < 104 or data[:4] not in (b"SMK2", b"SMK4"):
        raise ValueError("no SMK2/SMK4 signature")
    width, height, frames, _rate, flags = struct.unpack_from("<IIIiI", data, 4)
    treesize = struct.unpack_from("<I", data, 52)[0]
    nf = frames + (1 if flags & 1 else 0)
    if not (0 < nf < 1000000 and 0 < width <= 4096 and 0 < height <= 4096):
        raise ValueError("implausible header")
    sizes = struct.unpack_from("<%dI" % nf, data, 104)
    fflags = data[104 + 4 * nf:104 + 5 * nf]
    p = 104 + 5 * nf + treesize
    total = p + sum(s & ~3 for s in sizes)
    if total != len(data):
        raise ValueError("size table adds up to %d, file is %d" % (total, len(data)))
    for i in range(nf):
        fsz = sizes[i] & ~3
        q, f = p, fflags[i]
        if f & 1:
            q += data[q] * 4
        for t in range(7):
            if f & (2 << t):
                (asz,) = struct.unpack_from("<I", data, q)
                if asz < 4 or q + asz > p + fsz:
                    raise ValueError("frame %d: audio chunk %d runs past the frame" % (i, t))
                q += asz
        if q > p + fsz:
            raise ValueError("frame %d: chunks run past the frame" % i)
        p += fsz
    return "Smacker %dx%d, %d frames, every frame consistent" % (width, height, frames)


def structure_check(kind, data):
    """Return a one-line description, or raise ValueError."""
    if kind in MAGIC and not data.startswith(MAGIC[kind]):
        raise ValueError("%s signature missing (starts %s)" % (kind, data[:8].hex()))
    if kind == "rffl":
        return rffl_check(data)
    if kind == "bink":
        return bink_check(data)
    if kind == "smk":
        return smk_check(data)
    if kind == "ffinfo":
        return "ffinfo.txt, %d fast-file lines" % len(ffinfo_entries(data))
    if kind == "rif":
        return "REBCRIF1"
    raise ValueError("no structure check for %r" % kind)


def classify_head(head, sample=b""):
    """Coarse magic class for the whole-tree scan; '?' when nothing is recognised."""
    for kind, magics in MAGIC.items():
        if head.startswith(magics):
            return kind
    for m, name in ((b"REBINFF2", "rebinff2"), (b"MARSOUND", "marsound"), (b"MZ", "pe"),
                    (b"RIFF", "riff"), (b"\x00\x00\x01\x00", "ico"), (b"GIF8", "gif"),
                    (b"BM", "bmp"), (b"\xff\xd8\xff", "jpeg"), (b"8BPS", "psd"),
                    (b"REGEDIT", "reg")):
        if head.startswith(m):
            return name
    s = sample or head
    if s and all((32 <= b < 127) or b in (9, 10, 13) for b in s):
        return "text"
    return "?"


# ------------------------------------------------------------------- check ---

ORIGINAL, PATCHED, UNEXPECTED, MISSING = "original", "patched", "UNEXPECTED", "MISSING"


def title_root(share_root):
    return os.path.join(share_root, *TITLE_REL.split("/"))


def local_path(root, rel):
    return os.path.join(root, *rel.split("/"))


def file_state(row, data):
    """original / patched / UNEXPECTED for one deobfuscate row's bytes."""
    if len(data) != row.size:
        return UNEXPECTED, "size %d, table says %d" % (len(data), row.size)
    m = md5_bytes(data)
    if m == row.original_md5:
        if data[:16].hex() != row.raw_head16:
            return UNEXPECTED, "md5 matched but head differs"      # cannot happen; belt and braces
        return ORIGINAL, "obfuscated as staged (head %s)" % row.raw_head16[:16]
    if m == row.patched_md5:
        return PATCHED, "already de-obfuscated"
    return UNEXPECTED, "md5 %s is neither the staged original nor the patched file" % m


def run_check(share_root=SHARE_MNT, rows=None, full_scan=False, verbose=False, out=sys.stdout):
    rows = load_table() if rows is None else rows
    root = title_root(share_root)
    counts = {ORIGINAL: 0, PATCHED: 0, UNEXPECTED: 0, MISSING: 0, "keep_ok": 0, "keep_bad": 0}
    problems = []
    for r in rows:
        p = local_path(root, r.path)
        try:
            with open(p, "rb") as f:
                data = f.read()
        except OSError as e:
            counts[MISSING if r.action == "deobfuscate" else "keep_bad"] += 1
            problems.append("%s: MISSING (%s)" % (r.path, e.strerror))
            continue
        if r.action == "keep":
            m = md5_bytes(data)
            if m == r.original_md5 and len(data) == r.size:
                counts["keep_ok"] += 1
                if verbose:
                    print("  keep      %s  %s" % (m, r.path), file=out)
            else:
                counts["keep_bad"] += 1
                problems.append("%s: must stay as-is, md5 %s != reference %s" % (r.path, m, r.original_md5))
            continue
        state, why = file_state(r, data)
        counts[state] += 1
        if state == UNEXPECTED:
            problems.append("%s: %s" % (r.path, why))
        if verbose:
            print("  %-9s %s  %s" % (state, md5_bytes(data), r.path), file=out)
    scan_hits = []
    if full_scan:
        scan_hits = scan_tree(root, rows)
        for rel, why in scan_hits:
            problems.append("%s: %s" % (rel, why))
    ndeob = sum(1 for r in rows if r.action == "deobfuscate")
    nkeep = len(rows) - ndeob
    print("AvP check - %s" % root, file=out)
    print("  to de-obfuscate: %d  original=%d  patched=%d  UNEXPECTED=%d  missing=%d"
          % (ndeob, counts[ORIGINAL], counts[PATCHED], counts[UNEXPECTED], counts[MISSING]), file=out)
    print("  must stay as-is: %d  match=%d  WRONG/missing=%d" % (nkeep, counts["keep_ok"],
                                                                nkeep - counts["keep_ok"]), file=out)
    if full_scan:
        print("  whole-tree scan: %d file(s) outside the table look obfuscated" % len(scan_hits), file=out)
    if counts[ORIGINAL] and counts[PATCHED]:
        print("  NOTE: the share is part-way through a publish (%d patched, %d not yet)"
              % (counts[PATCHED], counts[ORIGINAL]), file=out)
    for pr in problems:
        print("  FAIL %s" % pr, file=out)
    ok = not problems
    print("  RESULT: %s" % ("OK" if ok else "FAILED (%d problem(s))" % len(problems)), file=out)
    return {"ok": ok, "counts": counts, "problems": problems, "scan_hits": scan_hits}


def scan_tree(root, rows):
    """Every file NOT in the table whose raw head is unrecognised but whose
    de-obfuscated head is a known format: the evidence that a file was missed."""
    known = set(r.path.lower() for r in rows)
    hits = []
    for dp, _dn, fn in os.walk(root):
        for name in fn:
            full = os.path.join(dp, name)
            rel = os.path.relpath(full, root).replace(os.sep, "/")
            if rel.lower() in known:
                continue
            with open(full, "rb") as f:
                head = f.read(4096)
            if classify_head(head[:16], head) != "?":
                continue
            d = deobfuscate(head)
            cls = classify_head(d[:16], d)
            if cls not in ("?", "text"):
                hits.append((rel, "raw head unknown, de-obfuscates to %s - not in the table" % cls))
            elif cls == "text" and len(head) >= 64:
                hits.append((rel, "raw head unknown, de-obfuscates to plain text - not in the table"))
    return hits


# ------------------------------------------------------------------- build ---

def run_build(outdir, share_root=SHARE_MNT, rows=None, out=sys.stdout, check_assets=True):
    rows = load_table() if rows is None else rows
    root = title_root(share_root)
    outdir = os.path.abspath(outdir)
    os.makedirs(outdir, exist_ok=True)
    results, failures = [], []
    for r in [x for x in rows if x.action == "deobfuscate"]:
        src = local_path(root, r.path)
        try:
            with open(src, "rb") as f:
                data = f.read()
        except OSError as e:
            failures.append("%s: cannot read (%s)" % (r.path, e.strerror))
            continue
        state, why = file_state(r, data)
        if state == UNEXPECTED:
            failures.append("%s: %s" % (r.path, why))
            continue
        plain = deobfuscate(data) if state == ORIGINAL else data
        m = md5_bytes(plain)
        if m != r.patched_md5:
            failures.append("%s: de-obfuscated md5 %s != pinned %s" % (r.path, m, r.patched_md5))
            continue
        if r.reference_md5 != "-" and m != r.reference_md5:
            failures.append("%s: md5 %s != NakedAVP reference %s" % (r.path, m, r.reference_md5))
            continue
        try:
            structure = structure_check(r.kind, plain)
        except (ValueError, struct.error) as e:
            failures.append("%s: structure check failed: %s" % (r.path, e))
            continue
        dst = local_path(outdir, r.path)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        tmp = dst + ".part"
        with open(tmp, "wb") as f:
            f.write(plain)
        os.replace(tmp, dst)
        written = md5_file(dst)
        if written != m:
            failures.append("%s: written file reads back as %s" % (r.path, written))
            continue
        results.append({
            "share_path": TITLE_REL + "/" + r.path,
            "local_path": dst,
            "md5": m,
            "size": len(plain),
            "original_md5": r.original_md5,
            "reference_md5": None if r.reference_md5 == "-" else r.reference_md5,
            "verified_by": ("NakedAVP Gold md5 list + " if r.reference_md5 != "-" else "") + structure,
            "source_state": state,
        })
    asset = None
    if check_assets and not failures:
        try:
            asset = menu_asset_check(outdir, root)
        except (ValueError, OSError, struct.error) as e:
            failures.append("asset resolution: %s" % e)
    manifest = {
        "title": TITLE,
        "key": "avp",
        "generated": datetime.datetime.now().isoformat(timespec="seconds"),
        "transform": "plain[i] = ror8(c[i] ^ 0xD5, 2) - (i % 0x47), seed 0 per file (InstallShield 5)",
        "share_root": share_root,
        "backup_rel": BACKUP_REL,
        "complete": not failures and len(results) == sum(1 for x in rows if x.action == "deobfuscate"),
        "outputs": results,
        "menu_asset_check": asset,
        "failures": failures,
    }
    with open(os.path.join(outdir, "manifest.json"), "w") as f:
        json.dump(manifest, f, indent=1)
    print("AvP build -> %s" % outdir, file=out)
    print("  written %d / %d, failures %d" % (len(results), sum(1 for x in rows if x.action == "deobfuscate"),
                                              len(failures)), file=out)
    if asset:
        print("  menu asset: %s" % asset, file=out)
    for fl in failures:
        print("  FAIL %s" % fl, file=out)
    print("  manifest: %s  complete=%s" % (os.path.join(outdir, "manifest.json"), manifest["complete"]), file=out)
    return manifest


def _find_ci(directory, name):
    """Case-insensitive lookup of one file in one directory (Windows semantics)."""
    try:
        for n in os.listdir(directory):
            if n.lower() == name.lower():
                return os.path.join(directory, n)
    except OSError:
        pass
    return None


def menu_asset_check(outdir, title_root_dir):
    """Prove the asset the failing boxes logged now resolves through the fast-files.

    LOGFILE.TXT on every LCD box: 'Menus\\IntroFont.rim ... file not found'. With
    the built ffinfo.txt, 'graphics\\Menus' must map to a Tex*.FFL whose index
    holds graphics\\Menus\\IntroFont.RIM as a whole IFF chunk - and every
    fast-file ffinfo.txt names must exist (Tex from the build, Snd as staged).
    """
    ff_path = local_path(outdir, "fastfile/ffinfo.txt")
    with open(ff_path, "rb") as f:
        entries = ffinfo_entries(f.read())
    missing = []
    for _d, ff in entries:
        if not (_find_ci(os.path.join(outdir, "fastfile"), ff) or
                _find_ci(os.path.join(title_root_dir, "fastfile"), ff)):
            missing.append(ff)
    if missing:
        raise ValueError("ffinfo.txt names %d fast-file(s) that do not exist: %s" % (len(missing), missing[:5]))
    menus = [ff for d, ff in entries if d.lower() == "graphics\\menus"]
    if len(menus) != 1:
        raise ValueError("ffinfo.txt maps graphics\\Menus to %r" % menus)
    ffl = _find_ci(os.path.join(outdir, "fastfile"), menus[0])
    with open(ffl, "rb") as f:
        data = f.read()
    hits = [(n, o, ln) for n, o, ln in rffl_entries(data) if n.lower() == "graphics\\menus\\introfont.rim"]
    if not hits:
        raise ValueError("%s has no graphics\\Menus\\IntroFont.RIM" % menus[0])
    n, o, ln = hits[0]
    if data[o:o + 4] not in (b"LIST", b"FORM") or struct.unpack_from(">I", data, o + 4)[0] + 8 != ln:
        raise ValueError("IntroFont.RIM in %s is not a whole IFF chunk" % menus[0])
    return "ffinfo.txt: graphics\\Menus -> %s, which holds %s (%d B, IFF %s); all %d listed fast-files present" % (
        menus[0], n, ln, data[o:o + 4].decode(), len(entries))


# ----------------------------------------------------------------- publish ---

def _sharewrite(local, dest):
    """Default runner: one sharewrite.py put. Returns its exit code."""
    return subprocess.run([sys.executable, SHAREWRITE, "put", local, dest]).returncode


def run_publish(outdir, share_root=SHARE_MNT, rows=None, dry_run=False, runner=None,
                out=sys.stdout, tmpdir=None):
    """FUTURE USE - not run in the build phase.

    For each file, ffinfo.txt last: (1) read the share copy through /mnt - patched
    already -> skip, not the staged original -> STOP; (2) back the original up to
    BACKUP_REL/<same path> unless an identical backup is there (a DIFFERENT one
    -> STOP, never overwrite a backup); (3) put the patched file. Every write is
    scripts/fleet/sharewrite.py put, which verifies the md5 through /mnt and a
    fresh mtime. The first failure stops the run: a half-published tree still
    fails exactly as today because ffinfo.txt flips last.
    """
    runner = runner or _sharewrite
    rows = load_table() if rows is None else rows
    byrel = dict((r.path, r) for r in rows if r.action == "deobfuscate")
    mpath = os.path.join(outdir, "manifest.json")
    with open(mpath) as f:
        manifest = json.load(f)
    if not manifest.get("complete"):
        print("REFUSED: %s is not a complete build - run --build again" % mpath, file=out)
        return 2
    outs = dict((o["share_path"][len(TITLE_REL) + 1:], o) for o in manifest["outputs"])
    if set(outs) != set(byrel):
        print("REFUSED: the manifest does not cover exactly the table's %d files" % len(byrel), file=out)
        return 2
    root = title_root(share_root)
    broot = local_path(share_root, BACKUP_REL)
    done = skipped = backed = 0
    own_tmp = tmpdir is None
    tmpdir = tmpdir or tempfile.mkdtemp(prefix="avp-publish-")
    try:
        for r in publish_order(list(byrel.values())):
            o = outs[r.path]
            if o["md5"] != r.patched_md5:
                print("STOP %s: manifest md5 %s != table %s" % (r.path, o["md5"], r.patched_md5), file=out)
                return 3
            loc = local_path(outdir, r.path)
            if not os.path.isfile(loc) or md5_file(loc) != r.patched_md5:
                print("STOP %s: the built file is missing or changed since --build" % r.path, file=out)
                return 3
            try:
                with open(local_path(root, r.path), "rb") as f:
                    cur = f.read()
            except OSError as e:
                print("STOP %s: cannot read the share copy (%s)" % (r.path, e.strerror), file=out)
                return 3
            state, why = file_state(r, cur)
            if state == PATCHED:
                skipped += 1
                print("skip %s (already patched on the share)" % r.path, file=out)
                continue
            if state != ORIGINAL:
                print("STOP %s: %s" % (r.path, why), file=out)
                return 3
            bpath = local_path(broot, r.path)
            if os.path.isfile(bpath):
                bm = md5_file(bpath)
                if bm != r.original_md5:
                    print("STOP %s: a DIFFERENT backup is already at %s (md5 %s)" % (r.path, bpath, bm), file=out)
                    return 3
                print("backup present %s" % r.path, file=out)
            else:
                tmp = os.path.join(tmpdir, "orig-" + md5_bytes(r.path.encode()))
                with open(tmp, "wb") as f:
                    f.write(cur)
                if md5_file(tmp) != r.original_md5:
                    print("STOP %s: local copy of the original does not verify" % r.path, file=out)
                    return 3
                dest = BACKUP_REL + "/" + r.path
                if dry_run:
                    print("would back up %s -> %s" % (r.path, dest), file=out)
                else:
                    rc = runner(tmp, dest)
                    if rc != 0:
                        print("STOP %s: backup put failed (sharewrite rc=%d)" % (r.path, rc), file=out)
                        return 3
                    backed += 1
                os.unlink(tmp)
            dest = TITLE_REL + "/" + r.path
            if dry_run:
                print("would put %s -> %s (md5 %s)" % (loc, dest, r.patched_md5), file=out)
                continue
            rc = runner(loc, dest)
            if rc != 0:
                print("STOP %s: put failed (sharewrite rc=%d)" % (r.path, rc), file=out)
                return 3
            done += 1
            print("put %s" % r.path, file=out)
    finally:
        if own_tmp:
            shutil.rmtree(tmpdir, ignore_errors=True)
    print("publish %s: %d put, %d already patched, %d backed up" % (
        "DRY-RUN" if dry_run else "done", done, skipped, backed), file=out)
    if not dry_run:
        print("next: purge C:\\Games\\AliensVsPredator on a test box, GAMESYNC, and look for "
              "'Loaded FastFile:' in LOGFILE.TXT (see README.md)", file=out)
    return 0


# ------------------------------------------------------------ AvP_Video.cfg ---

AVP_VIDEO_CFG_SIZE = 32


def avp_video_cfg(width, height, bpp=32, guid=b"\0" * 16, guid_is_set=0):
    """The 32 bytes avp.exe reads from AvP_Video.cfg (fread 0x20 into 0x85c5f0).

    Layout (avp.exe 0x559810 / 0x55999c): 16-byte DirectDraw device GUID,
    le32 DDGUIDIsSet, le32 width, le32 height, le32 colour depth. The loader
    matches ONLY the 16 GUID bytes: the primary display is enumerated with a
    NULL GUID and its record keeps 16 zero bytes, so all-zero selects it.

    The game accepts only modes its own EnumDisplayModes callback kept
    (bpp > 8, w >= 512, h >= 384, a depth the HAL device renders). A mode it did
    NOT keep is the dangerous case: the device still matches, the mode index
    stays -1, and 0x55999c then reads w/h/bpp from the 12 bytes BEFORE the mode
    table (the tail of DDCAPS). So refuse anything the game could never list.
    """
    if len(guid) != 16:
        raise ValueError("GUID must be 16 bytes")
    if width < 512 or height < 384:
        raise ValueError("avp.exe never lists a mode below 512x384")
    if bpp not in (16, 24, 32):
        raise ValueError("avp.exe lists 16, 24 and 32 bpp modes only")
    return bytes(guid) + struct.pack("<iiii", guid_is_set, width, height, bpp)


def parse_avp_video_cfg(data):
    if len(data) != AVP_VIDEO_CFG_SIZE:
        raise ValueError("AvP_Video.cfg is %d bytes, want 32" % len(data))
    is_set, w, h, bpp = struct.unpack_from("<iiii", data, 16)
    return {"guid": data[:16].hex(), "guid_is_set": is_set, "width": w, "height": h, "bpp": bpp,
            "primary_display": data[:16] == b"\0" * 16}


# --------------------------------------------------------------------- main ---

def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--check", action="store_true", help="read-only: verify every staged input")
    g.add_argument("--build", nargs="?", const=DEFAULT_OUT, metavar="OUTDIR",
                   help="write de-obfuscated copies + manifest.json (default %(const)s)")
    g.add_argument("--publish", action="store_true", help="FUTURE: back up originals and put the build")
    g.add_argument("--install-server", action="store_true", help="not applicable to AvP")
    g.add_argument("--video-cfg", nargs="+", type=int, metavar="N", help="W H [BPP]: write an AvP_Video.cfg")
    ap.add_argument("--from", dest="src", default=DEFAULT_OUT, help="--publish: the build directory")
    ap.add_argument("--dry-run", action="store_true", help="--publish: print the plan, write nothing")
    ap.add_argument("--full-scan", action="store_true", help="--check: also scan every other file in the tree")
    ap.add_argument("--share-root", default=SHARE_MNT, help=argparse.SUPPRESS)
    ap.add_argument("--out", default=None, help="--video-cfg: output file")
    ap.add_argument("-v", "--verbose", action="store_true")
    a = ap.parse_args(argv)

    if a.install_server:
        print("NOT APPLICABLE: Aliens versus Predator has no dedicated server on this fleet "
              "(multiplayer is hosted in-game). Nothing installed.")
        return 0
    if a.video_cfg:
        if len(a.video_cfg) not in (2, 3):
            ap.error("--video-cfg takes W H [BPP]")
        try:
            data = avp_video_cfg(*a.video_cfg)
        except ValueError as e:
            print("REFUSED: %s" % e)
            return 2
        # NOT inside the build directory: --publish ships exactly what manifest.json lists,
        # and this per-box file must never be staged in the library.
        dst = a.out or os.path.join(os.path.dirname(DEFAULT_OUT), "avp-video",
                                    "AvP_Video-%s.cfg" % "x".join(str(v) for v in a.video_cfg))
        os.makedirs(os.path.dirname(os.path.abspath(dst)), exist_ok=True)
        with open(dst, "wb") as f:
            f.write(data)
        print("%s: %s  %s" % (dst, data.hex(), parse_avp_video_cfg(data)))
        return 0
    if not os.path.isdir(title_root(a.share_root)):
        print("FAILED: %s is not there - is the share mounted?" % title_root(a.share_root))
        return 2
    if a.check:
        return 0 if run_check(a.share_root, full_scan=a.full_scan, verbose=a.verbose)["ok"] else 1
    if a.build:
        return 0 if run_build(a.build, a.share_root)["complete"] else 1
    if a.publish:
        return run_publish(a.src, a.share_root, dry_run=a.dry_run)
    return 2


if __name__ == "__main__":
    sys.exit(main())
