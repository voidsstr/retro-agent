"""Extract SETUP\\FFC11.DLL (LBA 78983, 120012 B) from sof.bin inside the retail zip and decode it the way
SoF.exe 0x2006bcb0 does: dword offset at 0, dword length L at offset (4<=L<=512, L%4==0), L bytes; bytes
4..L-1 XOR key[i % 400] where key = SoF.exe VA 0x201a8b28."""
import sys, zipfile, struct, hashlib
zpath, exe, outdir = sys.argv[1:4]
LBA, SIZE, SECT = 78983, 120012, 2352
z = zipfile.ZipFile(zpath)
f = z.open('sof.bin')
start = LBA * SECT
nsect = (SIZE + 2047) // 2048
remaining = start
while remaining > 0:
    chunk = f.read(min(remaining, 16 * 1024 * 1024))
    if not chunk:
        raise SystemExit('short read')
    remaining -= len(chunk)
raw = f.read(nsect * SECT)
data = b''.join(raw[i * SECT + 16: i * SECT + 16 + 2048] for i in range(nsect))[:SIZE]
open(outdir + '/FFC11.DLL.bin', 'wb').write(data)
print('FFC11.DLL', len(data), 'md5', hashlib.md5(data).hexdigest(), 'head', data[:16].hex())
off = struct.unpack('<I', data[0:4])[0]
L = struct.unpack('<I', data[off:off + 4])[0] if off + 4 <= len(data) else None
print('offset', off, 'length L', L)
blk = bytearray(data[off + 4: off + 4 + L])
# key
e = open(exe, 'rb').read()
# .rdata VA 0x2017f000 raw 0x17f000 ; .data VA 0x201a5000? compute via pefile-free scan of section table
import re
pe = struct.unpack('<I', e[0x3c:0x40])[0]
nsec = struct.unpack('<H', e[pe + 6:pe + 8])[0]
opt = struct.unpack('<H', e[pe + 20:pe + 22])[0]
st = pe + 24 + opt
base = struct.unpack('<I', e[pe + 24 + 28: pe + 24 + 32])[0]
secs = []
for i in range(nsec):
    s = e[st + 40 * i: st + 40 * (i + 1)]
    name = s[:8].rstrip(b'\0').decode(); vs, va, rs, ro = struct.unpack('<IIII', s[8:24])
    secs.append((name, base + va, rs, ro))
def va2off(v):
    for n, va, rs, ro in secs:
        if va <= v < va + rs:
            return ro + v - va
ko = va2off(0x201a8b28)
key = e[ko:ko + 400]
print('key[:16]', key[:16].hex())
n = L - 4
for i in range(n):
    blk[i] ^= key[i % 400]
dw = [struct.unpack('<I', blk[i:i + 4])[0] for i in range(0, len(blk) - 3, 4)]
print('count (L/4) =', L // 4, ' decoded dwords after the first:', dw)
for v in dw:
    print('   %d ms = %d:%06.3f' % (v, v // 60000, (v % 60000) / 1000))
