import sys
def ror8(x, n): return ((x >> n) | (x << (8 - n))) & 0xff
def deobf(b, seed=0):
    return bytes((ror8(c ^ 0xd5, 2) - ((seed + i) % 0x47)) & 0xff for i, c in enumerate(b))
for p in sys.argv[1:]:
    raw = open(p, 'rb').read()
    d = deobf(raw[:8192])
    pr = sum(1 for x in d[:3000] if 32 <= x < 127 or x in (9, 10, 13)) / min(3000, len(d))
    print(f'==== {p}: first4={d[:4]!r} printable={pr:.2f}')
    print(d[:700].decode('latin-1'))
