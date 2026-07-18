# Fleet AI algorithms — the actual math, per file

Every numeric scheme the engine implements, with the exact code location and
the parity guarantee it carries. Binding specs:
[`tools/rim/FORMAT.md`](../../tools/rim/FORMAT.md) (container + int8) and
[`tools/rim/bnn/BNN-SPEC.md`](../../tools/rim/bnn/BNN-SPEC.md) (integer XNOR).
System context: [ARCHITECTURE.md](ARCHITECTURE.md).

## GEMM kernels (runtime ISA dispatch)

Four kernel roles, each with a scalar oracle plus per-ISA variants, selected
at runtime by [`src/kernels.c:31`](../src/kernels.c#L31) `kernels_init` from
CPUID ([`src/cpuid.c:53`](../src/cpuid.c#L53)):

| role | contract | scalar | SSE | 3DNow! | MMX |
|---|---|---|---|---|---|
| `gemm_f32` (NN) | `C[M,N] += A[M,K]·B[K,N]` | [`gemm_scalar.c:9`](../src/ops/gemm_scalar.c#L9) | [`gemm_sse.c:16`](../src/ops/gemm_sse.c#L16) | [`gemm_3dnow.c:17`](../src/ops/gemm_3dnow.c#L17) | — |
| `gemm_i8` | i8×i8 → **i32** accumulate | [`gemm_scalar.c:25`](../src/ops/gemm_scalar.c#L25) | — | — | [`gemm_mmx.c:17`](../src/ops/gemm_mmx.c#L17) |
| `gemm_f32_nt` (training fwd) | `C[i,j] += dot(A_i, B_j)`, B is `[N,K]` | [`gemm_scalar.c:42`](../src/ops/gemm_scalar.c#L42) | [`gemm_sse.c:48`](../src/ops/gemm_sse.c#L48) | [`gemm_3dnow.c:57`](../src/ops/gemm_3dnow.c#L57) | — |
| `gemm_f32_tn` (weight grads) | `C[M,N] += Aᵀ·B`, axpy over k | [`gemm_scalar.c:61`](../src/ops/gemm_scalar.c#L61) | [`gemm_sse.c:77`](../src/ops/gemm_sse.c#L77) | [`gemm_3dnow.c:89`](../src/ops/gemm_3dnow.c#L89) | — |

Preference order: f32 **SSE > 3DNow! > scalar**; int8 **MMX > scalar**
([`src/kernels.c:40-57`](../src/kernels.c#L40)). `--scalar` forces the oracle
([`src/kernels.c:60`](../src/kernels.c#L60)) for A/B and parity runs.

**Why per-ISA?** The fleet spans 1998–2004 silicon: the P3 (.124) has SSE but
the classic Athlon (.143) has **3DNow!+MMX and no SSE** — an SSE-only build
would silently drop the Athlon to scalar (~1.7× slower). The NT/TN training
variants exist so *on-device training* also never falls back to scalar; adding
the 3DNow! NT/TN pair cut the Athlon MLP epoch from 146 s to 92 s (1.59×,
commit `487d73b`). Per-box rationale:
[`docs/machines/ai-capability-profiles.md`](../../docs/machines/ai-capability-profiles.md).

**Build isolation**: each vectorized kernel is a dedicated TU compiled with
only its own `-msse`/`-mmmx`/`-m3dnow` flag
([`Makefile:33-40`](../Makefile#L33), rules at
[`Makefile:59-69`](../Makefile#L59)); the rest of the binary is `-march=i586`,
so a plain Pentium never executes a vector instruction.

**Numeric contracts**:

- The scalar loops are the oracle: k-innermost, ascending, never reordered
  ([`src/ops/gemm_scalar.c:1-5`](../src/ops/gemm_scalar.c#L1)).
- `gemm_f32_sse` (NN form) keeps the identical per-element accumulation order
  → **bit-for-bit equal** to scalar; no reassociation, no FMA
  ([`src/ops/gemm_sse.c:5-8`](../src/ops/gemm_sse.c#L5)).
- `gemm_f32_nt_sse` horizontal-sums four partials at the end → fp order
  differs → f32 parity tests use **tolerance**
  ([`src/ops/gemm_sse.c:44-45`](../src/ops/gemm_sse.c#L44)).
- 3DNow! rounds to single precision each step (x87 scalar keeps excess
  precision) → close but not bit-identical; f32 tolerance again
  ([`src/ops/gemm_3dnow.c:4-8`](../src/ops/gemm_3dnow.c#L4)).
- `gemm_i8_mmx` (`pmaddwd` 4-lane, sign-extended i16 pairs) is integer math →
  **bit-identical** to scalar regardless of order
  ([`src/ops/gemm_mmx.c:1-9`](../src/ops/gemm_mmx.c#L1)).

### The x87-under-MMX-state hazard

MMX/3DNow! registers alias the x87 stack. **No x87 float op may execute while
an MMX/3DNow! register is live** — doing so corrupts the FPU state (a real bug
hit on the Athlon, fixed in `487d73b`). The 3DNow! kernels therefore:

- extract vector accumulators as **integer bits** (`_mm_cvtsi64_si32` +
  `memcpy` into a float) rather than storing through the FPU
  ([`src/ops/gemm_3dnow.c:74-77`](../src/ops/gemm_3dnow.c#L74)),
- broadcast scalars via integer loads
  ([`src/ops/gemm_3dnow.c:100-104`](../src/ops/gemm_3dnow.c#L100)), and
- issue `femms` before any scalar (x87) tail and at every exit
  ([`src/ops/gemm_3dnow.c:41-44`](../src/ops/gemm_3dnow.c#L41)).

Keep this discipline in any new MMX/3DNow! kernel.

## im2col convolution

Convolution is im2col + GEMM, batch-1: `(C,H,W)` unrolls to a
`[C·k·k, oH·oW]` patch matrix (zero padding), then one `gemm_f32`/`gemm_i8`
against the `[out_ch, C·k·k]` weight matrix.

- [`src/ops/nn.c:32`](../src/ops/nn.c#L32) `im2col_f32`,
  [`src/ops/nn.c:56`](../src/ops/nn.c#L56) `im2col_i8`.
- Executor conv path: [`src/exec.c:409-461`](../src/exec.c#L409) (f32 and int8
  branches); dense is the K=`in`, N=1 degenerate case
  ([`src/exec.c:463`](../src/exec.c#L463)).
- Pooling/activations/softmax: [`src/ops/nn.c:80-181`](../src/ops/nn.c#L80)
  (softmax always computed in f64 exp/f32 out,
  [`src/ops/nn.c:112`](../src/ops/nn.c#L112)).
- The numpy reference implements the same unroll
  ([`tools/rim/eval_ref.py:27`](../../tools/rim/eval_ref.py#L27)).

## int8 symmetric quantization (per FORMAT.md)

Spec: [`tools/rim/FORMAT.md`](../../tools/rim/FORMAT.md#int8-quantization-scheme-symmetric)
— per-tensor symmetric weights, per-layer symmetric activations, zero_point 0
everywhere.

- **Rounding is half-away-from-zero**, never banker's:
  [`src/ops/nn.c:14`](../src/ops/nn.c#L14) `ri_round` ↔ Python
  `round_half_away` ([`tools/rim/quantize.py:1-14`](../../tools/rim/quantize.py#L1)).
- Weights `w_q = clamp(round(w/scale_w), ±127)`, `scale_w = maxabs/127`
  ([`tools/rim/quantize.py:26-32`](../../tools/rim/quantize.py#L26)).
- Bias is **i32**: `b_q = round(b / (act_scale_in · scale_w))`
  ([`tools/rim/quantize.py:34-36`](../../tools/rim/quantize.py#L34)).
- Layer math on device: `acc_i32 = Σ a_q·w_q + b_q`, then requant
  `out_q = clamp(round(acc · m), ±127)` with the **fp32 multiplier**
  `m = act_scale_in·scale_w/act_scale_out` —
  [`src/ops/nn.c:188`](../src/ops/nn.c#L188) `bias_requant_i8`, driven from
  [`src/exec.c:436-440`](../src/exec.c#L436).
- `act_scale_out == 0` means "dequantize to f32":
  `out = acc · act_scale_in · scale_w`
  ([`src/ops/nn.c:200`](../src/ops/nn.c#L200), used by the last dense so
  logits/softmax run in fp32).
- Activations quantize lazily on entry to the first int8 layer
  ([`src/exec.c:418-427`](../src/exec.c#L418) via
  [`src/ops/nn.c:20`](../src/ops/nn.c#L20) `ri_quant_clamp`); relu/maxpool
  operate directly on the i8 buffer, preserving its scale.
- Calibration: max-abs over 2000 train images at each conv/dense
  pre-activation boundary ([`tools/rim/quantize.py`](../../tools/rim/quantize.py),
  policy notes in [`tools/rim/README.md`](../../tools/rim/README.md#reference-executor-semantics-match-these-in-c)).

Guarantee: int8 logits are **bit-exact** between the C engine and
[`tools/rim/eval_ref.py`](../../tools/rim/eval_ref.py) (verified on both fleet
boxes, commit `d66eaf7`).

## BNN / XNOR integer spec

Binding spec: [`tools/rim/bnn/BNN-SPEC.md`](../../tools/rim/bnn/BNN-SPEC.md).
Everything at inference is integer; bit 1 ⇔ +1, bit 0 ⇔ −1, LSB-first packing,
`sign(0)=+1` realized as `>=` threshold tests.

- **First layer (`first_layer_u8`)**: `acc_j = Σ W_{j,i}·x_i` over raw u8
  pixels in i32, then `h_j = (acc_j >= t_j)` with per-neuron **i32 thresholds**
  baked at export (`t = ceil(τ)`, BN+sign folded) —
  [`src/exec.c:352-359`](../src/exec.c#L352).
- **Hidden layers**: `m = popcount(XNOR(w_row_bits, h_bits))`; the signed dot
  is exactly `s = 2m − n_in`; threshold as above —
  [`src/exec.c:360-365`](../src/exec.c#L360) using the 16-bit-LUT popcount
  ([`src/ops/nn.c:242`](../src/ops/nn.c#L242) `bnn_popcount`,
  [`src/ops/nn.c:256`](../src/ops/nn.c#L256) `bnn_xnor_matches`).
- **Output layer**: no `thresh` field → raw integer scores `2m − n_in`,
  argmax with lowest-index tie-break
  ([`src/exec.c:366-371`](../src/exec.c#L366)); a training-time positive
  scalar temperature can't change the argmax, so integer argmax is exact.
- Manifest op `bdense` parsed at [`src/exec.c:169-181`](../src/exec.c#L169);
  the pure-`bdense` chain executes at
  [`src/exec.c:333-383`](../src/exec.c#L333).
- References: trainer [`tools/rim/bnn/train_bnn.py`](../../tools/rim/bnn/train_bnn.py)
  (BinaryConnect STE + gamma-fixed BatchNorm so BN+sign folds to one integer
  compare), integer reference
  [`tools/rim/bnn/eval_bnn_ref.py`](../../tools/rim/bnn/eval_bnn_ref.py).
  A conforming engine must reproduce `bnn_ref_labels_1000.bin` byte-for-byte.

## Glide XNOR-GEMM

The M5 GPU backend ([`src/gpu/glide_mac.c`](../src/gpu/glide_mac.c); read the
header comment [`src/gpu/glide_mac.c:1-27`](../src/gpu/glide_mac.c#L1)). The
Voodoo has no shaders, so it is used as a parallel XNOR-popcount array via
render-to-texture tricks — and the result is **exact**, not approximate:

```
match[i,j] = Σ_k XNOR(A[i,k], B[k,j])        A,B ∈ {0,1}
signed dot = 2·match − K                      (for {−1,+1} nets)
```

Per k-step, **two draws** ([`src/gpu/glide_mac.c:495-501`](../src/gpu/glide_mac.c#L495)):

1. TMU0 samples the A-texture, TMU1 the B-texture, both `ALPHA_8` with values
   only `0x00`/`0xFF` — so the TMU multiply `A·B/255` is exact (0 or 255).
   Draw 1 uses (A, B): pixels survive where **both bits are 1**.
2. Draw 2 uses the inverted copies (~A, ~B): survives where **both are 0**.
   Together = XNOR.
3. Alpha test `GEQUAL 0x80` gates survivors; color combine emits the constant
   `0xFF080808` and blend `ONE:ONE` adds it
   ([`src/gpu/glide_mac.c:405-428`](../src/gpu/glide_mac.c#L405)
   `set_accum_state`). **The +8/+1 constant**: the framebuffer is 16-bit 565;
   adding 8 per pass in the 8-bit pipeline is exactly **+1 in the stored 5-bit
   red channel** under truncating 888→565 conversion
   (`replicate(n)+8 >> 3 == n+1` for n ∈ 0..30 — comment at
   [`src/gpu/glide_mac.c:414-419`](../src/gpu/glide_mac.c#L414)). Dithering is
   disabled ([`src/gpu/glide_mac.c:327-329`](../src/gpu/glide_mac.c#L327)) —
   it would destroy exact accumulation.
4. **K-chunks of 31**: red has 5 bits (max 31), so after ≤31 k-steps the
   region is read back with `grLfbReadRegion` and accumulated in CPU i32
   (`CHUNK_K` at [`src/gpu/glide_mac.c:433`](../src/gpu/glide_mac.c#L433),
   readback extracts bits 11–15 at
   [`src/gpu/glide_mac.c:435-446`](../src/gpu/glide_mac.c#L435)).

Texture geometry ([`src/gpu/glide_mac.c:17-23`](../src/gpu/glide_mac.c#L17),
[`:454-479`](../src/gpu/glide_mac.c#L454)): `TA[t=k][s=i] = A[i,k]` (A
transposed), `TB[t=k][s=j] = B[k,j]`; one quad per k-step samples row k of
both 256×256 textures (TMU0 s runs along y ⇒ i, TMU1 s along x ⇒ j), so the
same two textures (plus inverted copies) serve all 256 k-steps with **no
re-download inside a tile**. Tile limit M,N,K ≤ 256
([`src/gpu/glide_mac.c:462`](../src/gpu/glide_mac.c#L462)).

Model-level batching: [`src/bnn_eval.c:81-165`](../src/bnn_eval.c#L81)
`bnn_gpu_batch` runs the binary layers as 256×256×256 tiles over
`glide_bgemm` in 256-image batches; the u8 first layer stays on the CPU.

Acceptance driver: [`src/gpu/glide_check.c:45`](../src/gpu/glide_check.c#L45)
`glide_check` — CPU reference ([`:18`](../src/gpu/glide_check.c#L18)
`bgemm_cpu`), exact compare, FNV-1a stability hash across two runs, and the
honest comparator: a **bit-packed CPU XNOR path**
([`src/gpu/glide_check.c:78-109`](../src/gpu/glide_check.c#L78)) that the GPU
does *not* beat (Athlon ~695 vs Voodoo ~61 MMAC/s) — the GPU result is exactness
and offload, not speed (roadmap's "honest numbers").

The GeForce mirror ([`src/gpu/nv_gl.c:1-20`](../src/gpu/nv_gl.c#L1)) uses the
same alpha-tested additive-accumulation idea over an 8-bit GL backbuffer
(chunks of 255); compile-verified, hardware acceptance pending (M6).

## Histogram GBDT (single node)

[`src/train/gbdt.c:34`](../src/train/gbdt.c#L34) `gbdt_run` +
[`src/train/trees.c:60`](../src/train/trees.c#L60) `grow_grad_rec`.
XGBoost-style second-order boosting on u8-binned features (≤255 bins/feature,
prepared by [`tools/rim/tabular/`](../../tools/rim/tabular)); deterministic —
no row/column sampling.

- Gradients per round ([`src/train/gbdt.c:117-127`](../src/train/gbdt.c#L117)):
  logistic `g = p − y`, `h = p(1−p) + 1e-6`; regression `g = F − y`, `h = 1`.
  Base score = train log-odds / mean
  ([`src/train/gbdt.c:83-98`](../src/train/gbdt.c#L83)).
- Split finding: per node, per feature, accumulate `(Σg, Σh, cnt)` into 256
  bins ([`src/train/trees.c:83-89`](../src/train/trees.c#L83)), then scan
  split points `left ≤ b` with gain

  ```
  gain = GL²/(HL+λ) + GR²/(HR+λ) − G²/(H+λ)
  ```

  ([`src/train/trees.c:99-103`](../src/train/trees.c#L99)), `min_child`
  enforced both sides. Leaf value `−G/(H+λ)`
  ([`src/train/trees.c:113`](../src/train/trees.c#L113)).
- Partition is **stable** (order-preserving,
  [`src/train/trees.c:39-51`](../src/train/trees.c#L39)) — part of the
  determinism contract.

## Random forest

[`src/train/forest.c:15`](../src/train/forest.c#L15) `forest_run` +
[`src/train/trees.c:155`](../src/train/trees.c#L155) `grow_gini_rec`:
bootstrap-sampled trees ([`:75-79`](../src/train/forest.c#L75)), per-node
random `⌈√F⌉` feature subset via Fisher–Yates prefix
([`src/train/trees.c:171-179`](../src/train/trees.c#L171)), weighted-gini
split score `(CL·gl + CR·gr)/n` with `g = 2p(1−p)`
([`src/train/trees.c:189-205`](../src/train/trees.c#L189)), leaves store the
class-1 probability. Prediction = probability-average vote; **OOB error**
accumulated from out-of-bag rows per tree
([`src/train/forest.c:87-92`](../src/train/forest.c#L87)).

## MLP backprop (SGD + momentum)

Batch trainer [`src/train/train_nn.c:115`](../src/train/train_nn.c#L115)
`train_nn_run` (arch string `"784,128,10"`; `"784,10"` = logistic regression)
and the step-wise fleet twin
[`src/train/nn_session.c:102`](../src/train/nn_session.c#L102) `nns_step`
(same math, gradients returned instead of applied):

- He-init `N(0, √(2/in))` from the deterministic PRNG
  ([`src/train/train_nn.c:45-55`](../src/train/train_nn.c#L45)).
- Forward `Z = A·Wᵀ + b` via `gemm_f32_nt`, ReLU between layers
  ([`src/train/train_nn.c:66-84`](../src/train/train_nn.c#L66)).
- Softmax cross-entropy; output grad `dZ = (P − onehot)/B`
  ([`src/train/train_nn.c:87-113`](../src/train/train_nn.c#L87),
  [`:249-256`](../src/train/train_nn.c#L249)).
- Backward: `dW = dZᵀ·A` via `gemm_f32_tn`, `dA_prev = dZ·W` via `gemm_f32`
  with ReLU mask ([`src/train/train_nn.c:259-277`](../src/train/train_nn.c#L259)).
- Momentum update `v ← μv − η·g`, `W ← W + v`
  ([`src/train/train_nn.c:279-288`](../src/train/train_nn.c#L279);
  fleet-side in `nns_apply`,
  [`src/train/nn_session.c:216`](../src/train/nn_session.c#L216)).
- Trained models export as dense-f32 `.rim`
  ([`src/rim_save.c:25`](../src/rim_save.c#L25) `rim_save_dense`).

## Linear SVM (Pegasos-style)

[`src/train/svm.c:13`](../src/train/svm.c#L13) `svm_run`: per-sample SGD on
hinge loss, features `u8/255`, labels {0,1}→{−1,+1}, deterministic shuffle.
Update `w ← w − η(λw − 1[y·f(x)<1]·y·x)`, bias updated on margin violation
only ([`src/train/svm.c:60-85`](../src/train/svm.c#L60)).

## Fleet allreduce (data-parallel SGD)

Tree-allreduce with the brain as root: shard-size-weighted gradient average
computed centrally
([`scripts/retro_ai_fleet.py:141-147`](../../scripts/retro_ai_fleet.py#L141)),
broadcast via `NTAPPLY`; identical seed + identical averaged update = weight
lockstep across nodes
([`src/train/nn_session.c:13-16`](../src/train/nn_session.c#L13)). With ≥3 AI
nodes the engine `TENSOR` slots ([`src/serve.c:326`](../src/serve.c#L326))
support node-to-node **ring** relay instead
([`scripts/retro_ai_fleet.py:7-9`](../../scripts/retro_ai_fleet.py#L7)).
Failover = drop node, fold shard into survivors
([`scripts/retro_ai_fleet.py:122-139`](../../scripts/retro_ai_fleet.py#L122)).

## Distributed GBDT (per-level histogram aggregation)

Rows stay sharded per node; only histograms travel. Node side
[`src/train/gb_dist.c`](../src/train/gb_dist.c) (verbs listed at
[`:1-18`](../src/train/gb_dist.c#L1)), brain side
[`scripts/retro_ai_gbdt.py`](../../scripts/retro_ai_gbdt.py):

1. `GBINIT` ships each node its row shard
   ([`src/train/gb_dist.c:40`](../src/train/gb_dist.c#L40)); `GBSUMY` lets the
   brain compute the **global** base score.
2. Per tree level, the brain sends the frontier node ids (`GBHIST`); each node
   returns per-(frontier × feature × 256-bin) cells of
   `(f32 Σg, f32 Σh, u32 cnt)` — 12 bytes/cell
   ([`src/train/gb_dist.c:97-131`](../src/train/gb_dist.c#L97)).
3. The brain **sums the histograms across nodes**
   ([`scripts/retro_ai_gbdt.py:147-157`](../../scripts/retro_ai_gbdt.py#L147))
   and finds splits with the *same* gain formula and tie-breaks as the
   on-device trainer
   ([`scripts/retro_ai_gbdt.py:169-191`](../../scripts/retro_ai_gbdt.py#L169)).
4. `GBSPLIT` broadcasts `(node, feat, thresh, left, right)` decisions — nodes
   re-route their rows locally
   ([`src/train/gb_dist.c:133-150`](../src/train/gb_dist.c#L133)); `GBLEAF`
   applies `F[i] += lr·leaf`
   ([`src/train/gb_dist.c:152-170`](../src/train/gb_dist.c#L152)).

Acceptance: 2-node val AUC within 0.01 of single-node (Δ 0.005 at 50 rounds,
commit `8a822ee`).

## Pipeline parallelism

A trained MLP is split layer-per-machine
([`scripts/retro_ai_pipeline.py:31`](../../scripts/retro_ai_pipeline.py#L31)
`split_mlp` writes stage-1/stage-2 `.rim`s: dense+relu with u8 input, dense
with **f32 input**); activations stream stage-to-stage as `INFER_RUN` payloads
([`scripts/retro_ai_pipeline.py:117-121`](../../scripts/retro_ai_pipeline.py#L117)).
Acceptance: 200/200 labels identical to the single-box model, p50 ≈ 20 ms
(commit `8a822ee`).

## Determinism

The rules that make "same seed ⇒ same bits" hold across the fleet:

- **One PRNG everywhere**: 32-bit xorshift
  ([`src/train/tutil.c:10-18`](../src/train/tutil.c#L10); seed 0 remaps to
  `0xBADC0DE1`, [`:5-8`](../src/train/tutil.c#L5)). `rng_normal` is
  Box–Muller with the second value discarded for determinism-simplicity
  ([`:25-33`](../src/train/tutil.c#L25)); Fisher–Yates shuffle
  ([`:35-44`](../src/train/tutil.c#L35)). The same xorshift generates
  `--glide-check` test matrices
  ([`src/gpu/glide_check.c:63-69`](../src/gpu/glide_check.c#L63)).
- **Fixed accumulation order** in every kernel (scalar oracle contract,
  [`src/ops/gemm_scalar.c:1-5`](../src/ops/gemm_scalar.c#L1)); stable tree
  partitions ([`src/train/trees.c:39`](../src/train/trees.c#L39)); explicit
  tie-breaks (argmax → lowest index; kNN ties per
  [`tools/rim/README.md`](../../tools/rim/README.md#formatmd-ambiguity-resolutions-packerreference-behavior)).
- **Parity classes** ([`../README.md`](../README.md#rules-that-keep-parity-exact)):
  integer paths (int8, BNN/XNOR, GPU bgemm, kNN) must be **bit-exact** against
  the Python references; f32 paths use tolerance (SSE-NN is bit-equal to
  scalar; SSE-NT and all 3DNow! differ in rounding/association order).
- Determinism acceptance (M2): fixed seed reproduces training metrics
  bit-for-bit across runs
  ([`docs/roadmap-fleet-ai.md`](../../docs/roadmap-fleet-ai.md#m2--on-device-training-cpu)),
  and the M7 2-node run is bit-identical to single-node (commit `c4556e4`).
