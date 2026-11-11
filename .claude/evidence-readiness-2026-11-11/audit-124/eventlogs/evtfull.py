import struct, sys
b = open(sys.argv[1], 'rb').read()
want = set(int(x) for x in sys.argv[2:])
i = 0
while True:
    j = b.find(b'LfLe', i)
    if j < 0: break
    off = j - 4; i = j + 4
    if off < 0: continue
    L = struct.unpack_from('<I', b, off)[0]
    if L < 0x38 or L > 0x10000 or off + L > len(b): continue
    if struct.unpack_from('<I', b, off + L - 4)[0] != L: continue
    rn = struct.unpack_from('<I', b, off + 8)[0]
    if rn not in want: continue
    (closing, stroff, sidlen, sidoff, dlen, doff) = struct.unpack_from('<IIIIII', b, off + 32)
    nstr = struct.unpack_from('<H', b, off + 26)[0]
    strs = b[off+stroff: off+doff if doff > stroff else off+L-4].decode('utf-16-le', 'replace').split('\x00')
    data = b[off+doff: off+doff+dlen]
    print('rn', rn, 'strings', strs[:nstr])
    for k in range(0, len(data), 16):
        print('   %04x  %s' % (k, data[k:k+16].hex(' ')))
