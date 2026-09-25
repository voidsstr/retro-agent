#!/usr/bin/env python3
"""icdprof.py - read a voodoo-cleanroom ICD profile (RETROGL_PROF) and name
where the render thread's CPU time went.

The ICD's fxprof.c (0.1.67+) samples the render thread's EIP about once a
millisecond while frames are being swapped, and at exit writes

    # retrogl fxprof v1 samples=N idle=I dropped=D
    <eip> <count> <allocation base> <module path>

Our Glide and our ICD keep their symbol tables, so an address inside either is
resolved here with the target toolchain's nm. Everything else (the game, the
OS) is reported per module: "18 % in quake2.exe" is still an answer.

    icdprof.py prof.txt --dll retrogl.dll=voodoo-cleanroom/out/opengl32_retail_v0.1.67.dll \\
                        --dll glide3x.dll=voodoo-cleanroom/out/glide3x_h5_x86.dll
    icdprof.py --fetch 192.168.1.124 'C:\\Games\\Quake2Complete\\prof.txt' --out prof.txt ...
"""
import argparse
import asyncio
import collections
import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parents[1]))

_HDR = re.compile(r"samples=(\d+)\s+idle=(\d+)\s+dropped=(\d+)")


def parse_profile(text):
    """-> (header dict, [(eip, count, base, module basename lower)])"""
    hdr, rows = {}, []
    for line in text.splitlines():
        if line.startswith("#"):
            m = _HDR.search(line)
            if m:
                hdr = {"samples": int(m.group(1)), "idle": int(m.group(2)),
                       "dropped": int(m.group(3))}
            continue
        parts = line.split(None, 3)
        if len(parts) < 4:
            continue
        mod = parts[3].strip().replace("\\", "/").rsplit("/", 1)[-1].lower()
        rows.append((int(parts[0], 16), int(parts[1]), int(parts[2], 16), mod))
    return hdr, rows


def summarize(rows, tables):
    """tables: module basename (lower) -> [(rva, symbol)] sorted.
    -> (total, Counter module->count, Counter 'module!symbol'->count)"""
    from v56k_diag import nearest
    total = sum(c for _, c, _, _ in rows)
    per_mod = collections.Counter()
    per_fn = collections.Counter()
    for eip, cnt, base, mod in rows:
        per_mod[mod] += cnt
        syms = tables.get(mod)
        if syms and base:
            s = nearest(syms, eip - base)
            per_fn[f"{mod}!{s[1] if s else '?'}"] += cnt
        else:
            per_fn[f"{mod}!?"] += cnt
    return total, per_mod, per_fn


def report(hdr, rows, tables, top=40):
    total, per_mod, per_fn = summarize(rows, tables)
    out = [f"samples {total} (header: {hdr})", "", "by module:"]
    for mod, c in per_mod.most_common():
        out.append(f"  {100.0 * c / max(total, 1):5.1f} %  {c:7d}  {mod}")
    out += ["", f"top {top} functions:"]
    for fn, c in per_fn.most_common(top):
        out.append(f"  {100.0 * c / max(total, 1):5.1f} %  {c:7d}  {fn}")
    return "\n".join(out) + "\n"


async def fetch(host, remote, local):
    from v56k_bench import Box
    data = await Box(host).download(remote)
    Path(local).write_bytes(data or b"")
    return len(data or b"")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("profile", nargs="?")
    ap.add_argument("--fetch", nargs=2, metavar=("HOST", "REMOTE"))
    ap.add_argument("--out")
    ap.add_argument("--dll", action="append", default=[],
                    help="module.dll=local unstripped copy (repeatable)")
    ap.add_argument("--top", type=int, default=40)
    a = ap.parse_args()
    path = a.profile
    if a.fetch:
        path = a.out or "prof.txt"
        n = asyncio.run(fetch(a.fetch[0], a.fetch[1], path))
        print(f"fetched {n} bytes -> {path}")
    if not path:
        ap.error("profile path required")
    from v56k_diag import nm_table
    tables = {}
    for spec in a.dll:
        mod, local = spec.split("=", 1)
        tables[mod.lower()] = nm_table(local)
    hdr, rows = parse_profile(Path(path).read_text(errors="replace"))
    sys.stdout.write(report(hdr, rows, tables, a.top))


if __name__ == "__main__":
    main()
