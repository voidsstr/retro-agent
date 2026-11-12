#!/usr/bin/env python3
"""Disassemble around a VA of a 32-bit PE so that a given instruction boundary
is hit: disat.py <pe> <anchorVA> [before_bytes] [after_bytes]
Tries start offsets until the decode stream lands exactly on (anchor-1..anchor-6)
i.e. the instruction CONTAINING the anchor."""
import sys, pefile, capstone
pe = pefile.PE(sys.argv[1], fast_load=True)
base = pe.OPTIONAL_HEADER.ImageBase
img = pe.get_memory_mapped_image()
anchor = int(sys.argv[2], 16)
before = int(sys.argv[3], 0) if len(sys.argv) > 3 else 0x80
after = int(sys.argv[4], 0) if len(sys.argv) > 4 else 0x80
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
best = None
for st in range(anchor - before, anchor - before + 16):
    ins = list(md.disasm(img[st-base:anchor+after-base], st))
    ok = any(i.address <= anchor < i.address + i.size for i in ins) and ins and ins[-1].address > anchor
    if ok:
        best = ins; break
if not best:
    print('no sync'); sys.exit(1)
for i in best:
    mark = '>>' if i.address <= anchor < i.address + i.size else '  '
    print('%s 0x%08x  %-7s %s' % (mark, i.address, i.mnemonic, i.op_str))
