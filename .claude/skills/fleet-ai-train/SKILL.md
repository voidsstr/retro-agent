---
name: fleet-ai-train
description: Run distributed ML training, distributed inference, or the pipeline-parallel demo across the retro-infer fleet (dp-train / dp-infer / retro_ai_pipeline.py), pick node sets, test failover, and verify results landed in ai_runs. Use when the user asks to "train a model on the fleet", "run distributed training/inference", "test failover", "benchmark the fleet training", or "run the pipeline demo".
---

# Fleet AI: distributed training, distributed inference, pipeline demo

Three fleet coordinators, all in `scripts/`, all connect over the agent AI
transport (:9898) and all now publish live status to
`scripts/ai_status_bus.py` + log a final row to `ai_runs` — watch a run with
[[fleet-ai-monitor]]'s console, or drive these scripts directly for
scripting/CI use. Companion: [[fleet-ai-diagnose]] if a box isn't
responding before you start. Full flag-level reference:
`retro-infer/docs/TRAINING-AND-INFERENCE.md`.

## Step 1 — Pick a node set

Cross-reference `docs/machines/ai-capability-profiles.md` for what each box
is (CPU ISA, GPU backend, known peak throughput) or run a live discovery
first (`d` in the console, or `AI_HELLO` per box). Mixing very different
hardware (e.g. a 1998 P3 with a 2024 Ryzen) in one `dp-train` run is fine
functionally but the barrier-synchronized step will run at the slowest
node's pace — that's expected, not a bug, and the console's ETA already
accounts for it.

## Step 2 — Data-parallel training (`dp-train`)

```bash
python3 scripts/retro_ai_fleet.py dp-train \
    --ips 192.168.1.124,192.168.1.143 \
    --arch 784,128,10 --epochs 2 --global-batch 128 --train-n 20000 \
    [--export C:\RETRO_AGENT\models\dp.rim]
```

Every node starts from identical weights (same `--seed`); each global step
shards the batch, gathers per-node gradients, weight-averages them, and
broadcasts back (`NTAPPLY`) — a hard sync point, so a straggler node slows
every step, not just its own shard. On completion it evals on every live
node (should agree closely — that's the weight-sync check) and logs to
`ai_runs` with `phase="train"`.

**Failover test**: `--kill-node <ip> --kill-at-step <n>` kills that node's
engine mid-run (via `EXEC taskkill /f /im retro-infer.exe`) to verify the
run continues on survivors — confirmed working: the dead node's shard gets
reassigned and the run completes normally. **Restart that node's engine
afterward** (`AI_RESTART` over a direct connection, or see
[[fleet-ai-diagnose]]) — the kill is real, not simulated.

## Step 3 — Distributed inference (`dp-infer`)

```bash
python3 scripts/retro_ai_fleet.py dp-infer \
    --ips 192.168.1.124,192.168.1.143,192.168.1.240 \
    --model lenet5-int8 --n 1000
```

Shards a batch of `n` samples across live nodes concurrently — no gradient
averaging (each sample is independent), just gathered predictions + fleet
aggregate top-1/img-per-sec. **The model must already be resident on every
target node first** (`MODEL_LOAD`, or via the console) — a node without it
loaded fails that shard's samples, which drops out of the aggregate rather
than crashing the whole run. Logs `phase="infer"`,
`settings.mode="distributed"`.

## Step 4 — Pipeline-parallel demo (`retro_ai_pipeline.py`)

```bash
python3 scripts/retro_ai_pipeline.py --a 192.168.1.124 --b 192.168.1.143 \
    --model tools/rim/out/mlp_60k.rim --n 200
```

A **fixed 2-stage, 2-box** layer-split demo (stage 1 = first dense+relu on
box A, stage 2 = final dense on box B) — not a general N-stage framework.
Verifies label-identical output vs. a single-box run of the same model
("IDENTICAL" in its output is the pass condition). Logs `phase="infer"`,
`settings.mode="pipeline"`.

## Step 5 — Verify results

```bash
python3 scripts/ai_metrics.py board --model lenet5-int8 --metric img_per_sec_total
```

or watch the run land live in the console's active-runs panel
([[fleet-ai-monitor]]). Every run of all three kinds above now reaches
`ai_runs` — `dp-train` previously logged nothing to the DB at all; that gap
is closed.

## Notes

- All three scripts authenticate with `RETRO_AGENT_SECRET` (env, defaults to
  the fleet secret) and are safe to run standalone without the console.
- A node that's offline or the wrong hardware for the chosen `--arch` fails
  loudly at `NTINIT`/connect time, before any training happens — check the
  IPs first if a run errors immediately.
- Don't start two distributed jobs against overlapping node sets at once —
  the agent is single-connection-per-box, so they'll silently queue/contend
  rather than crash.
