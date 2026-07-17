#!/usr/bin/env python3
"""Symmetric per-tensor int8 quantization + activation calibration (FORMAT.md).

- Weights: per-tensor symmetric, zero_point=0:
      scale_w = maxabs(w)/127, w_q = clamp(round(w/scale_w), -127, 127)
- Activations: per-layer symmetric scale calibrated from the max-abs observed
  at each conv/dense pre-activation boundary over a fp32 forward pass on a
  calibration batch:  act_scale = maxabs/127
- Bias: i32, b_q = round(b / (act_scale_in * scale_w))

All rounding is half-away-from-zero to match C roundf().

Standalone:  python3 quantize.py   (needs out/lenet5_f32.npz; writes
out/lenet5_int8.npz with q-weights, i32 biases and scales)
"""
from pathlib import Path

import numpy as np

from rim_common import round_half_away

OUT_DIR = Path(__file__).resolve().parent / "out"
CALIB_N = 2000


def quantize_weight(w):
    """Returns (w_q int8, scale float)."""
    maxabs = float(np.abs(w).max())
    scale = maxabs / 127.0 if maxabs > 0 else 1.0
    q = np.clip(round_half_away(w / scale), -127, 127).astype(np.int8)
    return q, scale


def quantize_bias(b, act_scale_in, w_scale):
    """i32 bias per FORMAT.md."""
    return round_half_away(b / (act_scale_in * w_scale)).astype(np.int32)


def calibrate_lenet(params, calib_images_u8, batch=500):
    """Run the fp32 LeNet forward over calibration images, recording max-abs at
    each layer boundary. Returns {boundary: act_scale} with act_scale=maxabs/127.

    Boundaries: 'input' (post /255), and pre-activation outputs of
    conv1, conv2, fc1, fc2, fc3.
    """
    from train_lenet import lenet_forward
    record = {}
    for i in range(0, len(calib_images_u8), batch):
        xb = calib_images_u8[i:i + batch].astype(np.float32)[:, None] / np.float32(255.0)
        lenet_forward(params, xb, record=record)
    return {k: (v / 127.0 if v > 0 else 1.0) for k, v in record.items()}


def quantize_lenet(params, calib_images_u8):
    """Full int8 quantization of the LeNet weights per FORMAT.md.

    Returns a dict per quantized layer:
      {name: {w_q, w_scale, b_q, act_scale_in, act_scale_out}}
    The activation scale chains: relu/maxpool/flatten preserve the int8 scale,
    so each layer's act_scale_in equals the previous conv/dense act_scale_out
    (conv1's act_scale_in is the input scale). fc3 (last dense) gets
    act_scale_out = 0 -> dequantize to fp32 logits.
    """
    scales = calibrate_lenet(params, calib_images_u8)
    chain = [
        ("conv1", "input"), ("conv2", "conv1"),
        ("fc1", "conv2"), ("fc2", "fc1"), ("fc3", "fc2"),
    ]
    out = {}
    for name, in_boundary in chain:
        w = params[f"{name}_w"]
        b = params[f"{name}_b"]
        w_q, w_scale = quantize_weight(w)
        asi = scales[in_boundary]
        aso = 0.0 if name == "fc3" else scales[name]
        out[name] = {
            "w_q": w_q, "w_scale": w_scale,
            "b_q": quantize_bias(b, asi, w_scale),
            "act_scale_in": asi, "act_scale_out": aso,
        }
    return out


def main():
    from fetch_mnist import load_mnist
    xtr, _, _, _ = load_mnist()
    params = dict(np.load(OUT_DIR / "lenet5_f32.npz"))
    q = quantize_lenet(params, xtr[:CALIB_N])
    flat = {}
    for name, d in q.items():
        flat[f"{name}_wq"] = d["w_q"]
        flat[f"{name}_bq"] = d["b_q"]
        flat[f"{name}_wscale"] = np.float64(d["w_scale"])
        flat[f"{name}_act_in"] = np.float64(d["act_scale_in"])
        flat[f"{name}_act_out"] = np.float64(d["act_scale_out"])
    np.savez(OUT_DIR / "lenet5_int8.npz", **flat)
    for name, d in q.items():
        print(f"{name}: w_scale={d['w_scale']:.6g} act_in={d['act_scale_in']:.6g} "
              f"act_out={d['act_scale_out']:.6g}")
    print(f"saved {OUT_DIR / 'lenet5_int8.npz'}")


if __name__ == "__main__":
    main()
