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

    print('== every driver import resolves on the media ==')
    # Checking for the literal string 'storport.sys' caught the first round of
    # unmet dependencies and MISSED the second: three Marvell miniports each
    # import a companion memory manager (mv61xx.sys -> mv61xxmm.sys) that lives
    # in a different pack directory. Text-mode setup then blames the MINIPORT -
    # "mv61xx.sys caused an unknown error (21)" - naming a file that is present
    # and perfectly fine, while never mentioning the one actually missing.
    # Reading the real import table is the only check that catches the class.
    import struct as _struct

    def _imports(path):
        with open(path, 'rb') as fh:
            d = fh.read()
        pe = d.find(b'PE\0\0')
        if pe < 0:
            return []
        nsec = _struct.unpack_from('<H', d, pe + 6)[0]
        opt = pe + 24
        secoff = opt + _struct.unpack_from('<H', d, pe + 20)[0]
        secs = [(_struct.unpack_from('<I', d, secoff + 40 * i + 12)[0],
                 _struct.unpack_from('<I', d, secoff + 40 * i + 16)[0],
                 _struct.unpack_from('<I', d, secoff + 40 * i + 20)[0])
                for i in range(nsec)]

        def r2o(r):
            for va, rs, rp in secs:
                if va <= r < va + max(rs, 1):
                    return rp + (r - va)
            return None

        idir = _struct.unpack_from('<I', d, opt + 96 + 8)[0]
        if not idir:
            return []
        o = r2o(idir)
        out = []
        while o:
            e = _struct.unpack_from('<IIIII', d, o)
            if e[3] == 0:
                break
            no = r2o(e[3])
            if no is None:
                break
            out.append(d[no:d.find(b'\0', no)].decode('latin1').lower())
            o += 20
        return out

    NATIVE = {'ntoskrnl.exe', 'hal.dll', 'scsiport.sys', 'ndis.sys'}
    unmet = {}
    for svc, fn in entries:
        p2 = os.path.join(i386, fn)
        if not os.path.isfile(p2):
            continue
        miss = {m for m in _imports(p2)
                if m not in NATIVE and m.lower() not in have
                and (m[:-1] + '_') not in have}
        if miss:
            unmet[fn] = sorted(miss)
    check(f'every [SCSI.Load] driver has its imports on the media ({len(unmet)})',
          not unmet)
    for fn, miss in list(unmet.items())[:6]:
        print(f'        {fn} needs {", ".join(miss)}')

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
