import struct, sys, datetime, json
def parse(path):
    b = open(path, 'rb').read()
    recs = {}
    i = 0
    n = len(b)
    while True:
        j = b.find(b'LfLe', i)
        if j < 0: break
        off = j - 4
        i = j + 4
        if off < 0: continue
        L = struct.unpack_from('<I', b, off)[0]
        if L < 0x38 or L > 0x10000 or off + L > n: continue
        # trailing length must match
        if struct.unpack_from('<I', b, off + L - 4)[0] != L: continue
        (rn, tg, tw, eid) = struct.unpack_from('<IIII', b, off + 8)
        (etype, nstr, cat, rflags) = struct.unpack_from('<HHHH', b, off + 24)
        (closing, stroff, sidlen, sidoff, dlen, doff) = struct.unpack_from('<IIIIII', b, off + 32)
        # source + computer names after header (0x38)
        p = off + 0x38
        def wstr(p):
            e = p
            while e + 1 < off + L and b[e:e+2] != b'\x00\x00': e += 2
            return b[p:e].decode('utf-16-le', 'replace'), e + 2
        src, p = wstr(p)
        comp, p = wstr(p)
        strs = []
        q = off + stroff
        for k in range(nstr):
            if q >= off + L: break
            s, q = wstr(q)
            strs.append(s)
        data = b[off+doff: off+doff+dlen] if dlen and doff else b''
        recs[rn] = dict(rn=rn, t=datetime.datetime.utcfromtimestamp(tg).strftime('%Y-%m-%d %H:%M:%S'),
                        tw=datetime.datetime.utcfromtimestamp(tw).strftime('%Y-%m-%d %H:%M:%S'),
                        id=eid & 0xFFFF, idfull=eid, type={1:'ERR',2:'WARN',4:'INFO',8:'AUDOK',16:'AUDFAIL'}.get(etype, etype),
                        src=src, strs=strs, data=data.hex()[:64])
    return [recs[k] for k in sorted(recs)]
if __name__ == '__main__':
    out = parse(sys.argv[1])
    json.dump(out, open(sys.argv[2], 'w'), indent=0)
    print(len(out), 'records', out[0]['rn'] if out else None, '..', out[-1]['rn'] if out else None)
