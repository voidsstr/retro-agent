#!/usr/bin/env python3
import sys, pefile, capstone
path, start, end = sys.argv[1], int(sys.argv[2], 16), int(sys.argv[3], 16)
p = pefile.PE(path); base = p.OPTIONAL_HEADER.ImageBase
data = open(path, 'rb').read()
off = p.get_offset_from_rva(start - base)
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
for x in md.disasm(data[off: off + (end - start)], start):
    print('0x%x: %s %s' % (x.address, x.mnemonic, x.op_str))
