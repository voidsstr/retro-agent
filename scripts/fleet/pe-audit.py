#!/usr/bin/env python3
"""pe-audit.py - sweep a staged game tree's PE binaries for the two faults that
reach a fleet box silently.

    python3 scripts/fleet/pe-audit.py <dir-or-file> [...]
    python3 scripts/fleet/pe-audit.py --json <dir>

FAIL conditions
---------------
1. SubsystemVersion >= 6.0.  Vista-only.  **XP's loader refuses the image before
   a single instruction runs** - no dialog that names the real cause, so it
   presents as "the game just does nothing".  GOG and re-release repacks are the
   usual offenders (SiN Gold shipped one and was unloadable on every XP box).
   This is checklist item 8 in CLAUDE.md's "Adding a NEW Staged Title".

2. An impossible PE TimeDateStamp.  A 1990s/2000s game binary whose stamp
   decodes outside roughly 1993..2038 has had that field overwritten, which in
   practice means a scene group used it as a watermark.  Halo PC's cracked
   halo.exe carried 0x21544c66, whose raw little-endian bytes spell "fLT!"
   (FAiRLiGHT) and which decodes to 1987.

The false positive this tool exists to NOT make
-----------------------------------------------
**"The TimeDateStamp bytes happen to be printable ASCII" is NOT evidence of
tampering on its own.**  In the very same Halo tree, haloupdate.exe (0x3f59224c
= 'L"Y?'), ogg.dll ('pQP?') and vorbis.dll ('8TP?') all have all-printable
stamps and perfectly correct 2003 dates - four bytes have a ~1-in-4 chance of
being printable, so in a 34-binary tree several will be.  Only a stamp that is
printable AND decodes to an impossible date is a finding.  This tool therefore
reports `ascii_stamp` as an observation and only ever FAILs on the date.

More generally: PE forensics without a same-product known-genuine control is
worthless.  "Clean section layout" and "import directory at the tail of .rdata"
were both cited as tamper evidence on this project and both turned out to be
identical in a genuine Microsoft control binary.
"""
from __future__ import annotations

import datetime
import json
import os
import struct
import sys

# PE TimeDateStamp is seconds since 1970.  Real build stamps for anything this
# project touches fall between 1993 and 2038; outside that the field was
# overwritten.
TDS_MIN = int(datetime.datetime(1993, 1, 1, tzinfo=datetime.timezone.utc).timestamp())
TDS_MAX = int(datetime.datetime(2038, 1, 1, tzinfo=datetime.timezone.utc).timestamp())

# XP's loader refuses an image whose SubsystemVersion is 6.0 or newer.
XP_MAX_SUBSYSTEM_MAJOR = 6


def parse_pe(data: bytes) -> dict | None:
    """Return the few PE header fields we judge on, or None if not a PE."""
    if len(data) < 0x40 or data[:2] != b"MZ":
        return None
    try:
        e = struct.unpack_from("<I", data, 0x3C)[0]
        if len(data) < e + 0x78 or data[e : e + 4] != b"PE\0\0":
            return None
        tds = struct.unpack_from("<I", data, e + 8)[0]
        raw = data[e + 8 : e + 12]
        opt = e + 24
        magic = struct.unpack_from("<H", data, opt)[0]
        if magic not in (0x10B, 0x20B):  # PE32 / PE32+
            return None
        sub_major, sub_minor = struct.unpack_from("<HH", data, opt + 48)
        subsystem = struct.unpack_from("<H", data, opt + 68)[0]
    except struct.error:
        return None
    return {
        "timedatestamp": tds,
        "timedatestamp_raw": raw.hex(),
        "subsystem_version": f"{sub_major}.{sub_minor}",
        "subsystem_version_major": sub_major,
        "subsystem": subsystem,
        "pe32plus": magic == 0x20B,
    }


def judge(pe: dict) -> dict:
    """Add verdict fields.  FAIL only on the two conditions in the docstring."""
    tds = pe["timedatestamp"]
    raw = bytes.fromhex(pe["timedatestamp_raw"])
    problems: list[str] = []

    if pe["subsystem_version_major"] >= XP_MAX_SUBSYSTEM_MAJOR:
        problems.append(
            f"SubsystemVersion {pe['subsystem_version']} is Vista-only; "
            "XP's loader refuses this image"
        )

    stamp_sane = TDS_MIN <= tds <= TDS_MAX
    if not stamp_sane:
        try:
            shown = datetime.datetime.fromtimestamp(
                tds, datetime.timezone.utc
            ).strftime("%Y-%m-%d")
        except (OverflowError, OSError, ValueError):
            shown = "unrepresentable"
        problems.append(
            f"TimeDateStamp {tds:#010x} decodes to {shown}, which is impossible "
            f"- the field was overwritten (raw bytes {raw!r})"
        )

    pe["date"] = (
        datetime.datetime.fromtimestamp(tds, datetime.timezone.utc).strftime("%Y-%m-%d")
        if stamp_sane
        else None
    )
    # Observation only.  Never a FAIL by itself - see the module docstring.
    pe["ascii_stamp"] = all(0x20 <= b < 0x7F for b in raw)
    pe["problems"] = problems
    pe["ok"] = not problems
    return pe


def walk(targets: list[str]) -> list[dict]:
    rows = []
    for target in targets:
        if os.path.isfile(target):
            paths = [target]
            root = os.path.dirname(target) or "."
        else:
            paths = []
            root = target
            for dirpath, _dirs, files in os.walk(target):
                paths.extend(os.path.join(dirpath, f) for f in sorted(files))
        for path in paths:
            try:
                with open(path, "rb") as fh:
                    head = fh.read(0x1000)
            except OSError:
                continue
            pe = parse_pe(head)
            if pe is None:
                continue
            pe["path"] = os.path.relpath(path, root)
            rows.append(judge(pe))
    return rows


def main(argv: list[str]) -> int:
    args = [a for a in argv[1:] if not a.startswith("--")]
    as_json = "--json" in argv
    if not args:
        print(__doc__.strip().splitlines()[0], file=sys.stderr)
        print(f"usage: {argv[0]} [--json] <dir-or-file> [...]", file=sys.stderr)
        return 2

    rows = walk(args)
    if as_json:
        print(json.dumps(rows, indent=2))
    else:
        for r in rows:
            mark = "ok  " if r["ok"] else "FAIL"
            ascii_note = "  [stamp is printable ASCII - not a fault by itself]" if (
                r["ascii_stamp"] and r["ok"]
            ) else ""
            print(
                f"{mark} {r['path']:44s} {r['date'] or 'IMPOSSIBLE':11s} "
                f"subsysver {r['subsystem_version']}{ascii_note}"
            )
            for p in r["problems"]:
                print(f"       -> {p}")
        bad = [r for r in rows if not r["ok"]]
        print(f"\n{len(rows)} PE binaries audited, {len(bad)} FAIL")
    return 1 if any(not r["ok"] for r in rows) else 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
