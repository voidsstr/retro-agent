#!/usr/bin/env python3
"""grep a PE's disassembly: insgrep.py <pe> <regex on 'mnemonic op_str'> [ctx_before] [ctx_after]"""
import sys, re, pefile, capstone
pe = pefile.PE(sys.argv[1], fast_load=True)
base = pe.OPTIONAL_HEADER.ImageBase
img = pe.get_memory_mapped_image()
rx = re.compile(sys.argv[2])
cb = int(sys.argv[3]) if len(sys.argv) > 3 else 6
ca = int(sys.argv[4]) if len(sys.argv) > 4 else 6
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
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
        if rx.search('%s %s' % (i.mnemonic, i.op_str)):
            print('---- 0x%x' % i.address)
            for y in ins[max(0, k - cb):k + ca + 1]:
                print('%s 0x%08x  %-7s %s' % ('>>' if y.address == i.address else '  ', y.address, y.mnemonic, y.op_str))
