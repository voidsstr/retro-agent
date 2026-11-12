#!/usr/bin/env python3
"""Independent, read-only audit of every display-mode call in a PE.

For each PE given:
  * every IAT slot for ChangeDisplaySettings*/EnumDisplaySettings*/DirectDrawCreate*
    /GetProcAddress, plus 'ChangeDisplaySettings' strings (a GetProcAddress'd CDS
    would not be in the IAT);
  * every instruction that reaches a slot: call [slot], jmp [slot] (thunk) and
    'mov reg,[slot]' followed by a later 'call reg';
  * for each CDS call site, the 70 preceding instructions, so the DEVMODE's
    dmFields and dmDisplayFrequency writes can be read by eye;
  * every immediate with bit 0x400000 set in ANY instruction, with the next
    instruction, so a dmFields value built by or/and/lea is not missed.

usage: cdsaudit.py <pe> [<pe> ...]
"""
import sys
import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_32

WANT = ('ChangeDisplaySettings', 'EnumDisplaySettings', 'DirectDrawCreate',
        'GetProcAddress', 'LoadLibrary')


def code_insns(pe, img, base):
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    md.skipdata = True
    out = []
    for s in pe.sections:
        if not (s.Characteristics & 0x20000000):
            continue
        code = img[s.VirtualAddress:s.VirtualAddress + max(s.Misc_VirtualSize, s.SizeOfRawData)]
        out.extend(md.disasm(code, base + s.VirtualAddress))
    return out


def main(path):
    pe = pefile.PE(path)
    img = pe.get_memory_mapped_image()
    base = pe.OPTIONAL_HEADER.ImageBase
    print('=' * 100)
    print('PE', path, 'imagebase 0x%x' % base)
    slots = {}
    for e in getattr(pe, 'DIRECTORY_ENTRY_IMPORT', []):
        for imp in e.imports:
            n = (imp.name or b'').decode('latin1')
            if any(w.lower() in n.lower() for w in WANT):
                slots[imp.address] = e.dll.decode() + '!' + n
    print('import slots:', {hex(k): v for k, v in slots.items()} or 'NONE')
    for w in (b'ChangeDisplaySettings', b'EnumDisplaySettings', b'SetDisplayMode'):
        i = img.find(w)
        while i >= 0:
            end = img.find(b'\0', i)
            print('  string %r at VA 0x%x' % (bytes(img[i:end]), base + i))
            i = img.find(w, i + 1)
    ins = code_insns(pe, img, base)
    idx = {x.address: k for k, x in enumerate(ins)}
    thunks = {}
    regslot = {}
    sites = []
    for k, x in enumerate(ins):
        for a, n in slots.items():
            if ('[0x%x]' % a) in x.op_str:
                if x.mnemonic == 'jmp':
                    thunks[x.address] = n
                elif x.mnemonic == 'call':
                    sites.append((k, n, 'call [slot]'))
                elif x.mnemonic == 'mov':
                    reg = x.op_str.split(',')[0].strip()
                    # follow forward to the first 'call reg' within 400 insns
                    for j in range(k + 1, min(k + 400, len(ins))):
                        y = ins[j]
                        if y.mnemonic == 'call' and y.op_str.strip() == reg:
                            sites.append((j, n, 'mov %s,[slot] @0x%x then call %s' % (reg, x.address, reg)))
                        if y.mnemonic in ('ret',):
                            break
    for k, x in enumerate(ins):
        if x.mnemonic == 'call':
            try:
                t = int(x.op_str, 16)
            except ValueError:
                continue
            if t in thunks:
                sites.append((k, thunks[t], 'call thunk 0x%x' % t))
    seen = set()
    for k, n, how in sorted(sites):
        if k in seen:
            continue
        seen.add(k)
        print('-' * 100)
        print('SITE 0x%x  %s  (%s)' % (ins[k].address, n, how))
        if 'ChangeDisplaySettings' in n:
            for j in range(max(0, k - 70), k + 3):
                y = ins[j]
                print('   0x%x: %s %s' % (y.address, y.mnemonic, y.op_str))
    print('-' * 100)
    print('every immediate with bit 0x400000 set (and the next insn):')
    for k, x in enumerate(ins):
        for tok in x.op_str.replace(',', ' ').replace('[', ' ').replace(']', ' ').split():
            if not tok.startswith('0x'):
                continue
            try:
                v = int(tok, 16)
            except ValueError:
                continue
            if v & 0x400000 and v < 0x1000000 and not (base <= v < base + 0x10000000):
                nxt = ins[k + 1] if k + 1 < len(ins) else None
                print('   0x%x: %s %s   | next: %s %s' % (x.address, x.mnemonic, x.op_str,
                      nxt.mnemonic if nxt else '', nxt.op_str if nxt else ''))


if __name__ == '__main__':
    for p in sys.argv[1:]:
        main(p)
