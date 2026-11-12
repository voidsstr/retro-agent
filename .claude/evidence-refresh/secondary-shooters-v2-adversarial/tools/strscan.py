#!/usr/bin/env python3
"""Case-insensitive ASCII + UTF-16LE string scan of every PE-ish file in a
staged title tree. Reports every printable string (>=4 chars) that matches
the pattern, with file, encoding and file offset. Read-only."""
import os, re, sys, hashlib
LIB = '/mnt/retro-share/Files/Games-Library'
EXTS = {'.exe', '.dll', '.ren', '.lto', '.mod', '.ocx', '.drv', '.asi', '.flt', '.int', '.u', '.ini', '.cfg', '.cs', '.con', '.txt', '.bat', '.cmd'}
BINEXT = {'.exe', '.dll', '.ren', '.lto', '.mod', '.ocx', '.drv', '.asi', '.flt', '.u'}
def scan(title, pat, exts=BINEXT, maxsize=64 << 20, sub=''):
    rx = re.compile(pat, re.I)
    root = os.path.join(LIB, title, sub)
    out = []
    for dp, dn, fn in os.walk(root):
        for f in fn:
            e = os.path.splitext(f)[1].lower()
            if e not in exts:
                continue
            p = os.path.join(dp, f)
            try:
                if os.path.getsize(p) > maxsize:
                    continue
                b = open(p, 'rb').read()
            except Exception as ex:
                out.append((p, 'ERR', 0, str(ex)))
                continue
            for m in re.finditer(rb'[\x20-\x7e]{4,}', b):
                s = m.group().decode('ascii')
                if rx.search(s):
                    out.append((p, 'A', m.start(), s[:200]))
            for m in re.finditer(rb'(?:[\x20-\x7e]\x00){4,}', b):
                s = m.group().decode('utf-16le')
                if rx.search(s):
                    out.append((p, 'W', m.start(), s[:200]))
    return out
if __name__ == '__main__':
    title, pat = sys.argv[1], sys.argv[2]
    exts = BINEXT if len(sys.argv) < 4 else set('.' + x.lower() for x in sys.argv[3].split(','))
    for p, enc, off, s in scan(title, pat, exts):
        print('%s\t%s\t0x%x\t%s' % (os.path.relpath(p, LIB), enc, off, s))
