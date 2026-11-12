#!/usr/bin/env python3
"""Independent whole-tree scan: is any file OUTSIDE the table still obfuscated?

Different discriminator from the builder's magic-only scan:
  * longest run of one repeated byte (plain binary data has long runs, its
    obfuscated form has none; de-obfuscating a PLAIN file turns runs into the
    descending 0x75,0x74,... ramp, so runs shrink),
  * printable-ASCII fraction (text),
  * a broad magic table, raw vs de-obfuscated (libunshield).
A file is suspicious if de-obfuscation makes it MORE structured than it is raw.
"""
import ctypes, os, sys, collections
LIB = ctypes.CDLL("/usr/lib/x86_64-linux-gnu/libunshield.so.1")
LIB.unshield_deobfuscate.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_uint)]
ROOT = "/mnt/retro-share/Files/Games-Library/AliensVsPredator"
TSV = "/home/voidsstr/development/retro-agent/.claude/worktrees/lcd1080-patches/provisioning/patches/avp/files.tsv"
N = 65536
def deob(b):
    buf = ctypes.create_string_buffer(b, len(b)); s = ctypes.c_uint(0)
    LIB.unshield_deobfuscate(buf, len(b), ctypes.byref(s)); return buf.raw[:len(b)]
def longest_run(b):
    best = cur = 0; prev = None
    for x in b:
        cur = cur + 1 if x == prev else 1; prev = x
        if cur > best: best = cur
    return best
def printable(b):
    return sum(1 for x in b if 32 <= x < 127 or x in (9, 10, 13)) / max(1, len(b))
MAGICS = [b"RFFL", b"REBCRIF1", b"REBINFF2", b"MARSOUND", b"BIK", b"SMK2", b"SMK4", b"MZ", b"RIFF", b"\x00\x00\x01\x00",
          b"GIF8", b"BM", b"\xff\xd8\xff", b"8BPS", b"REGEDIT", b"PK\x03\x04", b"\x89PNG", b"ID3", b"\xff\xfb", b"OggS",
          b"MThd", b"FORM", b"LIST", b"{", b"[", b"<", b"\xef\xbb\xbf", b"\xff\xfe", b"MSCF", b"ISc(", b"\x13]e\x8c"]
def magic(b):
    for m in MAGICS:
        if b.startswith(m): return m
    return None
known = set()
for line in open(TSV, encoding="utf-8"):
    if line.startswith("#") or line.startswith("action\t") or not line.strip(): continue
    known.add(line.split("\t")[1].lower())
stats = collections.Counter(); sus = []
allfiles = []
for dp, dn, fn in os.walk(ROOT):
    for n in fn:
        full = os.path.join(dp, n); rel = os.path.relpath(full, ROOT).replace(os.sep, "/")
        allfiles.append(rel)
        if rel.lower() not in known: continue
        with open(full, "rb") as f: raw = f.read(N)
        d = deob(raw)
        rr, dr = longest_run(raw), longest_run(d)
        rp, dp_ = printable(raw), printable(d)
        rm, dm = magic(raw), magic(d)
        stats["scanned"] += 1
        flag = []
        if dm and not rm: flag.append("deob has magic %r, raw has none" % dm)
        if dr >= 16 and dr > 4 * max(rr, 1): flag.append("deob run %d vs raw %d" % (dr, rr))
        if dp_ > 0.95 and rp < 0.80: flag.append("deob printable %.2f vs raw %.2f" % (dp_, rp))
        if flag: sus.append((rel, len(raw), flag))
print("files in tree:", len(allfiles), dict(stats))
print("suspicious:", len(sus))
for s in sus: print("  ", s)
