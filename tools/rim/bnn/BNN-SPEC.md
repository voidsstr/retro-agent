# BNN (XNOR) inference spec — bnn-cifar10.rim

Binding integer spec for the binarized-MLP reference (Fleet AI milestone M5).
The Python reference (`eval_bnn_ref.py`), the C engine, and the 3dfx GPU
backend must all produce **bit-identical** hidden activations, output scores,
and predicted labels for the same input bytes. Everything at inference is
integer; no floating point anywhere.

## Model

`out/bnn-cifar10.rim`, name `bnn-cifar10-xnor`:

```
input   u8[3072]   CIFAR-10 RGB image (layout below)
layer0  bdense 3072 -> 1024   first_layer_u8=true   (u8-input layer)
layer1  bdense 1024 -> 1024   first_layer_u8=false  (XNOR+popcount)
layer2  bdense 1024 ->   10   first_layer_u8=false, no thresh (output scores)
```

All weights are binary {-1,+1}; all hidden activations are binary {-1,+1}.
Bit encoding everywhere: **bit 1 &hArr; +1, bit 0 &hArr; -1**. `sign(0) = +1`
by definition (implemented as `>=` comparisons below — there is no explicit
sign function at inference).

## Input layout

One image = 3072 bytes, exactly the pixel block of a CIFAR-10 binary record
(the label byte stripped): **R plane (32×32, row-major), then G plane, then
B plane**. Manifest `input.shape` is `[3, 32, 32]` (C,H,W), `dtype` `u8`, no
`div` — pixels are consumed as raw integers 0..255, never scaled.

`out/cifar_test_1000.images.bin` is the first 1000 test images in this exact
layout, concatenated (3 072 000 bytes); `out/cifar_test_1000.labels.bin` is
the 1000 ground-truth labels, one u8 each.

## Layer semantics

### Layer 0 — first layer (u8 input), `first_layer_u8: true`

For each output neuron `j` (0..n_out-1):

```
acc_j = sum_{i=0}^{n_in-1} W_{j,i} * x_i        (int32 arithmetic)
h_j   = +1  if acc_j >= t_j   else -1
```

- `W_{j,i} ∈ {-1,+1}` decoded from the packed `w` tensor (packing below).
- `x_i` = raw u8 pixel value (0..255) at input index `i` in the layout above.
- `t_j` = `thresh[j]`, an **i32** per-neuron threshold baked at export.
- Range: |acc| ≤ 3072·255 = 783 360 — int32 is sufficient (and required;
  accumulate in i32, not i16).

### Hidden layers — XNOR + popcount, `first_layer_u8: false`

Inputs are the previous layer's binary activations, packed to bits with the
same LSB-first convention as weights. For each output neuron `j`:

```
m_j   = popcount( XNOR( w_row_bits_j , h_bits ) )    over n_in bit positions
s_j   = 2*m_j - n_in                                 (the signed dot product)
h_j   = +1  if s_j >= t_j   else -1
```

`m_j` counts positions where weight and activation agree; `s_j` equals
`sum_i w_i·h_i` exactly. When `n_in` is not a multiple of 8, the pad bits are
0 in **both** operands, so XNOR over the pad bits contributes matches —
implementations must count only the low `n_in` bit positions (mask the last
byte, or subtract the pad contribution). In this model `n_in` ∈ {1024} for
XNOR layers, so no padding exists.

### Output layer — bdense with **no `thresh` field**

```
score_j = 2*m_j - n_in        (int32, j = 0..9)
class   = argmax_j score_j    — ties resolve to the LOWEST index j
```

No threshold, no scaling: the raw integer scores are the model output.
(Training used `logits = alpha·score` with a single positive scalar `alpha`,
which cannot change the argmax, so integer argmax is exact.)

## `bdense` manifest op (FORMAT.md extension)

```json
{"op": "bdense",
 "n_in": 3072, "n_out": 1024,
 "first_layer_u8": true,
 "w":      {"off": ..., "dtype": "bin", "shape": [n_out, ceil(n_in/8)]},
 "thresh": {"off": ..., "dtype": "i32", "shape": [n_out]}}
```

| field | meaning |
|---|---|
| `n_in`, `n_out` | logical layer dimensions (in **weights/bits**, not bytes) |
| `first_layer_u8` | `true`: inputs are raw u8 values, use the integer-matmul form; `false`: inputs are packed activation bits, use XNOR+popcount |
| `w` | tensor ref, dtype `bin`: 1 bit/weight, **row-major, LSB-first** (below) |
| `thresh` | tensor ref, dtype `i32`, shape `[n_out]`. **Optional**: when absent the layer emits raw integer scores `2m − n_in` (the output layer) and must be last |

### `bin` tensor packing

- Row-major by output neuron: row `j` occupies `ceil(n_in/8)` consecutive
  bytes starting at `off + j*ceil(n_in/8)`.
- **LSB-first within each byte**: bit `k` (value `1<<k`) of byte `b` in row
  `j` encodes `W[j, 8*b + k]`; bit 1 &hArr; +1, bit 0 &hArr; −1.
  (`numpy.packbits(..., bitorder="little")`.)
- Pad bits of the last byte of a row (when `n_in % 8 != 0`) are 0.
- **Manifest `shape` of a `bin` ref is the packed byte shape**
  `[n_out, ceil(n_in/8)]`, itemsize 1 — this keeps `rim_dump`'s
  bounds/alignment verification exact. The logical bit width comes from the
  layer's `n_in`.
- Note: `FORMAT.md` names `bin` as a dtype but `rim_common.DTYPES` does not
  define it yet; the bnn tooling registers `"bin" → u8` at import
  (`bnn_common.py`), and `check_rim.py` wraps the stock `rim_dump`
  verification with that registration. When `rim_common` adopts `bin`
  natively it should use exactly this definition.

Activation bits at runtime use the identical packing (LSB-first, bit 1 = +1)
so weight rows and activation vectors XNOR byte-for-byte.

## Threshold derivation (export-time, documented for reproducibility)

Training uses BatchNorm (gamma fixed to 1, beta learned) followed by
`sign()`. At inference BN+sign folds to a pure integer comparison:

```
y = (a − μ)/σ + β  with σ = sqrt(running_var + 1e-5) > 0
sign(y) = +1  ⟺  a ≥ τ,   τ = μ − β·σ        (μ, σ from running stats)
```

- Hidden XNOR layers: the training-time accumulator already equals the
  integer `s = 2m − n_in`, so `t = ceil(τ)` — the smallest integer `t` with
  `s ≥ t ⟺ s ≥ τ` for integer `s`.
- Layer 0: training consumed `x/255`, so the training accumulator is
  `acc/255`; therefore `τ_int = 255·τ_train` and `t = ceil(255·τ_train)`.
- **Rounding rule: `t = ceil(τ)`** computed in float64
  (`numpy.ceil`), then cast to i32. `ceil` (not round/floor) is what makes
  the integer test `acc ≥ t` exactly equivalent to the real-valued test
  `acc ≥ τ`, including the `sign(0)=+1` boundary when τ is an exact integer.
- Because gamma ≡ 1, σ > 0 always and no per-neuron inequality flips exist.

## Reference outputs (out/)

| file | contents |
|---|---|
| `bnn-cifar10.rim` | the packed model |
| `cifar_test_1000.images.bin` | 1000 × 3072 u8, input layout above |
| `cifar_test_1000.labels.bin` | 1000 u8 ground-truth labels |
| `bnn_ref_labels_1000.bin` | 1000 u8 predictions from the integer reference |
| `bnn_report.json` | integer accuracy (full 10k + first 1000) and agreement with the float-shadow model |

A conforming engine run over `cifar_test_1000.images.bin` must reproduce
`bnn_ref_labels_1000.bin` **byte-for-byte** (and, layer by layer, the same
activation bits and output scores).
