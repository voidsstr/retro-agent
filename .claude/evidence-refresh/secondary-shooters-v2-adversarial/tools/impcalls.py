#!/usr/bin/env python3
"""List call sites of an imported function in a PE and disassemble context before each.
usage: impcalls.py <pe> <ImportName> [before_hex] [after_hex]   (read-only)"""
import sys, pefile, capstone, struct, re
pe = pefile.PE(sys.argv[1])
name = sys.argv[2].encode()
before = int(sys.argv[3], 16) if len(sys.argv) > 3 else 0x60
after = int(sys.argv[4], 16) if len(sys.argv) > 4 else 0x10
base = pe.OPTIONAL_HEADER.ImageBase
iat = None
for d in getattr(pe, 'DIRECTORY_ENTRY_IMPORT', []):
    for imp in d.imports:
        if imp.name and imp.name.lower() == name.lower():
            iat = imp.address
            print('import %s!%s IAT %08x' % (d.dll.decode(), imp.name.decode(), iat))
if iat is None:
    sys.exit('no import ' + sys.argv[2])
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
t = struct.pack('<I', iat)
sites = []
for s in pe.sections:
    if not (s.Characteristics & 0x20000000):
        continue
    data = s.get_data()
    for m in re.finditer(re.escape(t), data):
        va = base + s.VirtualAddress + m.start()
        sites.append(va)
for va in sites:
    # instruction starts 2 bytes before the imm (ff15 call [imm] or 8b3d mov edi,[imm])
    ins_va = va - 2
    print('---- ref at %08x' % ins_va)
    start = ins_va - before
    code = pe.get_data(start - base, before + after)
    best = None
    for skew in range(0, 24):
        insns = list(md.disasm(code[skew:], start + skew))
        if any(i.address == ins_va for i in insns):
            best = insns
            break
    for i in (best or []):
        mark = '>>' if i.address == ins_va else '  '
        print('%s %08x: %-20s %s %s' % (mark, i.address, i.bytes.hex(), i.mnemonic, i.op_str))
