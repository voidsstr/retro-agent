#!/usr/bin/env python3
"""Linear disassembly of a VA range printing calls/jumps/string pushes with the
ASCII string an immediate points at. usage: flow.py <pe> <start_hex> <end_hex> [all]"""
import sys, re, pefile, capstone
pe = pefile.PE(sys.argv[1], fast_load=True)
base = pe.OPTIONAL_HEADER.ImageBase
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
def strat(va):
    try:
        b = pe.get_data(va - base, 96)
    except Exception:
        return None
    s = b.split(b'\x00')[0]
    if len(s) >= 4 and all(32 <= c < 127 for c in s):
        return s.decode()
    w = b.decode('utf-16le', 'ignore').split('\x00')[0]
    if len(w) >= 4 and all(32 <= ord(c) < 127 for c in w):
        return 'W:' + w
    return None
start, end = int(sys.argv[2], 16), int(sys.argv[3], 16)
allins = len(sys.argv) > 4
code = pe.get_data(start - base, end - start)
for i in md.disasm(code, start):
    extra = ''
    for v in re.findall(r'0x[0-9a-f]+', i.op_str):
        s = strat(int(v, 16))
        if s:
            extra += '  ; "%s"' % s[:70]
    if allins or i.mnemonic in ('call', 'ret') or i.mnemonic.startswith('j') or extra:
        print('%08x: %-8s %s%s' % (i.address, i.mnemonic, i.op_str, extra))
