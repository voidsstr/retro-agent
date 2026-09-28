#!/usr/bin/env python3
"""Rename WFP-protected display-driver DLLs so Windows File Protection cannot
revert them, patching every inter-DLL reference in place.

WHY THIS EXISTS
---------------
On Windows XP a third-party display driver whose package carries a stub or
invalid catalog is *unsigned*.  Windows File Protection silently restores the
in-box SP3 copy of every PROTECTED file in the set, which leaves a new miniport
driving an old display DLL.  The miniport says so in the System event log --
"You are running different builds of the miniport and display driver." -- and
the box falls back to VGA: 800x600 at 4bpp with exactly ONE enumerated mode,
while Device Manager still reports status OK / error_code 0.

Two obvious routes DO NOT work, both measured on .110 (Radeon 9500 Pro/9700,
Catalyst 10.2) on 2026-09-28:

  * replacing system32\\dllcache\\<file> and then system32\\<file> -- WFP
    validates the dllcache copy, rejects it, and re-extracts from sp3.cab,
    reverting BOTH within 30 seconds;
  * PendingFileRenameOperations -- the swap really happens at Session Manager
    time, and WFP reconciles it at startup and re-extracts anyway.

What works is giving the files names WFP has never heard of.  Replacement names
must be EXACTLY the same length as the originals so that references inside the
binaries can be overwritten in place with no relocation.

See fleetbook recipe `xp-unsigned-display-driver-wfp-reverts-protected-dlls-vga-fa`
for the registry half of the job, which this script deliberately does not do.
"""

import argparse
import os
import re
import struct
import sys


def _encode(name, wide):
    """Encode an ASCII name as it appears in a PE: raw, or UTF-16LE."""
    raw = name.encode("ascii")
    if not wide:
        return raw
    return b"".join(bytes([c]) + b"\x00" for c in raw)


def pe_checksum(data, ck_off):
    """The PE image checksum: 16-bit ones-complement sum with the checksum
    field zeroed, folded to 16 bits, plus the file size.

    These DLLs are loaded kernel-side by win32k, so a stale checksum is not
    cosmetic.  `verify_checksums()` proves this routine against pristine files
    before we rely on it -- a checksum routine that has not reproduced a known
    value is a guess.
    """
    buf = bytearray(data)
    buf[ck_off:ck_off + 4] = b"\x00\x00\x00\x00"
    if len(buf) % 2:
        buf.append(0)
    total = 0
    for i in range(0, len(buf), 2):
        total += struct.unpack_from("<H", buf, i)[0]
        total = (total & 0xFFFF) + (total >> 16)
    total = (total & 0xFFFF) + (total >> 16)
    return (total + len(data)) & 0xFFFFFFFF


def checksum_offset(data):
    lfanew = struct.unpack_from("<I", data, 0x3C)[0]
    if data[lfanew:lfanew + 4] != b"PE\0\0":
        raise ValueError("not a PE image")
    return lfanew + 24 + 64            # optional header + CheckSum field


def verify_checksums(paths):
    """Every pristine file's stored checksum must equal our computed one."""
    ok = True
    for path in paths:
        data = open(path, "rb").read()
        off = checksum_offset(data)
        stored = struct.unpack_from("<I", data, off)[0]
        calc = pe_checksum(data, off)
        if stored != calc:
            print("  CHECKSUM ROUTINE MISMATCH on %s: stored 0x%08x calc 0x%08x"
                  % (os.path.basename(path), stored, calc))
            ok = False
    return ok


def patch_one(data, name_map):
    """Replace every occurrence of every old name, ASCII and UTF-16LE, keeping
    the original letter case so a case-sensitive consumer still matches."""
    out = bytes(data)
    count = 0
    for old, new in name_map.items():
        for wide in (False, True):
            pattern = re.compile(re.escape(_encode(old, wide)), re.IGNORECASE)

            def replace(match, new=new, wide=wide):
                found = match.group(0)
                text = found.decode("utf-16-le") if wide else found.decode("ascii")
                return _encode(new.upper() if text.isupper() else new, wide)

            out, n = pattern.subn(replace, out)
            count += n
    return out, count


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--src", required=True,
                    help="directory holding the EXPANDED original DLLs")
    ap.add_argument("--out", required=True, help="directory to write renamed DLLs into")
    ap.add_argument("--map", required=True, action="append", metavar="OLD=NEW",
                    help="rename pair, repeatable; names must be the same length")
    args = ap.parse_args()

    name_map = {}
    for entry in args.map:
        if "=" not in entry:
            ap.error("--map wants OLD=NEW, got %r" % entry)
        old, new = entry.split("=", 1)
        old, new = old.strip(), new.strip()
        if len(old) != len(new):
            ap.error("%r and %r differ in length; an in-place patch needs equal "
                     "lengths" % (old, new))
        name_map[old] = new

    missing = [f for f in name_map if not os.path.isfile(os.path.join(args.src, f))]
    if missing:
        print("missing from --src: %s" % ", ".join(sorted(missing)))
        return 1

    srcs = [os.path.join(args.src, f) for f in name_map]
    print("validating the checksum routine against the pristine files")
    if not verify_checksums(srcs):
        print("REFUSING to patch: the checksum routine does not reproduce known "
              "values, so any checksum it writes would be a guess.")
        return 1
    print("  ok - reproduces all %d stored checksums exactly" % len(srcs))

    os.makedirs(args.out, exist_ok=True)
    for old, new in name_map.items():
        data = open(os.path.join(args.src, old), "rb").read()
        patched, refs = patch_one(data, name_map)
        if len(patched) != len(data):
            print("  SIZE CHANGED on %s - aborting" % old)
            return 1
        buf = bytearray(patched)
        off = checksum_offset(buf)
        before = struct.unpack_from("<I", buf, off)[0]
        struct.pack_into("<I", buf, off, pe_checksum(bytes(buf), off))
        after = struct.unpack_from("<I", buf, off)[0]
        open(os.path.join(args.out, new), "wb").write(bytes(buf))
        print("%s -> %s  %d bytes, %d ref(s), checksum 0x%08x -> 0x%08x"
              % (old, new, len(buf), refs, before, after))

    # A leftover reference to a protected name would silently pull the reverted
    # in-box DLL back into the stack, which looks exactly like no fix at all.
    print("confirming no old name survives in any output")
    bad = 0
    for new in name_map.values():
        data = open(os.path.join(args.out, new), "rb").read()
        for old in name_map:
            for wide in (False, True):
                if re.search(re.escape(_encode(old, wide)), data, re.IGNORECASE):
                    print("  !! %s still references %s (wide=%s)" % (new, old, wide))
                    bad += 1
    if bad:
        return 1
    print("  ok - clean")
    print("\nNow upload these to system32 and repoint the registry (five places:")
    print("both Class\\{4D36E968-...}\\NNNN\\Settings, both Control\\Video\\{GUID}\\0000,")
    print("Services\\<miniport>\\Device0) plus the OpenGLDrivers\\<newbase> ICD key.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
