#!/usr/bin/env python3
"""Find direct E8 rel32 callers of a function VA, and the enclosing function start guess. usage: callers.py <pe> <hexva>..."""
import sys, re, struct, pefile
pe = pefile.PE(sys.argv[1], fast_load=True)
base = pe.OPTIONAL_HEADER.ImageBase
secs = [(base + s.VirtualAddress, s.get_data()) for s in pe.sections if s.Characteristics & 0x20000000]
def func_start(va):
    for sva, data in secs:
        if sva <= va < sva + len(data):
            off = va - sva
            i = off
            while i > 2 and off - i < 0x4000:
                if data[i-1] in (0x90, 0xcc) and data[i-2] in (0x90, 0xcc, 0xc3, 0xc2) and data[i] not in (0x90, 0xcc):
                    return sva + i
                i -= 1
    return None
for t in sys.argv[2:]:
    target = int(t, 16)
    print('callers of %08x:' % target)
    for sva, data in secs:
        for m in re.finditer(rb'\xe8', data):
            o = m.start()
            if o + 5 > len(data): continue
            rel = struct.unpack('<i', data[o+1:o+5])[0]
            if sva + o + 5 + rel == target:
                cva = sva + o
                fs = func_start(cva)
                print('   %08x  (in fn ~%s)' % (cva, ('%08x' % fs) if fs else '?'))
