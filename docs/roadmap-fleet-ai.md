# Roadmap — Fleet AI on Vintage Hardware (detailed plan, milestones & tests)

This is the detailed build plan behind the **Fleet AI** roadmap in the top-level
[`README.md`](../README.md#roadmap--fleet-ai-train-infer-and-benchmark-ml-on-vintage-hardware-contributors-welcome).
Each milestone lists concrete **deliverables** and the **acceptance tests** to
run once it is implemented, so "done" is measurable.

## Implementation status (2026-07-17)

Most of this roadmap is now **implemented and fleet-verified** (engine:
[`retro-infer/`](../retro-infer), Python tooling: [`tools/rim/`](../tools/rim),
fleet coordinators: `scripts/retro_ai_*.py`, metrics: `scripts/ai_metrics.py`
→ `ai_runs` in the specpicks DB):

| Milestone | Status |
|---|---|
| M0 scaffold + selfcheck | ✅ fleet-verified (.124 P3/SSE, .143 Athlon/3DNow!) |
| M1 CPU kernels | ✅ int8 LeNet-5 logits **bit-exact** vs numpy ref on both boxes; SSE/MMX/3DNow! all beat scalar |
| M2 on-device training | ✅ MLP ≥96% MNIST trained on both boxes; GBDT val AUC 0.9216 (sklearn ref 0.9205); RF, SVM; bit-for-bit determinism |
| M3 `.rim` + converter | ✅ (round-trip verified; 3-machine test ran on the 2 boxes online) |
| M4 agent ML transport | ✅ agent v1.9.1: AI_HELLO/MODEL_LOAD/MODEL_LIST/INFER_RUN/TENSOR (+AI_RAW pass-through); remote inference bit-exact |
| M5 Glide GPU backend | ✅ **exact** binary/XNOR GEMM on a real Voodoo5 (0 error, hash-stable); BNN CIFAR-10 on-GPU = 1000/1000 label agreement vs CPU |
| M6 GPU backend (`nv-gl`) | ✅ **exact** binary/XNOR GEMM on real Radeon 9800 XT, Radeon HD 3850, and Intel HD Graphics (`src/gpu/nv_gl.c` — portable OpenGL 1.1 + ARB_multitexture, vendor-neutral despite the filename); BNN CIFAR-10 on-GPU = 1000/1000 vs CPU on both Radeon boxes. No GeForce has been online to confirm on actual NVIDIA silicon, but the design has no NVIDIA-specific dependency. |
| M7 fleet training | ✅ data-parallel SGD (2-node result **bit-identical** to single-node; failover verified); distributed GBDT (histogram aggregation); pipeline parallelism (layer-per-machine, label-identical). Char-transformer pipeline flagship still open (needs attention ops). |
| M8 metrics + console | ✅ `ai_runs` table + leaderboards live (all milestone results logged); `scripts/retro_infer_console.py` TUI |

Honest numbers, as promised: on binary GEMM the Voodoo5 is exact at 61 MMAC/s
but the Athlon's bit-packed XNOR does 695 MMAC/s, and the same pattern holds
for `nv-gl` (e.g. the Radeon 9800 XT's own CPU beats it 1826 vs 368 MMAC/s) —
the GPU backend's win is *that it works at all, exactly*, not raw speed on
this class of hardware. Remaining invitations: the transformer pipeline
flagship, int8-via-bitplanes, actual GeForce hardware confirmation, and
RNN/VAE/keyword-spotting from the model zoo.

## Goals & non-goals

**Goals**
- Train and run a *variety* of ML models — classical, gradient-boosted, and
  neural (with variations) — on real 1998–2004 hardware.
- Use the period GPUs (3dfx Voodoo 3/4/5, GeForce 2/3/4/FX) as compute units.
- Use the **retro agent as the cross-machine transport** for all fleet-wide
  training and inference — no new daemons, one protocol.
- Make the whole thing observable: an ASCII console + a tracked metric database.

**Non-goals**
- Competing with modern accelerators on speed. The interesting result is *that it
  works at all*, plus clean systems design and honest numbers.
- Any dependency the hardware can't take (no CUDA/PyTorch/BLAS/modern libc). All
  **custom code**, cross-compiled with the same MinGW i586 toolchain as the agent.

## Architecture at a glance

```
   Modern dev box                         Fleet (Win98 / 2K / XP)
 +------------------------+   retro-agent TCP  +---------------------------+
 | retro_chat_brain       |<------------------>|  retro_agent.exe          |
 |  mcp__retro__ai_*      |  AI_HELLO / TENSOR |   +-- retro-infer.exe     |
 |  - discover ai agents  |  MODEL_LOAD/LIST   |   |    CPU kernels (SSE/   |
 |  - schedule train/infer|  INFER_RUN         |   |    3DNow!/MMX)        |
 |  - allreduce / pipeline|                    |   |    GPU backend:       |
 +------------------------+                    |   |     glide-mac (3dfx)  |
 | retro-infer console    |  metrics -> DB     |   |     nv-combiner/shader|
 | (ASCII TUI)            |                    |   +-----------------------+
 +------------------------+                    +---------------------------+
        |                                              (period GPU as MAC array)
        v
   specpicks Postgres  (ai_runs: model x machine x backend x precision x metrics)
```

Five components, built in dependency order by the milestones below:

1. **`retro-infer`** — the on-device C engine (train + infer), one binary per box.
2. **GPU backends** — `glide-mac` (3dfx) and `nv-combiner`/`nv-shader` (NVIDIA),
   both targeting a shared "tensor-over-textures" GPGPU core.
3. **Agent ML transport** — new protocol frames so the fleet coordinates over the
   existing agent link.
4. **Brain orchestration** — discovery registry, data-parallel/pipeline
   scheduling, `mcp__retro__ai_*` tools, the ASCII console.
5. **Metrics harness** — the full ML metric set logged per run to the DB.

## The `.rim` model format

A compact, self-describing container so any box can load a model without a
framework:

```
  header:   magic 'RIM1', endian, arch flags
  manifest: JSON — layers [{op, shape, dtype, quant, params...}], class labels,
            input/output tensor specs, training hyperparams (if trainable)
  weights:  concatenated tensors, each with {dtype: fp32|int8|int4|binary,
            scale, zero_point}, 16-byte aligned for SSE/MMX loads
```

An offline Python tool (`tools/rim/`) converts from ONNX or a small reference
trainer to `.rim`, and can dequantize a `.rim` back to fp32 for the parity tests.

## Protocol additions (retro agent as ML transport)

New length-prefixed frames, same framing as every other agent command:

| Frame | Direction | Purpose |
|---|---|---|
| `AI_HELLO` | agent → fleet | advertise capability: backends, precisions, GFLOP/s estimate, resident models (also folded into the UDP discovery beacon) |
| `MODEL_LOAD <name>` | brain → agent | push a `.rim` (two-frame, like UPLOAD) |
| `MODEL_LIST` | brain → agent | enumerate resident models + status |
| `TENSOR <meta>` | any ↔ any | typed tensor payload (activations between pipeline stages; gradients for allreduce) |
| `INFER_RUN <model> <opts>` | brain → agent | run inference, stream outputs/metrics |
| `TRAIN_STEP <job> <opts>` | brain → agent | run one local training step, return gradients/metrics |

All fleet-wide movement rides these frames, so activations (pipeline) and
gradient allreduce (data-parallel) use the one transport the fleet already trusts.

## Milestones

Milestones are incremental and independently testable. "Test" = the acceptance
check to run when the milestone lands (numbers are targets to beat/verify, not
promises).

### M0 — `retro-infer` scaffolding
**Deliverables:** C skeleton cross-compiled to i586; runs on Win98/2K/XP;
`retro-infer --selfcheck` prints detected ISA (MMX/SSE/3DNow!), RAM, and a
GFLOP/s microbench; a no-op `.rim` loader.
**Tests:**
- Builds with the agent's MinGW toolchain; binary runs on a real Win98 box and a
  real XP box without a missing-DLL error.
- `--selfcheck` correctly reports SSE present on the P3 and 3DNow! on the Athlon.

### M1 — CPU inference kernels
**Deliverables:** GEMM, conv2d, pooling, ReLU/sigmoid/tanh/softmax; fp32 + int8
paths; SSE/MMX kernels with a scalar fallback.
**Tests:**
- LeNet-5 int8 inference on MNIST matches the fp32 reference **top-1 within 0.5%**
  and per-logit **max abs error < 2 int8 steps** on a 1,000-image sample.
- Throughput recorded (images/sec) on P3/SSE vs scalar; SSE path is faster.
- k-NN and logistic-regression inference produce identical labels to the Python
  reference on the same test split.

### M2 — On-device training (CPU)
**Deliverables:** backprop for dense/conv (SGD + momentum); a from-scratch
**GBDT** (histogram split-finder) and a small **random forest**; a linear SVM.
**Tests:**
- Train logistic regression on MNIST on a single retro box to **≥ 90%** test
  accuracy; loss decreases monotonically (smoothed).
- Train a 2-layer MLP to **≥ 96%** MNIST test accuracy.
- GBDT on a tabular set (e.g. adult/California-housing subset) reaches **val AUC
  within 0.02** of the scikit-learn reference; RMSE within 5% for regression.
- Determinism: fixed seed reproduces metrics bit-for-bit across two runs.

### M3 — `.rim` format + converter
**Deliverables:** `tools/rim/` (ONNX→`.rim`, trainer→`.rim`, `.rim`→fp32);
loader in `retro-infer`.
**Tests:**
- Round-trip: fp32 → `.rim` int8 → dequantized fp32 reconstructs weights within
  the quantization bound.
- A model exported on the dev box loads and infers identically (within int8
  tolerance) on three different fleet machines.

### M4 — Agent ML transport + AI-agent discovery
**Deliverables:** `AI_HELLO`, `MODEL_LOAD/LIST`, `TENSOR`, `INFER_RUN` in the
agent; brain-side registry + `mcp__retro__ai_list` / `ai_load` / `ai_infer`.
**Tests:**
- From the chat: "which machines can do AI?" lists exactly the AI-capable boxes
  with backend/precision/GFLOP/s.
- Load a `.rim` to a remote box over `MODEL_LOAD`; `MODEL_LIST` shows it resident.
- Remote `INFER_RUN` on one image returns the same label as local inference.
- `TENSOR` round-trips a float and an int8 tensor with shape/scale intact
  (checksum equal both ways); graceful close preserved (no Win98 RST crash).

### M5 — 3dfx Voodoo (Glide) GPU backend
**Deliverables:** `glide-mac` render-to-texture GEMM on the open `retro3dfx`
stack; int8/binary tiling; wired into `retro-infer` as a backend.
**Tests:**
- `glide-mac` GEMM matches the CPU int8 GEMM **max abs error ≤ 1 step** on random
  matrices up to the tile limit.
- BNN (XNOR) CIFAR-10 inference on a real Voodoo3/Voodoo5 matches the CPU BNN
  labels on a 1,000-image sample.
- Measured Voodoo-vs-SSE images/sec recorded in the DB; result reproduced across
  two runs (kill–wait–poll discipline from `benchmarks/README.md`).
- Rendering-correctness guard: a fixed input produces a stable output hash across
  runs (no framebuffer garbage counted as signal).

### M6 — GPU backend for non-3dfx cards (`nv-gl`)
**What shipped, vs. the original plan:** the original deliverable named
GeForce-specific paths (`nv-combiner` via `NV_register_combiners`,
`nv-shader` via DX8 shaders/`NV_texture_shader`) because no non-3dfx GPU
was online when the milestone was scoped. Once Radeon and Intel boxes
joined the fleet, the shipped design is a single **portable OpenGL 1.1 +
ARB_multitexture backend** (`src/gpu/nv_gl.c`) — vendor-neutral by
construction, so it serves GeForce, Radeon, *and* Intel integrated
graphics with one code path instead of three vendor-specific ones. It
mirrors glide-mac's accumulation trick (constant-color-scaled additive
blend, GL_MODULATE chaining two textures as an AND-gate, XNOR via a
positive + inverted pass) rather than using register combiners or
shaders, and gets exact results without needing either.
**Tests (adapted to available hardware):**
- ✅ `--nv-check` (mirrors `--glide-check`): exact binary GEMM, 0
  mismatches, hash-stable across reruns, matching Glide's FNV-1a hash on
  the same seed — verified on Radeon 9800 XT, Radeon HD 3850 AGP, and
  Intel HD Graphics up to the full 256³ tile.
- ✅ `--nv-check-multi`: varying-size calls within one GL session (the
  real usage pattern, not just fixed-size repeats) — 10/10 shapes exact,
  including the odd output width (N=10) that exposed a real
  `GL_PACK_ALIGNMENT` readback bug.
- ✅ BNN CIFAR-10 (the M5 flagship test, run again here): 1000/1000 label
  agreement vs CPU on both Radeon boxes, top-1 matching exactly.
- ✅ Backend selection: `AI_HELLO` reports `nv-gl` in `backends` with
  `nv_gl_status: verified` whenever `ARB_multitexture` is present;
  `--bnn-eval ... nvgl` forces it explicitly.
- ⬜ Not yet run on actual GeForce silicon (none has come online) — the
  design has no NVIDIA-specific dependency, so this is an availability
  gap, not a known risk.
- ⬜ GF-FX float path / int8 error comparison — not pursued; the exact
  binary path already meets the accuracy bar the milestone wanted.

### M7 — Fleet training (all GPUs at once)
**Deliverables:** brain-coordinated **data-parallel SGD** (ring/tree allreduce
over `TENSOR`), **distributed GBDT** (per-round histogram aggregation), and
**pipeline parallelism** (layer-per-machine) with checkpointing to the share.
**Tests:**
- Data-parallel MLP/CNN across ≥ 3 GPUs converges to **within 1%** test accuracy
  of the single-node baseline in the same number of epochs; allreduce
  correctness verified against a local average on a fixed seed.
- Distributed GBDT across ≥ 3 nodes matches single-node val AUC within 0.01.
- **Straggler/failover:** killing one training node mid-epoch triggers shard
  reassignment (reusing the brain's failover) and training still completes with
  metrics within tolerance.
- Pipeline: a 6-layer char-transformer split across 4 machines produces the same
  token sequence (greedy, fixed seed) as the single-box run; per-stage activation
  `TENSOR` checksums match.

### M8 — Metrics harness + ASCII console
**Deliverables:** the `retro-infer` ASCII TUI (discover/train/infer/bench,
mirrorable into Retro Chat); full metric logging to `ai_runs` in the specpicks DB;
leaderboards; energy-per-inference capture.
**Tests:**
- Every train/infer run writes one DB row keyed by model × machine × backend ×
  precision with the full metric set (below) populated.
- Console renders correctly on a 16-color 80×25 console and streams live loss/acc
  without flicker; `[d]/[t]/[i]/[b]` actions work end-to-end.
- Leaderboard query returns a stable ranking; a re-run of a fixed config
  reproduces its metrics within noise.
- Energy: fleet wattage sampled during a run and stored (even if via a manual
  meter reading entered into the row) so energy-per-inference is comparable.

## Metrics reference (logged per run)

**Classification:** accuracy, top-5, error rate, precision, recall, F1,
AUC-ROC, log-loss, confusion matrix.
**Regression:** RMSE, MAE, R².
**Language models:** cross-entropy, perplexity, tokens/sec.
**Generative/autoencoders:** reconstruction loss, PSNR/SSIM.
**Training dynamics:** train/val loss curves, epochs-to-target, gradient-norm.
**Systems:** throughput (img/tok/sec), latency p50/p99, memory high-water,
allreduce ms/step, energy per inference, GFLOP/s achieved vs peak.

## Model zoo (targets)

See the table in the [README roadmap](../README.md#model-zoo-to-build-train-and-benchmark).
In build order they slot into the milestones as: linear/k-NN (M1–M2), GBDT/RF/SVM
(M2), MLP/LeNet-5 (M1–M2, GPU in M5–M6), BNN (M5 showcase), RNN/GRU/LSTM (M2),
nanoGPT-class transformer (M7 pipeline flagship), VAE/keyword-spotting (M6–M8).

## How to contribute

Pick a milestone or a single deliverable, open an issue naming it, and build
against the acceptance tests above. The fastest on-ramps: an M5 Glide GEMM
kernel, the M1 SSE GEMM, the M3 `.rim` packer/converter, or the M8 console. Keep
it dependency-free, keep the numbers honest, and add your run to the DB.
