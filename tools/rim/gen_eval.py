#!/usr/bin/env python3
"""Generate eval fixtures + reference outputs for the C side (retro-infer):

  out/mnist_test_1000.images.bin / .labels.bin   (first 1000 test samples)
  out/ref_logits_lenet5_f32.bin / ref_logits_lenet5_int8.bin  (fp32 LE)
  out/ref_report.json                            (top-1 accs + int8-vs-f32 stats)
  out/mini_dense_f32.rim  + mini_dense.input.bin / mini_dense.expected.bin
  out/mini_conv_int8.rim  + mini_conv.input.bin  / mini_conv.expected.bin

Run after export_models.py.
"""
import json
from pathlib import Path

import numpy as np

from fetch_mnist import load_mnist
from eval_ref import predict, run_model
from quantize import quantize_weight, quantize_bias
from rim_dump import read_rim
from rim_pack import tref, write_rim

OUT_DIR = Path(__file__).resolve().parent / "out"
N_EVAL = 1000


def eval_rim(name, images):
    manifest, blob, _ = read_rim(OUT_DIR / name)
    inp_shape = tuple(manifest["input"]["shape"])
    x = images.reshape((-1,) + inp_shape)
    preds, logits = predict(manifest, blob, x)
    return manifest, preds, logits


def build_mini_dense():
    """Single f32 dense 8->4, f32 input, no div. Returns paths written."""
    rng = np.random.default_rng(1234)
    w = rng.standard_normal((4, 8)).astype(np.float32)
    b = rng.standard_normal(4).astype(np.float32)
    manifest = {
        "rim": 1,
        "name": "mini-dense-f32",
        "input": {"shape": [8], "dtype": "f32"},
        "layers": [
            {"op": "dense", "dtype": "f32", "in": 8, "out": 4,
             "w": tref(w, "f32"), "b": tref(b, "f32")},
        ],
    }
    write_rim(OUT_DIR / "mini_dense_f32.rim", manifest)

    x = rng.standard_normal((1, 8)).astype(np.float32)
    m, blob, _ = read_rim(OUT_DIR / "mini_dense_f32.rim")
    out, _ = run_model(m, blob, x)
    (OUT_DIR / "mini_dense.input.bin").write_bytes(
        np.ascontiguousarray(x, dtype="<f4").tobytes())
    (OUT_DIR / "mini_dense.expected.bin").write_bytes(
        np.ascontiguousarray(out, dtype="<f4").tobytes())
    print(f"mini_dense_f32: out={out.ravel().tolist()}")


def build_mini_conv():
    """Single int8 conv2d 1->2 k3 s1 p0 on a 1x6x6 u8 input (div=255),
    act_scale_out=0 -> f32 output."""
    rng = np.random.default_rng(5678)
    w = (rng.standard_normal((2, 1, 3, 3)) * 0.5).astype(np.float32)
    b = (rng.standard_normal(2) * 0.1).astype(np.float32)
    w_q, w_scale = quantize_weight(w)
    act_scale_in = 1.0 / 127.0          # input in [0,1] after /255
    b_q = quantize_bias(b, act_scale_in, w_scale)
    manifest = {
        "rim": 1,
        "name": "mini-conv-int8",
        "input": {"shape": [1, 6, 6], "dtype": "u8", "div": 255.0},
        "layers": [
            {"op": "conv2d", "dtype": "i8", "in_ch": 1, "out_ch": 2,
             "k": 3, "stride": 1, "pad": 0,
             "w": tref(w_q, "i8", scale=w_scale), "b": tref(b_q, "i32"),
             "act_scale_in": act_scale_in, "act_scale_out": 0.0},
        ],
    }
    write_rim(OUT_DIR / "mini_conv_int8.rim", manifest)

    x = rng.integers(0, 256, size=(1, 1, 6, 6), dtype=np.uint8)
    m, blob, _ = read_rim(OUT_DIR / "mini_conv_int8.rim")
    out, _ = run_model(m, blob, x)
    (OUT_DIR / "mini_conv.input.bin").write_bytes(x.tobytes())
    (OUT_DIR / "mini_conv.expected.bin").write_bytes(
        np.ascontiguousarray(out, dtype="<f4").tobytes())
    print(f"mini_conv_int8: out shape {out.shape}, first row {out.ravel()[:4].tolist()}")


def main():
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    _, _, xte, yte = load_mnist()
    images = np.ascontiguousarray(xte[:N_EVAL])         # (1000,28,28) u8
    labels = np.ascontiguousarray(yte[:N_EVAL])
    (OUT_DIR / "mnist_test_1000.images.bin").write_bytes(images.tobytes())
    (OUT_DIR / "mnist_test_1000.labels.bin").write_bytes(labels.tobytes())
    print(f"wrote mnist_test_1000 images/labels ({N_EVAL} samples)")

    report = {"models": {}}
    logits_by_model = {}
    for rim_name, key in [
        ("lenet5-mnist-f32.rim", "lenet5-mnist-f32"),
        ("lenet5-mnist-int8.rim", "lenet5-mnist-int8"),
        ("logreg-mnist-f32.rim", "logreg-mnist-f32"),
        ("knn-mnist.rim", "knn-mnist"),
    ]:
        manifest, preds, logits = eval_rim(rim_name, images)
        acc = float((preds == labels).mean())
        notes = f"first {N_EVAL} MNIST test images"
        report["models"][key] = {"top1_acc": acc, "notes": notes}
        logits_by_model[key] = logits
        print(f"{key}: top-1 {acc * 100:.2f}%")
        if key == "lenet5-mnist-f32":
            (OUT_DIR / "ref_logits_lenet5_f32.bin").write_bytes(
                np.ascontiguousarray(logits, dtype="<f4").tobytes())
        elif key == "lenet5-mnist-int8":
            (OUT_DIR / "ref_logits_lenet5_int8.bin").write_bytes(
                np.ascontiguousarray(logits, dtype="<f4").tobytes())

    # int8 vs f32 parity
    lf = logits_by_model["lenet5-mnist-f32"]
    li = logits_by_model["lenet5-mnist-int8"]
    agree = float((lf.argmax(1) == li.argmax(1)).mean())
    max_abs = float(np.abs(lf - li).max())
    # one int8 logit step = act_scale_in * w_scale of the last dense
    m_int8, _, _ = read_rim(OUT_DIR / "lenet5-mnist-int8.rim")
    last_dense = [l for l in m_int8["layers"] if l["op"] == "dense"][-1]
    step = last_dense["act_scale_in"] * last_dense["w"]["scale"]
    acc_f32 = report["models"]["lenet5-mnist-f32"]["top1_acc"]
    acc_i8 = report["models"]["lenet5-mnist-int8"]["top1_acc"]
    report["int8_vs_f32"] = {
        "prediction_agreement": agree,
        "top1_acc_delta": acc_i8 - acc_f32,
        "max_abs_logit_diff": max_abs,
        "int8_logit_step": step,
        "max_abs_logit_diff_in_int8_steps": max_abs / step,
    }
    print(f"int8 vs f32: agreement {agree * 100:.2f}%, acc delta "
          f"{(acc_i8 - acc_f32) * 100:+.2f}%, max logit diff {max_abs:.4f} "
          f"({max_abs / step:.1f} int8 steps)")

    (OUT_DIR / "ref_report.json").write_text(json.dumps(report, indent=2) + "\n")
    print(f"wrote {OUT_DIR / 'ref_report.json'}")

    build_mini_dense()
    build_mini_conv()


if __name__ == "__main__":
    main()
