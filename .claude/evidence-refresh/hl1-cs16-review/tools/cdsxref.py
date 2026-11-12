#!/usr/bin/env python3
"""Adversarial re-check: every call through ChangeDisplaySettingsA / DirectDrawCreate
in the exact staged binaries, with the preceding instructions, so the DEVMODE
fields (dmFields at DEVMODE+0x28, dmDisplayFrequency at +0x78) can be read.
Also lists which vgui.dll exports the engine binaries import, to see whether
vgui::Surface::setFullscreenMode (the OTHER ChangeDisplaySettingsA caller in
these trees) is reachable."""
import sys, re, hashlib
import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_32

LIB = '/mnt/retro-share/Files/Games-Library/'

def load(rel):
    pe = pefile.PE(LIB + rel)
    return pe, pe.OPTIONAL_HEADER.ImageBase, pe.get_memory_mapped_image()

def slots(pe, names):
    out = {}
    for d in getattr(pe, 'DIRECTORY_ENTRY_IMPORT', []):
        for i in d.imports:
            if i.name and i.name.decode() in names:
                out[i.address] = d.dll.decode() + '!' + i.name.decode()
    return out

def sweep(pe, base, img):
    md = Cs(CS_ARCH_X86, CS_MODE_32); md.skipdata = True
    for s in pe.sections:
        if s.Characteristics & 0x20000000:
            a = base + s.VirtualAddress
            code = img[s.VirtualAddress:s.VirtualAddress + s.Misc_VirtualSize]
            for ins in md.disasm(code, a):
                yield ins

def window(base, img, va, back=0x90, fwd=0x20):
    md = Cs(CS_ARCH_X86, CS_MODE_32); md.skipdata = True
    for b in range(back, 0, -1):
        lst = list(md.disasm(img[va - b - base: va - base + fwd], va - b))
        if any(i.address == va for i in lst):
            return lst
    return list(md.disasm(img[va - base: va - base + fwd], va))

for rel in sys.argv[1:]:
    pe, base, img = load(rel)
    raw = open(LIB + rel, 'rb').read()
    print('=' * 78)
    print(rel, len(raw), hashlib.md5(raw).hexdigest(), 'base %#x' % base)
    sl = slots(pe, {'ChangeDisplaySettingsA', 'ChangeDisplaySettingsExA', 'DirectDrawCreate',
                    'DirectDrawCreateEx', 'EnumDisplaySettingsA'})
    print('slots:', {hex(k): v for k, v in sl.items()})
    hits = []
    for ins in sweep(pe, base, img):
        if ins.mnemonic in ('call', 'jmp', 'mov'):
            for s, n in sl.items():
                if ('[0x%x]' % s) in ins.op_str:
                    hits.append((ins.address, n, ins.mnemonic, ins.op_str))
    for a, n, m, o in hits:
        print('\n-- %08x %s %s  (%s)' % (a, m, o, n))
        for i in window(base, img, a)[-30:]:
            print('   %08x  %-6s %s' % (i.address, i.mnemonic, i.op_str))
    vg = []
    for d in getattr(pe, 'DIRECTORY_ENTRY_IMPORT', []):
        if d.dll.decode().lower() == 'vgui.dll':
            vg = [i.name.decode() for i in d.imports if i.name]
    sel = [n for n in vg if re.search(r'Surface|App@', n)]
    if vg:
        print('\nvgui.dll imports naming Surface/App (%d of %d):' % (len(sel), len(vg)))
        for n in sel:
            print('   ', n)
