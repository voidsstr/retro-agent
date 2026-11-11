import struct, sys, datetime, collections
TYPES = {1:'ERROR',2:'WARN',4:'INFO',8:'AUDIT_OK',16:'AUDIT_FAIL',0:'SUCCESS'}
def wstr(b, off):
    end = off
    while end + 1 < len(b) and (b[end] or b[end+1]):
        end += 2
    return b[off:end].decode('utf-16-le', errors='replace'), end + 2
def parse(path):
    b = open(path, 'rb').read()
    recs = []
    i = 0
    n = len(b)
    while i + 0x38 <= n:
        if b[i+4:i+8] == b'LfLe':
            ln = struct.unpack_from('<I', b, i)[0]
            if 0x38 <= ln <= 0x10000 and i + ln <= n and struct.unpack_from('<I', b, i+ln-4)[0] == ln:
                (ln, sig, recno, tgen, twr, evid, etype, nstr, cat, rflags, closing, stroff, sidlen, sidoff, datalen, dataoff) = struct.unpack_from('<I4sIIIIHHHHIIIIII', b, i)
                src, p = wstr(b, i + 0x38)
                comp, p = wstr(b, p)
                strs = []
                q = i + stroff
                for _ in range(nstr):
                    if q >= i + ln: break
                    s, q = wstr(b, q)
                    strs.append(s)
                data = b[i+dataoff:i+dataoff+datalen] if datalen else b''
                recs.append(dict(recno=recno, t=datetime.datetime.fromtimestamp(tgen, datetime.timezone.utc).strftime('%Y-%m-%d %H:%M:%S'), id=evid & 0xFFFF, type=TYPES.get(etype, str(etype)), src=src, strs=strs, data=data))
                i += ln
                continue
        i += 4
    recs.sort(key=lambda r: r['recno'])
    return recs
if __name__ == '__main__':
    path = sys.argv[1]
    ids = set(int(x) for x in sys.argv[2].split(',')) if len(sys.argv) > 2 and sys.argv[2] else set()
    srcs = set(sys.argv[3].split(',')) if len(sys.argv) > 3 and sys.argv[3] else set()
    recs = parse(path)
    print(f'{len(recs)} records; first {recs[0]["t"]} last {recs[-1]["t"]}')
    if not ids and not srcs:
        c = collections.Counter((r['src'], r['id'], r['type']) for r in recs)
        for (s, e, t), k in c.most_common():
            print(f'{k:5d}  {t:6s} {s} {e}')
    else:
        for rec in recs:
            if rec['id'] in ids or rec['src'] in srcs or ('ALLERR' in srcs and rec['type'] in ('ERROR','WARN')):
                txt = ' | '.join(s.replace('\r', ' ').replace('\n', ' ')[:300] for s in rec['strs'])
                print(rec['t'], '#' + str(rec['recno']), rec['type'], rec['src'], rec['id'], txt)
