"""Shared deterministic u8 quantile-binning helpers for the RIM tabular datasets.

Every feature is reduced to ONE byte: a u8 bin index in [0, n_bins-1] with
n_bins <= 255.  Numeric columns are quantile-binned; categorical columns are
integer-coded in alphabetical order of their string categories.
"""
import numpy as np

MAX_BINS = 255


def quantile_bin(values, max_bins=MAX_BINS):
    """Quantile-bin a 1-D float array into <= max_bins bins.

    Returns (u8 bin indices, interior edges).  Bin index for value v is
    searchsorted(interior_edges, v, side='right'), i.e. bin b holds
    edges[b-1] < v <= edges[b] style half-open quantile buckets.  Duplicate
    quantile edges (heavily-skewed columns) are collapsed, so the actual bin
    count can be far below max_bins.
    """
    values = np.asarray(values, dtype=np.float64)
    qs = np.linspace(0.0, 1.0, max_bins + 1)
    edges = np.unique(np.quantile(values, qs))
    interior = edges[1:-1]  # len <= max_bins - 1  ->  n_bins <= max_bins
    idx = np.searchsorted(interior, values, side="right")
    assert idx.max() <= 254, "bin index overflows u8 budget"
    return idx.astype(np.uint8), interior


def encode_categorical(str_values):
    """Integer-code strings in alphabetical category order. Returns (u8, cats)."""
    cats = sorted(set(str_values))
    assert len(cats) <= 255, "too many categories for u8"
    lut = {c: i for i, c in enumerate(cats)}
    codes = np.array([lut[v] for v in str_values], dtype=np.uint8)
    return codes, cats


def permute_and_split_meta(n, seed=42, train_frac=0.8):
    """Seed-`seed` numpy permutation of row order.

    The .bin files are written in this permuted order, so the C consumer takes
    the first `train_frac` rows as train and the rest as val, sequentially.
    """
    rng = np.random.default_rng(seed)
    perm = rng.permutation(n)
    n_train = int(n * train_frac)
    return perm, n_train
