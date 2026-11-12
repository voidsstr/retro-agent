import os, struct, hashlib, difflib
L = "/mnt/retro-share/Files/Games-Library"
OUT = os.path.expanduser("~/.retro-fleet/patch-out/idtech2")
FMT = {b"PACK": 64, b"SPAK": 128}
def entry(p, name):
    with open(p, "rb") as f:
        h = f.read(12); esz = FMT[h[:4]]; dofs, dlen = struct.unpack("<ii", h[4:12])
        f.seek(dofs); d = f.read(dlen)
        for i in range(dlen // esz):
            e = d[i*esz:(i+1)*esz]
            nm = e[:esz-8].split(b"\0")[0].decode("latin1")
            pos, ln = struct.unpack("<ii", e[esz-8:])
            if nm.lower() == name.lower():
                f.seek(pos); return f.read(ln), nm
    raise SystemExit("no " + name)
def md5(b): return hashlib.md5(b).hexdigest()
for src, out in (("SiNGold/base/pak5.sin", "SiNGold/base/menus/main.mnu"),
                 ("SiNGold/2015/pak0.sin", "SiNGold/2015/menus/main.mnu")):
    orig, nm = entry(os.path.join(L, src), "menus/main.mnu")
    new = open(os.path.join(OUT, out), "rb").read()
    print("=====", out, "src", src, nm, len(orig), md5(orig), "->", len(new), md5(new))
    ol = orig.split(b"\n"); nl = new.split(b"\n")
    for d in difflib.unified_diff([x.decode("latin1") for x in ol], [x.decode("latin1") for x in nl], lineterm="", n=0):
        print("   ", repr(d))
    # show the full con_vidmode block of the NEW file
    s = new.find(b"listitem con_vidmode"); e = new.find(b"enditem", s)
    print("   block:\n" + "\n".join("      " + repr(x) for x in new[s:e+7].split(b"\n")))
# SoF pak2
p2 = open(os.path.join(OUT, "SoldierOfFortune/base/pak2.pak"), "rb").read()
h = p2[:12]; dofs, dlen = struct.unpack("<ii", h[4:12])
print("===== pak2.pak", len(p2), md5(p2), h[:4], dofs, dlen)
for i in range(dlen // 64):
    e = p2[dofs+i*64: dofs+(i+1)*64]
    nm = e[:56]; pos, ln = struct.unpack("<ii", e[56:])
    print("   entry raw name bytes", nm.rstrip(b"\0"), "pad-all-zero:", set(nm[len(nm.rstrip(b'\0')):]) <= {0}, pos, ln)
    inner = p2[pos:pos+ln]
orig, nm = entry(os.path.join(L, "SoldierOfFortune/base/pak0.pak"), "menus/m_video.rmf")
print("   pak0 entry name", repr(nm), len(orig), md5(orig), " inner", len(inner), md5(inner))
for d in difflib.unified_diff(orig.decode("latin1").split("\n"), inner.decode("latin1").split("\n"), lineterm="", n=0):
    print("   ", repr(d))
print("   trailing bytes after dir:", len(p2) - (dofs + dlen))
