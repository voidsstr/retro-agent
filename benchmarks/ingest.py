#!/usr/bin/env python3
"""Ingest a bench_results.json (from the .124 suite runner) into the specpicks
production DB (retro_benchmark_machines / retro_benchmark_runs) and drop a
copy into benchmarks/.

Usage: python3 benchmarks/ingest.py <bench_results.json> [--driver-version X.Y.N]
DSN: env SPECPICKS_DATABASE_URL, or read from specpicks/CLAUDE.md convention.
"""
import json, os, re, sys, shutil, datetime

import psycopg2

DSN = os.environ.get(
    "SPECPICKS_DATABASE_URL",
    "postgresql://nscadmin:NscP0stgr3s!2026@nscappsdb.postgres.database.azure.com:5432/specpicks?sslmode=require",
)
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def main():
    path = sys.argv[1]
    data = json.load(open(path))
    ip = data["machine_ip"]

    # driver version: --driver-version flag, else parse from gl_renderer
    ver = None
    if "--driver-version" in sys.argv:
        ver = sys.argv[sys.argv.index("--driver-version") + 1]
    gl = data.get("driver", {}).get("gl_renderer") or ""
    m = re.search(r"\[retro3dfx ([0-9.]+)\]", gl)
    if not ver and m:
        ver = m.group(1)
    ver = ver or "unversioned"

    sysinfo = data.get("specs", {}).get("sysinfo", {}) or {}
    vdiag = data.get("specs", {}).get("videodiag", {}) or {}

    def flat(v):  # agent SYSINFO nests cpu/os/memory as dicts
        if isinstance(v, dict):
            return ", ".join(f"{k}={x}" for k, x in v.items() if not isinstance(x, (dict, list)))[:200]
        return v

    os_d = sysinfo.get("os") or {}
    mem_d = sysinfo.get("memory") or {}
    machine = {
        "ip": ip,
        "hostname": sysinfo.get("hostname") or sysinfo.get("computer_name"),
        "os": (f"{os_d.get('product','')} {os_d.get('version','')}".strip() if isinstance(os_d, dict) else os_d) or None,
        "cpu": sysinfo.get("cpu_name") or flat(sysinfo.get("cpu")),
        "ram_mb": (mem_d.get("total_mb") if isinstance(mem_d, dict) else None)
                  or sysinfo.get("memory_mb") or sysinfo.get("ram_mb"),
        "gpu": data.get("specs", {}).get("gpu_from_pciscan")
               or (vdiag.get("adapter") if isinstance(vdiag, dict) else None),
        "gpu_bus": vdiag.get("bus") if isinstance(vdiag, dict) else None,
    }

    conn = psycopg2.connect(DSN)
    conn.autocommit = True
    cur = conn.cursor()
    cur.execute(
        """INSERT INTO retro_benchmark_machines (ip, hostname, os, cpu, ram_mb, gpu, gpu_bus, specs)
           VALUES (%s,%s,%s,%s,%s,%s,%s,%s)
           ON CONFLICT (ip) DO UPDATE SET hostname=EXCLUDED.hostname, os=EXCLUDED.os,
             cpu=EXCLUDED.cpu, ram_mb=EXCLUDED.ram_mb, gpu=EXCLUDED.gpu,
             gpu_bus=EXCLUDED.gpu_bus, specs=EXCLUDED.specs, updated_at=now()
           RETURNING id""",
        (machine["ip"], machine["hostname"], machine["os"], machine["cpu"],
         machine["ram_mb"], machine["gpu"], machine["gpu_bus"],
         json.dumps(data.get("specs", {}))),
    )
    mid = cur.fetchone()[0]

    stack = {
        "display_driver": "AmigaMerlin 2.9 (retail)",
        "glide3x": "AmigaMerlin retail (underscore ABI)",
        "icd": "retro3dfx-gl (MesaFX 6.2 fork)",
        "icd_version": ver,
        "gl_renderer": gl or None,
    }

    n = 0
    for run in data.get("q3_timedemo", []):
        cur.execute(
            """INSERT INTO retro_benchmark_runs
               (machine_id, benchmark, settings, driver_stack, driver_version, result_fps, result, notes)
               VALUES (%s,%s,%s,%s,%s,%s,%s,%s)""",
            (mid, "q3-timedemo-four",
             json.dumps({"resolution": run["resolution"], "r_mode": run["mode"],
                         "colorbits": 16, "demo": run.get("demo"), "run_index": run["run"],
                         "q3_version": "1.32"}),
             json.dumps(stack), ver, run.get("fps"),
             json.dumps(run), "run 2 of 2 is official (run 1 warms caches)"),
        )
        n += 1
    gb = data.get("gfxbench")
    if gb and gb.get("csv"):
        rows = [r.split(",") for r in gb["csv"].splitlines()[1:] if r.count(",") >= 6]
        for r in rows:
            w, h, depth, fsaa, frames, ms, fps = r[:7]
            cur.execute(
                """INSERT INTO retro_benchmark_runs
                   (machine_id, benchmark, settings, driver_stack, driver_version, result_fps, result)
                   VALUES (%s,%s,%s,%s,%s,%s,%s)""",
                (mid, "gfxbench-sweep",
                 json.dumps({"resolution": f"{w}x{h}", "depth": int(depth),
                             "fsaa": int(fsaa), "frames": int(frames)}),
                 json.dumps({**stack, "note": "gfxbench_retail.exe direct-Glide micro-benchmark"}),
                 ver, float(fps), json.dumps({"ms": float(ms)})),
            )
            n += 1
    print(f"machine id={mid}, inserted {n} runs (driver {ver})")

    # repo copy
    date = datetime.date.today().isoformat()
    dst = os.path.join(REPO, "benchmarks", f"{ip}_{date}_retro3dfx-{ver}.json")
    shutil.copy(path, dst)
    print("copied ->", dst)
    conn.close()


if __name__ == "__main__":
    main()
