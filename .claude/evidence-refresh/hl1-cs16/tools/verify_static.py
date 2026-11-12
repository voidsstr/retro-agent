#!/usr/bin/env python3
"""Independent static check of GoldSrc's refresh handling (HalfLife1 WON 1.1.0.8
and CounterStrike16 build 4554), read straight from the staged library.

For every binary: every call through the ChangeDisplaySettingsA IAT slot, with
the instructions that fill the DEVMODE, and whether DM_DISPLAYFREQUENCY
(0x00400000) is ever set; every instruction anywhere that names the -freq /
-refresh strings; and every immediate that could be a dmFields value carrying
DM_DISPLAYFREQUENCY. Linear sweep over the executable sections with capstone;
each hit is re-disassembled from a window that ends on it so a desynchronised
sweep cannot invent one.
"""
import sys, hashlib, re
import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_32

LIB = '/mnt/retro-share/Files/Games-Library'
BINS = ['HalfLife1/hl.exe', 'HalfLife1/hw.dll', 'CounterStrike16/hl.exe',
        'CounterStrike16/hw.dll']
DM_BITSPERPEL, DM_PELSWIDTH, DM_PELSHEIGHT, DM_DISPLAYFREQUENCY = \
    0x40000, 0x80000, 0x100000, 0x400000


def load(path):
    pe = pefile.PE(path)
    base = pe.OPTIONAL_HEADER.ImageBase
    img = pe.get_memory_mapped_image()
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    md.skipdata = True
    return pe, base, img, md


def sweep(pe, base, img, md):
    for s in pe.sections:
        if not (s.Characteristics & 0x20000000):
            continue
        a = base + s.VirtualAddress
        code = img[s.VirtualAddress:s.VirtualAddress + s.Misc_VirtualSize]
        for ins in md.disasm(code, a):
            yield ins


def window(base, img, md, va, back=0x60, fwd=0x10):
    """Disassemble [va-back, va+fwd) choosing a start that lands exactly on va."""
    for b in range(back, 0, -1):
        start = va - b
        ins_list = list(md.disasm(img[start - base:va - base + fwd], start))
        if any(i.address == va for i in ins_list):
            return ins_list
    return list(md.disasm(img[va - base:va - base + fwd], va))


def strings_at(pe, base, img, needles):
    out = {}
    data = bytes(img)
    for n in needles:
        for m in re.finditer(re.escape(b'\0' + n + b'\0'), data):
            out.setdefault(n.decode(), []).append(base + m.start() + 1)
    return out


def iat_slots(pe, names):
    slots = {}
    for d in getattr(pe, 'DIRECTORY_ENTRY_IMPORT', []):
        for imp in d.imports:
            if imp.name and imp.name.decode() in names:
                slots[imp.address] = imp.name.decode()
    return slots


def main():
    for rel in BINS:
        path = '%s/%s' % (LIB, rel)
        raw = open(path, 'rb').read()
        pe, base, img, md = load(path)
        print('=' * 78)
        print('%s  %d B  md5 %s  base %#x' % (rel, len(raw),
              hashlib.md5(raw).hexdigest(), base))
        slots = iat_slots(pe, {'ChangeDisplaySettingsA', 'ChangeDisplaySettingsExA'})
        print('ChangeDisplaySettings IAT slots:', {hex(k): v for k, v in slots.items()} or 'NONE (not imported)')
        strs = strings_at(pe, base, img, [b'-freq', b'-refresh', b'-refreshrate',
                                          b'-hz', b'-gl', b'-d3d', b'-w', b'-h',
                                          b'-width', b'-height', b'EngineD3D',
                                          b'EngineType'])
        print('switch/value strings (exact, NUL-delimited):',
              {k: [hex(x) for x in v] for k, v in strs.items()})
        str_vas = {va: k for k, v in strs.items() for va in v}

        cds_calls, str_refs, freq_imm = [], [], []
        for ins in sweep(pe, base, img, md):
            o = ins.op_str
            for slot in slots:
                if ins.mnemonic in ('call', 'jmp') and ('[0x%x]' % slot) in o:
                    cds_calls.append(ins.address)
            if ins.mnemonic in ('push', 'mov', 'lea', 'cmp'):
                for va, name in str_vas.items():
                    if name in ('-freq', '-refresh', '-refreshrate', '-hz', '-gl', '-d3d', 'EngineD3D', 'EngineType') \
                            and ('0x%x' % va) in o.split(', ')[-1]:
                        str_refs.append((ins.address, name))
            # an immediate carrying DM_DISPLAYFREQUENCY plus the mode bits
            if ins.mnemonic in ('mov', 'or') and o.endswith(('0x5c0000', '0x400000', '0x4c0000', '0x580000', '0x540000')):
                freq_imm.append(ins.address)

        print('\n-- calls through the ChangeDisplaySettings slot: %d' % len(cds_calls))
        for a in cds_calls:
            w = window(base, img, md, a, back=0x70, fwd=6)
            fields = [i for i in w if i.mnemonic in ('mov', 'or') and
                      re.search(r'0x(1c0000|5c0000|400000|40000|80000|100000)$', i.op_str)]
            print('  call @%08x  dmFields writes nearby: %s' % (
                a, ['%08x %s %s' % (i.address, i.mnemonic, i.op_str) for i in fields] or 'none (NULL devmode / restore call)'))
            for i in w[-18:]:
                print('      %08x  %-6s %s' % (i.address, i.mnemonic, i.op_str))

        print('\n-- instructions naming the switch strings: %d' % len(str_refs))
        for a, name in str_refs:
            print('  %08x  %s' % (a, name))

        print('\n-- immediates that could set DM_DISPLAYFREQUENCY: %d' % len(freq_imm))
        for a in freq_imm:
            w = window(base, img, md, a, back=0x40, fwd=8)
            for i in w[-6:]:
                print('      %08x  %-6s %s' % (i.address, i.mnemonic, i.op_str))
            print('      --')


if __name__ == '__main__':
    main()
