import struct, sys
def dump(path):
    data = open(path,'rb').read()
    pat = struct.pack('<9I', 360,400,40,25,9,16,8,0xB8000,0x800)
    hits = []
    i = data.find(pat)
    while i != -1:
        hits.append(i); i = data.find(pat, i+1)
    for h in hits:
        base = h - 8
        m0, = struct.unpack_from('<H', data, base)
        if m0 != 0: continue
        modes = []
        off = base
        for k in range(300):
            m, = struct.unpack_from('<H', data, off)
            typ, = struct.unpack_from('<I', data, off+4)
            sw, sh = struct.unpack_from('<II', data, off+8)
            if m == 0xFFFF: break
            modes.append((m, typ, sw, sh))
            off += 64
        vesa = [x for x in modes if x[0] >= 0x100]
        print('table @%s: %d entries, %d VESA (>=0x100)' % (hex(base), len(modes), len(vesa)))
        if vesa:
            print('  ' + ', '.join('%03X:%dx%d(t%d)' % (x[0], x[2], x[3], x[1]) for x in vesa))
            big = max(vesa, key=lambda x: x[2]*x[3])
            print('  largest: %03X %dx%d; any 1920x1080: %s; 8bpp-type set: %s' % (big[0], big[2], big[3], any(x[2]==1920 and x[3]==1080 for x in modes), sorted(set(x[1] for x in vesa))))
for p in sys.argv[1:]:
    print('==', p)
    dump(p)
