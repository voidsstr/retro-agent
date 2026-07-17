#!/usr/bin/env python3
"""Parse a .rim file: pretty-print the manifest, verify container/alignment
invariants, optionally extract tensors (dequantized to fp32) to an .npz.

    python3 rim_dump.py model.rim
    python3 rim_dump.py model.rim --npz tensors.npz
"""
import argparse
import json
import sys
from pathlib import Path

import numpy as np

from rim_common import DTYPES, MAGIC, HEADER_LEN, align16


def read_rim(path):
    """Returns (manifest_dict, blob_bytes, blob_file_offset). Raises on a
    malformed container."""
    data = Path(path).read_bytes()
    if len(data) < HEADER_LEN:
        raise ValueError("file shorter than header")
    if data[:4] != MAGIC:
        raise ValueError(f"bad magic {data[:4]!r} (want {MAGIC!r})")
    flags = int.from_bytes(data[4:8], "little")
    if not (flags & 1):
        raise ValueError(f"flags bit0 (little-endian payload) not set: {flags:#x}")
    mlen = int.from_bytes(data[8:12], "little")
    if HEADER_LEN + mlen > len(data):
        raise ValueError("manifest_len runs past EOF")
    mjson = data[HEADER_LEN:HEADER_LEN + mlen]
    manifest = json.loads(mjson.decode("utf-8"))
    blob_off = align16(HEADER_LEN + mlen)
    if any(data[HEADER_LEN + mlen:blob_off]):
        raise ValueError("nonzero padding between manifest and weights blob")
    return manifest, data[blob_off:], blob_off


def is_tensor_ref(node):
    return (isinstance(node, dict) and "off" in node and "dtype" in node
            and "shape" in node)


def iter_tensor_refs(manifest):
    """Yields (path_string, ref) for every tensor reference in the manifest."""
    def walk(node, path):
        if is_tensor_ref(node):
            yield path, node
            return
        if isinstance(node, dict):
            for k, v in node.items():
                yield from walk(v, f"{path}.{k}" if path else k)
        elif isinstance(node, list):
            for i, v in enumerate(node):
                yield from walk(v, f"{path}[{i}]")
    yield from walk(manifest, "")


def get_tensor(ref, blob):
    dt = DTYPES[ref["dtype"]]
    shape = tuple(ref["shape"])
    n = int(np.prod(shape)) if shape else 1
    off = ref["off"]
    a = np.frombuffer(blob, dtype=dt, count=n, offset=off)
    return a.reshape(shape)


def verify(manifest, blob):
    """Check FORMAT.md invariants on every tensor ref. Returns list of error
    strings (empty = clean)."""
    errs = []
    for path, ref in iter_tensor_refs(manifest):
        off, dtype, shape = ref["off"], ref["dtype"], ref["shape"]
        if dtype not in DTYPES:
            errs.append(f"{path}: unknown dtype {dtype!r}")
            continue
        if off % 16 != 0:
            errs.append(f"{path}: off {off} not 16-byte aligned")
        nbytes = int(np.prod(shape)) * DTYPES[dtype].itemsize
        if off < 0 or off + nbytes > len(blob):
            errs.append(f"{path}: [{off}, {off + nbytes}) outside blob (len {len(blob)})")
    return errs


def dump(path, npz_out=None):
    manifest, blob, blob_off = read_rim(path)
    size = Path(path).stat().st_size
    print(f"{path}: {size} bytes, manifest at 12, weights blob at file offset "
          f"{blob_off} ({len(blob)} bytes)")
    print(f"  name:  {manifest.get('name')}")
    print(f"  rim:   {manifest.get('rim')}")
    inp = manifest.get("input", {})
    print(f"  input: shape={inp.get('shape')} dtype={inp.get('dtype')}"
          + (f" div={inp.get('div')}" if "div" in inp else ""))
    if "labels" in manifest:
        print(f"  labels: {manifest['labels']}")
    print(f"  layers ({len(manifest.get('layers', []))}):")
    for i, layer in enumerate(manifest.get("layers", [])):
        fields = []
        for k, v in layer.items():
            if k == "op":
                continue
            if is_tensor_ref(v):
                s = f"{k}=<{v['dtype']} {v['shape']} @{v['off']}"
                if "scale" in v:
                    s += f" scale={v['scale']:.6g}"
                fields.append(s + ">")
            else:
                fields.append(f"{k}={v}")
        print(f"    [{i}] {layer['op']:<8} {' '.join(fields)}")

    errs = verify(manifest, blob)
    if errs:
        print("  INVARIANT ERRORS:")
        for e in errs:
            print(f"    - {e}")
    else:
        print("  invariants: OK (magic, flags bit0, zero pad, 16-byte tensor "
              "alignment, in-bounds)")

    if npz_out:
        out = {}
        for i, layer in enumerate(manifest.get("layers", [])):
            for k, v in layer.items():
                if not is_tensor_ref(v):
                    continue
                a = get_tensor(v, blob)
                if v["dtype"] == "i8" and "scale" in v:
                    a = a.astype(np.float32) * np.float32(v["scale"])
                elif v["dtype"] == "i32":
                    # int8 bias: dequant scale = act_scale_in * weight scale
                    wref = layer.get("w")
                    asi = layer.get("act_scale_in")
                    if wref is not None and "scale" in wref and asi is not None:
                        a = a.astype(np.float32) * np.float32(asi * wref["scale"])
                    else:
                        a = a.astype(np.float32)
                else:
                    a = a.astype(np.float32)
                out[f"layer{i}_{layer['op']}_{k}"] = a
        np.savez(npz_out, **out)
        print(f"  extracted {len(out)} tensors (fp32) -> {npz_out}")
    return not errs


def main():
    ap = argparse.ArgumentParser(description="Inspect/verify a .rim file")
    ap.add_argument("rim", help=".rim file")
    ap.add_argument("--npz", help="extract tensors dequantized to fp32 into this .npz")
    args = ap.parse_args()
    ok = dump(args.rim, args.npz)
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
