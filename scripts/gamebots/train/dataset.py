#!/usr/bin/env python3
"""Loads recorded demonstration shards into plain numpy arrays for training.

Deliberately torch-free: the harness and the rest of the test suite must keep
working on a host with no ML stack at all (see model.py's own docstring for
why), so the loading and splitting logic lives here where it can be tested
without torch, and only bc.py — which actually trains — imports it.
"""

import glob
import os
import sys

try:
    import numpy as np
    HAVE_NUMPY = True
except ImportError:  # pragma: no cover - exercised on hosts without numpy
    np = None
    HAVE_NUMPY = False

_HERE = os.path.dirname(os.path.abspath(__file__))
_GB = os.path.dirname(_HERE)
if _GB not in sys.path:
    sys.path.insert(0, _GB)
import schema  # noqa: E402

_RECORD_DIR = os.path.join(_GB, "record")
if _RECORD_DIR not in sys.path:
    sys.path.insert(0, _RECORD_DIR)
import shard  # noqa: E402

SHARD_GLOB = "*.gbdemo"


def find_shards(paths):
    """paths: files and/or directories -> sorted list of .gbdemo file paths."""
    out = []
    for p in paths:
        if os.path.isdir(p):
            out.extend(sorted(glob.glob(os.path.join(p, SHARD_GLOB))))
        else:
            out.append(p)
    return out


_COLUMNS = ("bot_id", "episode_id", "tick", "done", "buttons", "pitch",
           "yaw", "fwd", "side", "weapon")


def load_dataset(paths, strict_schema=True):
    """paths: shard files and/or directories containing them.

    -> dict with "obs" ((N, OBS_DIM) float32), one array per column in
    _COLUMNS, "_files" (the shards actually read) and "_warnings" (non-fatal
    issues, e.g. a truncated tail shard.py detected and dropped).

    A schema mismatch is FATAL (raises), not a warning — training on
    misaligned floats produces a confidently wrong model with no error
    anywhere, which is exactly the failure schema hashing exists to prevent
    one layer up (schema.py's wire hash, runtime.py's checkpoint hash).
    """
    if not HAVE_NUMPY:
        raise RuntimeError(
            "numpy is not installed — use ~/.venvs/gamebots/bin/python")
    files = find_shards(paths)
    if not files:
        raise ValueError(f"no {SHARD_GLOB} shards found in {paths!r}")

    obs_parts = []
    cols = {k: [] for k in _COLUMNS}
    warnings = []
    total_trailing = 0

    for f in files:
        header, recs, trailing = shard.load_shard(f, strict_schema=strict_schema)
        if trailing:
            total_trailing += trailing
            warnings.append(
                f"{f}: {trailing} trailing byte(s) after the last complete "
                f"record (truncated write) — dropped, not treated as data")
        if len(recs) == 0:
            warnings.append(f"{f}: no complete records (empty or all-truncated)")
            continue
        obs_parts.append(np.ascontiguousarray(recs["obs"], dtype=np.float32))
        for k in cols:
            cols[k].append(np.ascontiguousarray(recs[k]))

    if not obs_parts:
        raise ValueError(
            f"found {len(files)} shard(s) but zero usable records — " +
            ("; ".join(warnings) if warnings else "all were empty"))

    data = {"obs": np.concatenate(obs_parts, axis=0)}
    for k, parts in cols.items():
        data[k] = np.concatenate(parts, axis=0)
    data["_files"] = files
    data["_warnings"] = warnings
    return data


def split_indices(n, val_frac=0.15, seed=0):
    """A deterministic, shuffled train/val split by row index.

    Row-level rather than episode-level: the BC trainer this feeds treats
    every (obs, action) pair independently (see bc.py's docstring for why),
    so there is no cross-episode leakage concern to guard against here yet —
    that becomes relevant only once a sequence-aware trainer groups rows by
    episode_id, which does not exist yet either.
    """
    if not HAVE_NUMPY:
        raise RuntimeError(
            "numpy is not installed — use ~/.venvs/gamebots/bin/python")
    if n < 2:
        raise ValueError(f"need at least 2 records to split, have {n}")
    rng = np.random.default_rng(seed)
    idx = rng.permutation(n)
    n_val = max(1, int(round(n * val_frac)))
    n_val = min(n_val, n - 1)
    return idx[n_val:], idx[:n_val]
