# Fleet AI — overview & project reflection

*"A language model, generated one token at a time, on a Pentium II with a
Voodoo doing the matrix math — trained on the whole fleet at once."* That line
in the [roadmap](../../docs/roadmap-fleet-ai.md) was the north star. This
document is the plain-English tour of what got built, why it's shaped the way
it is, and what the exercise taught — the on-ramp before the deep docs.

## What this is

**Fleet AI** turns a rack of 1998–2004 PCs (Pentium III, Athlon, with 3dfx
Voodoo cards) into a tiny distributed ML cluster. It trains and runs real
machine-learning models — classical, gradient-boosted, and neural — on that
hardware, using the period GPUs as compute units and the existing
[retro agent](../../README.md) TCP protocol as the cross-machine transport.

Everything is **custom code**. None of these machines can run PyTorch, CUDA,
scikit-learn, or a modern libc, so the engine (`retro-infer`) is a ~130 KB
freestanding C binary cross-compiled with the same MinGW i586 toolchain as the
agent. That constraint is the whole point: it's an honest systems problem and
every layer stays hackable.

## The five layers

```
  Modern dev box                         Fleet (Win98 / 2K / XP)
+------------------------+  retro-agent TCP  +---------------------------+
| brain / mcp ai_* tools |<----------------->|  retro_agent.exe          |
| retro_ai_*.py fleet    |  AI_HELLO/TENSOR  |   +-- retro-infer.exe     |
|  coordinators          |  MODEL_LOAD/INFER |   |   CPU kernels (SSE/    |
| ai_metrics -> Postgres |  NT*/GB* training |   |    3DNow!/MMX)         |
+------------------------+                   |   |   glide-mac (Voodoo)   |
                                             |   +-----------------------+
                                             +---------------------------+
```

1. **`retro-infer` engine** — on-device C: `.rim` model executor + CPU kernels
   + on-device trainers. One binary per box.
   ([ARCHITECTURE.md](ARCHITECTURE.md), [ALGORITHMS.md](ALGORITHMS.md))
2. **GPU backends** — the Voodoo as a fixed-function multiply-accumulate array
   (`glide-mac`, shipped + hardware-verified) and a GeForce OpenGL backend
   (`nv-gl`, code-ready, awaiting hardware).
3. **Agent ML transport** — new agent commands (`AI_HELLO`, `MODEL_LOAD`,
   `INFER_RUN`, `TENSOR`, `AI_RAW`) that ride the one TCP link the fleet
   already trusts. ([ARCHITECTURE.md](ARCHITECTURE.md))
4. **Brain orchestration** — `mcp__retro__ai_*` chat tools + Python
   coordinators for data-parallel SGD, distributed GBDT, and pipeline
   parallelism. ([TRAINING-AND-INFERENCE.md](TRAINING-AND-INFERENCE.md))
5. **Metrics** — every train/infer/bench run keyed by model × machine ×
   backend × precision into the specpicks Postgres DB, with leaderboards.

## The headline results

Real hardware, honest numbers (full table in
[the roadmap status section](../../docs/roadmap-fleet-ai.md#implementation-status-2026-07-17)):

- **int8 LeNet-5 inference is bit-exact** vs the numpy reference on the P3 and
  the Athlon — the vectorized integer paths reproduce the reference logits
  byte-for-byte.
- **MNIST MLP trained on-device to ≥96%**; from-scratch histogram **GBDT beat
  the sklearn reference** (val AUC 0.9216 vs 0.9205).
- **Exact matrix math on a 3dfx Voodoo**: binary/XNOR GEMM with zero error up
  to the full 256³ tile, and a **binarized CNN classifying CIFAR-10 on the
  Voodoo5 with 1000/1000 label agreement** vs CPU.
- **2-node data-parallel training is bit-identical to single-node**;
  **distributed GBDT lands within AUC 0.0002** of the single-node model with
  rows that never leave their box; **pipeline parallelism** runs an MLP's
  layers on different machines with label-identical output.

## Design decisions that mattered

- **The engine is a separate process the agent supervises**, not linked into
  the agent. GPU experiments crash constantly and the agent is the machine's
  lifeline — process isolation means a wedged Glide context never takes down
  remote access; the agent just respawns the engine.
- **Kernels are chosen at runtime from CPUID**, never a compile-time
  assumption. The load-bearing discovery: the Voodoo5 box is an *Athlon with
  no SSE* — an SSE-only build would silently fall back to scalar there. Every
  ISA (SSE, 3DNow!, MMX) gets its own GEMM, including separate NT/TN variants
  so on-device *training* never drops to scalar either.
- **Integer paths are bit-exact; float paths use tolerance.** int8 and binary
  math must match the numpy reference exactly (that's the parity oracle); f32
  vectorized kernels reorder accumulation and so are validated within a bound.
- **One `.rim` format, self-describing.** A model is a magic header + JSON
  manifest + aligned weight blob — any box loads it without a framework. The
  Python side and the C loader are bound to one spec
  ([FORMAT.md](../../tools/rim/FORMAT.md)).
- **Everything rides the existing agent transport.** No new daemons, no new
  ports on the fleet — activations, gradients, and model pushes all use the
  length-prefixed frames the agent already speaks.

## What we learned

- **The interesting result is "that it works at all, exactly."** On raw binary
  GEMM the Athlon's bit-packed XNOR popcount is ~11× faster than the Voodoo's
  render-to-texture GEMM (695 vs 61 MMAC/s). The GPU backend earns its place
  as an existence proof and an offload path, not a speed win — and the docs
  say so. Chasing honest numbers over flattering ones kept the project sane.
- **Vintage hardware punishes hidden assumptions.** No SSE on the Athlon; the
  x87 FPU and MMX share register state so a stray `float` op mid-kernel
  corrupts an accumulator; the 8-bit Voodoo framebuffer saturates so the K
  dimension has to be split into short passes; a dual-boot box keeps Windows
  on `D:`; a vendor `glide3x.dll` loads fine but silently fails to open a
  context because it doesn't match the installed display driver. Each of these
  was a real bug, each is now documented in [MAINTENANCE.md](MAINTENANCE.md)
  and [the machine profiles](../../docs/machines/ai-capability-profiles.md).
- **Determinism is a feature you design in, not verify at the end.** Fixed
  xorshift PRNG, fixed accumulation order, explicit rounding mode — with those
  in from day one, "reproduces bit-for-bit across two runs" is a cheap test
  instead of an expensive late-project mystery.

## What's still open

- **M6 GeForce hardware pass** — `nv-gl` is written and compiles; it needs a
  GeForce box online to validate.
- **The transformer pipeline flagship** — the wow demo (an LM whose layers
  each live on a different 25-year-old computer) needs attention ops added to
  the executor.
- int8-via-bitplanes on Glide, true ring allreduce (needs ≥3 AI nodes), and
  the RNN / VAE / keyword-spotting zoo models.

## Where to go next

- Deep architecture + data flow → [ARCHITECTURE.md](ARCHITECTURE.md)
- The actual math and numeric schemes → [ALGORITHMS.md](ALGORITHMS.md)
- The model zoo + acceptance numbers → [MODELS.md](MODELS.md)
- Build / train / infer runbook → [TRAINING-AND-INFERENCE.md](TRAINING-AND-INFERENCE.md)
- Keeping it running + extending it → [MAINTENANCE.md](MAINTENANCE.md)
- Per-machine hardware & kernel record → [../../docs/machines/ai-capability-profiles.md](../../docs/machines/ai-capability-profiles.md)
- The original milestone plan + acceptance tests → [../../docs/roadmap-fleet-ai.md](../../docs/roadmap-fleet-ai.md)
