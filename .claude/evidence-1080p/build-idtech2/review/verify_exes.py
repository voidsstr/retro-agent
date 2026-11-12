import hashlib, struct, sys, os, pefile
L = "/mnt/retro-share/Files/Games-Library"
OUT = os.path.expanduser("~/.retro-fleet/patch-out/idtech2")
def md5(b): return hashlib.md5(b).hexdigest()
specs = [("Quake2Complete","quake2.exe",0x543d0,0x54678),
         ("SiNGold","sin.exe",0x94450,None),
         ("SoldierOfFortune","SoF.exe",0x1c6e08,None)]
for title, rel, tab, lab in specs:
    p = os.path.join(L, title, rel)
    d = open(p,"rb").read()
    print("=====", title, rel, len(d), md5(d))
    pe = pefile.PE(data=d)
    base = pe.OPTIONAL_HEADER.ImageBase
    print(" imagebase 0x%x checksum 0x%x subsys %d.%d timestamp 0x%x" % (base, pe.OPTIONAL_HEADER.CheckSum,
          pe.OPTIONAL_HEADER.MajorSubsystemVersion, pe.OPTIONAL_HEADER.MinorSubsystemVersion, pe.FILE_HEADER.TimeDateStamp))
    ep = pe.OPTIONAL_HEADER.AddressOfEntryPoint
    for s in pe.sections:
        nm = s.Name.rstrip(b"\0").decode("latin1")
        own = " <-EP" if s.VirtualAddress <= ep < s.VirtualAddress + max(s.Misc_VirtualSize, s.SizeOfRawData) else ""
        print("  sec %-8s va 0x%06x vsz 0x%06x raw 0x%06x rsz 0x%06x%s" % (nm, s.VirtualAddress, s.Misc_VirtualSize, s.PointerToRawData, s.SizeOfRawData, own))
    print("  BoG_ marker:", d.find(b"BoG_ *90.0&!!  Yy>"), " stxt:", [s.Name for s in pe.sections if s.Name.startswith(b"stxt")])
    def v2o(va):
        try: return pe.get_offset_from_rva(va - base)
        except Exception: return None
    def cs(o): return d[o:d.index(b"\0", o)].decode("latin1")
    # which section holds the table (writable data?)
    for s in pe.sections:
        if s.PointerToRawData <= tab < s.PointerToRawData + s.SizeOfRawData:
            print("  table in section", s.Name.rstrip(b"\0"), "chars 0x%x" % s.Characteristics)
    for i in range(12):
        va, w, h, m = struct.unpack_from("<IiiI", d, tab + 16*i)
        o = v2o(va)
        txt = cs(o) if (o is not None and 0 <= o < len(d)) else None
        print("  [%2d] @0x%x desc va 0x%08x -> off %s %r w=%d h=%d mode=%d" % (i, tab+16*i, va, hex(o) if o is not None else None, txt, w, h, m))
    if lab:
        for i in range(12):
            (va,) = struct.unpack_from("<I", d, lab + 4*i)
            o = v2o(va) if va else None
            print("  label[%2d] @0x%x va 0x%08x -> %s %r" % (i, lab+4*i, va, hex(o) if o else None, cs(o) if o else None))
    # out file diff
    q = os.path.join(OUT, title, rel)
    o = open(q,"rb").read()
    print(" output", len(o), md5(o))
    diffs = [i for i in range(len(d)) if d[i] != o[i]]
    print("  differing byte offsets:", [hex(x) for x in diffs])
