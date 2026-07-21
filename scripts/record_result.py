#!/usr/bin/env python3
"""Insert a single benchmark result into specpicks (retro_benchmark_runs).
Used for collection games not yet wired into run_bench.py.

  python3 scripts/record_result.py <machine_ip> <benchmark> <resolution> <fps> \
      --renderer "..." --driver-version 0.1.31 --stack "..." --engine "..." --demo "..." --notes "..."
"""
import sys, os, json, argparse
sys.path.insert(0, os.path.join(os.path.dirname(__file__)))
from specpicks_dsn import resolve_dsn
import psycopg2

ap = argparse.ArgumentParser()
ap.add_argument("ip"); ap.add_argument("benchmark"); ap.add_argument("resolution")
ap.add_argument("fps", type=float)
ap.add_argument("--renderer", default=""); ap.add_argument("--driver-version", default="0.1.31", dest="dv")
ap.add_argument("--stack", default="retro3dfx-WFP"); ap.add_argument("--engine", default="")
ap.add_argument("--demo", default=""); ap.add_argument("--notes", default="")
a = ap.parse_args()

conn = psycopg2.connect(resolve_dsn()); cur = conn.cursor()
cur.execute("select id from retro_benchmark_machines where ip=%s", (a.ip,))
row = cur.fetchone()
mid = row[0] if row else None
if mid is None:
    raise SystemExit("machine %s not found; run run_bench once first" % a.ip)
settings = {"resolution": a.resolution, "engine": a.engine, "demo": a.demo,
            "renderer": a.renderer, "fsaa": "none (Voodoo3 has no T-buffer)"}
cur.execute(
    """INSERT INTO retro_benchmark_runs
       (machine_id, benchmark, settings, driver_stack, driver_version, result_fps,
        result, source, notes, lever)
       VALUES (%s,%s,%s,%s,%s,%s,%s,'driver-bench-skill',%s,'performance')""",
    (mid, a.benchmark, json.dumps(settings),
     json.dumps({"stack_composition": a.stack, "icd_version": a.dv, "gl_renderer": a.renderer}),
     a.dv, a.fps, json.dumps({"fps": a.fps}), a.notes))
conn.commit(); cur.close(); conn.close()
print("recorded: %s %s %s fps (renderer: %s)" % (a.benchmark, a.resolution, a.fps, a.renderer))
