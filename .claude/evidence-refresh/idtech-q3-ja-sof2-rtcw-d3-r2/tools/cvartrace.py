#!/usr/bin/env python3
"""Trace a cvar through an id Tech 3/4 binary, statically.

  cvartrace.py FILE CVARNAME [more cvar names...]

For each name: the string VA, every code reference to it, the global the
Cvar_Get result is stored into (the first 'mov [imm32], eax' after the
registration call), every read of that global, and a disassembly window at
each read. Also lists ChangeDisplaySettingsA / EnumDisplaySettingsA call sites
(direct IAT calls and mingw thunks) with the ENUM_CURRENT_SETTINGS (-1) pushes.
Linear sweeps start at a caller-chosen distance before the site; the window is
evidence to read, not a proof by itself.
"""
import sys, re, struct
sys.path.insert(0, __file__.rsplit('/', 1)[0])
from pe import PE, rel_calls


def sweep(p, start, end):
    off = p.va2off(start)
    out = []
    for i in p.md.disasm(p.data[off:off + (end - start) + 16], start):
        if i.address > end:
            break
        out.append(i)
    return out


def aligned_window(p, site, before=0x30, after=0x40):
    """Find a sweep start before `site` whose linear decode lands exactly on it."""
    for b in range(before, before + 16):
        ins = sweep(p, site - b, site + after)
        if any(i.address == site for i in ins):
            return ins
    return sweep(p, site, site + after)


def fmt(ins, mark=None):
    return '\n'.join('%s%08x: %-7s %s' % ('>>' if i.address == mark else '  ', i.address, i.mnemonic, i.op_str) for i in ins)


def main():
    p = PE(sys.argv[1])
    print('#### %s  image base %#x' % (sys.argv[1], p.base))
    for name in sys.argv[2:]:
        hits = [h for h in p.find_str(name) if h[2].lower() == name.lower().encode()]
        print('\n== cvar %s: exact string at %s' % (name, [hex(h[0]) for h in hits]))
        for s, _, _ in hits:
            refs = p.xref(s)
            print('   code refs to the string: %s' % [hex(r) for r in refs])
            for r in refs:
                w = aligned_window(p, r - 1, 0x10, 0x60)
                # the global: first `mov dword ptr [imm32], eax` after a call following the ref
                glob = None
                seen_call = False
                holders = {'eax'}
                for i in w:
                    if i.address < r - 8:
                        continue
                    if i.mnemonic == 'call':
                        if seen_call:
                            break           # the NEXT registration - stop
                        seen_call = True
                        continue
                    if not seen_call or i.mnemonic != 'mov':
                        continue
                    m = re.match(r'(e[a-z][a-z]), (e[a-z][a-z])$', i.op_str)
                    if m and m.group(2) in holders:
                        holders.add(m.group(1))
                        continue
                    m = re.match(r'dword ptr \[(0x[0-9a-f]+)\], (e[a-z][a-z])$', i.op_str)
                    if m and m.group(2) in holders:
                        glob = int(m.group(1), 16)
                        break
                print('   registration near %#x -> stored in global %s' % (r, hex(glob) if glob else '?'))
                print(fmt([i for i in w if r - 0x18 <= i.address <= r + 0x40]))
                if glob:
                    reads = p.xref(glob)
                    print('   every code ref to %#x: %s' % (glob, [hex(x) for x in reads]))
                    for x in reads:
                        w2 = aligned_window(p, x - 1, 0x28, 0x40)
                        print('   --- ref %#x' % x)
                        print(fmt([i for i in w2 if x - 0x20 <= i.address <= x + 0x38]))
    for api in ('ChangeDisplaySettingsA', 'EnumDisplaySettingsA'):
        for d, n, a in p.iat(api):
            sites = [r for r in p.xref(a)]
            thunks = []
            for r in sites:
                off = p.va2off(r - 2)
                if off is not None and p.data[off:off + 2] == b'\xff\x25':
                    thunks.append(r - 2)
            calls = [r - 2 for r in sites if r - 2 not in thunks]
            for t in thunks:
                calls += rel_calls(p, t)
            print('\n== %s (IAT %#x) call sites: %s' % (api, a, [hex(c) for c in sorted(calls)]))


if __name__ == '__main__':
    main()
