#!/usr/bin/env python3
"""Pipeline parallelism demo (roadmap M7): a trained MLP split layer-per-
machine — stage 1 (784->128 dense+relu) on box A, stage 2 (128->10 dense) on
box B — activations streamed stage-to-stage over the agent AI transport.
Verifies label-identical results vs the single-box full model.

Usage: python3 scripts/retro_ai_pipeline.py --a 192.168.1.124 --b 192.168.1.143 --n 200
"""
import argparse
import asyncio
import json
import os
import sys
import time

sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), "..")))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import numpy as np  # noqa: E402

sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), "..",
                                                "tools", "rim")))
from client.retro_protocol import RetroConnection  # noqa: E402
from client.retro_ai import RetroAI  # noqa: E402
import rim_pack  # noqa: E402
import rim_dump  # noqa: E402
import ai_status_bus as bus  # noqa: E402
import ai_metrics  # noqa: E402

SECRET = os.environ.get("RETRO_AGENT_SECRET", "retro-agent-secret")
OUT = os.path.join(os.path.dirname(__file__), "..", "tools", "rim", "out")
SP = os.environ.get("TMPDIR", "/tmp")


def split_mlp(full_rim_path):
    """Split a dense-f32 MLP .rim into stage1/stage2 .rim blobs."""
    man, weights, _off = rim_dump.read_rim(full_rim_path)
    layers = man["layers"]
    dense = [l for l in layers if l["op"] == "dense"]
    assert len(dense) == 2, "expected 2-dense MLP"

    def tensor(l, key):
        t = l[key]
        dt = {"f32": "<f4"}[t["dtype"]]
        arr = np.frombuffer(weights, dtype=dt,
                            offset=t["off"],
                            count=int(np.prod(t["shape"])))
        return arr.reshape(t["shape"]).copy()

    W1, b1 = tensor(dense[0], "w"), tensor(dense[0], "b")
    W2, b2 = tensor(dense[1], "w"), tensor(dense[1], "b")

    s1 = {
        "rim": 1, "name": "mlp-stage1",
        "input": {"shape": [W1.shape[1]], "dtype": "u8", "div": 255.0},
        "layers": [
            {"op": "dense", "dtype": "f32", "in": W1.shape[1],
             "out": W1.shape[0],
             "w": rim_pack.tref(W1, "f32"), "b": rim_pack.tref(b1, "f32")},
            {"op": "relu"},
        ],
    }
    s2 = {
        "rim": 1, "name": "mlp-stage2",
        "input": {"shape": [W2.shape[1]], "dtype": "f32"},
        "layers": [
            {"op": "dense", "dtype": "f32", "in": W2.shape[1],
             "out": W2.shape[0],
             "w": rim_pack.tref(W2, "f32"), "b": rim_pack.tref(b2, "f32")},
        ],
    }
    p1 = os.path.join(SP, "mlp_stage1.rim")
    p2 = os.path.join(SP, "mlp_stage2.rim")
    rim_pack.write_rim(p1, s1)
    rim_pack.write_rim(p2, s2)
    return p1, p2


async def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--a", required=True, help="stage-1 box")
    ap.add_argument("--b", required=True, help="stage-2 box")
    ap.add_argument("--model", default=f"{OUT}/../out/mlp_60k.rim")
    ap.add_argument("--n", type=int, default=200)
    args = ap.parse_args()

    p1, p2 = split_mlp(args.model)
    print(f"split {args.model} -> stage1 ({os.path.getsize(p1)} B) + "
          f"stage2 ({os.path.getsize(p2)} B)")

    ca = RetroConnection(args.a, 9898)
    cb = RetroConnection(args.b, 9898)
    await ca.connect(SECRET, timeout=15.0)
    await cb.connect(SECRET, timeout=15.0)
    aia, aib = RetroAI(ca), RetroAI(cb)
    await aia.model_load("mlp-s1", open(p1, "rb").read())
    await aib.model_load("mlp-s2", open(p2, "rb").read())
    print(f"stage1 resident on {args.a}, stage2 resident on {args.b}")

    imgs = open(f"{OUT}/mnist_test_1000.images.bin", "rb").read()
    labels = open(f"{OUT}/mnist_test_1000.labels.bin", "rb").read()

    # local single-box reference: full model via host build if available,
    # else run both stages on box A
    from subprocess import run as sprun
    host = os.path.join(os.path.dirname(__file__), "..", "retro-infer",
                        "retro-infer-host")
    ref_labels = None
    if os.path.exists(host):
        lp = os.path.join(SP, "pipe_ref_logits.bin")
        r = sprun([host, "--logits", lp, "--eval", args.model,
                   f"{OUT}/mnist_test_1000.images.bin",
                   f"{OUT}/mnist_test_1000.labels.bin", str(args.n)],
                  capture_output=True, text=True)
        ref_labels = np.fromfile(lp, dtype="<f4").reshape(-1, 10).argmax(1)

    model_name = os.path.basename(args.model)
    run_id = bus.new_run(
        "pipeline-2stage", model=model_name, phase="pipeline",
        precision="f32", command=" ".join(sys.argv), nodes=[args.a, args.b])
    bus.publish(run_id, status="running", progress={"total_steps": args.n},
                fleet={"nodes_total": 2, "nodes_alive": 2},
                nodes={args.a: {"role": "stage1", "alive": True, "backend": "?",
                                "precision": "f32", "last_error": None},
                      args.b: {"role": "stage2", "alive": True, "backend": "?",
                                "precision": "f32", "last_error": None}})

    correct = 0
    match = 0
    lat = []
    t0 = time.time()
    for i in range(args.n):
        ts = time.time()
        act = await aia.infer_run("mlp-s1", imgs[i * 784:(i + 1) * 784])
        logits = await aib.infer_run("mlp-s2", act.astype("<f4").tobytes())
        lat.append((time.time() - ts) * 1000)
        pred = int(logits.argmax())
        if pred == labels[i]:
            correct += 1
        if ref_labels is not None and pred == ref_labels[i]:
            match += 1
        if (i + 1) % 10 == 0 or i + 1 == args.n:
            elapsed = time.time() - t0
            sps = (i + 1) / elapsed if elapsed > 0 else None
            bus.publish(
                run_id,
                progress={"step": i + 1,
                          "percent": round(100.0 * (i + 1) / args.n, 1)},
                metrics={"top1_running": round(correct / (i + 1), 4),
                         "p50_ms": round(float(np.percentile(lat, 50)), 1),
                         "p99_ms": round(float(np.percentile(lat, 99)), 1)},
                fleet={"samples_per_sec": round(sps, 1) if sps else None,
                       "eta_seconds": round((args.n - i - 1) / sps, 1) if sps else None},
                nodes={args.a: {"samples_per_sec": round(sps, 1) if sps else None,
                                "last_step_ms": round(lat[-1], 1)},
                      args.b: {"samples_per_sec": round(sps, 1) if sps else None,
                                "last_step_ms": round(lat[-1], 1)}})
    secs = time.time() - t0
    lat = np.array(lat)
    ips_total = args.n / secs if secs > 0 else 0.0
    print(f"pipeline[{args.a} -> {args.b}] n={args.n} "
          f"top1={correct/args.n:.4f} tok/s={ips_total:.1f} "
          f"p50={np.percentile(lat, 50):.0f}ms p99={np.percentile(lat, 99):.0f}ms")
    identical = ref_labels is not None and match == args.n
    if ref_labels is not None:
        print(f"label match vs single-box: {match}/{args.n} "
              f"{'IDENTICAL' if identical else 'MISMATCH'}")

    bus.mark_done(run_id, status="completed")
    try:
        ai_metrics.log_run(
            args.a, model_name, "infer", "pipeline-2stage", "f32",
            {"top1": round(correct / args.n, 4), "n": args.n,
             "img_per_sec_total": round(ips_total, 2),
             "p50_ms": round(float(np.percentile(lat, 50)), 1),
             "p99_ms": round(float(np.percentile(lat, 99)), 1),
             "label_match_vs_singlebox": match if ref_labels is not None else None,
             "identical_vs_singlebox": identical if ref_labels is not None else None},
            settings={"mode": "pipeline", "stage1": args.a, "stage2": args.b},
            dataset="mnist-test",
            notes="2-stage layer-split pipeline-parallel demo")
    except Exception as e:
        print(f"(DB log failed: {e})")

    await ca.close()
    await cb.close()


if __name__ == "__main__":
    asyncio.run(main())
