#!/usr/bin/env python3
"""Read-only PE helper for the refresh review.

  pedis.py imports <pe> <name-substring>        -> IAT slots + every call/jmp/mov that uses them
  pedis.py dis <pe> <va_hex> [count] [back]     -> disassemble count insns from va (back = bytes before)
  pedis.py xref <pe> <va_hex>                   -> every instruction that references the absolute address
  pedis.py str <pe> <text>                      -> VA of every occurrence of an ASCII string
"""
import sys, struct
import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_32


def load(path):
    pe = pefile.PE(path, fast_load=False)
    img = pe.get_memory_mapped_image()
    base = pe.OPTIONAL_HEADER.ImageBase
    return pe, img, base


def text_sections(pe):
    for s in pe.sections:
        if s.Characteristics & 0x20000000:  # MEM_EXECUTE
            yield s


def disasm_all(pe, img, base):
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    md.skipdata = True
    for s in text_sections(pe):
        start = s.VirtualAddress
        code = img[start:start + max(s.Misc_VirtualSize, s.SizeOfRawData)]
        for ins in md.disasm(code, base + start):
            yield ins


def cmd_imports(path, needle):
    pe, img, base = load(path)
    slots = {}
    for entry in getattr(pe, 'DIRECTORY_ENTRY_IMPORT', []):
        for imp in entry.imports:
            name = (imp.name or b'').decode('latin1')
            if needle.lower() in name.lower():
                slots[imp.address] = entry.dll.decode() + '!' + name
    print('slots:', {hex(k): v for k, v in slots.items()})
    # thunks: jmp dword ptr [slot]
    thunks = {}
    for ins in disasm_all(pe, img, base):
        for a, n in slots.items():
            if ('[0x%x]' % a) in ins.op_str:
                print('  0x%x: %s %s   <- %s' % (ins.address, ins.mnemonic, ins.op_str, n))
                if ins.mnemonic == 'jmp':
                    thunks[ins.address] = n
    if thunks:
        for ins in disasm_all(pe, img, base):
            if ins.mnemonic == 'call':
                try:
                    tgt = int(ins.op_str, 16)
                except ValueError:
                    continue
                if tgt in thunks:
                    print('  0x%x: call thunk 0x%x (%s)' % (ins.address, tgt, thunks[tgt]))


def cmd_dis(path, va, count=40, back=0):
    pe, img, base = load(path)
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    md.skipdata = True
    rva = va - base - back
    code = img[rva:rva + count * 15 + back]
    n = 0
    for ins in md.disasm(code, base + rva):
        print('  0x%x: %-24s %s %s' % (ins.address, ins.bytes.hex(), ins.mnemonic, ins.op_str))
        n += 1
        if n >= count:
            break


def cmd_xref(path, va):
    pe, img, base = load(path)
    needle = '0x%x' % va
    for ins in disasm_all(pe, img, base):
        if needle in ins.op_str:
            print('  0x%x: %s %s' % (ins.address, ins.mnemonic, ins.op_str))


def cmd_str(path, text):
    pe, img, base = load(path)
    b = text.encode('latin1')
    i = img.find(b)
    while i >= 0:
        print('  "%s" at VA 0x%x' % (text, base + i))
        i = img.find(b, i + 1)


if __name__ == '__main__':
    c = sys.argv[1]
    if c == 'imports':
        cmd_imports(sys.argv[2], sys.argv[3])
    elif c == 'dis':
        cmd_dis(sys.argv[2], int(sys.argv[3], 16),
                int(sys.argv[4]) if len(sys.argv) > 4 else 40,
                int(sys.argv[5]) if len(sys.argv) > 5 else 0)
    elif c == 'xref':
        cmd_xref(sys.argv[2], int(sys.argv[3], 16))
    elif c == 'str':
        cmd_str(sys.argv[2], sys.argv[3])
