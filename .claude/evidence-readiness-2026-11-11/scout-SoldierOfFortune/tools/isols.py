"""List the ISO9660 tree of a MODE1/2352 .bin streamed from inside a zip (reads only the first N MB)."""
import sys, zipfile, struct
zpath, member, mb = sys.argv[1], sys.argv[2], int(sys.argv[3])
z = zipfile.ZipFile(zpath)
f = z.open(member)
raw = f.read(mb * 1024 * 1024)
print('cue:', z.read('sof.cue').decode('latin1'))
SECT = 2352
def sector(n):
    o = n * SECT
    if o + SECT > len(raw):
        return None
    s = raw[o:o + SECT]
    mode = s[15]
    return s[16:16 + 2048] if mode == 1 else s[24:24 + 2048]
pvd = sector(16)
print('PVD id', pvd[1:6], 'volume label', pvd[40:72].decode('latin1').strip())
root = pvd[156:156 + 34]
def walk(lba, size, path, depth=0):
    n = (size + 2047) // 2048
    data = b''
    for i in range(n):
        s = sector(lba + i)
        if s is None:
            print('  (beyond read window)', path); return
        data += s
    p = 0
    while p < len(data):
        ln = data[p]
        if ln == 0:
            p = ((p // 2048) + 1) * 2048
            continue
        rec = data[p:p + ln]
        elba = struct.unpack('<I', rec[2:6])[0]
        esz = struct.unpack('<I', rec[10:14])[0]
        flags = rec[25]
        nl = rec[32]
        name = rec[33:33 + nl]
        p += ln
        if name in (b'\x00', b'\x01'):
            continue
        nm = name.decode('latin1').split(';')[0]
        full = path + '/' + nm
        if flags & 2:
            print(f'{"  "*depth}[{nm}]  lba {elba}')
            walk(elba, esz, full, depth + 1)
        else:
            print(f'{"  "*depth}{nm}  {esz}  lba {elba}')
walk(struct.unpack('<I', root[2:6])[0], struct.unpack('<I', root[10:14])[0], '')
