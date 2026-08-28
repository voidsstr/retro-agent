#!/usr/bin/env python3
"""The image's DevicePath must decode as ANSI, the way regedit reads REGEDIT4.

WHY THIS TEST EXISTS. DevicePath tells Windows PnP where to look for drivers.
It was written into retroagent.reg as a hex(2) value encoded UTF-16LE - the
convention for .reg version 5 files. But retroagent.reg is REGEDIT4, an ANSI
format, so regedit read '%', hit the 00 byte that followed it, and stored a
DevicePath of exactly one character. PnP therefore had no driver search path at
all, and a Dell Dimension 3000 came up on the VGA fallback at 640x480 in 16
colours with its Intel 865G driver sitting unused on its own disk.

The failure was invisible from the build side twice over: regedit merged the
file without complaint, and the obvious way to check the value - decode the hex
as UTF-16 - is the same mistake that produced it, so it looked perfect. This
test decodes it as ANSI, which is what regedit does, and asserts the UTF-16
reading is NOT what comes out.
"""
import os
import re
import sys

REG = ('/mnt/retro-share/Files/OS/XPSP3-FLEET/$OEM$/retroagent.reg')
fails = []


def check(label, cond):
    print(f'  {"PASS" if cond else "FAIL"}  {label}')
    if not cond:
        fails.append(label)


def main():
    if not os.path.isfile(REG):
        print(f'  SKIP  {REG} not present (share not mounted)')
        return 0

    with open(REG, encoding='latin1') as fh:
        text = fh.read()

    print('== the file is REGEDIT4, so its hex(2) values are ANSI ==')
    check('retroagent.reg is REGEDIT4', text.startswith('REGEDIT4'))

    m = re.search(r'"DevicePath"=hex\(2\):([0-9a-f,\r\n\\ ]+)', text)
    check('DevicePath is present', m is not None)
    if not m:
        return 1

    raw = [b for b in re.sub(r'[\r\n\\ ]', '', m.group(1)).split(',') if b]
    by = bytes(int(b, 16) for b in raw)

    # What regedit actually stores.
    ansi = by.decode('latin1').rstrip('\x00')
    print('== decoded the way regedit reads it ==')
    check('starts with %SystemRoot%\\inf so Windows keeps its own driver store',
          ansi.startswith('%SystemRoot%\\inf'))
    check('holds the whole path, not just one character', len(ansi) > 100)
    check('lists many driver directories', len(ansi.split(';')) > 50)
    check('paths are absolute - PnP does not resolve relative ones',
          'C:\\D\\' in ansi)
    check('no interior NUL truncates it', '\x00' not in ansi)

    # The specific regression: a UTF-16 encoding would put a 00 after byte one.
    print('== the exact bug this file used to have ==')
    check('second byte is NOT 00 (that is the UTF-16 mistake)',
          len(by) > 1 and by[1] != 0)
    check('what regedit stores is longer than one character',
          len(by.decode('latin1').split('\x00')[0]) > 1)

    # And the drivers it names must be real, or the path is decoration.
    d = '/mnt/retro-share/Files/OS/XPSP3-FLEET/$OEM$/$1/D'
    if os.path.isdir(d):
        entries = [e for e in ansi.split(';') if e.startswith('C:\\D\\')]
        missing = [e for e in entries
                   if not os.path.isdir(os.path.join(d, e.split('\\')[-1]))]
        check(f'every one of the {len(entries)} listed dirs exists on the share',
              not missing)

    print(f'\npxe DevicePath: {"all checks passed" if not fails else str(len(fails)) + " FAILED"}')
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
