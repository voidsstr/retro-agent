#!/usr/bin/env python3
"""
v56k_versions.py - capture WHAT WAS RUNNING, for a results set that predates
the version columns.

A benchmark number without its software versions cannot be compared against a
later one, and "which build was that?" becomes unanswerable within days. From
now on v56k_bench stamps the game binary, the driver files, the OS and the
agent onto every row and writes a versions.json beside the CSV. This tool does
the same job for results already taken.

    python3 v56k_versions.py --host 192.168.1.124 --outdir <results dir>
    python3 v56k_versions.py --host 192.168.1.124 --outdir <dir> --backfill

⚠️ A RETROACTIVE PROBE IS NOT A MEASUREMENT OF THE PAST. It reads the box AS IT
IS NOW and can only be trusted for rows taken while nothing changed in between.
So the sidecar it writes is explicitly marked `retroactive: true`, carries the
time it was taken, and `--backfill` fills only rows whose version fields are
EMPTY - it never overwrites a value captured at run time. Anything else would
quietly turn an assumption into a record.
"""

import argparse
import asyncio
import csv
import importlib.util
import json
import sys
from datetime import datetime, timezone
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
sys.path.insert(0, str(REPO))


def _bench():
    spec = importlib.util.spec_from_file_location("v56k_bench", HERE / "v56k_bench.py")
    m = importlib.util.module_from_spec(spec)
    sys.modules["v56k_bench"] = m
    spec.loader.exec_module(m)
    return m


bench = _bench()

VERSION_COLS = ["game_exe", "game_size", "game_md5", "driver_pkg", "driver_ver",
                "glide3x_md5", "icd_md5", "os_build", "agent_ver", "gpu"]


async def amain(a):
    outdir = Path(a.outdir)
    box = bench.Box(a.host)
    glide_key, inst = None, None
    inst, prof = await bench.find_display_instance(box)
    glide_key = (r"SYSTEM\CurrentControlSet\Control\Class"
                 r"\{4D36E968-E325-11CE-BFC1-08002BE10318}"
                 rf"\{inst}\Settings\Glide")
    print(f"display-class instance {inst}")

    v = await bench.collect_versions(box, glide_key)
    v["retroactive"] = True
    v["retroactive_note"] = (
        "Probed after the runs were recorded. Valid only for rows taken while "
        "these files did not change; it is not a measurement of the past.")
    v["probed"] = datetime.now(timezone.utc).isoformat()

    v["titles"] = {}
    for tid in a.titles.split(","):
        tid = tid.strip()
        if not tid:
            continue
        api = None
        if ":" in tid:
            tid, api = tid.split(":", 1)
        maker = bench.TITLES.get(tid)
        if not maker:
            print(f"  unknown title {tid!r}, skipping")
            continue
        t = maker(api) if api else maker()
        v["titles"][t.tid] = await bench.title_identity(box, t)
        ti = v["titles"][t.tid]
        print(f"  {t.name}: {ti.get('path','?')} {ti.get('size','?')} B "
              f"md5 {str(ti.get('md5','?'))[:12]}")

    for n, f in v["files"].items():
        print(f"  {n}: {f.get('size','?')} B  md5 {str(f.get('md5','?'))[:12]}")

    dest = outdir / "versions.json"
    dest.write_text(json.dumps(v, indent=2))
    print(f"wrote {dest}  (retroactive)")

    if a.backfill:
        n = backfill(outdir / "results.csv", v)
        print(f"backfilled {n} row(s) - only fields that were EMPTY")
    return 0


def backfill(csv_path, v):
    """Fill ONLY empty version cells. A value captured at run time is evidence;
    a retroactive guess is not, and must never overwrite one."""
    if not csv_path.exists():
        print(f"no {csv_path}")
        return 0
    bench.migrate_header(csv_path)
    with csv_path.open(newline="") as fh:
        rows = list(csv.DictReader(fh))
    files, dc = v.get("files", {}), v.get("display_class", {})
    filled = 0
    for r in rows:
        vals = {
            "driver_pkg": dc.get("DriverDesc", ""),
            "driver_ver": dc.get("DriverVersion", ""),
            "glide3x_md5": files.get("glide3x", {}).get("md5", ""),
            "icd_md5": files.get("icd", {}).get("md5", ""),
            "os_build": v.get("os_str") or v.get("os", ""),
            "agent_ver": v.get("agent_ver", ""),
            "gpu": (v.get("gpu") or {}).get("name", ""),
        }
        # the game binary depends on which title the row is, so match by tid
        for tid, ti in v.get("titles", {}).items():
            # A FAILED row can carry an empty engine field, so compare first
            # tokens defensively rather than indexing a split that may be empty.
            row_eng = (r.get("engine") or "").split()
            ti_eng = (ti.get("engine") or "").split()
            if row_eng and ti_eng and row_eng[0] == ti_eng[0]:
                vals.update({"game_exe": ti.get("path", ""),
                             "game_size": ti.get("size", ""),
                             "game_md5": ti.get("md5", "")})
        touched = False
        for k, val in vals.items():
            if k in r and not (r.get(k) or "").strip() and val:
                r[k] = val
                touched = True
        # say so in the row itself, so nobody mistakes it for run-time capture
        if touched:
            note = (r.get("notes") or "").strip()
            mark = "versions backfilled retroactively"
            if mark not in note:
                r["notes"] = (note + "; " if note else "") + mark
            filled += 1
    with csv_path.open("w", newline="") as fh:
        w = csv.DictWriter(fh, fieldnames=bench.CSV_COLS, extrasaction="ignore")
        w.writeheader()
        w.writerows(rows)
    return filled


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", required=True)
    ap.add_argument("--outdir", required=True)
    ap.add_argument("--titles", default="quake3,quake2,ut:glide")
    ap.add_argument("--backfill", action="store_true",
                    help="fill EMPTY version cells in results.csv and mark each "
                         "such row as backfilled")
    raise SystemExit(asyncio.run(amain(ap.parse_args())))


if __name__ == "__main__":
    main()
