#!/usr/bin/env python3
"""Find code that references a string (ASCII or UTF-16LE) in a 32-bit PE.
usage: strxref.py <pe> <string> [--before N] [--after N]"""
import sys, re, pefile, capstone
path, needle = sys.argv[1], sys.argv[2]
before = int(sys.argv[sys.argv.index('--before')+1]) if '--before' in sys.argv else 8
after = int(sys.argv[sys.argv.index('--after')+1]) if '--after' in sys.argv else 8
pe = pefile.PE(path); base = pe.OPTIONAL_HEADER.ImageBase
targets = []
for s in pe.sections:
    d = s.get_data()
    for enc in (needle.encode('ascii') + b'\0', needle.encode('utf-16le') + b'\0\0'):
        for m in re.finditer(re.escape(enc), d):
            targets.append(base + s.VirtualAddress + m.start())
print('string VAs:', [hex(t) for t in targets])
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
    for i, ins in enumerate(insns):
        for t in targets:
            if re.search(r'(?<![0-9a-fx])0x%x(?![0-9a-f])' % t, ins.op_str):
                print('---- ref to 0x%x at 0x%x' % (t, ins.address))
                for p in insns[max(0, i-before): i+1+after]:
                    print('   0x%08x  %-6s %s' % (p.address, p.mnemonic, p.op_str))
