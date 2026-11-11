import struct, sys, datetime, collections
b = open(sys.argv[1], 'rb').read()
i = 0
rows = []
while True:
    j = b.find(b'LfLe', i)
    if j < 0: break
    off = j - 4; i = j + 4
    if off < 0: continue
    L = struct.unpack_from('<I', b, off)[0]
    if L < 0x38 or L > 0x10000 or off + L > len(b): continue
    if struct.unpack_from('<I', b, off + L - 4)[0] != L: continue
    rn, tg = struct.unpack_from('<II', b, off + 8)
    (closing, stroff, sidlen, sidoff, dlen, doff) = struct.unpack_from('<IIIIII', b, off + 32)
    txt = b[off+stroff: off+doff].decode('utf-16-le', 'replace')
    if 'Machine Check:' not in txt or 'Banks' not in txt: continue
    d = b[off+doff: off+doff+dlen]
    if len(d) < 0x60: continue
    ver, typ = struct.unpack_from('<II', d, 0x28)
    bank = d[0x40]
    st, addr, misc = struct.unpack_from('<QQQ', d, 0x48)
    val, over, uc, en, miscv, addrv, pcc = [(st >> k) & 1 for k in (63, 62, 61, 60, 59, 58, 57)]
    code = st & 0xFFFF
    pop = bin(st).count('1')
    rows.append((rn, datetime.datetime.utcfromtimestamp(tg).strftime('%Y-%m-%d %H:%M'), bank, st, addr, code, pop, f'V{val}O{over}U{uc}E{en}A{addrv}P{pcc}'))
for r in rows:
    print('%5d %s bank%d status=%016x addr=%016x code=%04x popcnt=%2d %s' % r)
c = collections.Counter((r[2], r[3]) for r in rows)
print('repeated (bank,status):', [(k[0], hex(k[1]), n) for k, n in c.items() if n > 1])
