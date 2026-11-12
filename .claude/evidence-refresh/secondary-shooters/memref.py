#!/usr/bin/env python3
"""Find instructions whose operand contains a given displacement, e.g. '+ 0x18f94]'.
usage: memref.py <pe> <hexdisp> [--before N] [--after N]"""
import sys, re, pefile, capstone
path, disp = sys.argv[1], int(sys.argv[2], 16)
before = int(sys.argv[sys.argv.index('--before')+1]) if '--before' in sys.argv else 8
after = int(sys.argv[sys.argv.index('--after')+1]) if '--after' in sys.argv else 8
pe = pefile.PE(path); base = pe.OPTIONAL_HEADER.ImageBase
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
for s in pe.sections:
    if not (s.Characteristics & 0x20000000): continue
    code = s.get_data(); va = base + s.VirtualAddress
    insns = []; off = 0
    while off < len(code):
        last = off; got = False
        for ins in md.disasm(code[off:], va + off):
            insns.append(ins); got = True; last = ins.address + ins.size - va
        off = last + 1 if got else off + 1
    pat = re.compile(r'\+ 0x%x\]' % disp)
    for i, ins in enumerate(insns):
        if pat.search(ins.op_str):
            print('---- 0x%x' % ins.address)
            for p in insns[max(0, i-before): i+1+after]:
                print('   0x%08x  %-6s %s' % (p.address, p.mnemonic, p.op_str))
