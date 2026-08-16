#!/usr/bin/env python3
"""Build a bare 1.44MB FAT12 floppy image (no bootsector code) from a file list.

Used to produce the SiI 3114 F6 driver floppy image for Windows XP setup.
Usage: mkfat12.py <out.img> <label> <file> [<file> ...]
"""
import os
import sys

SECTOR = 512
SECTORS = 2880
FAT_SECTORS = 9
NUM_FATS = 2
ROOT_ENTRIES = 224
ROOT_SECTORS = (ROOT_ENTRIES * 32) // SECTOR          # 14
DATA_START = 1 + NUM_FATS * FAT_SECTORS + ROOT_SECTORS  # sector 33


def boot_sector(label):
    b = bytearray(SECTOR)
    b[0:3] = b'\xeb\x3c\x90'
    b[3:11] = b'MSDOS5.0'
    b[11:13] = (SECTOR).to_bytes(2, 'little')
    b[13] = 1                                  # sectors per cluster
    b[14:16] = (1).to_bytes(2, 'little')       # reserved sectors
    b[16] = NUM_FATS
    b[17:19] = ROOT_ENTRIES.to_bytes(2, 'little')
    b[19:21] = SECTORS.to_bytes(2, 'little')
    b[21] = 0xF0                               # media descriptor: 1.44MB
    b[22:24] = FAT_SECTORS.to_bytes(2, 'little')
    b[24:26] = (18).to_bytes(2, 'little')      # sectors per track
    b[26:28] = (2).to_bytes(2, 'little')       # heads
    b[28:32] = (0).to_bytes(4, 'little')       # hidden sectors
    b[32:36] = (0).to_bytes(4, 'little')       # large sector count
    b[36] = 0x00                               # drive number
    b[38] = 0x29                               # extended boot signature
    b[39:43] = (0x53493331).to_bytes(4, 'little')
    b[43:54] = label.ljust(11)[:11].encode('ascii')
    b[54:62] = b'FAT12   '
    b[510:512] = b'\x55\xaa'
    return bytes(b)


def pack_fat(chain):
    """chain: list of next-cluster values indexed by cluster number."""
    raw = bytearray(FAT_SECTORS * SECTOR)
    for cl, val in enumerate(chain):
        off = cl + (cl >> 1)                   # cl * 3 // 2
        if cl & 1:
            raw[off] = (raw[off] & 0x0F) | ((val & 0x0F) << 4)
            raw[off + 1] = (val >> 4) & 0xFF
        else:
            raw[off] = val & 0xFF
            raw[off + 1] = (raw[off + 1] & 0xF0) | ((val >> 8) & 0x0F)
    return bytes(raw)


def short_name(name):
    base, _, ext = name.upper().partition('.')
    if len(base) > 8 or len(ext) > 3:
        raise SystemExit('not an 8.3 name: %s' % name)
    return base.ljust(8).encode('ascii') + ext.ljust(3).encode('ascii')


def dir_entry(name, size, first_cluster, attr=0x20):
    e = bytearray(32)
    e[0:11] = short_name(name)
    e[11] = attr
    e[22:24] = (0x6000).to_bytes(2, 'little')   # time 12:00:00
    e[24:26] = (0x5D0F).to_bytes(2, 'little')   # date 2026-08-15
    e[26:28] = first_cluster.to_bytes(2, 'little')
    e[28:32] = size.to_bytes(4, 'little')
    return bytes(e)


def main():
    out, label, files = sys.argv[1], sys.argv[2], sys.argv[3:]
    data = bytearray()
    root = bytearray()
    root += dir_entry(label if '.' in label else label + '.', 0, 0, attr=0x08)
    chain = [0xFF0, 0xFFF]                      # clusters 0 and 1 reserved
    next_cluster = 2

    for path in files:
        blob = open(path, 'rb').read()
        n = max(1, (len(blob) + SECTOR - 1) // SECTOR)
        first = next_cluster
        for i in range(n):
            chain.append(0xFFF if i == n - 1 else next_cluster + i + 1)
        next_cluster += n
        data += blob + b'\x00' * (n * SECTOR - len(blob))
        root += dir_entry(os.path.basename(path), len(blob), first)

    if next_cluster - 2 > SECTORS - DATA_START:
        raise SystemExit('files do not fit on a 1.44MB floppy')
    if len(root) > ROOT_ENTRIES * 32:
        raise SystemExit('too many root directory entries')

    fat = pack_fat(chain)
    img = bytearray(SECTORS * SECTOR)
    img[0:SECTOR] = boot_sector(label)
    img[SECTOR:SECTOR + len(fat)] = fat
    off = SECTOR * (1 + FAT_SECTORS)
    img[off:off + len(fat)] = fat
    off = SECTOR * (1 + NUM_FATS * FAT_SECTORS)
    img[off:off + len(root)] = root
    off = SECTOR * DATA_START
    img[off:off + len(data)] = data
    open(out, 'wb').write(img)
    print('wrote %s (%d files, %d/%d sectors used)'
          % (out, len(files), next_cluster - 2, SECTORS - DATA_START))


main()
