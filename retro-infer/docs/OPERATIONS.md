# Operations — the live console, status bus, and fleet training/inference

This is the operator-facing companion to [ARCHITECTURE.md](ARCHITECTURE.md)
(system design) and [TRAINING-AND-INFERENCE.md](TRAINING-AND-INFERENCE.md)
(the raw command/flag runbook). This doc covers **how the pieces are driven
day to day**: the live console, what "live" actually means given the
protocol's constraints, and end-to-end fleet workflows through it.

## Why the console looks the way it does

The agent/engine protocol (`agent/src/ai.c`, `retro-infer/src/serve.c`) is
strict blocking request/response over a single connection — there is no
progress-poll or streaming primitive anywhere in it. You cannot ask "what's
the current loss" while an `NTSTEP`/`INFER_RUN` call is in flight; you only
get data back after it returns. That's a deliberate simplicity trade-off in
the transport, not a bug, and rearchitecting it to be async/streaming is out
of scope here.

Instead, every orchestration script already knows its own step boundaries,
batch sizes, and timing — so each one publishes a small JSON status blob to
a shared **status bus** (`scripts/ai_status_bus.py`, files under
`/tmp/retro-ai/status/`, override with `RETRO_AI_STATUS_DIR`) as it runs, and
`scripts/retro_infer_console.py` polls that directory on a timer to render
live panels. This is honest "real-time" — it reflects what the orchestrator
observed a few hundred milliseconds ago — not a true push channel. See the
module docstring in `ai_status_bus.py` for the full schema.

## Launching the console

```bash
pip install -r requirements.txt   # rich, numpy, psycopg2-binary
python3 scripts/retro_infer_console.py
```

Needs a real terminal (it puts stdin into cbreak mode for non-blocking
single-key input) — it won't run usefully piped or in a non-interactive
shell. Targets an 80×25 floor (the historical bar this project has always
built the console against); chrome panel sizes and table row counts are
computed from the *actual* measured terminal height each redraw, so nothing
silently clips mid-row on a small window — a taller terminal just shows more
fleet/run rows, it doesn't change the layout.

### Keys

| Key | Action |
|---|---|
| `d` | discover — beacon-scan the LAN, `AI_HELLO` + `MODEL_LIST` every AI-capable box, populate the fleet table |
| `t` | train — spawn `retro_ai_fleet.py dp-train` (data-parallel SGD across chosen IPs) |
| `n` | dist-infer — spawn `retro_ai_fleet.py dp-infer` (data-parallel inference batch sharded across chosen IPs) |
| `i` | infer — one `INFER_RUN` against a resident model on one box |
| `b` | bench — 100-image `INFER_RUN` loop → img/s, logged to `ai_runs` |
| `p` | pipeline — spawn `retro_ai_pipeline.py` (2-stage layer-split pipeline-parallel demo) |
| `l` | leaderboard — query `ai_metrics.leaderboard()`, results land in the log pane |
| `g` | gc — clear old completed/failed entries out of the status bus |
| `k` | kill — terminate a tracked spawned subprocess by run-id substring |
| `Tab` | move focus to the next fleet machine (drives the GPU/throughput gauge) |
| `?` | help |
| `q` | quit (always restores the terminal's normal input mode, including on `Ctrl-C`/crash) |

Action keys (`t`/`n`/`i`/`b`/`p`/`l`/`k`) open a small parameter-entry
overlay — `Enter`/`Tab` advances fields and submits on the last one, `Esc`
cancels. Submitting spawns the action in the background (an `asyncio` task
or subprocess); the console keeps repainting while it runs — nothing freezes
waiting for a network round-trip, which is what made the previous
one-action-at-a-time console feel dead during a long run.

## Honesty notes

What's live, what's estimated, and what's never shown:

- **Ops/sec, progress, loss, ETA, per-node alive/dead** — real, derived
  directly from timing the orchestrator scripts already do (see the schema
  in `ai_status_bus.py`). ETA is computed from the measured step wall-clock
  time, which already reflects the slowest node in a barrier-synchronized
  step (`dp-train`'s `NTAPPLY` is a hard sync point) — it is not a naive
  per-node average, which would understate how long a straggler makes the
  whole step take.
- **GPU utilization: real only for the one NVIDIA box.** WHITEBEAST
  (`192.168.1.82`) is polled live via `EXEC nvidia-smi --query-gpu=...`
  through the agent's existing `EXEC` command (throttled to ~once per 2s,
  backing off to 60s after a failure — see `ai_status_bus.probe_gpu_util`).
  Every other GPU vendor in the fleet (3dfx, Radeon, Intel) has no reliably
  present CLI utilization tool, so the console does **not** show a
  percentage for them — it shows measured CPU ops/sec instead, labeled as
  such. This is a deliberate choice: a fabricated "(est.)" GPU% would need a
  live achieved-MMAC/s figure to compare against a peak, and **today's
  `dp-train`/`dp-infer`/pipeline workloads never touch the GPU binary-GEMM
  backend at all** — they run entirely on the CPU SIMD kernels (SSE/SSE2/
  3DNow!/MMX). The GPU backend (`glide-mac`/`nv-gl`) is only exercised live
  by the `--glide-check`/`--nv-check`/`--bnn-eval` acceptance paths invoked
  directly via `EXEC`/`EXECW`, not through the agent's structured
  `NTSTEP`/`INFER_RUN` commands. `ai_status_bus.py` still ships a
  `PEAK_THROUGHPUT` table (hand-curated from
  [`ai-capability-profiles.md`](../../docs/machines/ai-capability-profiles.md))
  and an `estimate_gpu_util_pct()` helper for any future producer that *does*
  drive the GPU backend live — it's just not wired into today's training/
  inference paths because doing so would be comparing incompatible workloads.
- **Never a fabricated number.** A box with no recorded peak throughput and
  no live vendor tool shows raw ops/sec only, never a guessed percentage.

See also: measured idle CPU/memory footprint (near-zero by design) in
[ARCHITECTURE.md's Idle resource footprint](ARCHITECTURE.md#idle-resource-footprint).

## End-to-end workflows through the console

All of these are also directly runnable as standalone scripts (see
[TRAINING-AND-INFERENCE.md](TRAINING-AND-INFERENCE.md) for the raw flags);
the console just gives them a shared, observable home.

**Fleet data-parallel training** (`t`): pick 2+ IPs, epochs, global batch,
train-n. Every global step publishes progress/loss/ETA/per-node
samples-per-sec/allreduce-ms to the bus; a node that fails mid-run (real
fault, or an injected `--kill-node`/`--kill-at-step` test run directly via
`retro_ai_fleet.py`) flips to `alive: false` in the bus within one step. On
completion the run logs to `ai_runs` with `phase="train"` — this was
previously a gap (`dp-train` logged nothing to the DB at all; it does now).

**Distributed inference** (`n`): pick 2+ IPs, a resident model name, and a
sample count `n`. Shards the batch across live nodes concurrently (no
gradient averaging — each sample is independent), gathers predictions,
reports aggregate top-1 and img/s, logs to `ai_runs` with `phase="infer"` and
`settings.mode="distributed"`.

**Pipeline-parallel demo** (`p`): a fixed 2-stage, 2-box layer-split demo
(not a general N-stage framework) — verifies label-identical output vs a
single-box run of the same model. Logs `phase="infer"`,
`settings.mode="pipeline"`.

**Single-node infer/bench** (`i`/`b`): unchanged in spirit from the original
console, now bus-published too so they show up in the same active-runs
panel as the distributed jobs.

## Extending the status bus

Any new orchestration script can participate: `bus.new_run(kind, model,
phase=..., nodes=[...])` to register, `bus.publish(run_id, progress=...,
metrics=..., fleet=..., nodes={...}, log_line=...)` as often as makes sense
(fields deep-merge, so partial updates are fine), `bus.mark_done(run_id,
status="completed"|"failed")` at the end. See the schema and worked examples
in `scripts/ai_status_bus.py`'s docstring and source.
