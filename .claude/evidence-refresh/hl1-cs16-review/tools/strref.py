#!/usr/bin/env python3
"""Find instructions that reference a string (by VA of its first byte) and print context.
usage: strref.py <lib-relative binary> <string> [<string> ...]"""
import sys, re
import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_32
LIB = '/mnt/retro-share/Files/Games-Library/'
rel = sys.argv[1]
pe = pefile.PE(LIB + rel); base = pe.OPTIONAL_HEADER.ImageBase; img = bytes(pe.get_memory_mapped_image())
md = Cs(CS_ARCH_X86, CS_MODE_32); md.skipdata = True
vas = {}
for s in sys.argv[2:]:
    for m in re.finditer(re.escape(b'\0' + s.encode() + b'\0'), img):
        vas[base + m.start() + 1] = s
print(rel, {hex(k): v for k, v in vas.items()})
code = []
for sec in pe.sections:
    if sec.Characteristics & 0x20000000:
        a = base + sec.VirtualAddress
        code += list(md.disasm(img[sec.VirtualAddress:sec.VirtualAddress + sec.Misc_VirtualSize], a))
idx = {i.address: k for k, i in enumerate(code)}
for k, ins in enumerate(code):
    for va, s in vas.items():
        if ('0x%x' % va) in ins.op_str:
            print('\n-- %08x %s %s   [%s]' % (ins.address, ins.mnemonic, ins.op_str, s))
            for j in code[max(0, k - 6):k + 14]:
                print('   %08x  %-6s %s' % (j.address, j.mnemonic, j.op_str))
