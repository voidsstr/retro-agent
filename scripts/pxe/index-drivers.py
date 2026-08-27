#!/usr/bin/env python3
"""Index every driver directory in a DriverPacks tree by the devices it claims.

WHY. OemPnPDriversPath is ONE semicolon-separated registry value and XP starts
truncating it somewhere around 4 KB - silently, so the drivers past the cut are
simply never found. PnP also does not recurse, so every directory holding an INF
has to be named explicitly. Shipping every pack wholesale would need ~490
directories; the budget is roughly 300.

So the set has to be chosen rather than dumped, and choosing it by hand across
2,900 INFs is not viable. This builds the index that makes the choice
mechanical: for each directory, which PCI/ISA device ids do its INFs claim.

A directory is the unit throughout, never a file: an INF names its .sys by
filename, so mixing files from two vendor builds produces a driver that looks
installed and cannot bind.
"""
import argparse
import json
import os
import re
import sys

# PCI\VEN_xxxx&DEV_xxxx, and the ISA/EISA style ids used by SB16 and friends.
PCI = re.compile(rb'VEN_([0-9A-Fa-f]{4})&DEV_([0-9A-Fa-f]{4})')
EISA = re.compile(rb'\b([A-Z]{3}[0-9A-Fa-f]{4})\b')


def scan_dir(path):
    pci, eisa = set(), set()
    try:
        names = os.listdir(path)
    except OSError:
        return pci, eisa
    for n in names:
        if not n.lower().endswith('.inf'):
            continue
        try:
            with open(os.path.join(path, n), 'rb') as fh:
                raw = fh.read()
        except OSError:
            continue
        for m in PCI.finditer(raw):
            pci.add((m.group(1).upper() + b'&' + m.group(2).upper()).decode())
        for m in EISA.finditer(raw):
            tok = m.group(1).decode()
            # Keep only plausible hardware ids, not random uppercase words.
            if tok[:3].isalpha() and any(c.isdigit() for c in tok[3:]):
                eisa.add(tok)
    return pci, eisa


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('roots', nargs='+', help='pack directories to index')
    ap.add_argument('--out', required=True)
    a = ap.parse_args()

    index = {}
    for root in a.roots:
        if not os.path.isdir(root):
            print(f'  skip (missing): {root}', file=sys.stderr)
            continue
        pack = os.path.basename(root.rstrip('/'))
        n_dirs = 0
        for dirpath, _dirnames, filenames in os.walk(root):
            if not any(f.lower().endswith('.inf') for f in filenames):
                continue
            pci, eisa = scan_dir(dirpath)
            if not pci and not eisa:
                continue
            size = 0
            for f in filenames:
                try:
                    size += os.path.getsize(os.path.join(dirpath, f))
                except OSError:
                    pass
            index[dirpath] = {
                'pack': pack,
                'pci': sorted(pci),
                'eisa': sorted(eisa),
                'bytes': size,
                'infs': sum(1 for f in filenames if f.lower().endswith('.inf')),
            }
            n_dirs += 1
        print(f'  {pack:<14} {n_dirs:>4} driver dirs')

    with open(a.out, 'w') as fh:
        json.dump(index, fh, indent=0, sort_keys=True)
    tot_pci = len({p for e in index.values() for p in e['pci']})
    print(f'\n  {len(index)} directories, {tot_pci} distinct PCI ids -> {a.out}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
