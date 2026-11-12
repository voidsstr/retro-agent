import os, re, struct, sys
L = "/mnt/retro-share/Files/Games-Library"
pair = struct.pack("<ii", 1600, 1200)
pats = [b"1600x1200", b"1600 1200", b"1600 x 1200", b"1600*1200", b"[1600", b"1600X1200"]
for title in ("Quake2Complete", "SiNGold", "SoldierOfFortune", "Quake2Win9x"):
    for root, dirs, files in os.walk(os.path.join(L, title)):
        for f in files:
            if not f.lower().endswith((".exe", ".dll")):
                continue
            p = os.path.join(root, f)
            try:
                d = open(p, "rb").read()
            except OSError as e:
                print("ERR", p, e); continue
            hits = []
            i = d.find(pair)
            while i >= 0:
                hits.append("pair@0x%x" % i); i = d.find(pair, i + 1)
            for pt in pats:
                j = d.lower().find(pt.lower())
                while j >= 0:
                    hits.append("%r@0x%x" % (pt, j)); j = d.lower().find(pt.lower(), j + 1)
            # also count the mode-table-ish string "Mode 9"
            k = d.find(b"Mode 9")
            if k >= 0: hits.append("'Mode 9'@0x%x" % k)
            rel = os.path.relpath(p, L)
            print("%-55s %8d  %s" % (rel, len(d), ", ".join(hits) if hits else "-"))
