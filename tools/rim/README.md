# tools/rim — Python reference/tooling for the .rim model format

Python side of the Fleet AI inference stack. The binding spec is
[`FORMAT.md`](FORMAT.md) — the C loader/executor (`retro-infer/`) and everything
here are written against that file.

Only dependencies: **numpy** + stdlib. All scripts run from this directory
(`python3 <script>.py`); generated data lives in `data/` (MNIST cache) and
`out/` (models, fixtures), both git-ignored.

## Pipeline

```bash
python3 fetch_mnist.py      # 1. download+decompress MNIST into data/ (idempotent)
python3 train_lenet.py      # 2. train LeNet-5 (>=97%) + logreg (>=91%) -> out/*.npz
python3 export_models.py    # 3. pack the four .rim models into out/
python3 gen_eval.py         # 4. eval fixtures, ref logits, ref_report.json, mini models
```

Inspect / verify any model:

```bash
python3 rim_dump.py out/lenet5-mnist-int8.rim            # pretty-print + invariants
python3 rim_dump.py out/lenet5-mnist-int8.rim --npz t.npz  # extract tensors as fp32
```

Reference-execute a model (the C side must match this bit-for-bit on int8,
closely on f32):

```bash
python3 eval_ref.py out/lenet5-mnist-f32.rim \
    --images out/mnist_test_1000.images.bin \
    --labels out/mnist_test_1000.labels.bin \
    --logits /tmp/logits.bin
```

## Files

| file | role |
|---|---|
| `rim_common.py` | shared dtype map, `round_half_away`, `align16` |
| `fetch_mnist.py` | MNIST download/cache + IDX loaders (`load_mnist()`) |
| `train_lenet.py` | pure-numpy LeNet-5 + logreg trainers (SGD+momentum, im2col); also the fp32 forward ops used for calibration |
| `rim_pack.py` | `.rim` writer library (`tref()` + `write_rim()`) and template+npz CLI |
| `rim_dump.py` | `.rim` parser/verifier/pretty-printer, `--npz` tensor extraction |
| `quantize.py` | symmetric per-tensor int8 weight quantizer, activation calibration (2000 train images), i32 bias |
| `export_models.py` | builds the four shipping models (below) |
| `eval_ref.py` | numpy reference executor for `.rim` (f32 + int8 + knn paths) |
| `gen_eval.py` | eval bins, reference logits, `ref_report.json`, mini debug models |

## Shipped models (out/)

- `lenet5-mnist-f32.rim` — all-f32 LeNet-5.
- `lenet5-mnist-int8.rim` — conv/dense int8 per FORMAT.md; relu/maxpool/flatten
  run on i8 buffers; the **last dense has `act_scale_out: 0`** → fp32 logits →
  softmax in fp32.
- `logreg-mnist-f32.rim` — dense 784→10 + softmax.
- `knn-mnist.rim` — op `knn`, k=3, 2000 train vectors (200 per class, first
  occurrences in train order), u8 features/labels.

Debug fixtures for isolating single-op bugs in the C executor:

- `mini_dense_f32.rim` + `mini_dense.input.bin` (1×8 f32 LE) +
  `mini_dense.expected.bin` (1×4 f32 LE). f32 input, no `div`.
- `mini_conv_int8.rim` + `mini_conv.input.bin` (1×1×6×6 u8) +
  `mini_conv.expected.bin` (1×2×4×4 f32 LE). u8 input `div=255`, single int8
  conv with `act_scale_out=0` → f32 output.

Parity fixtures: `mnist_test_1000.{images,labels}.bin`,
`ref_logits_lenet5_{f32,int8}.bin` (N×10 fp32 LE), `ref_report.json`.

## Reference-executor semantics (match these in C)

- **Rounding**: every `round()` in quantize/requant is half-**away-from-zero**
  (`sign(x)*floor(|x|+0.5)`), matching C `roundf()` — *not* numpy's default
  banker's rounding.
- **int8 layers**: `acc_i32 = sum(a_q*w_q) + b_q`; requant multiplies `acc` by
  the fp32 multiplier `act_scale_in*scale_w/act_scale_out`, rounds, clamps to
  ±127. `act_scale_out == 0` → dequantize: `acc * act_scale_in * scale_w` (f32).
- **Input**: u8 with `div` → fp32 `x/div`; then an int8 first layer quantizes by
  its `act_scale_in`. relu/maxpool/flatten preserve the i8 buffer and its scale,
  so each int8 layer's `act_scale_in` equals the previous conv/dense
  `act_scale_out` (the executor asserts this chain).

## FORMAT.md ambiguity resolutions (packer/reference behavior)

Decisions made where the spec is silent; treat as normative until FORMAT.md says
otherwise:

1. **`dtype` on non-weight layers** — `relu`/`maxpool`/`flatten`/`softmax`/`knn`
   layers carry **no** `dtype` field; only `conv2d`/`dense` do ("f32"/"i8").
   Executors must not require `dtype` on the parameterless ops.
2. **knn input has no `div`** — `knn-mnist.rim` declares `input.dtype: "u8"`
   *without* `div`; the executor feeds raw u8 features to the integer L2 path.
   (A `div` on a knn model would make the distances non-integer.)
3. **knn distance/ties** — "L2 distance in i32" is implemented as *squared* L2
   (monotone-equivalent, avoids sqrt): `a·a + b·b − 2a·b`, all i32 (max ~153M
   for 784 u8 features, fits). Neighbor-distance ties at the k-boundary are
   broken by **lowest train index** (stable sort). Vote ties → lowest label per
   spec.
4. **Softmax `act_scale_in`** — a softmax layer only needs `act_scale_in` when
   its input buffer is i8. In our int8 model the preceding dense dequantizes
   (`act_scale_out: 0`), so the softmax layer carries no fields.
5. **Calibration point** — activation scales are calibrated on the conv/dense
   **pre-activation outputs** (before relu/maxpool), since requantization
   happens at the conv/dense output; the i8 buffer then flows unchanged through
   relu/maxpool/flatten. Input scale calibrated on `pixels/255` (maxabs 1.0 →
   `act_scale_in = 1/127` for conv1).
6. **Zero tensors** — a weight tensor of all zeros gets `scale = 1.0` (avoids
   div-by-zero; doesn't occur in the shipped models).
7. **argmax ties** (top-1 and majority vote) — first/lowest index wins.
8. **Manifest floats** — scales are serialized at full double precision in the
   JSON; the executor casts to f32 before arithmetic to match C `float` math.

## Results (2026-07-17 build, seed 42)

Full 10k MNIST test set, via `eval_ref.py` on the packed `.rim` files:

| model | top-1 |
|---|---|
| lenet5-mnist-f32 | **97.97%** |
| lenet5-mnist-int8 | **97.94%** (Δ −0.03% vs f32) |
| logreg-mnist-f32 | **91.96%** |
| knn-mnist (2000 refs) | 88.0% (on the 1000-image eval set) |

On the 1000-image parity set: f32 97.0%, int8 97.0%, prediction agreement
99.8%, max per-logit |diff| 0.596 (the int8 logit quantum
`act_scale_in*scale_w` of the last dense is 0.00182 → ≈328 steps; the error is
dominated by accumulated earlier-layer quantization noise, not the final
dequant). See `out/ref_report.json`.
