#!/usr/bin/env python3
"""Train the binarized (XNOR-style) MLP on CIFAR-10 RGB and export the .rim.

Architecture (see BNN-SPEC.md for the exact integer inference spec):

    input  : 3072 raw u8 pixels (RGB, CIFAR binary plane order R,G,B)
    layer1 : bdense 3072 -> 1024, weights {-1,+1}, integer accumulator over u8
             pixels, per-neuron integer threshold -> h in {-1,+1}
    layer2 : bdense 1024 -> 1024, XNOR+popcount, per-neuron threshold
    layer3 : bdense 1024 -> 10, XNOR+popcount, raw integer scores, argmax

Training (fp32 shadow weights, all binarization via straight-through
estimator, seed 42 deterministic):
  - Weights: BinaryConnect-style. Forward uses sign(W_real) (sign(0)=+1);
    gradient passes where |W_real| <= 1; W_real clipped to [-1,1] after each
    Adam step.
  - Activations: y = BN(acc) + beta with BatchNorm *without* gamma (gamma==1,
    beta learned). h = sign(y); gradient uses the hard-tanh STE 1{|y|<=1}.
    Fixing gamma=1 keeps sigma > 0 so the BN+sign always folds to a single
    ">= threshold" test at inference (no per-neuron sign flips to handle).
  - First-layer input is x/255 in fp32 during training; this is an exact
    positive rescale of the integer pixel accumulator, so it folds into the
    integer thresholds (t1 = ceil(255 * tau)).
  - Output layer: logits = exp(log_alpha) * acc with a single learned scalar
    temperature; a global positive scale never changes the argmax, so integer
    argmax(acc) at inference matches training exactly.
  - Adam, batch 100, cosine-free step LR decay, random horizontal flips.

Outputs (out/):
  bnn_params.npz          — shadow weights + BN stats (reproducibility/debug)
  bnn_float_preds_10k.bin — u8 predictions of the float-shadow model on the
                            full 10k test set (for integer-parity measurement)
  bnn-cifar10.rim         — packed model, ops per BNN-SPEC.md
  bnn_train_report.json   — training config + float accuracies
"""
import json
import time
from pathlib import Path

import numpy as np

import bnn_common as C
from rim_pack import tref, write_rim

SEED = 42
EPOCHS = 30
BATCH = 100
LR = 3e-3
LR_DECAY_AT = 24      # epoch index where LR drops x0.1
BN_MOM = 0.9
EPS = C.BN_EPS

OUT = C.OUT_DIR


def binarize(w):
    return np.where(w >= 0, np.float32(1.0), np.float32(-1.0))


def sgn(y):
    return np.where(y >= 0, np.float32(1.0), np.float32(-1.0))


class Adam:
    def __init__(self, params, lr):
        self.lr = lr
        self.b1, self.b2, self.eps = 0.9, 0.999, 1e-8
        self.t = 0
        self.m = {k: np.zeros_like(v) for k, v in params.items()}
        self.v = {k: np.zeros_like(v) for k, v in params.items()}

    def step(self, params, grads):
        self.t += 1
        b1t = 1 - self.b1 ** self.t
        b2t = 1 - self.b2 ** self.t
        for k, g in grads.items():
            self.m[k] = self.b1 * self.m[k] + (1 - self.b1) * g
            self.v[k] = self.b2 * self.v[k] + (1 - self.b2) * g * g
            params[k] -= self.lr * (self.m[k] / b1t) / (np.sqrt(self.v[k] / b2t) + self.eps)


def bn_forward(a, beta):
    mu = a.mean(axis=0)
    var = a.var(axis=0)
    istd = 1.0 / np.sqrt(var + EPS)
    xh = (a - mu) * istd
    return xh + beta, (xh, istd), mu, var


def bn_backward(dy, cache):
    """BN (gamma=1) backward. Returns (da, dbeta)."""
    xh, istd = cache
    B = dy.shape[0]
    dbeta = dy.sum(axis=0)
    da = (istd / B) * (B * dy - dy.sum(axis=0) - xh * (dy * xh).sum(axis=0))
    return da.astype(np.float32), dbeta.astype(np.float32)


def float_predict(params, x_u8, batch=1000):
    """Float-shadow inference (running BN stats). Returns argmax predictions."""
    Wb1 = binarize(params["W1"]); Wb2 = binarize(params["W2"]); Wb3 = binarize(params["W3"])
    t1 = params["rm1"] - params["beta1"] * np.sqrt(params["rv1"] + EPS)
    t2 = params["rm2"] - params["beta2"] * np.sqrt(params["rv2"] + EPS)
    preds = np.empty(len(x_u8), dtype=np.uint8)
    for i in range(0, len(x_u8), batch):
        xb = x_u8[i:i + batch].astype(np.float32) / np.float32(255.0)
        h1 = sgn(xb @ Wb1.T - t1)
        h2 = sgn(h1 @ Wb2.T - t2)
        scores = h2 @ Wb3.T
        preds[i:i + batch] = np.argmax(scores, axis=1).astype(np.uint8)
    return preds


def hflip(x_u8_batch):
    """Random-eligible horizontal flip of CIFAR [N,3072] u8 rows."""
    v = x_u8_batch.reshape(-1, 3, 32, 32)
    return v[:, :, :, ::-1].reshape(-1, 3072)


def main():
    rng = np.random.default_rng(SEED)
    t0 = time.time()

    xtr, ytr = C.load_cifar_train()
    xte, yte = C.load_cifar_test()
    n = len(xtr)
    print(f"train {xtr.shape} test {xte.shape}")

    params = {
        "W1": (rng.uniform(-1, 1, (C.N_H, C.N_IN)) * 0.01).astype(np.float32),
        "beta1": np.zeros(C.N_H, dtype=np.float32),
        "W2": (rng.uniform(-1, 1, (C.N_H, C.N_H)) * 0.01).astype(np.float32),
        "beta2": np.zeros(C.N_H, dtype=np.float32),
        "W3": (rng.uniform(-1, 1, (C.N_OUT, C.N_H)) * 0.01).astype(np.float32),
        "la": np.array(np.log(1 / 32), dtype=np.float32),  # logits = exp(la)*acc
    }
    run = {  # BN running stats (not optimized)
        "rm1": np.zeros(C.N_H, np.float32), "rv1": np.ones(C.N_H, np.float32),
        "rm2": np.zeros(C.N_H, np.float32), "rv2": np.ones(C.N_H, np.float32),
    }
    opt = Adam(params, LR)
    onehot = np.eye(C.N_OUT, dtype=np.float32)

    for ep in range(EPOCHS):
        opt.lr = LR * (0.1 if ep >= LR_DECAY_AT else 1.0)
        order = rng.permutation(n)
        ep_loss, ep_hit = 0.0, 0
        for s in range(0, n, BATCH):
            idx = order[s:s + BATCH]
            xb_u8 = xtr[idx]
            flip = rng.random(len(idx)) < 0.5
            if flip.any():
                xb_u8 = xb_u8.copy()
                xb_u8[flip] = hflip(xb_u8[flip])
            xb = xb_u8.astype(np.float32) / np.float32(255.0)
            yb = ytr[idx]
            Y = onehot[yb]
            B = len(idx)

            Wb1 = binarize(params["W1"])
            Wb2 = binarize(params["W2"])
            Wb3 = binarize(params["W3"])

            a1 = xb @ Wb1.T
            y1, c1, mu1, var1 = bn_forward(a1, params["beta1"])
            h1 = sgn(y1)
            a2 = h1 @ Wb2.T
            y2, c2, mu2, var2 = bn_forward(a2, params["beta2"])
            h2 = sgn(y2)
            a3 = h2 @ Wb3.T
            alpha = np.exp(params["la"])
            logits = alpha * a3
            logits -= logits.max(axis=1, keepdims=True)
            e = np.exp(logits)
            p = e / e.sum(axis=1, keepdims=True)
            ep_loss += -np.log(np.maximum(p[np.arange(B), yb], 1e-12)).sum()
            ep_hit += int((np.argmax(p, axis=1) == yb).sum())

            run["rm1"] = BN_MOM * run["rm1"] + (1 - BN_MOM) * mu1
            run["rv1"] = BN_MOM * run["rv1"] + (1 - BN_MOM) * var1
            run["rm2"] = BN_MOM * run["rm2"] + (1 - BN_MOM) * mu2
            run["rv2"] = BN_MOM * run["rv2"] + (1 - BN_MOM) * var2

            d3 = (p - Y) / B
            dla = np.float32((d3 * a3).sum() * alpha)
            da3 = d3 * alpha
            gW3 = (da3.T @ h2) * (np.abs(params["W3"]) <= 1)
            dh2 = da3 @ Wb3
            dy2 = dh2 * (np.abs(y2) <= 1)
            da2, dbeta2 = bn_backward(dy2, c2)
            gW2 = (da2.T @ h1) * (np.abs(params["W2"]) <= 1)
            dh1 = da2 @ Wb2
            dy1 = dh1 * (np.abs(y1) <= 1)
            da1, dbeta1 = bn_backward(dy1, c1)
            gW1 = (da1.T @ xb) * (np.abs(params["W1"]) <= 1)

            opt.step(params, {"W1": gW1.astype(np.float32), "beta1": dbeta1,
                              "W2": gW2.astype(np.float32), "beta2": dbeta2,
                              "W3": gW3.astype(np.float32), "la": dla})
            for k in ("W1", "W2", "W3"):
                np.clip(params[k], -1, 1, out=params[k])

        full = {**params, **run}
        va = float((float_predict(full, xte[:2000]) == yte[:2000]).mean())
        print(f"epoch {ep + 1:2d}/{EPOCHS} lr {opt.lr:.4g} "
              f"loss {ep_loss / n:.4f} train_acc {ep_hit / n:.4f} "
              f"test2k_acc {va:.4f} [{time.time() - t0:.0f}s]", flush=True)

    # ----------------------------------------------------------- final eval
    full = {**params, **run}
    float_preds = float_predict(full, xte)
    test_acc = float((float_preds == yte).mean())
    train_acc = float((float_predict(full, xtr[:10000]) == ytr[:10000]).mean())
    print(f"FINAL float-shadow: test_acc {test_acc:.4f} train10k_acc {train_acc:.4f}")

    OUT.mkdir(parents=True, exist_ok=True)
    np.savez(OUT / "bnn_params.npz", **params, **run)
    (OUT / "bnn_float_preds_10k.bin").write_bytes(float_preds.tobytes())

    # ------------------------------------------------ integer export (.rim)
    # Thresholds: h = +1 iff acc_int >= t (see BNN-SPEC.md).
    # Layer 1 trains on x/255, so tau_int = 255 * tau_train; t = ceil(tau).
    tau1 = 255.0 * (run["rm1"].astype(np.float64)
                    - params["beta1"].astype(np.float64)
                    * np.sqrt(run["rv1"].astype(np.float64) + EPS))
    tau2 = (run["rm2"].astype(np.float64)
            - params["beta2"].astype(np.float64)
            * np.sqrt(run["rv2"].astype(np.float64) + EPS))
    t1 = np.ceil(tau1).astype(np.int32)
    t2 = np.ceil(tau2).astype(np.int32)

    pw1 = C.pack_pm1_rows(binarize(params["W1"]))
    pw2 = C.pack_pm1_rows(binarize(params["W2"]))
    pw3 = C.pack_pm1_rows(binarize(params["W3"]))

    manifest = {
        "rim": 1,
        "name": "bnn-cifar10-xnor",
        "input": {"shape": [3, 32, 32], "dtype": "u8"},
        "labels": C.CIFAR_LABELS,
        "layers": [
            {"op": "bdense", "n_in": C.N_IN, "n_out": C.N_H,
             "first_layer_u8": True,
             "w": tref(pw1, "bin"), "thresh": tref(t1, "i32")},
            {"op": "bdense", "n_in": C.N_H, "n_out": C.N_H,
             "first_layer_u8": False,
             "w": tref(pw2, "bin"), "thresh": tref(t2, "i32")},
            {"op": "bdense", "n_in": C.N_H, "n_out": C.N_OUT,
             "first_layer_u8": False,
             "w": tref(pw3, "bin")},  # no thresh -> raw integer scores out
        ],
    }
    nbytes = write_rim(OUT / "bnn-cifar10.rim", manifest)
    print(f"wrote {OUT / 'bnn-cifar10.rim'} ({nbytes} bytes)")

    report = {
        "arch": "3072-1024-1024-10 binarized MLP (RGB u8 input)",
        "seed": SEED, "epochs": EPOCHS, "batch": BATCH, "lr": LR,
        "float_shadow_test_acc_10k": test_acc,
        "float_shadow_train_acc_first10k": train_acc,
        "train_seconds": round(time.time() - t0, 1),
    }
    (OUT / "bnn_train_report.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
