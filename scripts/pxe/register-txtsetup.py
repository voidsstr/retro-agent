#!/usr/bin/env python3
"""Register injected drivers in txtsetup.sif so text-mode setup can see them.

WHY THIS EXISTS. Copying a NIC driver's .inf and .sys into I386 is necessary
but NOT sufficient, and the difference is invisible until a machine boots.
Text-mode setup does not enumerate the directory - it only knows the files
listed in txtsetup.sif's [SourceDisksFiles.x86]. An unlisted driver may as well
not be on the media.

That is what kept failing here. inject-drivers.sh placed 54 driver files in
I386 and 52 of them were never listed, so the Gateway 550 went on reporting
"The operating system image you selected does not have the required drivers"
against an image that physically contained its driver. Worse, the image
verified clean by every check that had been applied to it - including a
byte-comparison confirming txtsetup.sif matched the pristine media, which was
precisely the defect being read as reassurance.

NICs are matched through these INF listings, not through [HardwareIdsDatabase]:
retail XP SP3 has 224 entries there and not one is a network adapter - they are
all storage (intelide and friends). So the registration this writes is the only
mechanism that makes an injected NIC reachable.

The entry formats are copied from the media's own bundled NIC drivers rather
than invented:
    e100b325.sys = 1,,,,,,,,3,3,,1,4      (a driver binary)
    3dfxvs2k.inf = 1,,,,,,,20,0,0         (an INF)
Disk id 1 is the install source itself.

CRLF is preserved byte for byte. A txtsetup.sif with a stray LF is how you get
"txtsetup.sif is corrupt or missing" and a dead boot, so this never rewrites a
line it did not add.
"""
import argparse
import os
import shutil
import sys

SECTION = '[SourceDisksFiles.x86]'
FMT = {'.sys': '{} = 1,,,,,,,,3,3,,1,4',
       '.inf': '{} = 1,,,,,,,20,0,0'}


def load(path):
    with open(path, 'rb') as fh:
        return fh.read()


def already_listed(text_lower, name):
    """True if any line already declares this file, in any section."""
    needle = name.lower()
    for line in text_lower.splitlines():
        s = line.strip()
        if not s or s.startswith(';') or '=' not in s:
            continue
        if s.split('=', 1)[0].strip() == needle:
            return True
    return False


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('txtsetup')
    ap.add_argument('names', nargs='*',
                    help='driver file names to register (basenames)')
    ap.add_argument('--from-file', help='read names one per line')
    ap.add_argument('--dry-run', action='store_true')
    args = ap.parse_args()

    names = list(args.names)
    if args.from_file:
        with open(args.from_file, encoding='ascii') as fh:
            names += [l.strip() for l in fh if l.strip()]
    names = [n for n in names if os.path.splitext(n)[1].lower() in FMT]
    if not names:
        print('nothing to register')
        return 0

    raw = load(args.txtsetup)
    text = raw.decode('latin1')
    lower = text.lower()

    # Locate the section header line, then the end of that section.
    idx = lower.find(SECTION.lower())
    if idx < 0:
        print(f'FATAL: {SECTION} not found in {args.txtsetup}', file=sys.stderr)
        return 1
    lines = text.split('\n')          # keep '\r' attached to each line
    start = None
    for i, line in enumerate(lines):
        if line.strip().lower() == SECTION.lower():
            start = i
            break
    if start is None:
        print(f'FATAL: {SECTION} is not on a line of its own', file=sys.stderr)
        return 1
    end = len(lines)
    for i in range(start + 1, len(lines)):
        if lines[i].strip().startswith('['):
            end = i
            break

    todo, skipped = [], []
    for n in names:
        (skipped if already_listed(lower, n) else todo).append(n)

    eol = '\r' if lines[start].endswith('\r') else ''
    new = [FMT[os.path.splitext(n)[1].lower()].format(n) + eol for n in todo]

    print(f'{SECTION} spans lines {start + 1}..{end}')
    print(f'  already listed : {len(skipped)}')
    print(f'  to register    : {len(todo)}')
    if args.dry_run:
        for l in new[:8]:
            print('   +', l.rstrip('\r'))
        return 0
    if not todo:
        print('nothing to do - already registered')
        return 0

    # Insert at the end of the section, before the next header.
    while end > start + 1 and lines[end - 1].strip() == '':
        end -= 1
    out = lines[:end] + new + lines[end:]
    body = '\n'.join(out)

    backup = args.txtsetup + '.preinject'
    if not os.path.exists(backup):
        shutil.copy2(args.txtsetup, backup)
        print(f'  backup         : {backup}')

    data = body.encode('latin1')
    tmp = args.txtsetup + '.tmp'
    with open(tmp, 'wb') as fh:
        fh.write(data)
    os.replace(tmp, args.txtsetup)

    # Validate rather than trust: a broken txtsetup.sif kills the boot with
    # "corrupt or missing" and no clue which change did it.
    check = load(args.txtsetup).decode('latin1')
    bad = [n for n in todo if not already_listed(check.lower(), n)]
    lone_lf = sum(1 for i, l in enumerate(check.split('\n')[:-1])
                  if not l.endswith('\r'))
    print(f'  registered     : {len(todo) - len(bad)}/{len(todo)}')
    print(f'  LF-only lines  : {lone_lf} (must be 0)')
    if bad or lone_lf:
        print('FAILED validation - restoring backup', file=sys.stderr)
        shutil.copy2(backup, args.txtsetup)
        return 1
    print('  ok')
    return 0


if __name__ == '__main__':
    sys.exit(main())
