---
name: fleet-ai-monitor
description: Launch and interpret the live Fleet AI console (scripts/retro_infer_console.py) — a menu-driven, continuously-repainting terminal dashboard for the retro-infer ML fleet, not a run-once CLI. Use when the user asks to "open/launch the AI console", "show me GPU utilization", "is training working / what's it doing right now", "watch the fleet train", or wants a live view of ops/sec, loss, or per-node status instead of a one-shot command.
---

# Fleet AI live console

`scripts/retro_infer_console.py` is a full-screen `rich`-based dashboard over
the retro agent's AI transport. It keeps repainting while discover/train/
dist-infer/infer/bench/pipeline actions run in the background — nothing
freezes waiting on a network round-trip. Companion skills:
[[fleet-ai-train]] (what to actually run) and [[fleet-ai-diagnose]] (fixing
things when the fleet looks unhealthy from inside the console).

Full design rationale + status-bus schema: `retro-infer/docs/OPERATIONS.md`.

## Step 1 — Launch

```bash
pip install -r requirements.txt   # rich, numpy, psycopg2-binary — one-time
python3 scripts/retro_infer_console.py
```

Needs a real interactive terminal (it puts stdin into cbreak mode) — don't
run it through a non-interactive shell or pipe. Targets an 80×25 floor; a
taller/wider terminal just shows more fleet/run rows, the layout doesn't
change shape.

## Step 2 — Read the panels

- **fleet** (top-left): every discovered box (`d` to (re)discover), IP/host/
  backend/kernels/status/resident-model-count. `▶` marks the focused row
  (`Tab` to move it) — the GPU/throughput gauge below follows the focus.
- **active runs** (top-right): every in-flight or recently-finished status-
  bus entry — run id, phase, a progress bar, fleet-aggregate ops/sec. Colors
  distinguish liveness: green = actively updating, yellow = stalled (no
  update recently but the orchestrator process is still alive — plausibly
  waiting on a slow box, not necessarily broken), red = dead (process gone
  or very stale).
- **gpu / throughput** (middle): for the focused machine. **Only WHITEBEAST
  (192.168.1.82, the real RTX 4080 SUPER) shows a real percentage** — polled
  live via `nvidia-smi` through the agent. Every other box (3dfx, Radeon,
  Intel) shows measured CPU ops/sec instead of a percentage, on purpose —
  there's no reliable vendor utilization tool for them, and today's
  training/inference paths don't touch the GPU binary-GEMM backend anyway
  (see the "Honesty notes" section of OPERATIONS.md if a user asks why a
  Radeon box doesn't show a GPU%).
- **log**: recent event lines from both the console's own actions and the
  bus's `log_tail` for spawned jobs.
- **footer**: the hotkey bar. Action keys open a small form overlay — `Tab`/
  `Enter` moves between fields and submits on the last one, `Esc` cancels.

## Step 3 — Common asks and what to do

- *"Is training/inference running right now?"* → look at **active runs**;
  anything not `completed`/`failed` is live. Check `liveness` color before
  reporting something as hung — "stalled" on a Win98/P3 box during a real
  network round-trip is normal, not broken.
- *"What model is training and how far along?"* → the active-runs row shows
  phase + progress %; the run's full detail (model, arch, per-node loss/acc)
  is in the underlying status-bus JSON if more detail is needed:
  `python3 scripts/ai_status_bus.py list`, or read `/tmp/retro-ai/status/
  <run_id>.json` directly (override dir via `RETRO_AI_STATUS_DIR`).
- *"Show me GPU usage"* → focus the box with `Tab`, read the gauge — be
  explicit with the user about which boxes give a real percentage (NVIDIA
  only) vs. ops/sec (everyone else); never state an estimated GPU% as fact.
- *Clearing clutter*: `g` runs `ai_status_bus.gc()` (removes old completed/
  failed entries past the default 24h retention).
- *Something looks stuck*: `k` opens a kill-by-run-id-substring form for
  subprocess jobs the console itself spawned (train/dist-infer/pipeline).
  This only affects the *orchestrator* process on this Linux box, not the
  fleet machine's own `retro-infer.exe --serve` engine — for that, see
  [[fleet-ai-diagnose]]'s `AI_RESTART` step.

## Notes

- Multiple console instances can safely watch the fleet at once (the status
  bus is just files, many-reader-safe). Two people independently starting a
  `dp-train` against the *same* IPs at the same time will contend on the
  single-connection-per-agent limit — check the active-runs panel for an
  overlapping node set before kicking off a new distributed job.
- The keyboard-driven menu/overlay loop needs a live terminal — it cannot be
  driven or screenshotted from a headless/non-interactive session.
