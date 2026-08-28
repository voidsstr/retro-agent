#!/usr/bin/env python3
r"""Slipstream mass-storage drivers so text-mode setup can SEE the disk.

WHY THIS EXISTS. A machine PXE booted fine, loaded its NIC driver, pulled the
whole install chain - and then reported no hard drive at all. The disk was a
Toshiba on a SATA PCI card, visible in the BIOS and invisible to Windows,
because setup had no driver for the card's controller. Not one common SATA PCI
chip - Silicon Image 3112/3114, VIA 6421, JMicron, Promise, Marvell - is in
retail XP's txtsetup.sif. This is the problem the F6 floppy existed to solve.

Placing the .sys in I386 is not enough, and mass storage needs more registration
than a NIC does. A NIC is matched through its INF listing; a storage controller
is matched through [HardwareIdsDatabase] and then loaded via [SCSI.Load], so
FOUR sections have to agree:

    [SourceDisksFiles.x86]  driver.sys = 1,,,,,,,,3,3      the file exists
    [HardwareIdsDatabase]   PCI\VEN_x&DEV_y = "service"    this chip -> that service
    [SCSI.Load]             service = driver.sys,4         load it in text mode
    [SCSI]                  service = "Description"        what to call it

Miss any one and the controller stays invisible, with no error to say why -
which is exactly how a disk that is plainly listed in the BIOS ends up absent
from a Windows installer.

CRLF is preserved byte for byte. A txtsetup.sif with a stray LF gives
"txtsetup.sif is corrupt or missing" and a dead boot.
"""
import argparse
import os
import re
import shutil
import sys


def read_inf(path):
    """INFs here are a mix of ANSI and UTF-16; both appear in the packs."""
    with open(path, 'rb') as fh:
        raw = fh.read()
    if raw[:2] in (b'\xff\xfe', b'\xfe\xff'):
        return raw.decode('utf-16', 'replace')
    return raw.decode('latin1', 'replace')


def sections(text):
    out, cur = {}, None
    for line in text.splitlines():
        line = line.split(';', 1)[0].strip()
        if not line:
            continue
        m = re.match(r'^\[(.+?)\]$', line)
        if m:
            cur = m.group(1).strip().lower()
            out.setdefault(cur, [])
        elif cur is not None:
            out[cur].append(line)
    return out


def parse_driver(path):
    """-> (service, sysfile, [hardware ids], description) or None."""
    try:
        sec = sections(read_inf(path))
    except Exception:
        return None

    # Every model line under every manufacturer's model section.
    models = []
    for line in sec.get('manufacturer', []):
        if '=' not in line:
            continue
        rhs = line.split('=', 1)[1]
        parts = [p.strip() for p in rhs.split(',')]
        base = parts[0]
        # "%mfg% = Sec, NTx86" means the real section is Sec.NTx86
        for suffix in parts[1:] or ['']:
            name = (base + '.' + suffix if suffix else base).lower()
            if name in sec:
                models.append(name)
        if base.lower() in sec:
            models.append(base.lower())

    ids, install_secs = [], []
    for msec in dict.fromkeys(models):
        for line in sec.get(msec, []):
            if '=' not in line:
                continue
            rhs = [p.strip() for p in line.split('=', 1)[1].split(',')]
            if not rhs:
                continue
            install_secs.append(rhs[0].lower())
            for hw in rhs[1:]:
                hw = hw.strip()
                # Only PCI ids matter here, and only the VEN&DEV part: setup
                # matches the generic form, and listing every subsystem would
                # bloat txtsetup.sif for no gain.
                m = re.match(r'^PCI\\VEN_([0-9A-Fa-f]{4})&DEV_([0-9A-Fa-f]{4})',
                             hw, re.I)
                if m:
                    ids.append('PCI\\VEN_%s&DEV_%s'
                               % (m.group(1).upper(), m.group(2).upper()))
    if not ids:
        return None

    # install section -> .Services -> AddService -> ServiceBinary
    service = sysfile = None
    for isec in dict.fromkeys(install_secs):
        for cand in (isec + '.services', isec + '.ntx86.services', isec):
            for line in sec.get(cand, []):
                # [^,\s]+ for the section name, NOT \S+ : these lines often
                # carry a fourth field ("AddService=X,0x2,Y_Install, EventLog"),
                # and a greedy \S+ swallows the trailing comma, so the section
                # name never matches and the whole driver is silently skipped.
                # That cost 284 of 316 INFs on the first run, Silicon Image's
                # SATA controllers among them.
                m = re.match(r'AddService\s*=\s*([^,]+),([^,]*),\s*([^,\s]+)',
                             line, re.I)
                if not m:
                    continue
                svc, svcsec = m.group(1).strip(), m.group(3).strip().lower()
                for sl in sec.get(svcsec, []):
                    b = re.match(r'ServiceBinary\s*=\s*(.+)', sl, re.I)
                    if b:
                        sysfile = os.path.basename(b.group(1).strip().replace('\\', '/'))
                        service = svc
                        break
                if sysfile:
                    break
            if sysfile:
                break
        if sysfile:
            break
    if not (service and sysfile and sysfile.lower().endswith('.sys')):
        return None

    desc = os.path.splitext(os.path.basename(path))[0]
    return service, sysfile, sorted(set(ids)), desc


def load(path):
    with open(path, 'rb') as fh:
        return fh.read().decode('latin1')


def save(path, text):
    with open(path, 'wb') as fh:
        fh.write(text.encode('latin1'))


def add_lines(text, section, new_lines):
    """Append lines to a section, preserving CRLF, skipping duplicates."""
    pat = re.compile(r'(^\[' + re.escape(section) + r'\][^\r\n]*\r?\n)',
                     re.I | re.M)
    m = pat.search(text)
    if not m:
        return text, 0
    start = m.end()
    nxt = re.search(r'^\[', text[start:], re.M)
    end = start + (nxt.start() if nxt else len(text) - start)
    body = text[start:end]
    added = []
    for line in new_lines:
        key = line.split('=', 1)[0].strip().lower()
        if re.search(r'^\s*' + re.escape(key) + r'\s*=', body, re.I | re.M):
            continue
        added.append(line)
    if not added:
        return text, 0
    ins = ''.join(l + '\r\n' for l in added)
    return text[:end] + ins + text[end:], len(added)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('image', help='XP source root (holds I386 and $OEM$)')
    ap.add_argument('--limit', type=int, default=0,
                    help='stop after N drivers (for testing)')
    ap.add_argument('--dry-run', action='store_true')
    a = ap.parse_args()

    i386 = os.path.join(a.image, 'I386')
    sif = os.path.join(i386, 'txtsetup.sif')
    droot = os.path.join(a.image, '$OEM$', '$1', 'D')
    if not os.path.isfile(sif):
        print(f'no txtsetup.sif at {sif}', file=sys.stderr)
        return 2

    text = load(sif)
    if '\r\n' not in text:
        print('txtsetup.sif has no CRLF - refusing to touch it', file=sys.stderr)
        return 2

    mdirs = sorted(d for d in os.listdir(droot)
                   if d.startswith('M') and os.path.isdir(os.path.join(droot, d)))
    found, placed = {}, 0
    for d in mdirs:
        for fn in os.listdir(os.path.join(droot, d)):
            if not fn.lower().endswith('.inf'):
                continue
            got = parse_driver(os.path.join(droot, d, fn))
            if not got:
                continue
            service, sysfile, ids, desc = got
            src = None
            for cand in os.listdir(os.path.join(droot, d)):
                if cand.lower() == sysfile.lower():
                    src = os.path.join(droot, d, cand)
                    break
            if not src:
                continue                       # INF names a .sys it did not ship
            # First writer wins: the dirs are sorted, so this is deterministic.
            found.setdefault(service, (sysfile, ids, desc, src))

    if a.limit:
        found = dict(list(found.items())[:a.limit])

    src_lines, hwdb_lines, load_lines, scsi_lines = [], [], [], []
    for service, (sysfile, ids, desc, src) in sorted(found.items()):
        dst = os.path.join(i386, sysfile)
        if not a.dry_run and not os.path.exists(dst):
            shutil.copy2(src, dst)
            placed += 1
        src_lines.append('%s = 1,,,,,,,,3,3' % sysfile)
        load_lines.append('%s = %s,4' % (service, sysfile))
        scsi_lines.append('%s = "%s"' % (service, desc[:64]))
        for hw in ids:
            hwdb_lines.append('%s = "%s"' % (hw, service))

    counts = {}
    for sec, lines in (('SourceDisksFiles.x86', src_lines),
                       ('SourceDisksFiles', src_lines),
                       ('HardwareIdsDatabase', hwdb_lines),
                       ('SCSI.Load', load_lines),
                       ('SCSI', scsi_lines)):
        text, n = add_lines(text, sec, lines)
        if n:
            counts[sec] = n

    print(f'  mass-storage drivers found : {len(found)}')
    print(f'  .sys copied into I386      : {placed}')
    for k, v in counts.items():
        print(f'  [{k}] += {v}')
    if not a.dry_run:
        save(sif, text)
        print(f'  txtsetup.sif written ({len(text)} bytes)')
    return 0


if __name__ == '__main__':
    sys.exit(main())
