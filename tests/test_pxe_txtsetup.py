#!/usr/bin/env python3
"""txtsetup.sif must stay structurally valid and register storage drivers right.

TWO FAILURES THIS PINS, BOTH OF WHICH KILLED A REAL BOOT.

1. BOOT-CRITICAL STORAGE DRIVERS NEED THE BOOT-MEDIA FIELDS. A slipstreamed
   miniport registered as '1,,,,,,,,3,3' - the shape for an ordinary file -
   cannot be staged by setupldr, and text mode dies with "<driver>.sys caused an
   unexpected error ... at line 3540 in setup.c". That reads like a corrupt or
   missing file and is neither; the file is present, readable, 32-bit, and
   correctly named. Retail XP registers every such driver as
   '1,,,,,,4_,4,1,,,1,4', where 4_ and 4 mark it as belonging on the boot media.

2. A SINGLE LF ENDS THE BOOT. txtsetup.sif is CRLF throughout and one bare LF
   gives "txtsetup.sif is corrupt or missing" with nothing else to go on. A
   regex rewrite of this file introduced 190 of them in one pass, because '$'
   in multiline mode matches before the \\n and happily eats the \\r.
"""
import os
import re
import sys

SIF = '/mnt/retro-share/Files/OS/XPSP3-FLEET/I386/txtsetup.sif'
fails = []


def check(label, cond):
    print(f'  {"PASS" if cond else "FAIL"}  {label}')
    if not cond:
        fails.append(label)


def main():
    if not os.path.isfile(SIF):
        print(f'  SKIP  {SIF} not present (share not mounted)')
        return 0
    raw = open(SIF, 'rb').read()
    text = raw.decode('latin1')

    print('== line endings ==')
    lf = sum(1 for l in raw.split(b'\n')[:-1] if not l.endswith(b'\r'))
    check(f'no bare LF anywhere (found {lf})', lf == 0)

    print('== the sections setup needs ==')
    for sec in ('[SourceDisksFiles]', '[SourceDisksFiles.x86]',
                '[HardwareIdsDatabase]', '[SCSI.Load]', '[SCSI]'):
        check(f'{sec} present', sec in text)

    print('== every [SCSI.Load] driver is real, 32-bit, and boot-registered ==')
    i = text.index('[SCSI.Load]')
    j = text.index('\r\n[', i + 5)
    entries = re.findall(r'^([^\s=]+)\s*=\s*([^\s,]+)\s*,\s*4\r$', text[i:j], re.M)
    check(f'[SCSI.Load] has entries ({len(entries)})', len(entries) > 0)

    i386 = os.path.dirname(SIF)
    have = {f.lower() for f in os.listdir(i386)}
    missing, wrongarch, badreg = [], [], []
    import struct
    for svc, fn in entries:
        low = fn.lower()
        if low not in have and (low[:-1] + '_') not in have:
            missing.append(fn)
            continue
        p = os.path.join(i386, fn)
        if os.path.isfile(p):
            d = open(p, 'rb').read(4096)
            pe = d.find(b'PE\0\0')
            if pe >= 0 and struct.unpack_from('<H', d, pe + 4)[0] != 0x14c:
                wrongarch.append(fn)
        # its [SourceDisksFiles] line must carry the boot-media fields
        # 3_ and 4_ are both boot-media destination codes - retail uses 3_ for
        # some in-box drivers (aliide) and 4_ for others (atapi, aic78xx). What
        # matters is that ONE of them is there; a plain '3,3' entry is the
        # ordinary-file shape that setupldr cannot stage.
        m = re.search(r'^' + re.escape(fn) + r'\s*=\s*([^\r\n]+)', text, re.M | re.I)
        if m and '4_' not in m.group(1) and '3_' not in m.group(1):
            badreg.append((fn, m.group(1).strip()))

    check(f'no driver is missing from I386 ({len(missing)})', not missing)
    for f in missing[:5]:
        print(f'        missing: {f}')
    check(f'no driver is the wrong architecture ({len(wrongarch)})', not wrongarch)
    for f in wrongarch[:5]:
        print(f'        wrong arch: {f}')
    check(f'every boot driver carries a boot-media field ({len(badreg)})',
          not badreg)

    print('== no driver depends on storport.sys ==')
    # XP SP3 does not ship storport.sys. A driver that imports it loads, asks
    # setup for storport, and text mode dies - so such a driver turns a machine
    # that would have installed in IDE mode into one that cannot install at all.
    # 15 went in on the first pass, HpAHCIsr among them, and that one claims
    # Intel ICH9/ICH10 AHCI: real consumer hardware, not a corner case.
    has_storport = any(f.lower().startswith('storport.sy') for f in os.listdir(i386))
    needs = []
    for svc, fn in entries:
        p2 = os.path.join(i386, fn)
        if not os.path.isfile(p2):
            continue
        with open(p2, 'rb') as fh:
            if b'storport.sys' in fh.read().lower():
                needs.append((svc, fn))
    check(f'no [SCSI.Load] driver imports storport.sys ({len(needs)})',
          not needs or has_storport)
    for s2, f2 in needs[:6]:
        print(f'        {s2} ({f2}) needs storport.sys, which is not on the media')
    for f, line in badreg[:5]:
        print(f'        {f} = {line}')

    print(f'\npxe txtsetup: {"all checks passed" if not fails else str(len(fails)) + " FAILED"}')
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
