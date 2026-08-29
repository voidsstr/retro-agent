#!/usr/bin/env python3
"""Behavioural-cloning trainer for the bot policy (model.build()).

Loads recorded demonstration shards (record/shard.py), trains a fresh
model.build() with one loss per action head, and checkpoints in exactly the
format runtime.GpuPolicy.load() expects:
    {"state_dict": net.state_dict(), "schema_hash": schema.SCHEMA_HASH,
     "version": "..."}
so a trained checkpoint drops straight into `policyd.py --policy gpu
--weights <path>` with no conversion step.

**Training is i.i.d. over (obs, action) pairs, not truncated BPTT over
recorded episodes.** Every forward call uses hx=None (a zeroed hidden state).
This is a deliberate, documented simplification, not an oversight: the only
"expert" there is data from right now, `policyd.ScriptedPolicy`, is itself
stateless — its action is a pure function of the current observation, tick
and bot_id; it never reads its own hidden state (see its docstring). There is
therefore no temporal credit to assign, and full sequence-chunked recurrent
training would add BPTT machinery to prove nothing extra on this data.

The demo format (record/shard.py) records episode_id/done/tick precisely so
that when real, memory-dependent demonstrations exist — a human who ducks
behind cover and remembers where a flanker went, a bot that holds an angle —
a sequence-aware trainer can be built ON TOP of the existing shards without a
format change. That trainer does not exist yet. Do not read the i.i.d.
approach here as "the GRU doesn't matter" — it means "we have not yet proven
we need the harder trainer, and building it against data that can't exercise
it would not prove anything either."

Losses, one per head, matched to the action space in schema.py:
  - view (pitch, yaw): MSE between tanh(view_mean) and the target delta
    normalised into [-1, 1] by MAX_PITCH/YAW_DELTA_DEG — the same transform
    model.BotPolicy.act() applies at inference, so the loss is computed in
    the space the network actually outputs, not one requiring an atanh.
  - move (forward, side): MSE the same way, no rescale needed (already
    [-1, 1] on the wire).
  - buttons: independent BCEWithLogitsLoss per bit — they genuinely co-occur
    (jumping while firing), matching model.py's choice of independent
    Bernoulli heads rather than a joint categorical over 2^8 combinations.
  - weapon: cross-entropy. Downweighted by default (--weapon-weight) because
    ScriptedPolicy always emits weapon=0 ("no change") — on synthetic data
    this head has nothing informative to learn, and upweighting it would let
    a trivial "always predict 0" solution dominate the total loss.
  - value: NOT trained here. It exists for PPO (Phase 3); behavioural
    cloning has no reward signal to fit it to.

    ~/.venvs/gamebots/bin/python bc.py shards/ --out ckpt.pt --epochs 20
"""

import argparse
import os
import sys
import time

_HERE = os.path.dirname(os.path.abspath(__file__))
_GB = os.path.dirname(_HERE)
if _GB not in sys.path:
    sys.path.insert(0, _GB)
import schema  # noqa: E402
import model   # noqa: E402

if _HERE not in sys.path:
    sys.path.insert(0, _HERE)
import dataset  # noqa: E402

HAVE_TORCH = model.HAVE_TORCH
if HAVE_TORCH:
    import numpy as np
    import torch
    import torch.nn.functional as F

N_BUTTONS = len(schema.BUTTON_NAMES)


def _to_tensors(data, idx, device, dtype):
    """A row-index array + the numpy dataset dict -> torch tensors on device."""
    obs = torch.as_tensor(data["obs"][idx], dtype=dtype, device=device)
    buttons = data["buttons"][idx].astype(np.uint16)
    bits = ((buttons[:, None] >> np.arange(N_BUTTONS)) & 1).astype(np.float32)
    return {
        "obs": obs,
        "buttons_bits": torch.as_tensor(bits, dtype=dtype, device=device),
        "pitch": torch.as_tensor(data["pitch"][idx], dtype=dtype, device=device),
        "yaw": torch.as_tensor(data["yaw"][idx], dtype=dtype, device=device),
        "fwd": torch.as_tensor(data["fwd"][idx], dtype=dtype, device=device),
        "side": torch.as_tensor(data["side"][idx], dtype=dtype, device=device),
        "weapon": torch.as_tensor(data["weapon"][idx].astype(np.int64),
                                  dtype=torch.long, device=device),
    }


def compute_loss(net, batch, weapon_weight=0.1):
    out, _hx = net(batch["obs"])

    view_pred = torch.tanh(out["view_mean"])
    view_target = torch.stack(
        [batch["pitch"] / schema.MAX_PITCH_DELTA_DEG,
        batch["yaw"] / schema.MAX_YAW_DELTA_DEG], dim=1
    ).clamp(-1.0, 1.0).to(view_pred.dtype)
    view_loss = F.mse_loss(view_pred, view_target)

    move_pred = torch.tanh(out["move"])
    move_target = torch.stack([batch["fwd"], batch["side"]], dim=1) \
        .clamp(-1.0, 1.0).to(move_pred.dtype)
    move_loss = F.mse_loss(move_pred, move_target)

    btn_loss = F.binary_cross_entropy_with_logits(
        out["buttons"].float(), batch["buttons_bits"].float())

    wpn_loss = F.cross_entropy(out["weapon"], batch["weapon"])

    total = view_loss + move_loss + btn_loss + weapon_weight * wpn_loss
    parts = {"view": float(view_loss.detach()), "move": float(move_loss.detach()),
            "btn": float(btn_loss.detach()), "wpn": float(wpn_loss.detach())}
    return total, parts


def train(data, epochs=20, batch_size=256, val_frac=0.15, lr=3e-4, seed=0,
         hidden=256, device=None, dtype=None, weapon_weight=0.1, log=print):
    if not HAVE_TORCH:
        raise RuntimeError(
            "PyTorch is not installed — use ~/.venvs/gamebots/bin/python")
    device = torch.device(device or ("cuda" if torch.cuda.is_available() else "cpu"))
    dtype = dtype or torch.float32
    n = len(data["obs"])
    train_idx, val_idx = dataset.split_indices(n, val_frac=val_frac, seed=seed)

    torch.manual_seed(seed)
    net = model.build(hidden=hidden).to(device).to(dtype)
    opt = torch.optim.Adam(net.parameters(), lr=lr)
    rng = np.random.default_rng(seed)

    history = []
    for epoch in range(epochs):
        net.train()
        order = rng.permutation(train_idx)
        running, n_batches = 0.0, 0
        for start in range(0, len(order), batch_size):
            idx = order[start:start + batch_size]
            if len(idx) == 0:
                continue
            batch = _to_tensors(data, idx, device, dtype)
            opt.zero_grad(set_to_none=True)
            loss, _parts = compute_loss(net, batch, weapon_weight)
            loss.backward()
            opt.step()
            running += float(loss.detach())
            n_batches += 1
        train_loss = running / max(1, n_batches)

        net.eval()
        with torch.no_grad():
            vbatch = _to_tensors(data, val_idx, device, dtype)
            vloss, vparts = compute_loss(net, vbatch, weapon_weight)
        row = {"epoch": epoch, "train_loss": train_loss, "val_loss": float(vloss),
              **{f"val_{k}": v for k, v in vparts.items()}}
        history.append(row)
        log(f"epoch {epoch:>3}  train {train_loss:.4f}  val {float(vloss):.4f}  "
           f"(view {vparts['view']:.4f} move {vparts['move']:.4f} "
           f"btn {vparts['btn']:.4f} wpn {vparts['wpn']:.4f})")
    return net, history


def save_checkpoint(net, path, version=None):
    if version is None:
        version = f"bc-{time.strftime('%Y%m%d-%H%M%S')}"
    torch.save({"state_dict": net.state_dict(),
               "schema_hash": schema.SCHEMA_HASH,
               "version": version}, path)
    return version


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("data", nargs="+", help="shard files or directories")
    ap.add_argument("--out", default="bc_checkpoint.pt")
    ap.add_argument("--epochs", type=int, default=20)
    ap.add_argument("--batch-size", type=int, default=256)
    ap.add_argument("--val-frac", type=float, default=0.15)
    ap.add_argument("--lr", type=float, default=3e-4)
    ap.add_argument("--hidden", type=int, default=256)
    ap.add_argument("--seed", type=int, default=0)
    ap.add_argument("--device", default=None)
    ap.add_argument("--weapon-weight", type=float, default=0.1)
    args = ap.parse_args()

    if not HAVE_TORCH:
        print("PyTorch not installed — use ~/.venvs/gamebots/bin/python",
             file=sys.stderr)
        return 2

    data = dataset.load_dataset(args.data)
    for w in data["_warnings"]:
        print(f"warning: {w}", file=sys.stderr)
    print(f"loaded {len(data['obs']):,} records from {len(data['_files'])} shard(s)")

    net, _history = train(data, epochs=args.epochs, batch_size=args.batch_size,
                          val_frac=args.val_frac, lr=args.lr, seed=args.seed,
                          hidden=args.hidden, device=args.device,
                          weapon_weight=args.weapon_weight)
    version = save_checkpoint(net, args.out)
    print(f"wrote {args.out} (schema {schema.SCHEMA_HASH:#010x}, version {version})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
