#!/usr/bin/env python3
"""Write .rim files per FORMAT.md — library + CLI.

Library usage:
    from rim_pack import tref, write_rim
    manifest = {"rim": 1, "name": "m", "input": {...}, "layers": [
        {"op": "dense", "dtype": "f32", "in": 8, "out": 4,
         "w": tref(w, "f32"), "b": tref(b, "f32")},
    ]}
    write_rim("m.rim", manifest)

`tref(array, dtype, scale=None)` marks a tensor; write_rim replaces each with a
FORMAT.md tensor-reference object {"off","dtype","shape"[,"scale"]}, packing the
array bytes into the weights blob at a 16-byte-aligned blob-relative offset.

CLI (secondary path): pack from a manifest-template JSON whose tensor slots are
{"$npz": "key", "dtype": "...", "scale": ...} plus a .npz of arrays:
    python3 rim_pack.py template.json weights.npz -o model.rim
"""
import argparse
import json
from pathlib import Path

import numpy as np

from rim_common import DTYPES, MAGIC, HEADER_LEN, align16

_TENSOR_KEY = "_rim_tensor"


def tref(array, dtype, scale=None):
    """Mark a numpy array as a tensor to be packed into the weights blob."""
    if dtype not in DTYPES:
        raise ValueError(f"unknown tensor dtype {dtype!r}")
    arr = np.ascontiguousarray(array, dtype=DTYPES[dtype])
    d = {_TENSOR_KEY: arr, "dtype": dtype}
    if scale is not None:
        d["scale"] = float(scale)
    return d


def _pack_tree(node, blob):
    """Recursively replace tref placeholders with tensor-reference objects,
    appending tensor bytes to `blob` (bytearray) at 16-byte-aligned offsets."""
    if isinstance(node, dict):
        if _TENSOR_KEY in node:
            arr = node[_TENSOR_KEY]
            pad = align16(len(blob)) - len(blob)
            blob.extend(b"\x00" * pad)
            off = len(blob)
            blob.extend(arr.tobytes(order="C"))
            ref = {"off": off, "dtype": node["dtype"], "shape": list(arr.shape)}
            if "scale" in node:
                ref["scale"] = node["scale"]
            return ref
        return {k: _pack_tree(v, blob) for k, v in node.items()}
    if isinstance(node, list):
        return [_pack_tree(v, blob) for v in node]
    return node


def build_rim(manifest):
    """Returns the complete .rim file contents as bytes."""
    blob = bytearray()
    resolved = _pack_tree(manifest, blob)
    mjson = json.dumps(resolved, ensure_ascii=False).encode("utf-8")
    out = bytearray()
    out += MAGIC
    out += (1).to_bytes(4, "little")           # flags: bit0 = little-endian
    out += len(mjson).to_bytes(4, "little")    # manifest_len
    out += mjson
    out += b"\x00" * (align16(HEADER_LEN + len(mjson)) - (HEADER_LEN + len(mjson)))
    out += blob
    return bytes(out)


def write_rim(path, manifest):
    data = build_rim(manifest)
    Path(path).write_bytes(data)
    return len(data)


# --------------------------------------------------------------------- CLI

def _resolve_npz(node, npz):
    if isinstance(node, dict):
        if "$npz" in node:
            key = node["$npz"]
            if key not in npz:
                raise KeyError(f"npz has no array {key!r}")
            return tref(npz[key], node["dtype"], node.get("scale"))
        return {k: _resolve_npz(v, npz) for k, v in node.items()}
    if isinstance(node, list):
        return [_resolve_npz(v, npz) for v in node]
    return node


def main():
    ap = argparse.ArgumentParser(description="Pack a .rim from a manifest template + npz")
    ap.add_argument("template", help="manifest JSON with {'$npz': key, 'dtype': ...} tensor slots")
    ap.add_argument("weights", help=".npz with the tensor arrays")
    ap.add_argument("-o", "--out", required=True, help="output .rim path")
    args = ap.parse_args()

    template = json.loads(Path(args.template).read_text())
    npz = np.load(args.weights)
    manifest = _resolve_npz(template, npz)
    n = write_rim(args.out, manifest)
    print(f"wrote {args.out} ({n} bytes)")


if __name__ == "__main__":
    main()
