#!/usr/bin/env python3
"""Disassemble a 32-bit PE around a VA: dis.py <pe> <va_hex> [before_bytes] [after_bytes]"""
import sys, pefile, capstone
pe = pefile.PE(sys.argv[1], fast_load=True)
va = int(sys.argv[2], 16)
before = int(sys.argv[3]) if len(sys.argv) > 3 else 64
after = int(sys.argv[4]) if len(sys.argv) > 4 else 32
base = pe.OPTIONAL_HEADER.ImageBase
rva = va - base
data = pe.get_memory_mapped_image()
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
# linear sweep from several candidate starts, pick the one that lands exactly on va
best = None
for s in range(rva - before, rva - before + 16):
    ins = list(md.disasm(data[s:rva + after], base + s))
    if any(i.address == va for i in ins):
        best = ins; break
if best is None:
    best = list(md.disasm(data[rva:rva + after], va))
for i in best:
    mark = '>>' if i.address == va else '  '
    print('%s 0x%08x  %-8s %s' % (mark, i.address, i.mnemonic, i.op_str))
