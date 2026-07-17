#!/usr/bin/env python3
"""Pure-integer reference evaluator for the BNN .rim (see BNN-SPEC.md).

Runs out/bnn-cifar10.rim over the CIFAR-10 test set using ONLY integer numpy
ops in the inference path (int32 matmul for the u8 first layer, byte-wise
XNOR + popcount-table for the hidden/output layers). This is the bit-exact
reference the C engine and the 3dfx GPU backend must match.

Writes to out/:
  cifar_test_1000.images.bin — first 1000 test images, raw u8, 3072 bytes each
                               (R plane 32*32 row-major, then G, then B — the
                               exact CIFAR-10 binary record pixel layout and
                               the exact input the spec defines)
  cifar_test_1000.labels.bin — 1000 u8 ground-truth labels
  bnn_ref_labels_1000.bin    — 1000 u8 predicted labels (integer path)
  bnn_report.json            — accuracy on full 10k + on the 1000 subset,
                               plus agreement with the float-shadow preds
"""
import json
import sys
import time
from pathlib import Path

import numpy as np

import bnn_common as C
from rim_dump import read_rim, get_tensor

OUT = C.OUT_DIR
RIM_PATH = OUT / "bnn-cifar10.rim"


def load_model(path):
    manifest, blob, _ = read_rim(path)
    layers = []
    for ly in manifest["layers"]:
        assert ly["op"] == "bdense", f"unsupported op {ly['op']}"
        w = get_tensor(ly["w"], blob)                      # u8 packed bytes
        assert ly["w"]["dtype"] == "bin"
        assert w.shape == (ly["n_out"], (ly["n_in"] + 7) // 8)
        thresh = None
        if "thresh" in ly:
            thresh = get_tensor(ly["thresh"], blob)        # i32 [n_out]
            assert ly["thresh"]["dtype"] == "i32"
        layers.append({"n_in": ly["n_in"], "n_out": ly["n_out"],
                       "first_layer_u8": bool(ly.get("first_layer_u8")),
                       "w_packed": w, "thresh": thresh})
    return manifest, layers


def infer_batch(layers, x_u8, w1_pm1):
    """Integer-only forward pass for a [B, 3072] u8 batch -> u8 predictions."""
    # Layer 1: signed integer accumulate over raw u8 pixels.
    l1 = layers[0]
    acc = x_u8.astype(np.int32) @ w1_pm1.T                 # [B, n_out] i32
    h_bits = (acc >= l1["thresh"]).astype(np.uint8)        # 1 <=> +1

    # Hidden layers: XNOR + popcount on packed bits.
    for ly in layers[1:]:
        hp = np.packbits(h_bits, axis=1, bitorder="little")  # [B, n_in/8]
        m = C.xnor_popcount_matches(hp, ly["w_packed"])      # matches, i32
        s = 2 * m - np.int32(ly["n_in"])                     # signed sum
        if ly["thresh"] is None:                             # output layer
            return s                                         # integer scores
        h_bits = (s >= ly["thresh"]).astype(np.uint8)
    raise AssertionError("model has no output layer (last bdense must omit thresh)")


def main():
    t0 = time.time()
    manifest, layers = load_model(RIM_PATH)
    print(f"loaded {RIM_PATH.name}: {manifest['name']}, "
          f"{len(layers)} bdense layers")

    xte, yte = C.load_cifar_test()

    # Layer-1 weights unpacked once to {-1,+1} i32 for the integer matmul.
    w1_pm1 = C.unpack_rows_pm1(layers[0]["w_packed"], layers[0]["n_in"])

    preds = np.empty(len(xte), dtype=np.uint8)
    scores0 = None
    for i in range(0, len(xte), 500):
        s = infer_batch(layers, xte[i:i + 500], w1_pm1)
        # argmax: ties resolve to the LOWEST class index (np.argmax = first max)
        preds[i:i + 500] = np.argmax(s, axis=1).astype(np.uint8)
        if i == 0:
            scores0 = s[0]
        print(f"  {min(i + 500, len(xte))}/{len(xte)} "
              f"[{time.time() - t0:.0f}s]", flush=True)

    acc_10k = float((preds == yte).mean())
    acc_1000 = float((preds[:1000] == yte[:1000]).mean())
    print(f"integer test acc: 10k {acc_10k:.4f}  first-1000 {acc_1000:.4f}")
    print(f"image0 integer scores: {scores0.tolist()} -> pred {preds[0]}")

    OUT.mkdir(parents=True, exist_ok=True)
    (OUT / "cifar_test_1000.images.bin").write_bytes(xte[:1000].tobytes())
    (OUT / "cifar_test_1000.labels.bin").write_bytes(yte[:1000].tobytes())
    (OUT / "bnn_ref_labels_1000.bin").write_bytes(preds[:1000].tobytes())

    report = {
        "rim": RIM_PATH.name,
        "model": manifest["name"],
        "test_acc_10k": acc_10k,
        "test_acc_1000": acc_1000,
        "n_test_10k": int(len(yte)),
    }

    fp = OUT / "bnn_float_preds_10k.bin"
    if fp.exists():
        float_preds = np.frombuffer(fp.read_bytes(), dtype=np.uint8)
        agree = float((float_preds == preds).mean())
        report["float_shadow_agreement_10k"] = agree
        report["float_shadow_mismatches_10k"] = int((float_preds != preds).sum())
        print(f"float-shadow agreement: {agree:.4f} "
              f"({report['float_shadow_mismatches_10k']} mismatches / {len(preds)})")
    else:
        print(f"warning: {fp} not found, skipping agreement check", file=sys.stderr)

    (OUT / "bnn_report.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
