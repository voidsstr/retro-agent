#!/usr/bin/env python3
"""Synthetic demonstrations from a known scripted policy.

No engine adapter exists yet (Phase 1), so there is no real corpus to record
from. This generator produces one anyway, honestly: it builds plausible-shaped
synthetic observations (an enemy that wanders in and out of view, a wall the
bot drifts toward, damage events, a health bar that ticks down) and asks
`policyd.ScriptedPolicy` — the SAME hand-written policy the Phase 0 loadgen
already exercises — what it would do. That gives a known, inspectable
"expert" to clone, so the record -> train -> evaluate pipeline can be proven
end to end before any real demonstration exists.

This is explicitly NOT a claim that the synthetic distribution resembles real
gameplay well enough to produce a bot worth deploying — see
`docs/game-ai-bots-plan.md` §2.2 and the honesty note in `train/bc.py`. It is
a fixture with a known ground truth, which is exactly what proving a pipeline
needs and exactly what a real corpus (which has no ground truth to check
against) cannot give you.

Usage:
    python3 gen_synthetic.py --out /tmp/gb-demo --episodes 300 --length 150
"""

import argparse
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
import policyd  # noqa: E402  (ScriptedPolicy — the "expert")

if _HERE not in sys.path:
    sys.path.insert(0, _HERE)
import shard  # noqa: E402

# offset, count per field name — derived from the schema by name, never
# hand-counted, same discipline policyd.py's own hot loop follows.
_FIELD_OFF = {f[1]: (f[2], f[3]) for f in schema.FIELD_TABLE}


def _set(obs, name, t_slice, values):
    off, cnt = _FIELD_OFF[name]
    if cnt == 1:
        obs[t_slice, off] = values
    else:
        obs[t_slice, off:off + cnt] = values


def gen_episode_obs(rng, length):
    """-> (obs (length, OBS_DIM) float32, alive (length,) float32) for one
    bot's synthetic episode.

    Ego-centric/rotation-normalised the way the schema documents, though the
    numbers are synthetic rather than sampled from a real map: a self state
    that decays plausibly (health ticks down on random damage events, ammo
    drains), open geometry with an occasional wandering wall, and 0-2 entity
    slots (an enemy that random-walks in direction/distance and flickers in
    and out of visibility, an optional teammate that should never be shot).
    """
    if not HAVE_NUMPY:
        raise RuntimeError(
            "numpy is not installed — use ~/.venvs/gamebots/bin/python")
    obs = np.zeros((length, schema.OBS_DIM), dtype=np.float32)

    # --- self: health/death, resolved tick by tick because death is a
    # one-way transition that has to see its own running total ------------
    health = np.empty(length, dtype=np.float32)
    took_damage = np.zeros(length, dtype=np.float32)
    damage_dir = np.zeros((length, 2), dtype=np.float32)
    died = np.zeros(length, dtype=np.float32)
    killed = np.zeros(length, dtype=np.float32)
    alive = np.ones(length, dtype=np.float32)
    dmg_events = rng.random(length) < 0.03
    dmg_amount = rng.uniform(0.05, 0.35, size=length).astype(np.float32)
    kill_events = rng.random(length) < 0.01
    h = 1.0
    dead = False
    for t in range(length):
        if dead:
            health[t] = 0.0
            alive[t] = 0.0
            continue
        if dmg_events[t]:
            took_damage[t] = dmg_amount[t]
            ang = rng.uniform(0, 2 * np.pi)
            damage_dir[t] = (np.cos(ang), np.sin(ang))
            h = max(0.0, h - dmg_amount[t])
        health[t] = h
        alive[t] = 1.0
        if h <= 0.0:
            dead = True
            died[t] = 1.0
        elif kill_events[t] and h > 0.3:
            killed[t] = 1.0

    _set(obs, "health_frac", slice(None), health)
    _set(obs, "took_damage", slice(None), took_damage)
    _set(obs, "damage_dir", slice(None), damage_dir)
    _set(obs, "died", slice(None), died)
    _set(obs, "killed_someone", slice(None), killed)
    _set(obs, "alive", slice(None), alive)

    _set(obs, "armor_frac", slice(None),
        np.clip(1.0 - np.cumsum(rng.random(length) < 0.01) * 0.2, 0, 1))
    _set(obs, "ammo_frac", slice(None),
        np.clip(1.0 - np.linspace(0, rng.uniform(0.2, 0.9), length), 0, 1))
    _set(obs, "ammo_reserve_frac", slice(None),
        np.full(length, rng.uniform(0.3, 1.0), dtype=np.float32))
    _set(obs, "weapon_id_norm", slice(None),
        np.full(length, rng.uniform(0.0, 1.0), dtype=np.float32))
    vel_local = rng.normal(0, 0.3, size=(length, 3)).astype(np.float32)
    _set(obs, "vel_local", slice(None), vel_local)
    _set(obs, "speed_frac", slice(None),
        np.clip(np.linalg.norm(vel_local[:, :2], axis=1), 0, 1))
    _set(obs, "pitch_norm", slice(None),
        np.clip(rng.normal(0, 0.2, size=length), -1, 1))
    _set(obs, "on_ground", slice(None), (rng.random(length) > 0.05).astype(np.float32))
    _set(obs, "crouching", slice(None), (rng.random(length) < 0.1).astype(np.float32))
    _set(obs, "reloading", slice(None), (rng.random(length) < 0.05).astype(np.float32))

    # --- geometry: open rays, with an occasional wandering wall ahead -----
    ray_h = np.ones((length, schema.NUM_RAYS_H), dtype=np.float32)
    if rng.random() < 0.35:
        period = rng.uniform(1.0, 3.0)
        phase = rng.uniform(0, 2 * np.pi)
        wall = 0.5 + 0.45 * np.sin(np.linspace(0, period * np.pi, length) + phase)
        wall = np.clip(wall, 0.01, 1.0).astype(np.float32)
        ray_h[:, 0] = wall
        ray_h[:, 1] = wall
        ray_h[:, -1] = wall
    _set(obs, "ray_h", slice(None), ray_h)
    _set(obs, "ray_up", slice(None), np.ones(length, dtype=np.float32))
    _set(obs, "ray_down", slice(None), np.ones(length, dtype=np.float32))

    # --- entities: an enemy that random-walks, an optional teammate -------
    def _fill_slot(i, teammate, dist_lo=0.1, dist_hi=0.9, vis_flip_p=0.08):
        d_ang = rng.normal(0, 0.12, size=length).astype(np.float32)
        d_ang[0] = rng.uniform(0, 2 * np.pi)
        ang = np.cumsum(d_ang)
        d_dist = rng.normal(0, 0.03, size=length).astype(np.float32)
        dist = np.clip(np.cumsum(d_dist) + rng.uniform(dist_lo, dist_hi),
                       0.03, 1.0).astype(np.float32)
        vis = np.empty(length, dtype=np.float32)
        cur = 1.0
        flips = rng.random(length) < vis_flip_p
        for t in range(length):
            if flips[t]:
                cur = 1.0 - cur
            vis[t] = cur

        off, _c = _FIELD_OFF[f"e{i}_present"]
        obs[:, off] = 1.0
        obs[:, _FIELD_OFF[f"e{i}_is_teammate"][0]] = 1.0 if teammate else 0.0
        do, _dc = _FIELD_OFF[f"e{i}_dir"]
        obs[:, do] = np.cos(ang)
        obs[:, do + 1] = np.sin(ang)
        obs[:, do + 2] = np.clip(rng.normal(0, 0.1, size=length), -1, 1)
        obs[:, _FIELD_OFF[f"e{i}_dist_norm"][0]] = dist
        ro, rc = _FIELD_OFF[f"e{i}_rel_vel"]
        obs[:, ro:ro + rc] = rng.normal(0, 0.2, size=(length, rc))
        obs[:, _FIELD_OFF[f"e{i}_health_frac"][0]] = np.clip(
            rng.uniform(0.3, 1.0) - np.linspace(0, rng.uniform(0, 0.5), length),
            0, 1)
        obs[:, _FIELD_OFF[f"e{i}_visible"][0]] = vis

    if rng.random() < 0.8:
        _fill_slot(0, teammate=False)
    if rng.random() < 0.3:
        _fill_slot(1, teammate=True, dist_lo=0.2, dist_hi=0.6, vis_flip_p=0.03)

    # --- match context ------------------------------------------------------
    _set(obs, "round_time_frac", slice(None), np.linspace(0, 1, length, dtype=np.float32))
    _set(obs, "score_diff_norm", slice(None),
        np.full(length, rng.uniform(-1, 1), dtype=np.float32))
    _set(obs, "teammates_alive_frac", slice(None),
        np.full(length, rng.uniform(0.3, 1.0), dtype=np.float32))
    _set(obs, "enemies_alive_frac", slice(None),
        np.full(length, rng.uniform(0.3, 1.0), dtype=np.float32))
    _set(obs, "objective", slice(None), rng.uniform(0, 1, size=(length, 2)))
    # intent is left all-zero: no planner in Phase 2's synthetic data, and
    # FiLM treats all-zero as "behave as trained" (model.py's FiLM docstring).

    return obs, alive


def generate(out_dir, n_episodes=300, episode_len=150, seed=0,
            policy_name="scripted-synthetic", max_records_per_shard=50_000,
            log=None):
    """Writes n_episodes synthetic episodes (ScriptedPolicy's own actions as
    labels) into shard.ShardWriter shards under out_dir. Returns the total
    record count written."""
    rng = np.random.default_rng(seed)
    policy = policyd.ScriptedPolicy()
    writer = shard.ShardWriter(out_dir, policy_name=policy_name,
                               max_records_per_shard=max_records_per_shard)
    for ep in range(n_episodes):
        obs, alive = gen_episode_obs(rng, episode_len)
        bot_id = (seed * 100_003 + ep) % 65536
        rows = shard.record_array(episode_len)
        for t in range(episode_len):
            action = policy.act(t, 0, [(bot_id, obs[t])])[0]
            _bid, buttons, pitch, yaw, fwd, side, weapon = action
            rows[t]["bot_id"] = bot_id
            rows[t]["episode_id"] = ep
            rows[t]["tick"] = t
            rows[t]["done"] = 0 if alive[t] > 0.5 else 1
            rows[t]["obs"] = obs[t]
            rows[t]["buttons"] = buttons
            rows[t]["pitch"] = pitch
            rows[t]["yaw"] = yaw
            rows[t]["fwd"] = fwd
            rows[t]["side"] = side
            rows[t]["weapon"] = weapon
        writer.write_batch(rows)
        if log and (ep + 1) % max(1, n_episodes // 10) == 0:
            log(f"  {ep + 1}/{n_episodes} episodes "
               f"({writer.total_written:,} records)")
    writer.close()
    return writer.total_written


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--out", required=True)
    ap.add_argument("--episodes", type=int, default=300)
    ap.add_argument("--length", type=int, default=150)
    ap.add_argument("--seed", type=int, default=0)
    ap.add_argument("--policy-name", default="scripted-synthetic")
    args = ap.parse_args()

    n = generate(args.out, n_episodes=args.episodes, episode_len=args.length,
                seed=args.seed, policy_name=args.policy_name, log=print)
    print(f"wrote {n:,} records to {args.out} "
         f"(schema {schema.SCHEMA_HASH:#010x})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
