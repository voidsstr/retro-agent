#!/usr/bin/env python3
"""Find call sites of an imported function in a PE and print the ~16 instructions
before each call, so the pushed arguments can be read. Read-only static analysis."""
import sys, pefile, capstone

def main(path, needle, back=24):
    p = pefile.PE(path)
    base = p.OPTIONAL_HEADER.ImageBase
    iat = {}
    for e in getattr(p, 'DIRECTORY_ENTRY_IMPORT', []):
        for i in e.imports:
            if i.name and needle.encode() in i.name:
                iat[i.address] = (e.dll.decode(), i.name.decode())
    if not iat:
        print("no import matching", needle); return
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    for s in p.sections:
        if not (s.Characteristics & 0x20000000):  # executable
            continue
        data = s.get_data()
        va = base + s.VirtualAddress
        insns = list(md.disasm(data, va))
        for idx, ins in enumerate(insns):
            if ins.mnemonic in ('call', 'jmp'):
                for a, nm in iat.items():
                    tgt = '0x%x' % a
                    if tgt in ins.op_str:
                        print('==== %s %s at 0x%x  (%s!%s)' % (ins.mnemonic, ins.op_str, ins.address, nm[0], nm[1]))
                        for j in insns[max(0, idx-back):idx+1]:
                            print('   0x%x: %s %s' % (j.address, j.mnemonic, j.op_str))
                # indirect through a register loaded from IAT
            if ins.mnemonic == 'mov':
                for a, nm in iat.items():
                    if ('0x%x' % a) in ins.op_str:
                        print('==== IAT load %s %s at 0x%x (%s)' % (ins.mnemonic, ins.op_str, ins.address, nm[1]))
                        for j in insns[max(0, idx-4):idx+30]:
                            print('   0x%x: %s %s' % (j.address, j.mnemonic, j.op_str))

if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2], int(sys.argv[3]) if len(sys.argv) > 3 else 24)
