#!/usr/bin/env python3
"""Phase 2's key deliverable, made runnable: record -> train -> checkpoint
loads in runtime.GpuPolicy -> the trained net measurably imitates the
scripted policy better than a random one.

No real demonstration corpus exists yet (no engine adapter — Phase 1's
remaining half), so this proves the PIPELINE using record/gen_synthetic.py's
known scripted "expert" instead of claiming anything about real play. See
train/bc.py's docstring for why training is i.i.d. rather than sequence-BPTT,
and eval_imitation.py's docstring for what the weapon_accuracy number does
and does not prove.

    ~/.venvs/gamebots/bin/python e2e_synthetic.py
    ~/.venvs/gamebots/bin/python e2e_synthetic.py --report-json report.json
"""

import argparse
import json
import os
import sys
import tempfile

_HERE = os.path.dirname(os.path.abspath(__file__))
_GB = os.path.dirname(_HERE)
if _GB not in sys.path:
    sys.path.insert(0, _GB)
import schema  # noqa: E402
import model   # noqa: E402

if _HERE not in sys.path:
    sys.path.insert(0, _HERE)
import bc              # noqa: E402
import dataset          # noqa: E402
import eval_imitation   # noqa: E402

_REC = os.path.join(_GB, "record")
if _REC not in sys.path:
    sys.path.insert(0, _REC)
import gen_synthetic  # noqa: E402

HAVE_TORCH = model.HAVE_TORCH


def run(train_episodes=300, episode_len=150, eval_episodes=60, eval_len=100,
       epochs=15, batch_size=256, hidden=256, seed=0, eval_seed=999,
       device=None, out_dir=None, verify_runtime_load=True, log=print):
    if not HAVE_TORCH:
        raise RuntimeError("PyTorch is not installed")
    out_dir = out_dir or tempfile.mkdtemp(prefix="gamebots-e2e-")
    train_dir = os.path.join(out_dir, "train_shards")

    log(f"[1/5] generating {train_episodes} synthetic training episodes "
       f"({train_episodes * episode_len:,} ticks) -> {train_dir}")
    n_written = gen_synthetic.generate(train_dir, n_episodes=train_episodes,
                                       episode_len=episode_len, seed=seed)
    log(f"      {n_written:,} records written")

    log("[2/5] loading dataset")
    data = dataset.load_dataset([train_dir])
    for w in data["_warnings"]:
        log(f"      warning: {w}")
    log(f"      {len(data['obs']):,} records from {len(data['_files'])} shard(s)")

    log(f"[3/5] training ({epochs} epochs, hidden={hidden})")
    net, history = bc.train(data, epochs=epochs, batch_size=batch_size,
                            hidden=hidden, seed=seed, device=device, log=log)

    ckpt_path = os.path.join(out_dir, "bc_checkpoint.pt")
    version = bc.save_checkpoint(net, ckpt_path)
    log(f"      checkpoint: {ckpt_path} (version {version})")

    runtime_check = None
    if verify_runtime_load:
        log("[4/5] verifying the checkpoint round-trips through runtime.GpuPolicy.load()")
        runtime_check = _verify_runtime_load(ckpt_path, hidden=hidden)
        log(f"      {runtime_check}")
    else:
        log("[4/5] skipped (--no-runtime-check)")

    log(f"[5/5] evaluating on {eval_episodes} FRESH held-out episodes "
       f"(seed={eval_seed}, never used for training)")
    eval_obs, eval_target = eval_imitation.fresh_examples(
        n_episodes=eval_episodes, episode_len=eval_len, seed=eval_seed)

    trained_metrics = eval_imitation.evaluate(net, eval_obs, eval_target)
    random_net = model.build(hidden=hidden).eval()
    random_metrics = eval_imitation.evaluate(random_net, eval_obs, eval_target)

    report = {"history": history, "trained": trained_metrics,
             "random": random_metrics, "checkpoint": ckpt_path,
             "out_dir": out_dir, "runtime_check": runtime_check,
             "schema_hash": f"{schema.SCHEMA_HASH:#010x}"}

    log("\n=== trained vs. random, on FRESH held-out synthetic episodes "
       f"(n={trained_metrics['n']}) ===")
    log(f"{'metric':<20}{'trained':>12}{'random':>12}")
    for k in ("pitch_mae_deg", "yaw_mae_deg", "fwd_mae", "side_mae",
             "attack_precision", "attack_recall", "weapon_accuracy"):
        log(f"{k:<20}{trained_metrics[k]:>12.4f}{random_metrics[k]:>12.4f}")
    log(f"{'attack_base_rate':<20}{trained_metrics['attack_base_rate']:>12.4f}"
       f"  (fraction of frames the expert actually fires — context for "
       f"precision/recall, not a head to beat)")
    log("(weapon_accuracy is not meaningful evidence either way — "
       "ScriptedPolicy never changes weapon; see eval_imitation.py)")

    return report


def _verify_runtime_load(ckpt_path, hidden=256):
    """Loads the checkpoint the same way policyd's --policy gpu does, on CPU
    so this works without a CUDA device, and confirms it produces finite
    actions."""
    import numpy as np
    import runtime
    p = runtime.GpuPolicy(device="cpu", use_graphs=False, hidden=hidden,
                          prewarm=False)
    version = p.load(ckpt_path)
    ids = np.arange(8, dtype=np.uint16)
    obs = np.random.default_rng(0).normal(size=(8, schema.OBS_DIM)).astype(np.float32)
    btn, pitch, yaw, fwd, side, wpn = p.act_arrays(0, 0, ids, obs)
    assert np.isfinite(pitch).all() and np.isfinite(yaw).all()
    assert np.isfinite(fwd).all() and np.isfinite(side).all()
    return (f"OK — GpuPolicy.load() accepted it (version={version!r}), "
           f"act_arrays() on 8 bots produced finite actions")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--train-episodes", type=int, default=300)
    ap.add_argument("--episode-len", type=int, default=150)
    ap.add_argument("--eval-episodes", type=int, default=60)
    ap.add_argument("--eval-len", type=int, default=100)
    ap.add_argument("--epochs", type=int, default=15)
    ap.add_argument("--batch-size", type=int, default=256)
    ap.add_argument("--hidden", type=int, default=256)
    ap.add_argument("--seed", type=int, default=0)
    ap.add_argument("--eval-seed", type=int, default=999)
    ap.add_argument("--device", default=None)
    ap.add_argument("--out-dir", default=None)
    ap.add_argument("--no-runtime-check", action="store_true")
    ap.add_argument("--report-json", default=None)
    args = ap.parse_args()

    if not HAVE_TORCH:
        print("PyTorch not installed — use ~/.venvs/gamebots/bin/python",
             file=sys.stderr)
        return 2

    report = run(train_episodes=args.train_episodes, episode_len=args.episode_len,
                eval_episodes=args.eval_episodes, eval_len=args.eval_len,
                epochs=args.epochs, batch_size=args.batch_size,
                hidden=args.hidden, seed=args.seed, eval_seed=args.eval_seed,
                device=args.device, out_dir=args.out_dir,
                verify_runtime_load=not args.no_runtime_check)
    if args.report_json:
        with open(args.report_json, "w") as fh:
            json.dump(report, fh, indent=2)
        print(f"\nreport written to {args.report_json}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
