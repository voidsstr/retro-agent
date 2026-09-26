#!/usr/bin/env python3
"""vcrdump.py - what the driver was doing when XP bugchecked. No WinDbg.

The miniport keeps its flight recorder in non-paged pool, which a KERNEL
memory dump (CrashDumpEnabled=2) contains. This reads the dump header for the
bugcheck, then SCANS the file for the recorder's 16-byte magic - no symbols,
no address translation - and decodes the ring it finds. Entries are 128 bytes
and carry their own sequence number, so a ring split across non-contiguous
physical pages still sorts back into order; entries torn by the crash
(sequence 0) are dropped.

    vcrdump.py MEMORY.DMP [--tail 200]

XP minidumps (DumpType 4, ~64 KB) do not contain the ring; the tool says so.
"""
import argparse
import struct
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import vcrlog  # noqa: E402

MAGIC = b"VCRKMD-FLIGHTREC"
HDR_FMT = "<16s10I6I"            # vcr_log_header: magic + 10 fields + reserved[6]
ENTRY_FMT = "<IIHBBIIIII96s"     # vcr_log_entry
DUMP_TYPES = {1: "full", 2: "kernel", 4: "triage (minidump)"}


def dump_header(data):
    if data[:8] != b"PAGEDUMP":
        return None
    code, p1, p2, p3, p4 = struct.unpack_from("<5I", data, 0x28)
    dtype = struct.unpack_from("<I", data, 0xF88)[0]
    return {"bugcheck": code, "params": (p1, p2, p3, p4), "dump_type": dtype}


def find_rings(data):
    """Every recorder header in the file (a ring may appear more than once:
    a stale copy in freed pool, or the page file's older contents)."""
    rings, pos = [], 0
    while True:
        pos = data.find(MAGIC, pos)
        if pos < 0:
            return rings
        f = struct.unpack_from(HDR_FMT, data, pos)
        version, header_size, entry_size, nentries, next_seq = f[1:6]
        if version == 1 and header_size == 80 and entry_size == 128 and 0 < nentries <= 65536:
            rings.append({"offset": pos, "nentries": nentries, "next_seq": next_seq,
                          "boot_count": f[6], "drv_version": f[7],
                          "created": (f[9] << 32) | f[8], "dropped": f[10]})
        pos += 1


def read_entries(data, ring):
    """Entries that follow the header contiguously, plus any page-aligned 4 KB
    chunk elsewhere in the file that parses as a run of this ring's entries
    is NOT attempted: the ring is allocated in one piece and a kernel dump
    stores the pages of one allocation in physical order, which for pool is
    almost always contiguous. What cannot be read is reported, not guessed."""
    base = ring["offset"] + 80
    out = []
    for i in range(ring["nentries"]):
        off = base + i * 128
        if off + 128 > len(data):
            break
        seq, ms, code, src, level, pid, a, b, c, d, msg = struct.unpack_from(ENTRY_FMT, data, off)
        if seq == 0 or seq > ring["next_seq"] or (seq - 1) % ring["nentries"] != i:
            continue
        out.append({"seq": seq, "ms": ms, "code": code, "src": src, "level": level,
                    "pid": pid, "a": a, "b": b, "c": c, "d": d,
                    "msg": msg.split(b"\0", 1)[0].decode("ascii", "replace")})
    out.sort(key=lambda e: e["seq"])
    return out


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("dump")
    ap.add_argument("--tail", type=int, default=200)
    a = ap.parse_args(argv)
    data = Path(a.dump).read_bytes()
    h = dump_header(data)
    if h:
        print(f"bugcheck 0x{h['bugcheck']:08X} ({', '.join(f'0x{p:08X}' for p in h['params'])})"
              f"  dump type {h['dump_type']} = {DUMP_TYPES.get(h['dump_type'], '?')}")
        if h["dump_type"] == 4:
            print("a minidump holds no pool pages - set CrashDumpEnabled=2 for the recorder")
    else:
        print("no PAGEDUMP header - scanning the file anyway")
    rings = find_rings(data)
    if not rings:
        print("no vcr-kmd flight recorder found in this file")
        return 1
    events = vcrlog.load_events()
    best = max(rings, key=lambda r: r["next_seq"])
    for r in rings:
        print(f"ring at file offset 0x{r['offset']:x}: {r['nentries']} entries, next_seq "
              f"{r['next_seq']}, boot_count {r['boot_count']}"
              f"{'  <- newest' if r is best else ''}")
    entries = read_entries(data, best)
    print(f"# {len(entries)} readable entries; last {min(a.tail, len(entries))}:")
    for e in entries[-a.tail:]:
        print(vcrlog.format_entry(e, events))
    return 0


if __name__ == "__main__":
    sys.exit(main())
