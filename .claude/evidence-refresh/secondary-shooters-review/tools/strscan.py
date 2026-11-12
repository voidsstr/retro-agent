#!/usr/bin/env python3
"""Case-insensitive strings scan (ASCII + UTF-16LE) of every PE-ish file in a staged title tree.
usage: strscan.py <title-dir> <regex> [extensions...]"""
import os, re, sys
root, rx = sys.argv[1], re.compile(sys.argv[2].encode(), re.I)
exts = tuple(e.lower() for e in (sys.argv[3:] or ['.exe', '.dll', '.ren', '.drv', '.asi', '.flt', '.m3d', '.vxd']))
A = re.compile(rb'[\x20-\x7e]{4,}')
W = re.compile(rb'(?:[\x20-\x7e]\x00){4,}')
for dp, dn, fn in os.walk(root):
    for f in sorted(fn):
        if not f.lower().endswith(exts):
            continue
        p = os.path.join(dp, f)
        try:
            b = open(p, 'rb').read()
        except Exception as e:
            print('ERR', p, e); continue
        hits = set()
        for m in A.finditer(b):
            if rx.search(m.group()):
                hits.add(('A', m.start(), m.group().decode('latin-1')))
        for m in W.finditer(b):
            s = m.group().decode('utf-16le', 'replace').encode('latin-1', 'replace')
            if rx.search(s):
                hits.add(('W', m.start(), s.decode('latin-1')))
        if hits:
            rel = os.path.relpath(p, root)
            for kind, off, s in sorted(hits, key=lambda x: x[1]):
                print('%s  %s@0x%x  %s' % (rel, kind, off, s[:160]))
