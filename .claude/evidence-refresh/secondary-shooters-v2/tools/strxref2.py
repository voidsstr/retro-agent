#!/usr/bin/env python3
"""Find code references (push imm32 / mov reg,imm32 / any imm or disp) to the VA of a string.
usage: strxref.py <pe> <string> [context_before] [context_after]"""
import sys, pefile, capstone
pe = pefile.PE(sys.argv[1], fast_load=True)
s = sys.argv[2].encode()
cb = int(sys.argv[3]) if len(sys.argv) > 3 else 12
ca = int(sys.argv[4]) if len(sys.argv) > 4 else 6
base = pe.OPTIONAL_HEADER.ImageBase
img = pe.get_memory_mapped_image()
vas = []
i = img.find(s)
while i != -1:
    vas.append(base + i); i = img.find(s, i + 1)
print('string VAs:', [hex(v) for v in vas])
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
md.detail = True
for sec in pe.sections:
    if not (sec.Characteristics & 0x20000000): continue
    start = sec.VirtualAddress; code = img[start:start + sec.Misc_VirtualSize]
    ins = list(md.disasm(code, base + start))
    for k, x in enumerate(ins):
        hit = False
        for op in x.operands:
            if op.type == capstone.x86.X86_OP_IMM and op.imm in vas: hit = True
            if op.type == capstone.x86.X86_OP_MEM and op.mem.disp in vas: hit = True
        if hit:
            print('---- xref at 0x%x' % x.address)
            for y in ins[max(0, k - cb):k + ca + 1]:
                print('%s 0x%08x  %-7s %s' % ('>>' if y.address == x.address else '  ', y.address, y.mnemonic, y.op_str))
