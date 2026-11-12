#!/usr/bin/env python3
"""Find every direct call/jmp to a given thunk address (e.g. an import's jmp stub)
and print the instructions before each call, so rate-less vs rate-carrying DEVMODEs
can be told apart. usage: thunkcallers.py <lib-relative binary> <hex thunk> [...]"""
import sys
import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_32
LIB = '/mnt/retro-share/Files/Games-Library/'
rel = sys.argv[1]
targets = [int(x, 16) for x in sys.argv[2:]]
pe = pefile.PE(LIB + rel); base = pe.OPTIONAL_HEADER.ImageBase; img = pe.get_memory_mapped_image()
md = Cs(CS_ARCH_X86, CS_MODE_32); md.skipdata = True
def window(va, back=0xa0, fwd=0x18):
    for b in range(back, 0, -1):
        lst = list(md.disasm(img[va - b - base: va - base + fwd], va - b))
        if any(i.address == va for i in lst):
            return lst
    return []
hits = []
for s in pe.sections:
    if s.Characteristics & 0x20000000:
        a = base + s.VirtualAddress
        for ins in md.disasm(img[s.VirtualAddress:s.VirtualAddress + s.Misc_VirtualSize], a):
            if ins.mnemonic in ('call', 'jmp'):
                try:
                    t = int(ins.op_str, 16)
                except ValueError:
                    continue
                if t in targets:
                    hits.append((ins.address, t))
print(rel, 'callers of', [hex(t) for t in targets], ':', len(hits))
for a, t in hits:
    print('\n-- %08x -> %08x' % (a, t))
    for i in window(a)[-34:]:
        print('   %08x  %-6s %s' % (i.address, i.mnemonic, i.op_str))
