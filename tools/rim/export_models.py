#!/usr/bin/env python3
"""Export the four .rim models into tools/rim/out/:

  - lenet5-mnist-f32.rim   (all layers f32)
  - lenet5-mnist-int8.rim  (conv/dense int8; last dense act_scale_out=0)
  - logreg-mnist-f32.rim
  - knn-mnist.rim          (k=3, 2000 train vectors subsampled evenly by class)

Needs out/lenet5_f32.npz + out/logreg_f32.npz from train_lenet.py.
"""
from pathlib import Path

import numpy as np

from fetch_mnist import load_mnist
from quantize import quantize_lenet, CALIB_N
from rim_pack import tref, write_rim

OUT_DIR = Path(__file__).resolve().parent / "out"
LABELS = [str(i) for i in range(10)]
KNN_K = 3
KNN_N_TRAIN = 2000


def lenet_f32_manifest(p):
    return {
        "rim": 1,
        "name": "lenet5-mnist-f32",
        "input": {"shape": [1, 28, 28], "dtype": "u8", "div": 255.0},
        "labels": LABELS,
        "layers": [
            {"op": "conv2d", "dtype": "f32", "in_ch": 1, "out_ch": 6,
             "k": 5, "stride": 1, "pad": 2,
             "w": tref(p["conv1_w"], "f32"), "b": tref(p["conv1_b"], "f32")},
            {"op": "relu"},
            {"op": "maxpool", "k": 2, "stride": 2},
            {"op": "conv2d", "dtype": "f32", "in_ch": 6, "out_ch": 16,
             "k": 5, "stride": 1, "pad": 0,
             "w": tref(p["conv2_w"], "f32"), "b": tref(p["conv2_b"], "f32")},
            {"op": "relu"},
            {"op": "maxpool", "k": 2, "stride": 2},
            {"op": "flatten"},
            {"op": "dense", "dtype": "f32", "in": 400, "out": 120,
             "w": tref(p["fc1_w"], "f32"), "b": tref(p["fc1_b"], "f32")},
            {"op": "relu"},
            {"op": "dense", "dtype": "f32", "in": 120, "out": 84,
             "w": tref(p["fc2_w"], "f32"), "b": tref(p["fc2_b"], "f32")},
            {"op": "relu"},
            {"op": "dense", "dtype": "f32", "in": 84, "out": 10,
             "w": tref(p["fc3_w"], "f32"), "b": tref(p["fc3_b"], "f32")},
            {"op": "softmax"},
        ],
    }


def lenet_int8_manifest(q):
    def conv(name, in_ch, out_ch, pad):
        d = q[name]
        return {"op": "conv2d", "dtype": "i8", "in_ch": in_ch, "out_ch": out_ch,
                "k": 5, "stride": 1, "pad": pad,
                "w": tref(d["w_q"], "i8", scale=d["w_scale"]),
                "b": tref(d["b_q"], "i32"),
                "act_scale_in": d["act_scale_in"],
                "act_scale_out": d["act_scale_out"]}

    def dense(name, n_in, n_out):
        d = q[name]
        return {"op": "dense", "dtype": "i8", "in": n_in, "out": n_out,
                "w": tref(d["w_q"], "i8", scale=d["w_scale"]),
                "b": tref(d["b_q"], "i32"),
                "act_scale_in": d["act_scale_in"],
                "act_scale_out": d["act_scale_out"]}

    return {
        "rim": 1,
        "name": "lenet5-mnist-int8",
        "input": {"shape": [1, 28, 28], "dtype": "u8", "div": 255.0},
        "labels": LABELS,
        "layers": [
            conv("conv1", 1, 6, 2),
            {"op": "relu"},
            {"op": "maxpool", "k": 2, "stride": 2},
            conv("conv2", 6, 16, 0),
            {"op": "relu"},
            {"op": "maxpool", "k": 2, "stride": 2},
            {"op": "flatten"},
            dense("fc1", 400, 120),
            {"op": "relu"},
            dense("fc2", 120, 84),
            {"op": "relu"},
            dense("fc3", 84, 10),          # act_scale_out == 0 -> fp32 logits
            {"op": "softmax"},             # fp32 input (last dense dequantized)
        ],
    }


def logreg_manifest(p):
    return {
        "rim": 1,
        "name": "logreg-mnist-f32",
        "input": {"shape": [784], "dtype": "u8", "div": 255.0},
        "labels": LABELS,
        "layers": [
            {"op": "dense", "dtype": "f32", "in": 784, "out": 10,
             "w": tref(p["w"], "f32"), "b": tref(p["b"], "f32")},
            {"op": "softmax"},
        ],
    }


def knn_subsample(xtr, ytr, n_train=KNN_N_TRAIN):
    """Evenly-by-class subsample: n_train/10 first occurrences of each class,
    in class order (deterministic)."""
    per = n_train // 10
    feats = np.empty((n_train, 784), dtype=np.uint8)
    labels = np.empty(n_train, dtype=np.uint8)
    row = 0
    for c in range(10):
        idx = np.flatnonzero(ytr == c)[:per]
        feats[row:row + per] = xtr[idx].reshape(per, 784)
        labels[row:row + per] = c
        row += per
    return feats, labels


def knn_manifest(feats, labels):
    return {
        "rim": 1,
        "name": "knn-mnist",
        "input": {"shape": [784], "dtype": "u8"},   # raw u8 features, no div
        "labels": LABELS,
        "layers": [
            {"op": "knn", "k": KNN_K, "n_train": len(feats), "n_feat": 784,
             "train": tref(feats, "u8"),
             "train_labels": tref(labels, "u8")},
        ],
    }


def main():
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    xtr, ytr, _, _ = load_mnist()

    lenet = dict(np.load(OUT_DIR / "lenet5_f32.npz"))
    logreg = dict(np.load(OUT_DIR / "logreg_f32.npz"))

    n = write_rim(OUT_DIR / "lenet5-mnist-f32.rim", lenet_f32_manifest(lenet))
    print(f"wrote lenet5-mnist-f32.rim ({n} bytes)")

    q = quantize_lenet(lenet, xtr[:CALIB_N])
    n = write_rim(OUT_DIR / "lenet5-mnist-int8.rim", lenet_int8_manifest(q))
    print(f"wrote lenet5-mnist-int8.rim ({n} bytes)")

    n = write_rim(OUT_DIR / "logreg-mnist-f32.rim", logreg_manifest(logreg))
    print(f"wrote logreg-mnist-f32.rim ({n} bytes)")

    feats, labels = knn_subsample(xtr, ytr)
    n = write_rim(OUT_DIR / "knn-mnist.rim", knn_manifest(feats, labels))
    print(f"wrote knn-mnist.rim ({n} bytes)")


if __name__ == "__main__":
    main()
