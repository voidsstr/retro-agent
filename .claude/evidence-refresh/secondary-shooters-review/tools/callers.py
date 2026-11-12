#!/usr/bin/env python3
"""List direct call/jmp sites targeting a VA: callers.py <pe> <target_hex> [ctx]"""
import sys, pefile, capstone
pe = pefile.PE(sys.argv[1], fast_load=True)
t = int(sys.argv[2], 16); ctx = int(sys.argv[3]) if len(sys.argv) > 3 else 6
base = pe.OPTIONAL_HEADER.ImageBase; img = pe.get_memory_mapped_image()
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32); md.detail = True
for sec in pe.sections:
    if not (sec.Characteristics & 0x20000000): continue
    st = sec.VirtualAddress
    ins = list(md.disasm(img[st:st + sec.Misc_VirtualSize], base + st))
    for k, x in enumerate(ins):
        if x.mnemonic in ('call', 'jmp') and x.operands and x.operands[0].type == capstone.x86.X86_OP_IMM and x.operands[0].imm == t:
            print('---- %s at 0x%x' % (x.mnemonic, x.address))
            for y in ins[max(0, k - ctx):k + 3]:
                print('   0x%08x  %-7s %s' % (y.address, y.mnemonic, y.op_str))
        # also data refs (vtable entries / pushes of the function address)
        for op in x.operands:
            if op.type == capstone.x86.X86_OP_IMM and op.imm == t and x.mnemonic not in ('call','jmp'):
                print('---- imm ref at 0x%x: %s %s' % (x.address, x.mnemonic, x.op_str))
# raw dword refs (vtables)
import struct
needle = struct.pack('<I', t)
i = img.find(needle)
while i != -1:
    print('dword ref at VA 0x%x' % (base + i)); i = img.find(needle, i + 1)
