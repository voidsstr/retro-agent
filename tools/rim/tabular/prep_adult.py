#!/usr/bin/env python3
"""Prepare the UCI Adult (census income) dataset as device-ready u8 binaries.

Deterministic (seed 42). Downloads adult.data + adult.test from UCI (cached in
cache/), drops rows with missing values ('?'), integer-codes categoricals in
alphabetical order, quantile-bins numerics into <= 255 bins. Every feature is
one byte. Rows are written in seed-42 permuted order so the C side takes the
first 80% as train and the last 20% as val, sequentially.

Outputs (in out/):
  adult.features.bin  u8, row-major [N, F]
  adult.labels.bin    u8, 0/1  (>50K = 1)
  adult.meta.json     N, F, columns, bin summary, class balance, split rule
"""
import json
import os
import urllib.request

import numpy as np

from binning import MAX_BINS, encode_categorical, permute_and_split_meta, quantile_bin

SEED = 42
TRAIN_FRAC = 0.8
BASE = "https://archive.ics.uci.edu/ml/machine-learning-databases/adult/"
HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "out")
CACHE = os.path.join(HERE, "cache")

COLUMNS = [
    "age", "workclass", "fnlwgt", "education", "education_num",
    "marital_status", "occupation", "relationship", "race", "sex",
    "capital_gain", "capital_loss", "hours_per_week", "native_country",
]
NUMERIC = {"age", "fnlwgt", "education_num", "capital_gain", "capital_loss",
           "hours_per_week"}


def fetch(name):
    os.makedirs(CACHE, exist_ok=True)
    path = os.path.join(CACHE, name)
    if not os.path.exists(path):
        print(f"downloading {BASE}{name} ...")
        urllib.request.urlretrieve(BASE + name, path)
    return path


def load_rows(path):
    """Parse one UCI adult file -> (list of 14-field rows, list of 0/1 labels)."""
    rows, labels, dropped = [], [], 0
    with open(path, "r", encoding="latin-1") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("|"):  # adult.test header line
                continue
            parts = [p.strip() for p in line.split(",")]
            if len(parts) != 15:
                continue
            label = parts[14].rstrip(".")  # adult.test labels end with '.'
            if "?" in parts[:14]:
                dropped += 1
                continue
            rows.append(parts[:14])
            labels.append(1 if label == ">50K" else 0)
    return rows, labels, dropped


def main():
    os.makedirs(OUT, exist_ok=True)
    train_rows, train_labels, d1 = load_rows(fetch("adult.data"))
    test_rows, test_labels, d2 = load_rows(fetch("adult.test"))
    rows = train_rows + test_rows
    labels = np.array(train_labels + test_labels, dtype=np.uint8)
    n, f = len(rows), len(COLUMNS)
    print(f"rows: {len(train_rows)} (train file) + {len(test_rows)} (test file)"
          f" = {n}; dropped {d1 + d2} rows with missing values")

    features = np.zeros((n, f), dtype=np.uint8)
    col_meta = []
    for j, col in enumerate(COLUMNS):
        raw = [r[j] for r in rows]
        if col in NUMERIC:
            vals = np.array([float(v) for v in raw])
            idx, interior = quantile_bin(vals, MAX_BINS)
            features[:, j] = idx
            col_meta.append({
                "name": col, "kind": "numeric",
                "n_bins": int(len(interior) + 1),
                "raw_min": float(vals.min()), "raw_max": float(vals.max()),
                "edge_min": float(interior[0]) if len(interior) else None,
                "edge_max": float(interior[-1]) if len(interior) else None,
                "n_interior_edges": int(len(interior)),
            })
        else:
            codes, cats = encode_categorical(raw)
            features[:, j] = codes
            col_meta.append({
                "name": col, "kind": "categorical",
                "n_bins": len(cats), "categories": cats,
            })

    perm, n_train = permute_and_split_meta(n, SEED, TRAIN_FRAC)
    features = features[perm]
    labels = labels[perm]

    features.tofile(os.path.join(OUT, "adult.features.bin"))
    labels.tofile(os.path.join(OUT, "adult.labels.bin"))

    meta = {
        "dataset": "uci-adult",
        "source": [BASE + "adult.data", BASE + "adult.test"],
        "N": n, "F": f,
        "feature_dtype": "u8", "feature_layout": "row-major [N, F]",
        "label_dtype": "u8", "label_rule": ">50K = 1, <=50K = 0",
        "missing_rows_dropped": d1 + d2,
        "columns": [c["name"] for c in col_meta],
        "column_bins": col_meta,
        "class_balance": {
            "n_pos": int(labels.sum()),
            "n_neg": int(n - labels.sum()),
            "pos_frac": float(labels.mean()),
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
                                    "bin = searchsorted(interior_edges, v, 'right')",
                    "categorical_rule": "alphabetical integer codes"},
    }
    with open(os.path.join(OUT, "adult.meta.json"), "w") as fh:
        json.dump(meta, fh, indent=2)

    print(f"N={n} F={f}  train={n_train} val={n - n_train}  "
          f"pos_frac={labels.mean():.4f}")
    print("wrote out/adult.features.bin out/adult.labels.bin out/adult.meta.json")


if __name__ == "__main__":
    main()
