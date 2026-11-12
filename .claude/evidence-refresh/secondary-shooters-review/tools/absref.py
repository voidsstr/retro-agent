#!/usr/bin/env python3
"""Every instruction whose memory displacement or immediate equals one of the given VAs.
Uses capstone SKIPDATA so a linear sweep does not stop at the first undecodable byte."""
import sys, pefile, capstone
pe = pefile.PE(sys.argv[1], fast_load=True); vas = {int(a, 16) for a in sys.argv[2:]}
base = pe.OPTIONAL_HEADER.ImageBase; img = pe.get_memory_mapped_image()
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32); md.detail = True; md.skipdata = True
n = 0
for sec in pe.sections:
    if not (sec.Characteristics & 0x20000000): continue
    st = sec.VirtualAddress
    for x in md.disasm(img[st:st + sec.Misc_VirtualSize], base + st):
        n += 1
        if x.id == 0: continue
        try: ops = x.operands
        except Exception: continue
        for op in ops:
            if (op.type == capstone.x86.X86_OP_MEM and op.mem.disp in vas) or (op.type == capstone.x86.X86_OP_IMM and op.imm in vas):
                print('0x%08x  %-7s %s' % (x.address, x.mnemonic, x.op_str)); break
print('# instructions swept:', n)
