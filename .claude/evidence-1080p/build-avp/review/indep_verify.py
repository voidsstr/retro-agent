#!/usr/bin/env python3
"""Independent re-verification of the avp builder's work.

Oracle 1: libunshield.so.1's own exported unshield_deobfuscate() (unshield 1.6.2),
          called through ctypes - NOT the builder's Python transform.
Oracle 2: the NakedAVP README md5 list fetched fresh from GitHub by this review.
Reads every staged original from /mnt (read-only), every builder output from
~/.retro-fleet/patch-out/avp, and the builder's manifest.json.
"""
import ctypes, hashlib, json, os, re, sys

LIB = ctypes.CDLL("/usr/lib/x86_64-linux-gnu/libunshield.so.1")
LIB.unshield_deobfuscate.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_uint)]
LIB.unshield_deobfuscate.restype = None

ROOT = "/mnt/retro-share/Files/Games-Library/AliensVsPredator"
OUT = os.path.expanduser("~/.retro-fleet/patch-out/avp")
TSV = "/home/voidsstr/development/retro-agent/.claude/worktrees/lcd1080-patches/provisioning/patches/avp/files.tsv"
UPSTREAM = os.path.join(os.path.dirname(os.path.abspath(__file__)), "nakedavp_master_README.md")

def md5(b): return hashlib.md5(b).hexdigest()

def unshield(data):
    buf = ctypes.create_string_buffer(bytes(data), len(data))
    seed = ctypes.c_uint(0)
    LIB.unshield_deobfuscate(buf, len(data), ctypes.byref(seed))
    assert seed.value == len(data) & 0xffffffff
    return buf.raw[:len(data)]

ref = {}
for line in open(UPSTREAM, encoding="utf-8"):
    m = re.match(r"MD5 \((.+)\) = ([0-9a-f]{32})\s*$", line.strip())
    if m: ref[m.group(1).lower()] = m.group(2)
assert len(ref) == 201, len(ref)

rows = []
for line in open(TSV, encoding="utf-8"):
    line = line.rstrip("\n")
    if not line or line.startswith("#") or line.startswith("action\t"): continue
    a, p, size, om, pm, rm, ver, head = line.split("\t")
    rows.append(dict(action=a, path=p, size=int(size), om=om, pm=pm, rm=rm, verify=ver, head=head))

man = json.load(open(os.path.join(OUT, "manifest.json")))
mo = dict((o["share_path"].split("AliensVsPredator/", 1)[1], o) for o in man["outputs"])

bad = []
n_deob = n_keep = n_ref_deob = n_ref_keep = 0
covered = set()
for r in rows:
    src = os.path.join(ROOT, *r["path"].split("/"))
    data = open(src, "rb").read()
    if len(data) != r["size"] or md5(data) != r["om"]:
        bad.append("ORIGINAL %s: size %d md5 %s vs table %d %s" % (r["path"], len(data), md5(data), r["size"], r["om"]))
    if data[:16].hex() != r["head"]:
        bad.append("HEAD %s" % r["path"])
    low = r["path"].lower()
    if r["action"] == "keep":
        n_keep += 1
        if ref.get(low) != md5(data): bad.append("KEEP %s != upstream" % r["path"])
        else: n_ref_keep += 1; covered.add(low)
        continue
    n_deob += 1
    plain = unshield(data)
    pm = md5(plain)
    if pm != r["pm"]: bad.append("DEOB %s libunshield %s != table %s" % (r["path"], pm, r["pm"]))
    outp = os.path.join(OUT, *r["path"].split("/"))
    ob = open(outp, "rb").read()
    if ob != plain: bad.append("OUTPUT %s differs from libunshield(original)" % r["path"])
    if len(ob) != len(data): bad.append("SIZE %s" % r["path"])
    o = mo.get(r["path"])
    if not o or o["md5"] != pm or o["size"] != len(plain) or o["original_md5"] != r["om"]:
        bad.append("MANIFEST %s" % r["path"])
    if low in ref:
        if ref[low] != pm: bad.append("UPSTREAM %s %s != %s" % (r["path"], pm, ref[low]))
        else: n_ref_deob += 1; covered.add(low)
    elif r["rm"] != "-":
        bad.append("REF-COLUMN %s" % r["path"])
print("rows", len(rows), "deob", n_deob, "keep", n_keep)
print("deob == upstream NakedAVP:", n_ref_deob, " keep == upstream:", n_ref_keep, " upstream covered:", len(covered), "/", len(ref))
print("manifest outputs:", len(mo), "complete:", man.get("complete"), "failures:", man.get("failures"))
print("outputs dir files:", sum(len(f) for _, _, f in os.walk(OUT)) - 1, "(excluding manifest.json)")
print("uncovered upstream:", sorted(set(ref) - covered)[:10])
print("PROBLEMS:", len(bad))
for b in bad[:50]: print("  ", b)
sys.exit(1 if bad else 0)
