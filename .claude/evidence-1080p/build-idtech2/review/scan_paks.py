import os, struct, hashlib, re
L = "/mnt/retro-share/Files/Games-Library"
FMT = {b"PACK": 64, b"SPAK": 128}
def pakdir(p):
    with open(p, "rb") as f:
        h = f.read(12)
        if h[:4] not in FMT: return None, None
        esz = FMT[h[:4]]; dofs, dlen = struct.unpack("<ii", h[4:12])
        f.seek(dofs); d = f.read(dlen)
    out = []
    for i in range(dlen // esz):
        e = d[i*esz:(i+1)*esz]
        nm = e[:esz-8].split(b"\0")[0].decode("latin1")
        pos, ln = struct.unpack("<ii", e[esz-8:])
        out.append((nm, pos, ln))
    return h[:4], out
def rd(p, pos, ln):
    with open(p, "rb") as f:
        f.seek(pos); return f.read(ln)
for title, dirs in (("SiNGold", ["base", "2015", "ctf"]), ("SoldierOfFortune", ["base", "user"]), ("Quake2Complete", ["baseq2","xatrix","rogue","ctf"])):
    for gd in dirs:
        gp = os.path.join(L, title, gd)
        if not os.path.isdir(gp):
            print("--", title, gd, "absent"); continue
        names = sorted(os.listdir(gp), key=str.lower)
        paks = [n for n in names if re.match(r"(?i)pak\d+\.(pak|sin)$", n)]
        print("==", title, gd, "paks:", paks, " loose menus dir:", [n for n in names if n.lower()=="menus"])
        for n in paks:
            p = os.path.join(gp, n)
            magic, ents = pakdir(p)
            if ents is None:
                print("   ", n, "not a pak"); continue
            menu = [e for e in ents if e[0].lower().startswith("menus/")]
            print("   %s %s size %d entries %d, menus/* %d" % (n, magic, os.path.getsize(p), len(ents), len(menu)))
            for nm, pos, ln in menu:
                b = rd(p, pos, ln)
                if b"1600" in b or nm.lower().endswith(("main.mnu", "m_video.rmf")):
                    lines = [l for l in b.split(b"\n") if b"1600" in l]
                    print("      %-28s %6d md5 %s  1600-lines: %s" % (nm, ln, hashlib.md5(b).hexdigest(), [l.strip()[:110] for l in lines]))
        # loose menus
        md = [n for n in names if n.lower()=="menus"]
        for m in md:
            for f in os.listdir(os.path.join(gp, m)):
                b = open(os.path.join(gp, m, f), "rb").read()
                if b"1600" in b:
                    print("   LOOSE", m, f, len(b))
