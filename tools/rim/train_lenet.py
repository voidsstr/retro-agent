#!/usr/bin/env python3
"""Pure-numpy LeNet-5 + logistic-regression MNIST trainers.

LeNet-5: conv2d(1->6,k5,pad2) relu maxpool2 -> conv2d(6->16,k5,pad0) relu maxpool2
         -> flatten(400) -> dense120 relu -> dense84 relu -> dense10 softmax
Inputs are pixels/255.0.  SGD + momentum, im2col-vectorized.
Saves out/lenet5_f32.npz and out/logreg_f32.npz.

The forward-op helpers here are also imported by quantize.py for calibration.
"""
import time
from pathlib import Path

import numpy as np

from fetch_mnist import load_mnist

OUT_DIR = Path(__file__).resolve().parent / "out"
SEED = 42


# ---------------------------------------------------------------- conv helpers

def conv_out_hw(H, W, k, stride, pad):
    return (H + 2 * pad - k) // stride + 1, (W + 2 * pad - k) // stride + 1


def im2col(x, k, stride, pad):
    """x (N,C,H,W) -> cols (N, C*k*k, Ho*Wo)."""
    N, C, H, W = x.shape
    Ho, Wo = conv_out_hw(H, W, k, stride, pad)
    if pad:
        x = np.pad(x, ((0, 0), (0, 0), (pad, pad), (pad, pad)))
    cols = np.empty((N, C, k, k, Ho, Wo), dtype=x.dtype)
    for i in range(k):
        for j in range(k):
            cols[:, :, i, j] = x[:, :, i:i + stride * Ho:stride, j:j + stride * Wo:stride]
    return cols.reshape(N, C * k * k, Ho * Wo)


def col2im(dcols, xshape, k, stride, pad):
    N, C, H, W = xshape
    Ho, Wo = conv_out_hw(H, W, k, stride, pad)
    dxp = np.zeros((N, C, H + 2 * pad, W + 2 * pad), dtype=dcols.dtype)
    dcols = dcols.reshape(N, C, k, k, Ho, Wo)
    for i in range(k):
        for j in range(k):
            dxp[:, :, i:i + stride * Ho:stride, j:j + stride * Wo:stride] += dcols[:, :, i, j]
    return dxp[:, :, pad:H + pad, pad:W + pad] if pad else dxp


def conv2d_forward(x, w, b, stride, pad):
    """x (N,C,H,W), w (O,C,k,k), b (O,) -> (y, cache)."""
    N = x.shape[0]
    O, C, k, _ = w.shape
    Ho, Wo = conv_out_hw(x.shape[2], x.shape[3], k, stride, pad)
    cols = im2col(x, k, stride, pad)                    # (N, C*k*k, Ho*Wo)
    wm = w.reshape(O, C * k * k)
    y = np.matmul(wm, cols) + b[None, :, None]          # (N, O, Ho*Wo)
    return y.reshape(N, O, Ho, Wo), (cols, x.shape, w.shape, stride, pad)


def conv2d_backward(dy, w, cache, need_dx=True):
    cols, xshape, wshape, stride, pad = cache
    N = dy.shape[0]
    O, C, k, _ = wshape
    dym = dy.reshape(N, O, -1)                          # (N, O, Ho*Wo)
    dw = np.einsum("nop,ncp->oc", dym, cols).reshape(wshape)
    db = dym.sum(axis=(0, 2))
    dx = None
    if need_dx:
        wm = w.reshape(O, C * k * k)
        dcols = np.matmul(wm.T[None], dym)              # (N, C*k*k, Ho*Wo)
        dx = col2im(dcols, xshape, k, stride, pad)
    return dx, dw, db


def maxpool2_forward(x):
    """2x2 stride-2 max pool. x (N,C,H,W)."""
    N, C, H, W = x.shape
    xr = x.reshape(N, C, H // 2, 2, W // 2, 2).transpose(0, 1, 2, 4, 3, 5)
    xr = xr.reshape(N, C, H // 2, W // 2, 4)
    idx = xr.argmax(axis=-1)
    out = np.take_along_axis(xr, idx[..., None], axis=-1)[..., 0]
    return out, (idx, x.shape)


def maxpool2_backward(dy, cache):
    idx, xshape = cache
    N, C, H, W = xshape
    dxr = np.zeros((N, C, H // 2, W // 2, 4), dtype=dy.dtype)
    np.put_along_axis(dxr, idx[..., None], dy[..., None], axis=-1)
    dxr = dxr.reshape(N, C, H // 2, W // 2, 2, 2).transpose(0, 1, 2, 4, 3, 5)
    return dxr.reshape(N, C, H, W)


def dense_forward(x, w, b):
    """x (N,in), w (out,in) row-major, b (out,)."""
    return x @ w.T + b


def softmax(z):
    z = z - z.max(axis=1, keepdims=True)
    e = np.exp(z)
    return e / e.sum(axis=1, keepdims=True)


# ---------------------------------------------------------------- LeNet model

def init_lenet(rng):
    def he(shape, fan_in):
        return (rng.standard_normal(shape) * np.sqrt(2.0 / fan_in)).astype(np.float32)

    return {
        "conv1_w": he((6, 1, 5, 5), 25), "conv1_b": np.zeros(6, np.float32),
        "conv2_w": he((16, 6, 5, 5), 150), "conv2_b": np.zeros(16, np.float32),
        "fc1_w": he((120, 400), 400), "fc1_b": np.zeros(120, np.float32),
        "fc2_w": he((84, 120), 120), "fc2_b": np.zeros(84, np.float32),
        "fc3_w": he((10, 84), 84), "fc3_b": np.zeros(10, np.float32),
    }


def lenet_forward(p, x, record=None):
    """x (N,1,28,28) fp32 in [0,1]. Returns logits (N,10).

    If `record` is a dict, stores max-abs of each conv/dense pre-activation
    output (and the input) for int8 calibration.
    """
    def rec(key, a):
        if record is not None:
            m = float(np.abs(a).max())
            record[key] = max(record.get(key, 0.0), m)

    rec("input", x)
    c1, _ = conv2d_forward(x, p["conv1_w"], p["conv1_b"], 1, 2)
    rec("conv1", c1)
    a1, _ = maxpool2_forward(np.maximum(c1, 0))
    c2, _ = conv2d_forward(a1, p["conv2_w"], p["conv2_b"], 1, 0)
    rec("conv2", c2)
    a2, _ = maxpool2_forward(np.maximum(c2, 0))
    f = a2.reshape(a2.shape[0], -1)                     # (N,400) channel-major
    z1 = dense_forward(f, p["fc1_w"], p["fc1_b"])
    rec("fc1", z1)
    z2 = dense_forward(np.maximum(z1, 0), p["fc2_w"], p["fc2_b"])
    rec("fc2", z2)
    z3 = dense_forward(np.maximum(z2, 0), p["fc3_w"], p["fc3_b"])
    rec("fc3", z3)
    return z3


def lenet_loss_grads(p, x, y):
    """Forward + backward. y int labels. Returns (loss, grads dict)."""
    N = x.shape[0]
    c1, cache1 = conv2d_forward(x, p["conv1_w"], p["conv1_b"], 1, 2)
    r1 = np.maximum(c1, 0)
    p1, mcache1 = maxpool2_forward(r1)
    c2, cache2 = conv2d_forward(p1, p["conv2_w"], p["conv2_b"], 1, 0)
    r2 = np.maximum(c2, 0)
    p2, mcache2 = maxpool2_forward(r2)
    f = p2.reshape(N, -1)
    z1 = dense_forward(f, p["fc1_w"], p["fc1_b"]); a1 = np.maximum(z1, 0)
    z2 = dense_forward(a1, p["fc2_w"], p["fc2_b"]); a2 = np.maximum(z2, 0)
    z3 = dense_forward(a2, p["fc3_w"], p["fc3_b"])

    probs = softmax(z3)
    loss = -np.log(np.maximum(probs[np.arange(N), y], 1e-12)).mean()

    g = {}
    dz3 = probs.copy(); dz3[np.arange(N), y] -= 1.0; dz3 /= N
    g["fc3_w"] = dz3.T @ a2; g["fc3_b"] = dz3.sum(0)
    da2 = dz3 @ p["fc3_w"]; dz2 = da2 * (z2 > 0)
    g["fc2_w"] = dz2.T @ a1; g["fc2_b"] = dz2.sum(0)
    da1 = dz2 @ p["fc2_w"]; dz1 = da1 * (z1 > 0)
    g["fc1_w"] = dz1.T @ f; g["fc1_b"] = dz1.sum(0)
    df = dz1 @ p["fc1_w"]
    dp2 = df.reshape(p2.shape)
    dr2 = maxpool2_backward(dp2, mcache2)
    dc2 = dr2 * (c2 > 0)
    dp1, g["conv2_w"], g["conv2_b"] = conv2d_backward(dc2, p["conv2_w"], cache2)
    dr1 = maxpool2_backward(dp1, mcache1)
    dc1 = dr1 * (c1 > 0)
    _, g["conv1_w"], g["conv1_b"] = conv2d_backward(dc1, p["conv1_w"], cache1, need_dx=False)
    return loss, g


def lenet_accuracy(p, x_u8, y, batch=500):
    correct = 0
    for i in range(0, len(x_u8), batch):
        xb = x_u8[i:i + batch].astype(np.float32)[:, None] / np.float32(255.0)
        logits = lenet_forward(p, xb)
        correct += int((logits.argmax(1) == y[i:i + batch]).sum())
    return correct / len(x_u8)


def train_lenet(xtr, ytr, xte, yte, max_epochs=4, target=0.97):
    rng = np.random.default_rng(SEED)
    p = init_lenet(rng)
    vel = {k: np.zeros_like(v) for k, v in p.items()}
    lr, mu, batch = 0.05, 0.9, 64
    n = len(xtr)
    for epoch in range(max_epochs):
        t0 = time.time()
        order = rng.permutation(n)
        tot_loss = 0.0
        nb = 0
        for i in range(0, n, batch):
            idx = order[i:i + batch]
            xb = xtr[idx].astype(np.float32)[:, None] / np.float32(255.0)
            yb = ytr[idx]
            loss, g = lenet_loss_grads(p, xb, yb)
            tot_loss += loss; nb += 1
            for k in p:
                vel[k] = mu * vel[k] - lr * g[k]
                p[k] = (p[k] + vel[k]).astype(np.float32)
        acc = lenet_accuracy(p, xte, yte)
        print(f"lenet epoch {epoch + 1}: loss={tot_loss / nb:.4f} "
              f"test_acc={acc * 100:.2f}% ({time.time() - t0:.0f}s)", flush=True)
        lr *= 0.5
        if acc >= target:
            break
    return p, acc


# ------------------------------------------------------------------- logreg

def train_logreg(xtr, ytr, xte, yte, epochs=8, target=0.91):
    rng = np.random.default_rng(SEED)
    w = (rng.standard_normal((10, 784)) * 0.01).astype(np.float32)
    b = np.zeros(10, np.float32)
    vw = np.zeros_like(w); vb = np.zeros_like(b)
    lr, mu, batch = 0.1, 0.9, 128
    xtef = xte.reshape(len(xte), -1).astype(np.float32) / np.float32(255.0)
    n = len(xtr)
    acc = 0.0
    for epoch in range(epochs):
        order = rng.permutation(n)
        for i in range(0, n, batch):
            idx = order[i:i + batch]
            xb = xtr[idx].reshape(len(idx), -1).astype(np.float32) / np.float32(255.0)
            yb = ytr[idx]
            probs = softmax(xb @ w.T + b)
            dz = probs; dz[np.arange(len(idx)), yb] -= 1.0; dz /= len(idx)
            gw = dz.T @ xb; gb = dz.sum(0)
            vw = mu * vw - lr * gw; vb = mu * vb - lr * gb
            w = (w + vw).astype(np.float32); b = (b + vb).astype(np.float32)
        acc = float(((xtef @ w.T + b).argmax(1) == yte).mean())
        print(f"logreg epoch {epoch + 1}: test_acc={acc * 100:.2f}%", flush=True)
        if acc >= target and epoch >= 2:
            break
    return {"w": w, "b": b}, acc


def main():
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    xtr, ytr, xte, yte = load_mnist()

    p, acc = train_lenet(xtr, ytr, xte, yte)
    if acc < 0.97:
        raise SystemExit(f"lenet test accuracy {acc:.4f} < 0.97 target")
    np.savez(OUT_DIR / "lenet5_f32.npz", **p)
    print(f"saved {OUT_DIR / 'lenet5_f32.npz'} (test acc {acc * 100:.2f}%)")

    lp, lacc = train_logreg(xtr, ytr, xte, yte)
    if lacc < 0.91:
        raise SystemExit(f"logreg test accuracy {lacc:.4f} < 0.91 target")
    np.savez(OUT_DIR / "logreg_f32.npz", **lp)
    print(f"saved {OUT_DIR / 'logreg_f32.npz'} (test acc {lacc * 100:.2f}%)")


if __name__ == "__main__":
    main()
