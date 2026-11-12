#!/usr/bin/env python3
"""Disassemble a PE (x86) around a virtual address, and find xrefs to a string.
usage: dis.py <pe> va <hexva> [before] [after]
       dis.py <pe> str <text> [utf16]    -> VA(s) of the string and code refs (push imm32 / mov imm32)
       dis.py <pe> refs <hexva>          -> instructions whose immediate/displacement == va
Read-only."""
import sys, pefile, capstone, struct, re
pe = pefile.PE(sys.argv[1], fast_load=True)
base = pe.OPTIONAL_HEADER.ImageBase
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
md.detail = False
def sec_of_va(va):
    for s in pe.sections:
        a = base + s.VirtualAddress
        if a <= va < a + max(s.Misc_VirtualSize, s.SizeOfRawData):
            return s
def read(va, n):
    return pe.get_data(va - base, n)
def dis(va, before=0x40, after=0x80):
    start = va - before
    code = read(start, before + after)
    # resync: try offsets until we land on va
    for skew in range(0, 16):
        insns = list(md.disasm(code[skew:], start + skew))
        if any(i.address == va for i in insns):
            break
    for i in insns:
        mark = '>>' if i.address == va else '  '
        print('%s %08x: %-24s %s %s' % (mark, i.address, i.bytes.hex(), i.mnemonic, i.op_str))
def find_str(text, utf16=False):
    b = text.encode('utf-16le') if utf16 else text.encode('latin-1')
    out = []
    for s in pe.sections:
        data = s.get_data()
        for m in re.finditer(re.escape(b), data):
            out.append(base + s.VirtualAddress + m.start())
    return out
def refs(target):
    t = struct.pack('<I', target)
    hits = []
    for s in pe.sections:
        if not (s.Characteristics & 0x20000000):
            continue
        data = s.get_data()
        for m in re.finditer(re.escape(t), data):
            hits.append(base + s.VirtualAddress + m.start())
    return hits
cmd = sys.argv[2]
if cmd == 'va':
    va = int(sys.argv[3], 16)
    b = int(sys.argv[4], 16) if len(sys.argv) > 4 else 0x40
    a = int(sys.argv[5], 16) if len(sys.argv) > 5 else 0x80
    dis(va, b, a)
elif cmd == 'str':
    utf16 = len(sys.argv) > 4 and sys.argv[4] == 'utf16'
    for va in find_str(sys.argv[3], utf16):
        print('string at %08x' % va, 'code refs:', ['%08x' % r for r in refs(va)])
elif cmd == 'refs':
    for r in refs(int(sys.argv[3], 16)):
        print('%08x' % r)
