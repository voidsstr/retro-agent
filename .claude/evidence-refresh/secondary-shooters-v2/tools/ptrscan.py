#!/usr/bin/env python3
"""Find every place a 32-bit little-endian pointer to a string (or any VA) is
stored in a PE image (data tables), and every code reference to it.
usage: ptrscan.py <pe> <string-or-0xVA> [...]"""
import sys, struct, pefile, capstone
pe = pefile.PE(sys.argv[1], fast_load=True)
base = pe.OPTIONAL_HEADER.ImageBase
img = pe.get_memory_mapped_image()
for arg in sys.argv[2:]:
    if arg.startswith('0x'):
        vas = [int(arg, 16)]
    else:
        s = arg.encode() + b'\0'
        vas = []
        i = img.find(s)
        while i != -1:
            if i == 0 or img[i-1] == 0:
                vas.append(base + i)
            i = img.find(s, i + 1)
    print('==', arg, [hex(v) for v in vas])
    for v in vas:
        p = struct.pack('<I', v)
        j = img.find(p)
        while j != -1:
            sec = pe.get_section_by_rva(j)
            print('   ptr stored at VA 0x%x (%s)' % (base + j, sec.Name.rstrip(b'\0').decode() if sec else '?'))
            j = img.find(p, j + 1)
