#!/usr/bin/env python3
"""Scan a PE's code for immediates that are DEVMODE dmFields combos carrying
DM_DISPLAYFREQUENCY (0x400000), i.e. any of {0x400000,0x440000,0x480000,0x4c0000,
0x500000,0x540000,0x580000,0x5c0000}, and report each with its neighbours.
Also report the combos WITHOUT it (0x180000/0x1c0000) for contrast. Read-only."""
import sys
import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_32
WITH = {0x400000,0x440000,0x480000,0x4c0000,0x500000,0x540000,0x580000,0x5c0000}
WITHOUT = {0x180000,0x1c0000,0x80000,0x100000,0x140000}
for path in sys.argv[1:]:
    pe = pefile.PE(path); img = pe.get_memory_mapped_image(); base = pe.OPTIONAL_HEADER.ImageBase
    md = Cs(CS_ARCH_X86, CS_MODE_32); md.skipdata = True
    w, wo = [], []
    for s in pe.sections:
        if not (s.Characteristics & 0x20000000): continue
        code = img[s.VirtualAddress:s.VirtualAddress + max(s.Misc_VirtualSize, s.SizeOfRawData)]
        for ins in md.disasm(code, base + s.VirtualAddress):
            if ins.mnemonic not in ('mov', 'or', 'push'): continue
            for tok in ins.op_str.replace(',', ' ').split():
                try: v = int(tok, 16)
                except ValueError: continue
                if v in WITH: w.append('0x%x: %s %s' % (ins.address, ins.mnemonic, ins.op_str))
                elif v in WITHOUT and 'dword ptr' in ins.op_str: wo.append('0x%x: %s %s' % (ins.address, ins.mnemonic, ins.op_str))
    print('==', path.split('Games-Library/')[-1])
    print('   dmFields WITH DM_DISPLAYFREQUENCY :', w if w else 'NONE')
    print('   dmFields-like stores without it  :', wo[:8], '...' if len(wo) > 8 else '')
