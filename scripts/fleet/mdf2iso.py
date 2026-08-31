#!/usr/bin/env python3
r"""mdf2iso.py - turn an Alcohol/Daemon-Tools ``.mdf`` (or any raw CD image)
into a plain 2048-byte-sector ``.iso`` that 7z, bsdtar and a loop mount can read.

    python3 scripts/fleet/mdf2iso.py Generals1.mdf out.iso
    python3 scripts/fleet/mdf2iso.py --probe Generals1.mdf     # just say what it is

WHY THIS EXISTS
---------------
This share is full of ``.mdf``/``.mds`` pairs, and **a raw CD image is not an
ISO**.  A Mode-1 sector on a pressed CD is 2352 bytes (12-byte sync + 4-byte
header + 2048 bytes of data + 288 bytes of ECC/EDC), and Daemon Tools writes an
extra 96 bytes of subchannel per sector on top of that, giving **2448**.  Only
the 2048-byte payload is the filesystem.

The trap that cost real time on 2026-08-30, staging C&C Generals: the ISO
Primary Volume Descriptor lives at logical sector 16, so on a plain ISO it is at
byte 32768 - and **everybody's first move is to `dd` at 32768 and look for
"CD001"**.  On a 2448-byte-sector `.mdf` there are zeros there, because sector
16's payload actually starts at ``16 * 2448 + 16 = 39184``.  The image reads as
empty and the natural conclusion is that it is corrupt or encrypted.  It is
neither.

So this tool **never assumes**: it searches the head of the file for the `CD001`
signature and derives the geometry from where it lands, which works for every
layout the fleet has met and fails loudly rather than writing a garbage ISO.

WHAT IT CANNOT DO
-----------------
It recovers the *filesystem*, not the *disc*.  DPM / weak-sector data - what a
SafeDisc or SecuROM title actually measures - is not in the payload and is not
in a small `.mds`, so **converting (or mounting) an image like this will never
satisfy a disc check**.  A `.mds` of a few hundred bytes is a data-only backup;
a real DPM-carrying one is kilobytes.  Check that before spending an afternoon
proving it on hardware.
"""
from __future__ import annotations

import argparse
import os
import sys

#: (sector size, payload offset within the sector).  Ordered widest-first so a
#: 2448 image is never mis-read as 2352 - the 2352 test would also "pass" on
#: some 2448 images at a different sector index, and the wrong stride silently
#: produces an ISO whose directory records drift.
GEOMETRIES = (
    (2448, 16),   # Daemon Tools .mdf: 2352 raw + 96 subchannel
    (2352, 16),   # raw Mode 1 / .bin
    (2352, 24),   # raw Mode 2 FORM 1 / .bin - 12 sync + 4 header + 8 SUBHEADER
    (2336, 8),    # Mode 2 Form 1, no sync/header
    (2048, 0),    # already a plain ISO
)

#: Why 2352/24 is its own entry, and why it sits AFTER 2352/16.
#:
#: A raw Mode 2 Form 1 sector is the same 2352 bytes as Mode 1, but carries an
#: extra **8-byte subheader** between the 4-byte header and the payload, so the
#: filesystem starts at +24 rather than +16.  Serious Sam's First and Second
#: Encounter discs are both pressed this way (``TRACK 01 MODE2/2352`` in their
#: own cue sheets), and reading them at +16 lands in the subheader: no `CD001`,
#: so the file looks like it has no filesystem at all.  Converting at the wrong
#: offset is worse - it writes a full-size ISO that nothing can mount, which
#: reads as "the image is corrupt" rather than "we used the wrong stride".
#:
#: The order is safe because the two cannot be confused: at 2352/16 a Mode 2
#: image exposes its subheader (four repeated bytes, never `\0CD001`), and at
#: 2352/24 a Mode 1 image exposes bytes 8..12 of the volume descriptor.  The
#: first geometry whose sector 16 really begins a volume descriptor wins.

PVD_LBA = 16          # ISO 9660 puts the Primary Volume Descriptor here
SIG = b"CD001"
SIG_OFF = 1           # "CD001" sits at byte 1 of the descriptor
PAYLOAD = 2048


def detect(fh) -> tuple[int, int]:
    """Return (sector_size, payload_offset) for an open binary file.

    Raises ValueError when no geometry puts a `CD001` where sector 16's payload
    would begin.  That is the honest answer for an audio-only disc, a partial
    download, or a format this does not know - and it is far better than
    guessing 2048 and emitting an ISO nobody can open.
    """
    for sector, off in GEOMETRIES:
        pos = PVD_LBA * sector + off + SIG_OFF
        fh.seek(pos)
        if fh.read(len(SIG)) == SIG:
            return sector, off
    raise ValueError(
        "no ISO 9660 volume descriptor found at sector 16 for any known "
        "geometry (tried %s). This may be an audio disc, a truncated image, "
        "or a format mdf2iso does not know."
        % ", ".join("%d/%d" % g for g in GEOMETRIES)
    )


def volume_label(fh, sector: int, off: int) -> str:
    """The PVD's volume identifier - the name the disc shows up as when mounted.

    Worth printing: it is how you confirm you converted the disc you meant to,
    and a mount launcher matches on it.
    """
    fh.seek(PVD_LBA * sector + off + 40)
    return fh.read(32).decode("latin-1").rstrip(" \0")


def convert(src: str, dst: str, sector: int, off: int, chunk_sectors: int = 512) -> int:
    """Copy every sector's 2048-byte payload out to `dst`.  Returns the count."""
    n = 0
    with open(src, "rb") as fh, open(dst, "wb") as out:
        while True:
            buf = fh.read(sector * chunk_sectors)
            if not buf:
                break
            if sector == PAYLOAD and off == 0:
                out.write(buf)
                n += len(buf) // PAYLOAD
                continue
            parts = []
            for i in range(0, len(buf) - sector + 1, sector):
                parts.append(buf[i + off:i + off + PAYLOAD])
                n += 1
            out.write(b"".join(parts))
    return n


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("image", help="the .mdf/.bin/.iso to read")
    ap.add_argument("out", nargs="?", help="the .iso to write (omit with --probe)")
    ap.add_argument("--probe", action="store_true",
                    help="report the geometry and volume label, write nothing")
    a = ap.parse_args(argv)

    with open(a.image, "rb") as fh:
        try:
            sector, off = detect(fh)
        except ValueError as e:
            print("mdf2iso: %s" % e, file=sys.stderr)
            return 2
        label = volume_label(fh, sector, off)

    size = os.path.getsize(a.image)
    print("%s: %d-byte sectors, payload at +%d, volume %r, %d sectors"
          % (a.image, sector, off, label, size // sector))
    if a.probe:
        return 0
    if not a.out:
        print("mdf2iso: no output path given (use --probe to only report)",
              file=sys.stderr)
        return 2
    n = convert(a.image, a.out, sector, off)
    print("%s: wrote %d sectors (%d bytes)" % (a.out, n, n * PAYLOAD))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
