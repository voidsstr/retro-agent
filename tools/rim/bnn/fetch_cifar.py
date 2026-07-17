#!/usr/bin/env python3
"""Download + extract the CIFAR-10 binary dataset for the BNN reference.

Caches the tarball and the extracted batch files under tools/rim/bnn/data/;
re-running is a no-op once everything is in place.

Layout after extraction (the layout eval/train code depends on):
    data/cifar-10-binary.tar.gz
    data/cifar-10-batches-bin/data_batch_1.bin .. data_batch_5.bin
    data/cifar-10-batches-bin/test_batch.bin
    data/cifar-10-batches-bin/batches.meta.txt

Each record in a batch file is 3073 bytes: 1 label byte (0..9) followed by
3072 pixel bytes = R plane (32*32, row-major), G plane, B plane.
"""
import hashlib
import sys
import tarfile
import urllib.request
from pathlib import Path

URL = "https://www.cs.toronto.edu/~kriz/cifar-10-binary.tar.gz"
MD5 = "c32a1d4ab5d03f1284b67883e8d87530"  # published on the CIFAR page

DATA_DIR = Path(__file__).resolve().parent / "data"
TARBALL = DATA_DIR / "cifar-10-binary.tar.gz"
BATCH_DIR = DATA_DIR / "cifar-10-batches-bin"
TRAIN_FILES = [BATCH_DIR / f"data_batch_{i}.bin" for i in range(1, 6)]
TEST_FILE = BATCH_DIR / "test_batch.bin"
RECORD = 3073  # 1 label byte + 3072 pixel bytes


def _md5(path, chunk=1 << 20):
    h = hashlib.md5()
    with open(path, "rb") as f:
        while True:
            b = f.read(chunk)
            if not b:
                break
            h.update(b)
    return h.hexdigest()


def ensure_data(verbose=True):
    """Download+extract if needed; returns BATCH_DIR. Raises on bad download."""
    if all(p.exists() for p in TRAIN_FILES) and TEST_FILE.exists():
        if verbose:
            print(f"cifar-10 already extracted in {BATCH_DIR}")
        return BATCH_DIR

    DATA_DIR.mkdir(parents=True, exist_ok=True)
    if not TARBALL.exists() or _md5(TARBALL) != MD5:
        if verbose:
            print(f"downloading {URL} -> {TARBALL} ...")
        tmp = TARBALL.with_suffix(".part")
        urllib.request.urlretrieve(URL, tmp)
        got = _md5(tmp)
        if got != MD5:
            tmp.unlink(missing_ok=True)
            raise RuntimeError(f"md5 mismatch for {URL}: got {got}, want {MD5}")
        tmp.rename(TARBALL)

    if verbose:
        print(f"extracting {TARBALL} ...")
    with tarfile.open(TARBALL, "r:gz") as tf:
        tf.extractall(DATA_DIR)

    missing = [p for p in TRAIN_FILES + [TEST_FILE] if not p.exists()]
    if missing:
        raise RuntimeError(f"extraction incomplete, missing: {missing}")
    if verbose:
        print(f"ok: {BATCH_DIR}")
    return BATCH_DIR


if __name__ == "__main__":
    try:
        ensure_data()
    except Exception as e:  # pragma: no cover
        print(f"error: {e}", file=sys.stderr)
        sys.exit(1)
