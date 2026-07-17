#!/usr/bin/env python3
"""Numpy REFERENCE EXECUTOR for .rim models, implementing FORMAT.md exactly.

Both f32 and int8 paths, plus knn. All round() operations are
round-half-AWAY-FROM-ZERO (sign(x)*floor(|x|+0.5)) to match C roundf(), not
numpy's default banker's rounding. int8 layer math: acc_i32 = sum(a_q*w_q)+b_q,
requant via a fp32 multiplier as specced.

CLI:
    python3 eval_ref.py model.rim --images imgs.bin --labels lbls.bin \
        [--logits out.bin] [--limit N]

Prints top-1 accuracy; --logits dumps N*n_classes fp32 LE pre-softmax logits.
"""
import argparse
import sys
from pathlib import Path

import numpy as np

from rim_common import DTYPES, round_half_away
from rim_dump import read_rim, get_tensor


# ------------------------------------------------------------------ shape ops

def _im2col(x, k, stride, pad):
    """x (N,C,H,W) -> (N, C*k*k, Ho*Wo). Preserves dtype."""
    N, C, H, W = x.shape
    Ho = (H + 2 * pad - k) // stride + 1
    Wo = (W + 2 * pad - k) // stride + 1
    if pad:
        x = np.pad(x, ((0, 0), (0, 0), (pad, pad), (pad, pad)))
    cols = np.empty((N, C, k, k, Ho, Wo), dtype=x.dtype)
    for i in range(k):
        for j in range(k):
            cols[:, :, i, j] = x[:, :, i:i + stride * Ho:stride, j:j + stride * Wo:stride]
    return cols.reshape(N, C * k * k, Ho * Wo), Ho, Wo


def _maxpool(x, k, stride):
    N, C, H, W = x.shape
    Ho = (H - k) // stride + 1
    Wo = (W - k) // stride + 1
    out = None
    for i in range(k):
        for j in range(k):
            v = x[:, :, i:i + stride * Ho:stride, j:j + stride * Wo:stride]
            out = v.copy() if out is None else np.maximum(out, v)
    return out


def _quantize_act(a_f32, act_scale):
    """f32 activations -> int8 with per-layer symmetric scale."""
    q = round_half_away(a_f32.astype(np.float32) / np.float32(act_scale))
    return np.clip(q, -127, 127).astype(np.int8)


# ------------------------------------------------------------------ executor

class Buffer:
    """Activation buffer between layers: array + quantization state.
    kind: 'f32' | 'i8' | 'u8'; scale set when kind == 'i8'."""
    def __init__(self, data, kind, scale=None):
        self.data = data
        self.kind = kind
        self.scale = scale


def _as_i8_input(buf, act_scale_in):
    if buf.kind == "f32":
        return _quantize_act(buf.data, act_scale_in)
    if buf.kind == "i8":
        if not np.isclose(buf.scale, act_scale_in, rtol=1e-6):
            raise ValueError(
                f"chained act scale mismatch: buffer {buf.scale} vs layer "
                f"act_scale_in {act_scale_in}")
        return buf.data
    raise ValueError(f"int8 layer got {buf.kind} input buffer")


def _finish_int8(acc_i32, layer, w_scale):
    """Requant (act_scale_out != 0) or dequantize to f32 (act_scale_out == 0)."""
    asi = layer["act_scale_in"]
    aso = layer["act_scale_out"]
    if aso == 0:
        out = acc_i32.astype(np.float32) * np.float32(asi * w_scale)
        return out, "f32", None
    m = np.float32(asi * w_scale / aso)
    q = round_half_away(acc_i32.astype(np.float32) * m)
    return np.clip(q, -127, 127).astype(np.int8), "i8", aso


def _run_layer(buf, layer, blob):
    op = layer["op"]

    if op == "conv2d":
        w = get_tensor(layer["w"], blob)
        b = get_tensor(layer["b"], blob)
        k, stride, pad = layer["k"], layer["stride"], layer["pad"]
        O = layer["out_ch"]
        if layer.get("dtype") == "i8":
            a_q = _as_i8_input(buf, layer["act_scale_in"])
            cols, Ho, Wo = _im2col(a_q.astype(np.int32), k, stride, pad)
            wm = w.reshape(O, -1).astype(np.int32)
            acc = np.matmul(wm, cols) + b.astype(np.int32)[None, :, None]
            out, kind, scale = _finish_int8(acc, layer, layer["w"]["scale"])
            return Buffer(out.reshape(a_q.shape[0], O, Ho, Wo), kind, scale)
        x = buf.data.astype(np.float32)
        cols, Ho, Wo = _im2col(x, k, stride, pad)
        y = np.matmul(w.reshape(O, -1), cols) + b[None, :, None]
        return Buffer(y.reshape(x.shape[0], O, Ho, Wo).astype(np.float32), "f32")

    if op == "dense":
        w = get_tensor(layer["w"], blob)   # [out, in] row-major
        b = get_tensor(layer["b"], blob)
        if layer.get("dtype") == "i8":
            a_q = _as_i8_input(buf, layer["act_scale_in"])
            acc = a_q.astype(np.int32) @ w.T.astype(np.int32) + b.astype(np.int32)
            out, kind, scale = _finish_int8(acc, layer, layer["w"]["scale"])
            return Buffer(out, kind, scale)
        y = buf.data.astype(np.float32) @ w.T + b
        return Buffer(y.astype(np.float32), "f32")

    if op == "relu":
        return Buffer(np.maximum(buf.data, 0), buf.kind, buf.scale)

    if op == "maxpool":
        return Buffer(_maxpool(buf.data, layer["k"], layer["stride"]),
                      buf.kind, buf.scale)

    if op == "flatten":
        # C,H,W -> C*H*W row-major, channel-major order preserved
        return Buffer(buf.data.reshape(buf.data.shape[0], -1), buf.kind, buf.scale)

    if op == "softmax":
        z = buf.data
        if buf.kind == "i8":
            scale = layer.get("act_scale_in", buf.scale)
            z = z.astype(np.float32) * np.float32(scale)
        z = z.astype(np.float32)
        z = z - z.max(axis=1, keepdims=True)
        e = np.exp(z)
        return Buffer(e / e.sum(axis=1, keepdims=True), "f32")

    if op == "knn":
        train = get_tensor(layer["train"], blob).astype(np.int32)         # (n_train, n_feat)
        labels = get_tensor(layer["train_labels"], blob).astype(np.int64)  # (n_train,)
        k = layer["k"]
        x = buf.data.reshape(buf.data.shape[0], -1).astype(np.int32)
        # squared-L2 in i32 integer math: |a-b|^2 = a.a + b.b - 2 a.b
        # (u8 features, 784 dims: max ~51M per term, fits i32)
        a2 = (x * x).sum(axis=1, dtype=np.int32)
        b2 = (train * train).sum(axis=1, dtype=np.int32)
        ab = x @ train.T                                                   # i32 matmul
        dist = a2[:, None] + b2[None, :] - 2 * ab
        # k nearest; ties in distance broken by lowest train index (stable sort)
        nn = np.argsort(dist, axis=1, kind="stable")[:, :k]
        votes = labels[nn]                                                 # (N, k)
        n_lab = int(labels.max()) + 1
        preds = np.empty(len(x), dtype=np.int64)
        for i in range(len(x)):
            counts = np.bincount(votes[i], minlength=n_lab)
            preds[i] = counts.argmax()   # majority vote; tie -> lowest label
        return Buffer(preds, "f32")

    raise ValueError(f"unknown op {op!r}")


def prepare_input(manifest, raw):
    """raw: np array shaped (N, ...) in the manifest input dtype. Returns the
    initial Buffer per FORMAT.md input rules."""
    inp = manifest["input"]
    shape = tuple(inp["shape"])
    x = raw.reshape((-1,) + shape)
    if "div" in inp and inp["div"]:
        return Buffer(x.astype(np.float32) / np.float32(inp["div"]), "f32")
    if inp["dtype"] == "u8":
        return Buffer(x, "u8")   # e.g. knn: raw integer features
    return Buffer(x.astype(np.float32), "f32")


def run_model(manifest, blob, raw_input, batch=250):
    """Run all layers. Returns (output, logits):
    - output: final layer output (softmax probs, f32 logits, or knn labels)
    - logits: last pre-softmax f32 buffer (None for knn)
    """
    outs, logits = [], []
    has_softmax = any(l["op"] == "softmax" for l in manifest["layers"])
    is_knn = any(l["op"] == "knn" for l in manifest["layers"])
    N = raw_input.shape[0]
    for i in range(0, N, batch):
        buf = prepare_input(manifest, raw_input[i:i + batch])
        pre_softmax = None
        for layer in manifest["layers"]:
            if layer["op"] == "softmax":
                z = buf.data
                if buf.kind == "i8":
                    s = layer.get("act_scale_in", buf.scale)
                    z = z.astype(np.float32) * np.float32(s)
                pre_softmax = z.astype(np.float32)
            buf = _run_layer(buf, layer, blob)
        outs.append(buf.data)
        if is_knn:
            continue
        if pre_softmax is None:  # no softmax: final f32 output is the logits
            pre_softmax = buf.data.astype(np.float32)
        logits.append(pre_softmax)
    output = np.concatenate(outs, axis=0)
    return output, (None if is_knn else np.concatenate(logits, axis=0))


def predict(manifest, blob, raw_input, batch=250):
    out, logits = run_model(manifest, blob, raw_input, batch=batch)
    if logits is None:          # knn: output is already class labels
        return out.astype(np.int64), None
    return logits.argmax(axis=1), logits


def load_eval_inputs(manifest, images_path):
    inp = manifest["input"]
    dt = DTYPES[inp["dtype"]]
    per = int(np.prod(inp["shape"])) * dt.itemsize
    data = Path(images_path).read_bytes()
    if len(data) % per:
        raise ValueError(f"{images_path}: size {len(data)} not a multiple of "
                         f"{per} bytes/sample")
    n = len(data) // per
    return np.frombuffer(data, dtype=dt).reshape((n,) + tuple(inp["shape"]))


def main():
    ap = argparse.ArgumentParser(description="Reference-execute a .rim model")
    ap.add_argument("rim")
    ap.add_argument("--images", required=True, help="raw input samples (see FORMAT.md)")
    ap.add_argument("--labels", help="N bytes u8 class index")
    ap.add_argument("--logits", help="write N*n_classes fp32 LE logits here")
    ap.add_argument("--limit", type=int, help="use only first N samples")
    args = ap.parse_args()

    manifest, blob, _ = read_rim(args.rim)
    x = load_eval_inputs(manifest, args.images)
    if args.limit:
        x = x[:args.limit]
    preds, logits = predict(manifest, blob, x)

    if args.labels:
        y = np.frombuffer(Path(args.labels).read_bytes(), dtype=np.uint8)[:len(x)]
        acc = float((preds == y).mean())
        print(f"{manifest.get('name')}: top-1 accuracy {acc * 100:.2f}% "
              f"({int((preds == y).sum())}/{len(x)})")
    else:
        print(f"{manifest.get('name')}: predictions {preds[:20].tolist()}"
              + (" ..." if len(preds) > 20 else ""))

    if args.logits:
        if logits is None:
            print("no logits for this model (knn)", file=sys.stderr)
            sys.exit(1)
        Path(args.logits).write_bytes(
            np.ascontiguousarray(logits, dtype="<f4").tobytes())
        print(f"wrote logits {logits.shape} fp32 LE -> {args.logits}")


if __name__ == "__main__":
    main()
