# retro-infer — dependency-free ML engine for the retro fleet

The on-device half of the [Fleet AI roadmap](../docs/roadmap-fleet-ai.md):
a single ~130KB static Win32 binary (same MinGW i586 toolchain as the agent,
runs on Win98SE+) that trains and runs ML models on 1998–2004 hardware, plus
a native Linux build (`make host`) for fast parity iteration.

## Build

```bash
make          # retro-infer.exe (i586 Windows, SSE/MMX/3DNow! TUs behind CPUID dispatch)
make host     # retro-infer-host (Linux, for local testing; no 3DNow!)
make release  # bump infer-vX.Y.Z tag + build
```

## CLI

```
retro-infer --selfcheck                              ISA + RAM + GFLOP/s
retro-infer --riminfo <m.rim>                        validate container
retro-infer --infer <m.rim> <input.bin>              one inference
retro-infer --eval <m.rim> <imgs> <lbls> <N>         top-1 + img/s
            [--logits out.bin] [--scalar]
retro-infer --train-mlp <trX trY Ntr teX teY Nte arch epochs lr mom batch seed out.rim>
retro-infer --train-gbdt <feat lab N F valfrac rounds depth minchild lr lambda> [--regress]
retro-infer --train-rf <feat lab N F valfrac ntrees depth seed>
retro-infer --train-svm <feat lab N F valfrac epochs lr reg [seed]>
retro-infer --bnn-eval <m.rim imgs lbls N> [cpu|glide]   batched BNN (GPU on Voodoo)
retro-infer --glide-check [M N K seed]               Voodoo GEMM acceptance
retro-infer --serve [port]                           engine server (agent proxies to :9896)
```

## Layout

- `src/ops/` — GEMM kernels (scalar oracle + SSE/MMX/3DNow! TUs), im2col
  conv, pooling, activations, XNOR/popcount
- `src/exec.c` — `.rim` model executor (f32 / int8 / binary `bdense` paths);
  format spec: [`tools/rim/FORMAT.md`](../tools/rim/FORMAT.md)
- `src/train/` — MLP backprop, histogram GBDT, random forest, linear SVM,
  step-wise NT* sessions (fleet data-parallel), GB* distributed-GBDT node side
- `src/gpu/glide_mac.c` — 3dfx Voodoo binary-GEMM backend (exact
  alpha-test accumulation; see file header for the method)
- `src/gpu/nv_gl.c` — GeForce backend (compile-verified, awaiting hardware)
- `src/serve.c` — loopback server the agent's AI_* commands proxy to

## Rules that keep parity exact

- Scalar GEMM loops are the oracle: fixed k-ascending accumulation, never
  reorder. Integer (int8/binary) paths must be bit-exact vs the Python
  reference (`tools/rim/eval_ref.py`); f32 paths use tolerance.
- All rounding is half-away-from-zero (`ri_round`), matching the reference.
- Every train/infer/bench result gets a row in `ai_runs`
  (`scripts/ai_metrics.py`) keyed by model × machine × backend × precision.
