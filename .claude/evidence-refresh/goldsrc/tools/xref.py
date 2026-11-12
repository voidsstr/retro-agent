#!/usr/bin/env python3
"""Tiny static xref helper (capstone + pefile) used for the GoldSrc refresh survey.

usage:
  xref.py <pe> import <dll> <func>        callers of an imported function
  xref.py <pe> str <fileoffset-hex>       code that pushes / movs the VA of a string
  xref.py <pe> dis <va-hex> <nbytes>      disassemble a window
Each hit prints a disassembly window of `--before` bytes before and `--after` after.
"""
import sys, struct, argparse
import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_32

ap = argparse.ArgumentParser()
ap.add_argument("pe"); ap.add_argument("kind"); ap.add_argument("args", nargs="*")
ap.add_argument("--before", type=lambda s: int(s, 0), default=0x60)
ap.add_argument("--after", type=lambda s: int(s, 0), default=0x30)
a = ap.parse_args()

pe = pefile.PE(a.pe)
base = pe.OPTIONAL_HEADER.ImageBase
img = pe.get_memory_mapped_image()
md = Cs(CS_ARCH_X86, CS_MODE_32)

text = [s for s in pe.sections if s.Name.startswith(b".text")][0]
t0 = text.VirtualAddress; t1 = t0 + text.Misc_VirtualSize
tbytes = img[t0:t1]

def dis(va, n):
    rva = va - base
    code = img[rva:rva+n]
    out = []
    for ins in md.disasm(code, va):
        out.append("  %08x  %-8s %s" % (ins.address, ins.mnemonic, ins.op_str))
    return "\n".join(out)

def window(hit_va):
    # start disassembly at a nearby instruction boundary: scan back to find a start
    # that decodes cleanly through hit_va
    for back in range(a.before, a.before + 16):
        start = hit_va - back
        ok = False
        for ins in md.disasm(img[start-base:hit_va-base+16], start):
            if ins.address == hit_va: ok = True; break
            if ins.address > hit_va: break
        if ok: break
    return dis(start, (hit_va - start) + a.after)

def find_u32(val):
    needle = struct.pack("<I", val)
    hits = []; i = 0
    while True:
        j = tbytes.find(needle, i)
        if j < 0: break
        hits.append(base + t0 + j); i = j + 1
    return hits

if a.kind == "import":
    dll, fn = a.args
    iat = None
    for e in pe.DIRECTORY_ENTRY_IMPORT:
        if e.dll.decode().lower() == dll.lower():
            for i in e.imports:
                if i.name and i.name.decode() == fn: iat = i.address
    print("IAT slot", hex(iat))
    for h in find_u32(iat):
        print("---- ref at operand %#x" % h)
        print(window(h - 2))
elif a.kind == "str":
    off = int(a.args[0], 16)
    rva = pe.get_rva_from_offset(off)
    va = base + rva
    print("string VA %#x" % va)
    for h in find_u32(va):
        print("---- ref at operand %#x" % h)
        print(window(h - 1))
elif a.kind == "dis":
    print(dis(int(a.args[0], 16), int(a.args[1], 16)))
