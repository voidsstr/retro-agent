#!/usr/bin/env python3
"""Prepare the California Housing regression dataset as device-ready binaries.

Deterministic (seed 42). Uses sklearn.datasets.fetch_california_housing
(cached under ~/scikit_learn_data). All 8 numeric features are quantile-binned
into <= 255 bins (one u8 byte per feature); the regression target (median
house value, in $100k) stays float32 little-endian. Rows are written in
seed-42 permuted order so the C side takes the first 80% as train and the
last 20% as val, sequentially.

Outputs (in out/):
  calif.features.bin  u8, row-major [N, F]
  calif.targets.bin   f32 LE, [N]
  calif.meta.json     N, F, columns, bin summary, target mean/std, split rule
"""
import json
import os

import numpy as np
from sklearn.datasets import fetch_california_housing

from binning import MAX_BINS, permute_and_split_meta, quantile_bin

SEED = 42
TRAIN_FRAC = 0.8
HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "out")


def main():
    os.makedirs(OUT, exist_ok=True)
    ds = fetch_california_housing()
    x = np.asarray(ds.data, dtype=np.float64)
    y = np.asarray(ds.target, dtype=np.float32)
    n, f = x.shape
    cols = list(ds.feature_names)

    features = np.zeros((n, f), dtype=np.uint8)
    col_meta = []
    for j, col in enumerate(cols):
        idx, interior = quantile_bin(x[:, j], MAX_BINS)
        features[:, j] = idx
        col_meta.append({
            "name": col, "kind": "numeric",
            "n_bins": int(len(interior) + 1),
            "raw_min": float(x[:, j].min()), "raw_max": float(x[:, j].max()),
            "edge_min": float(interior[0]) if len(interior) else None,
            "edge_max": float(interior[-1]) if len(interior) else None,
            "n_interior_edges": int(len(interior)),
        })

    perm, n_train = permute_and_split_meta(n, SEED, TRAIN_FRAC)
    features = features[perm]
    y = y[perm]

    features.tofile(os.path.join(OUT, "calif.features.bin"))
    y.astype("<f4").tofile(os.path.join(OUT, "calif.targets.bin"))

    meta = {
        "dataset": "california-housing",
        "source": "sklearn.datasets.fetch_california_housing "
                  "(StatLib/1990 US census)",
        "N": int(n), "F": int(f),
        "feature_dtype": "u8", "feature_layout": "row-major [N, F]",
        "target_dtype": "f32 little-endian",
        "target_units": "median house value in $100,000",
        "columns": cols,
        "column_bins": col_meta,
        "target_stats": {
            "mean": float(y.mean()), "std": float(y.std()),
            "min": float(y.min()), "max": float(y.max()),
        },
        "split": {
            "rule": "rows in the .bin files are already permuted with "
                    "numpy.random.default_rng(42).permutation(N); "
                    "train = first floor(0.8*N) rows, val = remaining rows, "
                    "taken sequentially",
            "seed": SEED, "train_frac": TRAIN_FRAC,
            "n_train": int(n_train), "n_val": int(n - n_train),
        },
        "binning": {"max_bins": MAX_BINS,
                    "numeric_rule": "quantile bins, duplicate edges collapsed; "
                                    "bin = searchsorted(interior_edges, v, 'right')"},
    }
    with open(os.path.join(OUT, "calif.meta.json"), "w") as fh:
        json.dump(meta, fh, indent=2)

    print(f"N={n} F={f}  train={n_train} val={n - n_train}  "
          f"target mean={y.mean():.4f} std={y.std():.4f}")
    print("wrote out/calif.features.bin out/calif.targets.bin out/calif.meta.json")


if __name__ == "__main__":
    main()
