import struct, sys, hashlib
def ver(path):
    b = open(path, 'rb').read()
    i = b.find(struct.pack('<I', 0xFEEF04BD))
    if i < 0:
        return None
    sig, sv, fvms, fvls, pvms, pvls = struct.unpack_from('<IIIIII', b, i)
    fv = f'{fvms>>16}.{fvms&0xffff}.{fvls>>16}.{fvls&0xffff}'
    pv = f'{pvms>>16}.{pvms&0xffff}.{pvls>>16}.{pvls&0xffff}'
    return fv, pv, len(b), hashlib.md5(b).hexdigest()
for p in sys.argv[1:]:
    print(p.rsplit('/',1)[-1], ver(p))
