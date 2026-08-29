#!/usr/bin/env python3
"""Measures how well a policy net imitates the scripted expert on FRESH
synthetic episodes it never trained on.

This is the actual proof the BC pipeline works end to end — a training loss
that went down on a shuffled held-out slice of the SAME recording proves the
net can fit that recording, not that it generalises. `fresh_examples()` draws
a new seed through record.gen_synthetic's generator (never used for training)
and asks ScriptedPolicy — the ground truth — what it would have done, so
`evaluate()` compares a candidate net's predictions against a known-correct
answer on data it has not seen.
"""

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
import schema   # noqa: E402
import model    # noqa: E402
import policyd  # noqa: E402

_REC = os.path.join(_GB, "record")
if _REC not in sys.path:
    sys.path.insert(0, _REC)
import gen_synthetic  # noqa: E402

HAVE_TORCH = model.HAVE_TORCH


def fresh_examples(n_episodes=60, episode_len=100, seed=999):
    """-> (obs (N, OBS_DIM) float32, target dict of numpy arrays) — the
    scripted expert's own action for each observation, the ground truth to
    imitate. Uses a seed disjoint from any training run by convention (the
    e2e driver uses 0 for training data, 999 for this)."""
    if not HAVE_NUMPY:
        raise RuntimeError(
            "numpy is not installed — use ~/.venvs/gamebots/bin/python")
    rng = np.random.default_rng(seed)
    policy = policyd.ScriptedPolicy()
    obs_all = []
    pitch, yaw, fwd, side, buttons, weapon = [], [], [], [], [], []
    for ep in range(n_episodes):
        obs, _alive = gen_synthetic.gen_episode_obs(rng, episode_len)
        bot_id = (seed * 100_003 + ep) % 65536
        for t in range(episode_len):
            action = policy.act(t, 0, [(bot_id, obs[t])])[0]
            _bid, btn, p, y, f, s, w = action
            obs_all.append(obs[t])
            pitch.append(p)
            yaw.append(y)
            fwd.append(f)
            side.append(s)
            buttons.append(btn)
            weapon.append(w)
    return (np.array(obs_all, dtype=np.float32),
           {"pitch": np.array(pitch, dtype=np.float32),
            "yaw": np.array(yaw, dtype=np.float32),
            "fwd": np.array(fwd, dtype=np.float32),
            "side": np.array(side, dtype=np.float32),
            "buttons": np.array(buttons, dtype=np.uint16),
            "weapon": np.array(weapon, dtype=np.uint8)})


def evaluate(net, obs_np, target, device="cpu"):
    """Runs net.act() (hx=None -- the same stateless-inference assumption
    bc.py trains under) over obs_np and compares against the expert's own
    target action. Every number here is honest about what it measures:

      * continuous heads (pitch/yaw/fwd/side): mean absolute error in the
        SAME units the game server sees (degrees for view, [-1,1] for move).
      * attack button: precision/recall, not accuracy — ScriptedPolicy only
        fires a fraction of the time (attack_base_rate), so a net that always
        predicts "no" scores high accuracy while being useless. Precision and
        recall (and the base rate to compare against) are the honest numbers.
      * weapon: plain accuracy, WITH A CAVEAT — ScriptedPolicy never changes
        weapon (always emits 0), so this head has nothing informative to
        learn from THIS expert on synthetic data. A high weapon_accuracy
        number here is not evidence the weapon head learned anything; it is
        evidence the target is degenerate. Do not cite it as a win.
    """
    import torch
    device = torch.device(device)
    net = net.to(device).eval()
    obs_t = torch.as_tensor(obs_np, dtype=next(net.parameters()).dtype,
                            device=device)
    with torch.no_grad():
        act, _hx = net.act(obs_t, hx=None, deterministic=True)

    pitch = act["pitch"].float().cpu().numpy()
    yaw = act["yaw"].float().cpu().numpy()
    fwd = act["forward"].float().cpu().numpy()
    side = act["side"].float().cpu().numpy()
    attack_pred = act["buttons"][:, 0].cpu().numpy().astype(bool)  # bit 0 == BTN_ATTACK
    weapon_pred = act["weapon"].cpu().numpy()

    attack_true = ((target["buttons"].astype(np.uint16) & schema.BTN_ATTACK) != 0)

    def mae(a, b):
        return float(np.mean(np.abs(a - b)))

    tp = int(np.sum(attack_pred & attack_true))
    fp = int(np.sum(attack_pred & ~attack_true))
    fn = int(np.sum(~attack_pred & attack_true))
    precision = tp / (tp + fp) if (tp + fp) else 0.0
    recall = tp / (tp + fn) if (tp + fn) else 0.0

    return {
        "n": int(len(obs_np)),
        "pitch_mae_deg": mae(pitch, target["pitch"]),
        "yaw_mae_deg": mae(yaw, target["yaw"]),
        "fwd_mae": mae(fwd, target["fwd"]),
        "side_mae": mae(side, target["side"]),
        "attack_precision": precision,
        "attack_recall": recall,
        "attack_base_rate": float(np.mean(attack_true)),
        "weapon_accuracy": float(np.mean(weapon_pred == target["weapon"])),
    }
