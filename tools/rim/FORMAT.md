# .rim format v1 — binding spec for the C loader and the Python packer

Keep `retro-infer/src/rim.c` + `retro-infer/src/exec.c` and `tools/rim/rim_pack.py`
in sync with THIS file. Change here first.

## Container (see also retro-infer/src/rim.c)

All integers u32 little-endian.

| offset | field |
|---|---|
| 0 | magic `RIM1` |
| 4 | flags (bit0 = little-endian payload, always 1) |
| 8 | manifest_len |
| 12 | manifest JSON, UTF-8, no BOM |
| pad | zero bytes to next 16-byte file offset |
| .. | weights blob to EOF |

All tensor `off` values in the manifest are **relative to the weights blob
start**, and every tensor must start at a 16-byte-aligned blob offset (packer
inserts padding between tensors).

## Manifest JSON

```json
{
  "rim": 1,
  "name": "lenet5-mnist-int8",
  "input": {"shape": [1, 28, 28], "dtype": "u8", "div": 255.0},
  "labels": ["0", "1", "..."],
  "layers": [ ... ]
}
```

- `input.shape` — C,H,W for images, [N_features] for tabular.
- `input.dtype` — always `u8` or `f32` on the wire; executor divides by `div`
  (if present) to get fp32, then (int8 models) quantizes by the first layer's
  `act_scale_in`.
- `labels` — class names for pretty-print; optional.

### Tensor reference object

```json
{"off": 1024, "dtype": "f32|i8|i32|bin", "shape": [6,1,5,5], "scale": 0.0123}
```

`scale` only for quantized tensors (symmetric, zero_point = 0 always).

### Layer ops

fp32 model layers carry `"dtype": "f32"`; int8 layers `"dtype": "i8"`.

| op | fields |
|---|---|
| `conv2d` | `in_ch,out_ch,k,stride,pad`, `w` (shape [out_ch,in_ch,k,k]), `b`; int8 adds `act_scale_in`, `act_scale_out` |
| `dense` | `in,out`, `w` (shape [out,in] row-major), `b`; int8 adds `act_scale_in`, `act_scale_out` |
| `relu` | — (works on f32 or i8 buffers) |
| `maxpool` | `k`, `stride` |
| `flatten` | — (C,H,W → C*H*W, row-major, channel-major order preserved) |
| `softmax` | — (always computed in fp32; int8 input dequantized by `act_scale_in`) |
| `knn` | `k`, `n_train`, `n_feat`, `train` (tensor ref, u8), `train_labels` (tensor ref, i8/u8 one byte per label); L2 distance in i32, majority vote, tie → lowest label |

### int8 quantization scheme (symmetric)

- Weights: per-tensor symmetric int8, `w_q = clamp(round(w / scale_w), -127, 127)`.
- Activations: per-layer symmetric int8, calibrated scale;
  `a_q = clamp(round(a / act_scale), -127, 127)`.
- Bias: **i32**, `b_q = round(b / (act_scale_in * scale_w))`.
- Layer math: `acc_i32 = sum(a_q * w_q) + b_q`;
  requant `out_q = clamp(round(acc_i32 * (act_scale_in*scale_w/act_scale_out)), -127, 127)`
  computed via fp32 multiply on device.
- Final dense before softmax: int8 layer may set `"act_scale_out": 0` meaning
  "dequantize to f32" — output = `acc_i32 * act_scale_in * scale_w` (f32
  logits), so softmax/logit-parity runs in fp32.

## Eval file formats (for --eval / parity tests)

- `images.bin` — N images, raw u8, C*H*W bytes each (row-major, channel-major).
- `labels.bin` — N bytes, u8 class index.
- `--logits out.bin` — N * n_classes fp32 LE, written by retro-infer for
  per-logit parity comparison against the Python reference.
