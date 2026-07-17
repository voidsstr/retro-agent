#!/usr/bin/env python3
"""Fetch the 4 MNIST IDX files into tools/rim/data/ (idempotent) and provide loaders."""
import gzip
import os
import struct
import sys
import urllib.request
from pathlib import Path

import numpy as np

BASE_URL = "https://storage.googleapis.com/cvdf-datasets/mnist/"
FILES = [
    "train-images-idx3-ubyte",
    "train-labels-idx1-ubyte",
    "t10k-images-idx3-ubyte",
    "t10k-labels-idx1-ubyte",
]
DATA_DIR = Path(__file__).resolve().parent / "data"


def fetch(verbose=True):
    """Download + decompress any missing MNIST files. Returns DATA_DIR."""
    DATA_DIR.mkdir(parents=True, exist_ok=True)
    for name in FILES:
        dst = DATA_DIR / name
        if dst.exists() and dst.stat().st_size > 0:
            if verbose:
                print(f"  cached: {dst}")
            continue
        url = BASE_URL + name + ".gz"
        if verbose:
            print(f"  fetching {url} ...")
        with urllib.request.urlopen(url, timeout=60) as r:
            gz = r.read()
        raw = gzip.decompress(gz)
        tmp = dst.with_suffix(".tmp")
        tmp.write_bytes(raw)
        os.replace(tmp, dst)
        if verbose:
            print(f"  wrote {dst} ({len(raw)} bytes)")
    return DATA_DIR


def load_idx_images(path):
    data = Path(path).read_bytes()
    magic, n, rows, cols = struct.unpack(">IIII", data[:16])
    if magic != 2051:
        raise ValueError(f"{path}: bad IDX image magic {magic}")
    a = np.frombuffer(data, dtype=np.uint8, offset=16)
    return a.reshape(n, rows, cols)


def load_idx_labels(path):
    data = Path(path).read_bytes()
    magic, n = struct.unpack(">II", data[:8])
    if magic != 2049:
        raise ValueError(f"{path}: bad IDX label magic {magic}")
    return np.frombuffer(data, dtype=np.uint8, offset=8)


def load_mnist():
    """Returns (x_train, y_train, x_test, y_test); images u8 (N,28,28)."""
    fetch(verbose=False)
    xtr = load_idx_images(DATA_DIR / "train-images-idx3-ubyte")
    ytr = load_idx_labels(DATA_DIR / "train-labels-idx1-ubyte")
    xte = load_idx_images(DATA_DIR / "t10k-images-idx3-ubyte")
    yte = load_idx_labels(DATA_DIR / "t10k-labels-idx1-ubyte")
    return xtr, ytr, xte, yte


if __name__ == "__main__":
    print(f"MNIST cache dir: {DATA_DIR}")
    fetch()
    xtr, ytr, xte, yte = load_mnist()
    print(f"train: {xtr.shape} labels {ytr.shape}; test: {xte.shape} labels {yte.shape}")
    sys.exit(0)
