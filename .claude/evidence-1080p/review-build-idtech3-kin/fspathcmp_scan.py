"""Find FS_PathCmp-shaped comparators in a PE: a short function that maps
'a'..'z' by +/-0x20 and maps '\\' and ':' to '/'. Reports whether it UPPERCASES
(sub 0x20 / add -0x20) or LOWERCASES (add 0x20)."""
import struct, sys
from capstone import Cs, CS_ARCH_X86, CS_MODE_32
def scan(path):
    d=open(path,'rb').read()
    e=struct.unpack_from('<I',d,0x3c)[0]; nsec=struct.unpack_from('<H',d,e+6)[0]; optsz=struct.unpack_from('<H',d,e+20)[0]
    opt=e+24; ib=struct.unpack_from('<I',d,opt+28)[0]; sh=opt+optsz
    out=[]
    for i in range(nsec):
        o=sh+i*40; name=d[o:o+8].rstrip(b'\0'); vsz,va,rsz,rp=struct.unpack_from('<IIII',d,o+8)
        if name!=b'.text': continue
        code=d[rp:rp+rsz]; md=Cs(CS_ARCH_X86,CS_MODE_32)
        ins=list(md.disasm(code, ib+va))
        for k,x in enumerate(ins):
            # anchor: compare with 0x5c ('\\')
            if x.mnemonic=='cmp' and x.op_str.endswith(', 0x5c'):
                win=ins[max(0,k-40):k+40]
                txt=' | '.join('%s %s'%(w.mnemonic,w.op_str) for w in win)
                if ', 0x3a' in txt and '0x2f' in txt and ('0x61' in txt or '0x7a' in txt or '0x60' in txt):
                    up=any((w.mnemonic=='sub' and w.op_str.endswith(', 0x20')) or (w.mnemonic=='add' and w.op_str.endswith(', -0x20')) or (w.mnemonic=='add' and w.op_str.endswith(', 0xe0')) for w in win)
                    lo=any(w.mnemonic in ('add','or') and w.op_str.endswith(', 0x20') for w in win)
                    out.append((hex(x.address),'UPPER' if up else ('lower' if lo else '?')))
    return out
for p in sys.argv[1:]:
    r=scan(p)
    print(p.split('/')[-1], r[:6], '(%d candidate sites)'%len(r))
