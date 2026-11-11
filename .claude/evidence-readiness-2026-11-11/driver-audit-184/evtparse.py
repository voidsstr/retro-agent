import struct, sys, datetime, collections
def utf16z(b, off):
    end = off
    while end + 1 < len(b) and b[end:end+2] != b'\x00\x00':
        end += 2
    return b[off:end].decode('utf-16-le', errors='replace'), end + 2
def parse(path):
    b = open(path, 'rb').read()
    hdr = struct.unpack_from('<12I', b, 0)
    recs = []
    i = 0x30
    seen = set()
    while i < len(b) - 0x38:
        if b[i+4:i+8] == b'LfLe':
            L = struct.unpack_from('<I', b, i)[0]
            if 0x38 <= L <= 0x10000 and i + L <= len(b):
                (length, sig, recno, tgen, twr, evid, etype, nstr, cat, rflags, closing,
                 stroff, sidlen, sidoff, datalen, dataoff) = struct.unpack_from('<IIIIIIHHHHIIIIII', b, i)
                tail = struct.unpack_from('<I', b, i + L - 4)[0]
                if tail == L and recno not in seen:
                    seen.add(recno)
                    src, p = utf16z(b, i + 0x38)
                    comp, p = utf16z(b, p)
                    strs = []
                    q = i + stroff
                    for _ in range(min(nstr, 20)):
                        if q >= i + L: break
                        s, q = utf16z(b, q)
                        strs.append(s)
                    data = b[i+dataoff:i+dataoff+datalen] if datalen and dataoff + datalen <= L else b''
                    recs.append(dict(rec=recno, t=datetime.datetime.utcfromtimestamp(tgen), id=evid & 0xFFFF,
                                     full=evid, type=etype, src=src, strs=strs, data=data))
                    i += L
                    continue
        i += 4
    recs.sort(key=lambda r: r['rec'])
    return hdr, recs
TYPES = {1:'ERR', 2:'WARN', 4:'INFO', 8:'AUDOK', 16:'AUDFAIL', 0:'SUCC'}
if __name__ == '__main__':
    hdr, recs = parse(sys.argv[1])
    print('header', hdr)
    print('records', len(recs), 'first', recs[0]['t'] if recs else None, 'last', recs[-1]['t'] if recs else None)
    mode = sys.argv[2] if len(sys.argv) > 2 else 'errwarn'
    c = collections.Counter((TYPES.get(r['type'], r['type']), r['src'], r['id']) for r in recs)
    for k, v in sorted(c.items(), key=lambda kv: (-kv[1])):
        print(f'{v:5}  {k}')
    print('---- detail (errors/warnings + key ids)')
    KEY = {6008, 6005, 6006, 6009, 1001, 1003, 1000, 1002, 1004}
    for r in recs:
        if r['type'] in (1, 2) or r['id'] in KEY:
            print(r['t'].strftime('%Y-%m-%d %H:%M:%S'), 'UTC', TYPES.get(r['type'], r['type']), r['src'], r['id'], '|', ' || '.join(s.replace('\r\n',' ')[:160] for s in r['strs'])[:400])
