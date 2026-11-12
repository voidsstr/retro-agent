#!/usr/bin/env python3
"""Linear-sweep helper for the GoldSrc refresh review (capstone + pefile).
usage: sweep.py <pe> calls <va-hex>...      direct call/jmp xrefs to each VA
       sweep.py <pe> memref <va-hex>...     instructions whose operand names the VA
       sweep.py <pe> vcall <off-hex>        indirect calls [reg+off]
       sweep.py <pe> dis <va-hex> <n-hex>   disassemble a window
       sweep.py <pe> func <va-hex>          disassemble from VA to the first ret (max 0x800)
Linear sweep can desync after data in .text; every hit is re-checked by
re-disassembling a window that ends exactly on it."""
import sys, pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_32
pe = pefile.PE(sys.argv[1]); base = pe.OPTIONAL_HEADER.ImageBase
img = pe.get_memory_mapped_image(); md = Cs(CS_ARCH_X86, CS_MODE_32); md.skipdata = True
texts = [s for s in pe.sections if s.Characteristics & 0x20000000]
def sweep():
    for s in texts:
        a = base + s.VirtualAddress
        code = img[s.VirtualAddress:s.VirtualAddress + s.Misc_VirtualSize]
        for ins in md.disasm(code, a):
            yield ins
def dis(va, n):
    for ins in md.disasm(img[va-base:va-base+n], va):
        print("  %08x  %-8s %s" % (ins.address, ins.mnemonic, ins.op_str))
kind = sys.argv[2]
if kind == 'calls':
    tg = {int(x, 16) for x in sys.argv[3:]}
    for ins in sweep():
        if ins.mnemonic in ('call', 'jmp') and ins.op_str.startswith('0x'):
            try: t = int(ins.op_str, 16)
            except ValueError: continue
            if t in tg: print("%08x  %s %s" % (ins.address, ins.mnemonic, ins.op_str))
elif kind == 'memref':
    tg = [x.lower().lstrip('0x') for x in sys.argv[3:]]
    for ins in sweep():
        o = ins.op_str.lower()
        if any(('0x' + t) in o for t in tg): print("%08x  %-6s %s" % (ins.address, ins.mnemonic, ins.op_str))
elif kind == 'vcall':
    off = sys.argv[3].lower()
    for ins in sweep():
        if ins.mnemonic == 'call' and ins.op_str.startswith('dword ptr [') and ('+ ' + off + ']') in ins.op_str:
            print("%08x  call %s" % (ins.address, ins.op_str))
elif kind == 'dis':
    dis(int(sys.argv[3], 16), int(sys.argv[4], 16))
elif kind == 'func':
    va = int(sys.argv[3], 16)
    for ins in md.disasm(img[va-base:va-base+0x800], va):
        print("  %08x  %-8s %s" % (ins.address, ins.mnemonic, ins.op_str))
        if ins.mnemonic in ('ret', 'retn'): break
