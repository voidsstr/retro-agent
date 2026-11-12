#!/usr/bin/env python3
"""Find every instruction that references an import's IAT slot (call [slot],
jmp [slot], mov reg,[slot]) and print a window of disassembly around it, so a
register-indirect call (MSVC loads a repeatedly-used import into esi/edi/ebx)
is visible too. Read-only static analysis."""
import sys, struct, pefile, capstone

def main(path, needle, before=40, after=40):
    p = pefile.PE(path)
    base = p.OPTIONAL_HEADER.ImageBase
    data = open(path, 'rb').read()
    slots = {}
    for e in getattr(p, 'DIRECTORY_ENTRY_IMPORT', []):
        for i in e.imports:
            if i.name and needle.encode() in i.name:
                slots[i.address] = i.name.decode()
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    for slot, nm in slots.items():
        sb = struct.pack('<I', slot)
        k = data.find(sb)
        while k >= 0:
            try:
                rva = p.get_rva_from_offset(k)
            except Exception:
                rva = None
            if rva is not None:
                va = base + rva
                # find instruction start: opcode bytes precede the operand
                for olen in (2, 1, 3):
                    ins_va = va - olen
                    off0 = k - olen
                    ins = list(md.disasm(data[off0:off0 + 8], ins_va, 1))
                    if ins and ins[0].size == olen + 4 and ('0x%x' % slot) in ins[0].op_str:
                        print('==== %s %s at 0x%x' % (ins[0].mnemonic, ins[0].op_str, ins_va))
                        start = ins_va - before * 3
                        s_off = p.get_offset_from_rva(start - base)
                        blob = data[s_off: s_off + before * 3 + after * 4]
                        for shift in range(16):
                            dis = list(md.disasm(blob[shift:], start + shift))
                            if any(x.address == ins_va for x in dis):
                                idx = [x.address for x in dis].index(ins_va)
                                for x in dis[max(0, idx - before): idx + after]:
                                    mark = '>>' if x.address == ins_va else '  '
                                    print(' %s 0x%x: %s %s' % (mark, x.address, x.mnemonic, x.op_str))
                                break
                        break
            k = data.find(sb, k + 1)

if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2], int(sys.argv[3]) if len(sys.argv) > 3 else 40,
         int(sys.argv[4]) if len(sys.argv) > 4 else 40)
