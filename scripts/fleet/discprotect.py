#!/usr/bin/env python3
"""Answer TWO different questions about a copy-protected title, and keep them apart.

    python3 scripts/fleet/discprotect.py exe   <binary> [...]      # what protects it
    python3 scripts/fleet/discprotect.py image <bin-or-iso> [...]  # what the media carries

WHY BOTH, AND WHY THEY ARE NOT THE SAME QUESTION
------------------------------------------------
CLAUDE.md already tells you to read the SafeDisc VERSION before planning around
it, because the version decides whether the fleet's DAEMON Tools 3.47 can help.
That is necessary and it is not sufficient. Comanche 4 (2026-09-01) is the case
that showed why:

    C4setup\\C4.EXE   SafeDisc 2.40.011   - a version DAEMON Tools 3.x targets
    FLT-COM4.BIN     0 bad-EDC sectors   - and the media carries no protection

The game still answered "Cannot locate the CD-ROM" with the image mounted and
every emulation option on, because **DAEMON Tools' SafeDisc emulation REPLAYS a
protection the image must still carry. It cannot invent one.** The FLT/IGG
re-master is a clean data rip with the protection region stripped, which is
exactly why the release shipped a Crack folder.

So "the exe is a version we can emulate" is only half an answer. The other half
is whether the image is a raw dump that kept the deliberately-corrupt sectors.

HOW THE IMAGE CHECK WORKS
-------------------------
SafeDisc 2 authenticates the medium by reading sectors that MUST fail. In a
2352-byte raw (MODE1/2352) dump those survive as sectors whose stored EDC does
not match the EDC of their own bytes. So: recompute the Mode-1 EDC of every
sector and count the mismatches. Measured 2026-09-01:

    SystemShock2/_disc/System Shock 2 (USA).bin   284,667 sectors, 793 bad EDC
                                                  (673 runs, sectors 819-10058)
    MaxPayne/_disc/MaxPayne.bin                   357,635 sectors, 600 bad EDC
                                                  (536 runs, 347779-357324)
    Comanche.4/FLT-COM4.BIN                       358,557 sectors,   0 bad EDC

The first two are SafeDisc titles that WORK on this fleet under DAEMON Tools.
They are the control: without them, "0 bad sectors" would be an observation
rather than a finding.

A 2048-byte .iso cannot carry the protection AT ALL - it has no EDC field to be
wrong - and this tool says so rather than reporting a reassuring zero. That is
the same distinction the rest of the repo keeps between "absent" and "fine".
"""
from __future__ import annotations

import os
import struct
import sys

# The Mode-1/Mode-2-Form-1 EDC is a CRC-32 with the reversed polynomial
# 0x8001801B, i.e. 0xD8018001 in the shift-right form used here. It covers
# bytes 0..2063 of the sector (sync + header + user data) and is stored
# little-endian at offset 2064.
_EDC_TABLE = []
for _i in range(256):
    _e = _i
    for _ in range(8):
        _e = (_e >> 1) ^ (0xD8018001 if _e & 1 else 0)
    _EDC_TABLE.append(_e)

RAW_SECTOR = 2352
EDC_OFFSET = 2064
EDC_COVERS = 2064
SYNC = b"\x00" + b"\xff" * 10 + b"\x00"

SAFEDISC_MARKER = b"BoG_ *90.0&!!"
#: The version is three little-endian dwords at MARKER + 0x20. It is NOT at a
#: fixed file offset - see tests/python/test_safedisc_version_offset.py, which
#: exists because CLAUDE.md once said 0xfd4 and that was the address the field
#: happened to land at in the first two binaries anyone measured.
SAFEDISC_VERSION_AT = 0x20


def edc(buf: bytes) -> int:
    """The CD-ROM Mode-1 EDC of `buf`."""
    e = 0
    for b in buf:
        e = (e >> 8) ^ _EDC_TABLE[(e ^ b) & 0xFF]
    return e


def sector_edc_ok(sector: bytes) -> bool:
    """True when a 2352-byte Mode-1 sector's stored EDC matches its contents."""
    if len(sector) != RAW_SECTOR:
        raise ValueError("a raw sector is %d bytes, got %d"
                         % (RAW_SECTOR, len(sector)))
    stored = struct.unpack_from("<I", sector, EDC_OFFSET)[0]
    return edc(sector[:EDC_COVERS]) == stored


def safedisc_version(path):
    """(major, minor, subminor) for a SafeDisc-wrapped PE, else None.

    Returns None both for "not SafeDisc" and for "not a file we can read" -
    callers that need to tell those apart should check the path themselves.
    """
    try:
        with open(path, "rb") as fh:
            data = fh.read()
    except OSError:
        return None
    i = data.find(SAFEDISC_MARKER)
    if i < 0:
        return None
    off = i + SAFEDISC_VERSION_AT
    if off + 12 > len(data):
        return None
    return struct.unpack_from("<III", data, off)


class ImageReport:
    """What an image says about the protection it is carrying.

    `kind` is deliberately three-valued, never two:
      'raw'    a 2352-byte-sector dump - the only kind that CAN carry it
      'iso'    2048-byte sectors, so there is no EDC field to be wrong; the
               protection is not merely absent, it is unrepresentable
      'other'  a size that is neither, so we did not measure anything
    """

    __slots__ = ("path", "kind", "sectors", "bad_edc", "runs", "first", "last",
                 "bad_sync")

    def __init__(self, path, kind, sectors=0, bad_edc=0, runs=0,
                 first=None, last=None, bad_sync=0):
        self.path, self.kind, self.sectors = path, kind, sectors
        self.bad_edc, self.runs = bad_edc, runs
        self.first, self.last, self.bad_sync = first, last, bad_sync

    @property
    def carries_protection(self):
        """True only for a raw image with at least one deliberately-bad sector.

        An .iso answers False, and that is not the same fact as a raw image
        answering False - read `kind` before quoting this.
        """
        return self.kind == "raw" and self.bad_edc > 0

    def __str__(self):
        if self.kind == "iso":
            return ("%s: 2048-byte ISO - CANNOT carry a SafeDisc protection "
                    "region (no EDC field)" % os.path.basename(self.path))
        if self.kind == "other":
            return ("%s: size is neither a multiple of 2352 nor of 2048 - not "
                    "measured" % os.path.basename(self.path))
        where = ("" if self.first is None
                 else "  (%d runs, sectors %d-%d)" % (self.runs, self.first,
                                                      self.last))
        return ("%s: %d sectors, %d bad EDC%s, %d bad sync  ->  %s"
                % (os.path.basename(self.path), self.sectors, self.bad_edc,
                   where, self.bad_sync,
                   "carries a protection region"
                   if self.carries_protection else
                   "NO protection region - a clean rip"))


def scan_image(path, max_sectors=None):
    """Count the sectors whose EDC does not verify.

    `max_sectors` bounds the read: the fleet's images live on a CIFS share and
    a test that pulls 700 MB across it every run will be turned off, which is
    worse than a bounded one. The protection region is early on some discs and
    late on others, so a bounded scan can only ever CONFIRM protection, never
    refute it - the caller has to know that, which is why the parameter is
    explicit rather than a default.
    """
    size = os.path.getsize(path)
    if size % RAW_SECTOR:
        return ImageReport(path, "iso" if size % 2048 == 0 else "other")

    total = size // RAW_SECTOR
    limit = total if max_sectors is None else min(total, max_sectors)
    bad = runs = bad_sync = 0
    first = last = None
    prev = -2
    with open(path, "rb") as fh:
        n = 0
        while n < limit:
            chunk = fh.read(RAW_SECTOR * 512)
            if not chunk:
                break
            for off in range(0, len(chunk) - RAW_SECTOR + 1, RAW_SECTOR):
                if n >= limit:
                    break
                sec = chunk[off:off + RAW_SECTOR]
                if sec[:12] != SYNC:
                    bad_sync += 1
                if not sector_edc_ok(sec):
                    bad += 1
                    if first is None:
                        first = n
                    last = n
                    if n != prev + 1:
                        runs += 1
                    prev = n
                n += 1
    return ImageReport(path, "raw", sectors=limit, bad_edc=bad, runs=runs,
                       first=first, last=last, bad_sync=bad_sync)


def main(argv=None):
    argv = list(sys.argv[1:] if argv is None else argv)
    if len(argv) < 2 or argv[0] not in ("exe", "image"):
        print(__doc__.strip().splitlines()[0], file=sys.stderr)
        print("usage: discprotect.py exe <binary> [...]", file=sys.stderr)
        print("       discprotect.py image <bin-or-iso> [...]", file=sys.stderr)
        return 2
    mode, paths = argv[0], argv[1:]
    for p in paths:
        if mode == "exe":
            v = safedisc_version(p)
            print("%-50s %s" % (os.path.basename(p),
                                "SafeDisc %d.%02d.%03d" % v if v
                                else "no SafeDisc marker"))
        else:
            print(scan_image(p))
    return 0


if __name__ == "__main__":
    sys.exit(main())
