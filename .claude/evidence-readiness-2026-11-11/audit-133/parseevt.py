import struct, sys, datetime
p = sys.argv[1]
b = open(p,'rb').read()
recs = {}
i = 0
while True:
    i = b.find(b'LfLe', i)
    if i < 0: break
    off = i - 4
    if off < 0:
        i += 4; continue
    L = struct.unpack_from('<I', b, off)[0]
    if L < 0x38 or L > 0x10000 or off + L > len(b):
        i += 4; continue
    tail = struct.unpack_from('<I', b, off + L - 4)[0]
    if tail != L:
        i += 4; continue
    (Length, Res, RecNo, TGen, TWri, EvID, EvType, NStr, Cat, RFlags, Closing, StrOff, SidLen, SidOff, DataLen, DataOff) = struct.unpack_from('<IIIIIIHHHHIIIIII', b, off)
    body = b[off+0x38: off+L]
    # source and computer name
    raw = b[off+0x38: off+StrOff]
    parts = raw.decode('utf-16le', errors='replace').split('\x00')
    src = parts[0] if parts else ''
    comp = parts[1] if len(parts) > 1 else ''
    strs = b[off+StrOff: off+(DataOff if DataLen else L-4)].decode('utf-16le', errors='replace').split('\x00')[:NStr]
    data = b[off+DataOff: off+DataOff+DataLen] if DataLen else b''
    recs[RecNo] = (TGen, EvID & 0xFFFF, EvID, EvType, src, strs, data)
    i = off + L
types = {1:'ERR',2:'WARN',4:'INFO',8:'AUDS',16:'AUDF',0:'INFO0'}
since = int(sys.argv[2]) if len(sys.argv) > 2 else 0
for rn in sorted(recs):
    TGen, eid, full, et, src, strs, data = recs[rn]
    if TGen < since: continue
    t = datetime.datetime.utcfromtimestamp(TGen).strftime('%Y-%m-%d %H:%M:%S')
    s = ' | '.join(x.strip() for x in strs if x.strip())
    if len(s) > 300: s = s[:300] + '...'
    extra = ''
    if src in ('Save Dump','System Error') or eid in (1001, 1003):
        extra = ' DATA=' + data[:64].hex()
    print(f'{rn:6d} {t}Z {types.get(et,et):4} {src:28.28} {eid:5d} {s}{extra}')
