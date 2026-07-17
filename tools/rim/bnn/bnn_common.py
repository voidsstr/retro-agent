"""Shared helpers for the BNN reference (tools/rim/bnn).

Importing this module also:
  - puts tools/rim/ on sys.path so `rim_pack`, `rim_common`, `rim_dump` import,
  - registers the `bin` dtype (packed 1-bit weights, stored as u8 bytes) in
    rim_common.DTYPES. FORMAT.md lists `bin` as a valid tensor dtype but
    rim_common does not define it, so rim_pack/rim_dump would otherwise reject
    it. The patch mutates the shared DTYPES dict, so rim_pack and rim_dump
    (imported afterwards in the same process) both accept `bin`. For a `bin`
    tensor the manifest `shape` is the PACKED BYTE shape [n_out, ceil(n_in/8)]
    (itemsize 1), so rim_dump's alignment/bounds verification stays exact.
"""
import sys
from pathlib import Path

import numpy as np

BNN_DIR = Path(__file__).resolve().parent
RIM_DIR = BNN_DIR.parent
if str(RIM_DIR) not in sys.path:
    sys.path.insert(0, str(RIM_DIR))

import rim_common  # noqa: E402

# `bin` = packed binary weights, 8 weights/byte, stored as raw bytes.
rim_common.DTYPES.setdefault("bin", np.dtype("u1"))

DATA_DIR = BNN_DIR / "data"
OUT_DIR = BNN_DIR / "out"

CIFAR_LABELS = ["airplane", "automobile", "bird", "cat", "deer",
                "dog", "frog", "horse", "ship", "truck"]

N_IN, N_H, N_OUT = 3072, 1024, 10
BN_EPS = 1e-5

# 256-entry popcount lookup table for byte-wise XNOR+popcount.
POPCOUNT8 = np.array([bin(i).count("1") for i in range(256)], dtype=np.uint8)


# ------------------------------------------------------------------ dataset

def _load_batches(paths):
    recs = []
    for p in paths:
        raw = np.frombuffer(Path(p).read_bytes(), dtype=np.uint8)
        recs.append(raw.reshape(-1, 3073))
    r = np.concatenate(recs, axis=0)
    labels = r[:, 0].copy()
    images = r[:, 1:].copy()  # [N, 3072] u8: R plane, G plane, B plane row-major
    return images, labels


def load_cifar_train():
    import fetch_cifar
    fetch_cifar.ensure_data(verbose=False)
    return _load_batches(fetch_cifar.TRAIN_FILES)


def load_cifar_test():
    import fetch_cifar
    fetch_cifar.ensure_data(verbose=False)
    return _load_batches([fetch_cifar.TEST_FILE])


# ------------------------------------------------------------- bit packing

def pack_pm1_rows(pm1):
    """Pack a {-1,+1} matrix [n_out, n_in] to u8 [n_out, ceil(n_in/8)].

    Row-major, LSB-first: bit k of byte b in row j encodes weight
    w[j, 8*b + k]; bit 1 <=> +1, bit 0 <=> -1. Padding bits (when n_in is not
    a multiple of 8) are 0.
    """
    bits = (np.asarray(pm1) > 0).astype(np.uint8)
    return np.packbits(bits, axis=1, bitorder="little")


def unpack_rows_pm1(packed, n_in, dtype=np.int32):
    """Inverse of pack_pm1_rows -> {-1,+1} matrix [n_out, n_in]."""
    bits = np.unpackbits(packed, axis=1, bitorder="little")[:, :n_in]
    return (2 * bits.astype(dtype) - 1)


def xnor_popcount_matches(h_packed, w_packed):
    """m[b, j] = number of bit positions where activation row b equals weight
    row j, over n_in = 8 * n_bytes positions (n_in must be a multiple of 8).

    Pure integer: byte-wise XNOR (xor then complement) + popcount table.
    h_packed: [B, n_bytes] u8, w_packed: [n_out, n_bytes] u8 -> [B, n_out] i32.
    """
    x = h_packed[:, None, :] ^ w_packed[None, :, :]
    np.bitwise_xor(x, np.uint8(0xFF), out=x)  # XNOR
    return POPCOUNT8[x].sum(axis=2, dtype=np.int32)
