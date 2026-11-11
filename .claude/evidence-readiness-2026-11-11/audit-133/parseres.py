import json, struct, sys
d = json.load(open(sys.argv[1]))
TYPES = {1:'Port',2:'Int',3:'Mem',4:'Dma',5:'DevSpec',6:'BusNum',0x81:'Priv',0x80:'Class',0:'Null'}
for v in d['values']:
    name = v['name']
    if not name.endswith('.Translated'): continue
    try:
        b = bytes(int(x,16) for x in v['data'].split())
    except Exception:
        continue
    o = 0
    cnt = struct.unpack_from('<I', b, o)[0]; o += 4
    out = []
    for _ in range(cnt):
        itype, bus = struct.unpack_from('<iI', b, o); o += 8
        ver, rev, pc = struct.unpack_from('<HHI', b, o); o += 8
        for _ in range(pc):
            t, share, flags = struct.unpack_from('<BBH', b, o)
            u = b[o+4:o+16]
            o += 16
            if t == 2:
                lvl, vec, aff = struct.unpack('<III', u)
                out.append(f'INT irql={lvl} vec=0x{vec:X} aff=0x{aff:X} share={share} flags={flags}')
            elif t == 3:
                start, length = struct.unpack('<QI', u)
                out.append(f'MEM 0x{start:X}+0x{length:X}')
            elif t == 1:
                start, length = struct.unpack('<QI', u)
                out.append(f'IO 0x{start:X}+0x{length:X}')
            elif t == 4:
                ch, port, res = struct.unpack('<III', u)
                out.append(f'DMA {ch}')
            elif t == 5:
                ds, r1, r2 = struct.unpack('<III', u)
                o += ds  # device specific data follows
                out.append(f'DEVSPEC {ds}')
    ints = [x for x in out if x.startswith('INT')]
    print(name, '|', '; '.join(ints) if ints else '-', '|', '; '.join(x for x in out if not x.startswith('INT'))[:200])
