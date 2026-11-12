#!/usr/bin/env python3
"""Find writes to [reg+OFF] in a PE's code, optionally only where another
offset is also written within +-N instructions (a struct-fill signature).
usage: memwrites.py <pe> <off> [--near OFF2] [--win N] [--ctx N]"""
import sys, re, pefile, capstone
pe = pefile.PE(sys.argv[1], fast_load=True)
base = pe.OPTIONAL_HEADER.ImageBase
img = pe.get_memory_mapped_image()
off = int(sys.argv[2], 0)
near = None; win = 40; ctx = 6
a = sys.argv[3:]
while a:
    k = a.pop(0)
    if k == '--near': near = int(a.pop(0), 0)
    elif k == '--win': win = int(a.pop(0))
    elif k == '--ctx': ctx = int(a.pop(0))
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
pat = re.compile(r'^(dword ptr )?\[e[a-z]{2} \+ 0x%x\],' % off)
patn = re.compile(r'^(dword ptr )?\[e[a-z]{2} \+ 0x%x\],' % near) if near is not None else None
for s in pe.sections:
    if not (s.Characteristics & 0x20000000): continue
    st = s.VirtualAddress; code = img[st:st + s.Misc_VirtualSize]
    ins = []; o = 0
    while o < len(code):
        got = False
        for i in md.disasm(code[o:], base + st + o):
            ins.append(i); got = True; last = i.address + i.size - (base + st)
        o = last if got and last > o else o + 1
    for k, i in enumerate(ins):
        if i.mnemonic == 'mov' and pat.match(i.op_str):
            if patn:
                w = ins[max(0, k - win):k + win]
                if not any(x.mnemonic == 'mov' and patn.match(x.op_str) for x in w):
                    continue
            print('---- 0x%x' % i.address)
            for y in ins[max(0, k - ctx):k + ctx + 1]:
                print('%s 0x%08x  %-7s %s' % ('>>' if y.address == i.address else '  ', y.address, y.mnemonic, y.op_str))
