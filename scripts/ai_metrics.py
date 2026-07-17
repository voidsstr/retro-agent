#!/usr/bin/env python3
"""Fleet AI metrics logging to the specpicks Postgres DB (roadmap M8, wired
early per docs/roadmap-fleet-ai.md — every train/infer run lands here from M1
onward).

Table `ai_runs` mirrors the driver-bench `retro_benchmark_runs` pattern:
one row per run keyed by model x machine x backend x precision, JSON metrics
blob, FK into the shared `retro_benchmark_machines` table.

CLI:
  python3 scripts/ai_metrics.py init                 # create table
  python3 scripts/ai_metrics.py log --ip .. --model .. --phase infer \
      --backend cpu-sse --precision i8 --metrics '{"top1":0.97}' [...]
  python3 scripts/ai_metrics.py board [--model X]    # leaderboard
"""
import argparse
import json
import os
import sys

import psycopg2

DSN = os.environ.get(
    "SPECPICKS_DATABASE_URL",
    "postgresql://nscadmin:NscP0stgr3s!2026@nscappsdb.postgres.database.azure.com:5432/specpicks?sslmode=require",
)

DDL = """
CREATE TABLE IF NOT EXISTS ai_runs (
    id          serial PRIMARY KEY,
    machine_id  integer REFERENCES retro_benchmark_machines(id),
    model       text NOT NULL,
    phase       text NOT NULL,             -- train | infer | bench
    backend     text NOT NULL,             -- cpu-scalar|cpu-sse|cpu-3dnow|cpu-mmx|glide-mac|nv-combiner|nv-shader|fleet-*
    precision   text NOT NULL,             -- f32 | i8 | i4 | bin | mixed
    engine_ver  text,
    dataset     text,
    metrics     jsonb NOT NULL,            -- full metric set (acc/f1/auc/rmse/ppl/img_s/latency/energy...)
    settings    jsonb,                     -- hyperparams / run config
    notes       text,
    source      text DEFAULT 'retro-infer',
    created_at  timestamptz DEFAULT now()
);
CREATE INDEX IF NOT EXISTS ai_runs_board
    ON ai_runs (model, backend, precision);
"""


def connect():
    conn = psycopg2.connect(DSN)
    conn.autocommit = True
    return conn


def machine_id(cur, ip, hostname=None, os_str=None, cpu=None, ram_mb=None,
               gpu=None):
    cur.execute(
        """INSERT INTO retro_benchmark_machines (ip, hostname, os, cpu, ram_mb, gpu)
           VALUES (%s, %s, %s, %s, %s, %s)
           ON CONFLICT (ip) DO UPDATE SET
             hostname = COALESCE(EXCLUDED.hostname, retro_benchmark_machines.hostname),
             os = COALESCE(EXCLUDED.os, retro_benchmark_machines.os),
             cpu = COALESCE(EXCLUDED.cpu, retro_benchmark_machines.cpu),
             ram_mb = COALESCE(EXCLUDED.ram_mb, retro_benchmark_machines.ram_mb),
             gpu = COALESCE(EXCLUDED.gpu, retro_benchmark_machines.gpu),
             updated_at = now()
           RETURNING id""",
        (ip, hostname, os_str, cpu, ram_mb, gpu),
    )
    return cur.fetchone()[0]


def log_run(ip, model, phase, backend, precision, metrics, settings=None,
            engine_ver=None, dataset=None, notes=None, hostname=None,
            cpu=None, gpu=None, source="retro-infer"):
    """Log one run; returns the ai_runs row id."""
    conn = connect()
    try:
        cur = conn.cursor()
        mid = machine_id(cur, ip, hostname=hostname, cpu=cpu, gpu=gpu)
        cur.execute(
            """INSERT INTO ai_runs (machine_id, model, phase, backend, precision,
                                    engine_ver, dataset, metrics, settings, notes, source)
               VALUES (%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s) RETURNING id""",
            (mid, model, phase, backend, precision, engine_ver, dataset,
             json.dumps(metrics), json.dumps(settings or {}), notes, source),
        )
        return cur.fetchone()[0]
    finally:
        conn.close()


def leaderboard(model=None, metric="img_per_sec"):
    conn = connect()
    try:
        cur = conn.cursor()
        q = """SELECT m.ip, r.model, r.backend, r.precision,
                      r.metrics->>%s AS val, r.created_at
               FROM ai_runs r JOIN retro_benchmark_machines m ON m.id = r.machine_id
               WHERE r.metrics ? %s"""
        params = [metric, metric]
        if model:
            q += " AND r.model = %s"
            params.append(model)
        q += " ORDER BY (r.metrics->>%s)::float DESC LIMIT 30"
        params.append(metric)
        cur.execute(q, params)
        return cur.fetchall()
    finally:
        conn.close()


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("init")
    lg = sub.add_parser("log")
    lg.add_argument("--ip", required=True)
    lg.add_argument("--model", required=True)
    lg.add_argument("--phase", required=True, choices=["train", "infer", "bench"])
    lg.add_argument("--backend", required=True)
    lg.add_argument("--precision", required=True)
    lg.add_argument("--metrics", required=True, help="JSON object")
    lg.add_argument("--settings", default="{}")
    lg.add_argument("--engine-ver")
    lg.add_argument("--dataset")
    lg.add_argument("--notes")
    lg.add_argument("--cpu")
    lg.add_argument("--gpu")
    bd = sub.add_parser("board")
    bd.add_argument("--model")
    bd.add_argument("--metric", default="img_per_sec")
    args = ap.parse_args()

    if args.cmd == "init":
        conn = connect()
        conn.cursor().execute(DDL)
        conn.close()
        print("ai_runs table ready")
    elif args.cmd == "log":
        rid = log_run(args.ip, args.model, args.phase, args.backend,
                      args.precision, json.loads(args.metrics),
                      settings=json.loads(args.settings),
                      engine_ver=args.engine_ver, dataset=args.dataset,
                      notes=args.notes, cpu=args.cpu, gpu=args.gpu)
        print(f"ai_runs id={rid}")
    elif args.cmd == "board":
        for row in leaderboard(args.model, args.metric):
            print(" | ".join(str(x) for x in row))


if __name__ == "__main__":
    main()
