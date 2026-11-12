#!/usr/bin/env python3
"""Print the instructions leading up to interesting call sites in a 32-bit PE.

usage: callsites.py <pe> [--import NAME ...] [--vcall OFF ...] [--const HEX ...] [--before N]

--import NAME   calls through the IAT slot of an imported function (e.g.
                ChangeDisplaySettingsA, ChangeDisplaySettingsExA)
--vcall OFF     indirect calls `call dword ptr [reg + OFF]` (COM vtable slot),
                e.g. 0x54 = IDirectDraw7::SetDisplayMode
--const HEX     any instruction carrying this immediate (e.g. 0x5c0000 =
                DM_BITSPERPEL|DM_PELSWIDTH|DM_PELSHEIGHT|DM_DISPLAYFREQUENCY)
"""
import sys, re, pefile, capstone

args = sys.argv[1:]
path = args.pop(0)
imports, vcalls, consts, before, after = [], [], [], 14, 0
while args:
    a = args.pop(0)
    if a == '--import': imports.append(args.pop(0))
    elif a == '--vcall': vcalls.append(int(args.pop(0), 0))
    elif a == '--const': consts.append(int(args.pop(0), 0))
    elif a == '--before': before = int(args.pop(0))
    elif a == '--after': after = int(args.pop(0))

pe = pefile.PE(path, fast_load=False)
base = pe.OPTIONAL_HEADER.ImageBase
iat = {}
for ent in getattr(pe, 'DIRECTORY_ENTRY_IMPORT', []):
    for imp in ent.imports:
        if imp.name:
            iat[imp.address] = (ent.dll.decode(), imp.name.decode())
want_iat = {addr: nm for addr, nm in iat.items() if nm[1] in imports}

md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
md.detail = False
for s in pe.sections:
    if not (s.Characteristics & 0x20000000):   # executable
        continue
    code = s.get_data()
    va = base + s.VirtualAddress
    insns = []
    off = 0
    while off < len(code):
        got = False
        for ins in md.disasm(code[off:], va + off):
            insns.append(ins)
            got = True
            last = ins.address + ins.size - va
        if got and last > off:
            off = last
        # capstone stops at the first undecodable byte: skip it and resume
        off += 1 if not got or last <= off else 0
        if got and off < len(code):
            off += 1
    for i, ins in enumerate(insns):
        hit = None
        op = ins.op_str
        for addr, nm in want_iat.items():
            if ('0x%x]' % addr) in op:
                hit = '%s!%s (%s)' % (nm[0], nm[1], ins.mnemonic)
        if ins.mnemonic == 'call':
            for off in vcalls:
                if op.startswith('dword ptr [') and op.endswith('+ 0x%x]' % off) and '*' not in op:
                    hit = 'vtable+0x%x' % off
        for c in consts:
            if re.search(r'(?<![0-9a-fx])0x%x(?![0-9a-f])' % c, ins.op_str) and ins.mnemonic in ('mov', 'or', 'push', 'and', 'cmp'):
                hit = hit or 'const 0x%x' % c
        if hit:
            print('---- %s at 0x%x' % (hit, ins.address))
            for p in insns[max(0, i - before):i + 1 + after]:
                print('   0x%08x  %-6s %s' % (p.address, p.mnemonic, p.op_str))
