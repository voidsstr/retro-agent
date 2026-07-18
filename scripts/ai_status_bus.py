#!/usr/bin/env python3
"""Fleet AI status bus — synthesized "live telemetry" for the operator
console (retro_infer_console.py).

Why this exists: the agent/engine protocol (agent/src/ai.c, retro-infer/src/
serve.c) is strict blocking request/response, single connection, with no
progress-poll or streaming primitive at all — you cannot ask "what's the
current loss" while an NTSTEP/INFER_RUN call is in flight, only after it
returns. Rearchitecting that protocol to be async/streaming is out of scope.
Instead, every orchestration script (retro_ai_fleet.py's dp-train/dp-infer,
retro_ai_pipeline.py, retro_infer_console.py's inline infer/bench actions)
already knows its own step boundaries, timing, and batch sizes — so each one
publishes a small JSON status blob here as it runs, and the console polls
this directory on a timer to render live panels. This is honest "real-time"
in the sense that it reflects what the orchestrator observed a few hundred
milliseconds ago, not a true push/streaming channel.

Layout: one file per run, /tmp/retro-ai/status/<run_id>.json (override with
RETRO_AI_STATUS_DIR), written via write-tmp + os.replace so a reader never
sees a torn write. Mirrors the existing /tmp/retro-chat/ ephemeral-runtime-
state convention used elsewhere in this project.

Usage (producer):
    import ai_status_bus as bus
    run_id = bus.new_run("dp-train", model="lenet5-mnist", phase="train",
                          arch="784,128,10", command=" ".join(sys.argv))
    bus.publish(run_id, progress={"epoch": 1, "step": 50}, metrics={"loss": 0.3})
    bus.mark_done(run_id, status="completed")

Usage (consumer, e.g. the console):
    for run in bus.list_active():
        print(run["run_id"], run["liveness"], run["progress"])

CLI: python3 scripts/ai_status_bus.py [list|gc]
"""
import json
import os
import random
import time

SCHEMA_VERSION = 1

STATUS_DIR = os.environ.get(
    "RETRO_AI_STATUS_DIR", os.path.join("/tmp", "retro-ai", "status"))

# Terminal statuses — once a run reaches one of these it no longer updates.
TERMINAL_STATUSES = ("completed", "failed")

# Hand-curated peak throughput figures, sourced from
# docs/machines/ai-capability-profiles.md's recorded measurements. Used to
# turn a live achieved MMAC/s figure into an honest "(est.)" utilization %
# for GPUs with no vendor utilization tool (i.e. everything but the one real
# NVIDIA box). A box with no entry here shows raw ops/sec only — never a
# fabricated percentage. Keep this in sync by hand; it's a snapshot, not a
# live query, exactly like the doc it mirrors.
PEAK_THROUGHPUT = {
    "192.168.1.124": {  # Voodoo3, glide-mac
        "gpu_mmacs": 61.0,
        "note": "glide-mac exact binary GEMM, hash 31872c0d",
    },
    "192.168.1.143": {  # Voodoo5 5500, glide-mac
        "gpu_mmacs": 62.0,
        "note": "glide-mac exact binary GEMM, M5 flagship box",
    },
    "192.168.1.240": {  # Radeon 9800 XT, nv-gl
        "gpu_mmacs": 368.0,
        "cpu_mmacs": 1826.0,
        "note": "nv-gl vs CPU bit-packed XNOR",
    },
    "192.168.1.82": {  # WHITEBEAST, RTX 4080 SUPER, nv-gl
        "gpu_mmacs": 3720.0,
        "cpu_mmacs": 8070.0,
        "cpu_gflops": 15.5,
        "note": "the one box where GPU meaningfully narrows the CPU gap (2.2x)",
    },
}

# nvidia-smi polling state, per-IP: {"backoff_until": ts, "last": dict}.
# Module-level so repeated probe_gpu_util() calls from a render loop
# self-throttle without the caller having to track anything.
_gpu_probe_state = {}
_GPU_PROBE_POLL_SECS = 2.0     # normal cadence after a successful probe
_GPU_PROBE_FAIL_BACKOFF_SECS = 60.0  # back off harder after a failure


def _ensure_dir():
    os.makedirs(STATUS_DIR, exist_ok=True)


def _path(run_id):
    return os.path.join(STATUS_DIR, f"{run_id}.json")


def _atomic_write(path, obj):
    _ensure_dir()
    tmp = f"{path}.tmp.{os.getpid()}.{random.randint(0, 1 << 30)}"
    with open(tmp, "w") as f:
        json.dump(obj, f)
    os.replace(tmp, path)


def _deep_merge(base, updates):
    for k, v in updates.items():
        if isinstance(v, dict) and isinstance(base.get(k), dict):
            _deep_merge(base[k], v)
        else:
            base[k] = v
    return base


def new_run(kind, model, phase="train", arch=None, precision="f32",
            command=None, nodes=None, run_id=None):
    """Create+publish the initial 'starting' status blob. Returns run_id."""
    now = time.time()
    if run_id is None:
        stamp = time.strftime("%Y%m%d-%H%M%S", time.localtime(now))
        run_id = f"{stamp}-{kind.replace(' ', '')}-{random.randint(0, 0xFFFFFF):06x}"
    blob = {
        "schema_version": SCHEMA_VERSION,
        "run_id": run_id,
        "phase": phase,
        "kind": kind,
        "model": model,
        "arch": arch,
        "precision": precision,
        "status": "starting",
        "pid": os.getpid(),
        "orchestrator_host": os.uname().nodename,
        "command": command or "",
        "started_at": now,
        "updated_at": now,
        "progress": {"epoch": None, "total_epochs": None, "step": None,
                     "total_steps": None, "percent": None},
        "metrics": {},
        "fleet": {"nodes_total": len(nodes) if nodes else 0,
                  "nodes_alive": len(nodes) if nodes else 0,
                  "samples_per_sec": None, "eta_seconds": None,
                  "allreduce_ms_avg": None},
        "nodes": {ip: _empty_node() for ip in (nodes or [])},
        "log_tail": [],
        "error": None,
    }
    _atomic_write(_path(run_id), blob)
    return run_id


def _empty_node():
    return {
        "role": "worker", "alive": True, "backend": None, "precision": None,
        "samples_per_sec": None, "last_step_ms": None,
        "gpu_util_pct": None, "gpu_util_source": "unavailable",
        "gpu_util_est_pct": None, "last_error": None,
    }


def get(run_id):
    try:
        with open(_path(run_id)) as f:
            return json.load(f)
    except (OSError, ValueError):
        return None


def publish(run_id, log_line=None, **fields):
    """Partial update: deep-merges `fields` into the existing blob (creating
    it if missing), refreshes updated_at, appends log_line to log_tail
    (kept to the last 5), and rewrites atomically."""
    blob = get(run_id)
    if blob is None:
        blob = new_run(fields.pop("kind", "unknown"),
                       fields.pop("model", "?"), run_id=run_id)
        blob = get(run_id)
    if fields.get("status") is None:
        fields["status"] = blob.get("status", "running")
    elif fields["status"] == "running" and blob.get("status") == "starting":
        pass  # normal starting -> running transition
    _deep_merge(blob, fields)
    blob["updated_at"] = time.time()
    if log_line:
        tail = blob.get("log_tail", [])
        tail.append(str(log_line)[:200])
        blob["log_tail"] = tail[-5:]
    _atomic_write(_path(run_id), blob)
    return blob


def mark_done(run_id, status="completed", error=None):
    assert status in TERMINAL_STATUSES, status
    return publish(run_id, status=status, error=error)


def list_active(stale_after=15.0, dead_after=60.0):
    """Read every status file, annotate with a 'liveness' field:
      live      - updated recently, still running
      stalled   - orchestrator pid alive, no update past stale_after
                  (plausibly blocked on a slow node's real I/O)
      dead      - orchestrator pid gone, or no update past dead_after
      completed/failed - left as-is, no liveness judgement needed
    Returns newest-first."""
    _ensure_dir()
    out = []
    now = time.time()
    for name in os.listdir(STATUS_DIR):
        if not name.endswith(".json") or ".tmp." in name:
            continue
        try:
            with open(os.path.join(STATUS_DIR, name)) as f:
                blob = json.load(f)
        except (OSError, ValueError):
            continue
        if blob.get("status") in TERMINAL_STATUSES:
            blob["liveness"] = blob["status"]
        else:
            age = now - blob.get("updated_at", 0)
            pid = blob.get("pid")
            pid_alive = _pid_alive(pid)
            if not pid_alive or age > dead_after:
                blob["liveness"] = "dead"
            elif age > stale_after:
                blob["liveness"] = "stalled"
            else:
                blob["liveness"] = "live"
        out.append(blob)
    out.sort(key=lambda b: b.get("started_at", 0), reverse=True)
    return out


def _pid_alive(pid):
    if not pid:
        return False
    try:
        os.kill(pid, 0)
        return True
    except ProcessLookupError:
        return False
    except PermissionError:
        return True  # exists, just not ours
    except OSError:
        return False


def gc(older_than_hours=24):
    """Delete terminal-state status files older than the cutoff. Returns the
    count removed."""
    _ensure_dir()
    cutoff = time.time() - older_than_hours * 3600
    removed = 0
    for run in list_active():
        if run["liveness"] in TERMINAL_STATUSES and run.get("updated_at", 0) < cutoff:
            try:
                os.remove(_path(run["run_id"]))
                removed += 1
            except OSError:
                pass
    return removed


def peak_for(ip):
    return PEAK_THROUGHPUT.get(ip)


def estimate_gpu_util_pct(ip, achieved_mmacs):
    """Throughput-vs-known-peak estimate. Returns None (never a fabricated
    number) if this ip has no recorded peak."""
    peak = PEAK_THROUGHPUT.get(ip)
    if not peak or not peak.get("gpu_mmacs") or achieved_mmacs is None:
        return None
    return round(min(100.0, 100.0 * achieved_mmacs / peak["gpu_mmacs"]), 1)


async def probe_gpu_util(conn, ip):
    """Poll real nvidia-smi utilization via the agent's EXEC command. Only
    meaningful for the one real NVIDIA box in the fleet (WHITEBEAST) — every
    other vendor has no reliable always-present CLI tool. Self-throttling: a
    successful probe is cached for _GPU_PROBE_POLL_SECS, a failed one (agent
    unreachable, or nvidia-smi not on PATH) backs off for
    _GPU_PROBE_FAIL_BACKOFF_SECS — either way a render loop can call this
    every tick without hammering EXEC. `conn` is an already-connected
    RetroConnection (EXEC is a base agent command, not an AI_RAW passthrough).
    """
    now = time.time()
    state = _gpu_probe_state.setdefault(ip, {"backoff_until": 0, "last": None})
    if now < state["backoff_until"]:
        return state["last"] or _gpu_unavailable("backing off after a prior failure")
    try:
        out = await conn.command_text(
            "EXEC nvidia-smi --query-gpu=utilization.gpu,utilization.memory,"
            "memory.used,memory.total,temperature.gpu,power.draw "
            "--format=csv,noheader,nounits",
            timeout=10)
        line = out.strip().splitlines()[0] if out.strip() else ""
        parts = [p.strip() for p in line.split(",")]
        if len(parts) != 6:
            raise ValueError(f"unexpected nvidia-smi output: {out!r}")
        util, mem_util, mem_used, mem_total, temp, power = parts
        result = {
            "available": True, "source": "nvidia-smi",
            "util_pct": float(util), "mem_util_pct": float(mem_util),
            "mem_used_mb": float(mem_used), "mem_total_mb": float(mem_total),
            "temp_c": float(temp), "power_w": float(power), "error": None,
        }
        state["last"] = result
        state["backoff_until"] = now + _GPU_PROBE_POLL_SECS
        return result
    except Exception as e:
        result = _gpu_unavailable(str(e))
        state["last"] = result
        state["backoff_until"] = now + _GPU_PROBE_FAIL_BACKOFF_SECS
        return result


def _gpu_unavailable(reason):
    return {"available": False, "source": "unavailable", "util_pct": None,
            "mem_util_pct": None, "mem_used_mb": None, "mem_total_mb": None,
            "temp_c": None, "power_w": None, "error": reason}


def _main():
    import sys
    cmd = sys.argv[1] if len(sys.argv) > 1 else "list"
    if cmd == "list":
        for run in list_active():
            print(f"{run['run_id']:<32} {run['liveness']:<10} "
                  f"{run['phase']:<10} {run['status']:<10} "
                  f"{run.get('progress', {}).get('percent')}")
    elif cmd == "gc":
        n = gc()
        print(f"removed {n} terminal run(s)")
    else:
        print(f"usage: {sys.argv[0]} [list|gc]")


if __name__ == "__main__":
    _main()
