#!/usr/bin/env python3
"""The bot policy network — our architecture, built around our schema.

Not a flat MLP over 144 floats. The observation has structure that a flat net
would have to rediscover from data we do not have much of, so the structure is
built in:

**Grouped encoders.** `self`, `geom`, `game` and `intent` are semantically
different and are encoded separately before being fused. A flat first layer
mixes "my health" with "distance to the wall behind me" in its very first
matmul and has to learn to unmix them.

**Per-entity encoder + attention over the slots.** The 8 entity slots share one
encoder — an enemy is an enemy whichever slot it lands in — and are pooled by
attention rather than concatenated. Concatenation makes the net learn slot 3
and slot 5 independently; shared weights plus attention gives it "the
threatening one" for free, and it degrades gracefully when fewer than 8 are
present because absent slots are masked out rather than fed as zeros.

**A recurrent core.** An FPS is not a Markov game from the bot's point of view:
an enemy that ducks behind a wall still exists. A GRU gives it "something was
there a moment ago", which is the difference between a bot that fights and one
that forgets. This is the same reason DeepMind's FTW agent was recurrent.

**Intent conditioning via FiLM.** The LLM planner's intent vector (Phase 4)
modulates the trunk with a learned scale and shift rather than being
concatenated. Concatenated conditioning gets ignored early in training and
often never recovers; FiLM multiplies, so it cannot be routed around.

**Heads matched to the action space.** A squashed Gaussian for the two
continuous view deltas, independent Bernoullis for buttons (they genuinely
co-occur -- jumping while firing), a categorical for weapon, and a value head
so the same network serves behavioural cloning now and PPO in Phase 3 without
a rebuild.

Sizing: the default is ~1.5M parameters and ~3 MFLOPs per decision. At 512 bots
and 30 Hz that is ~46 GFLOP/s against a card that does ~100 TFLOP/s. The model
is deliberately small because latency inside a game frame is the constraint,
not capacity -- and because we will not have the data to feed a large one.

    python3 model.py --summary
    python3 model.py --bench          # measure on the 5090
"""

import argparse
import math
import sys

try:
    import torch
    import torch.nn as nn
    import torch.nn.functional as F
    HAVE_TORCH = True
except ImportError:  # the harness must import without torch (Phase 0 has none)
    HAVE_TORCH = False
    torch = None
    nn = object

import os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import schema  # noqa: E402


# --------------------------------------------------------------------------
# where each group lives in the observation — derived, never hardcoded
# --------------------------------------------------------------------------

def group_slices():
    """(name, start, length) per schema group, in wire order.

    Read from the schema rather than written down, so a layout change moves the
    encoders with it instead of silently feeding the wrong floats to the wrong
    encoder -- the exact bug class that cost us twice in Phase 0.
    """
    out, cur, start = [], None, 0
    total = 0
    for group, _name, off, count, _doc in schema.FIELD_TABLE:
        if group != cur:
            if cur is not None:
                out.append((cur, start, total - start))
            cur, start = group, off
        total = off + count
    out.append((cur, start, total - start))
    return out


GROUPS = group_slices()
GROUP_DIMS = {g: n for g, _s, n in GROUPS}
ENT_START = GROUP_DIMS and next(s for g, s, _n in GROUPS if g == "ent")
ENT_SLOT_DIM = GROUP_DIMS["ent"] // schema.MAX_ENTITIES

# Everything that is not an entity slot and not intent: fused as the "context".
CONTEXT_GROUPS = [g for g, _s, _n in GROUPS if g not in ("ent", "intent")]
CONTEXT_DIM = sum(GROUP_DIMS[g] for g in CONTEXT_GROUPS)


def _assert_layout():
    assert GROUP_DIMS["ent"] % schema.MAX_ENTITIES == 0, \
        "entity group is not an exact multiple of the slot count"
    assert GROUP_DIMS["intent"] == schema.INTENT_DIM


_assert_layout()


# --------------------------------------------------------------------------
# the network
# --------------------------------------------------------------------------

if HAVE_TORCH:

    class FiLM(nn.Module):
        """Feature-wise linear modulation: h <- gamma(intent) * h + beta(intent).

        Used for the planner's intent because concatenated conditioning is easy
        for a net to ignore -- it can zero those input weights and never look
        again. A multiplicative gate cannot be routed around, so "push B"
        actually changes behaviour rather than being a suggestion.
        """

        def __init__(self, cond_dim, feat_dim):
            super().__init__()
            self.to_scale = nn.Linear(cond_dim, feat_dim)
            self.to_shift = nn.Linear(cond_dim, feat_dim)
            # Start as the identity: an untrained planner must not perturb a
            # trained policy, and at init the intent vector is all zeros anyway.
            nn.init.zeros_(self.to_scale.weight)
            nn.init.ones_(self.to_scale.bias)
            nn.init.zeros_(self.to_shift.weight)
            nn.init.zeros_(self.to_shift.bias)

        def forward(self, h, cond):
            return self.to_scale(cond) * h + self.to_shift(cond)

    class EntityAttention(nn.Module):
        """Shared per-slot encoder + masked attention pooling.

        The mask is the point. Absent slots are all-zero floats, and a mean or
        sum over them drags the pooled vector toward zero as the fight thins
        out -- so the net would read "few enemies" and "far enemies" the same
        way. Masking to -inf before the softmax makes an absent slot contribute
        nothing at all.
        """

        def __init__(self, slot_dim, hidden, heads=4):
            super().__init__()
            self.encode = nn.Sequential(
                nn.Linear(slot_dim, hidden), nn.SiLU(),
                nn.Linear(hidden, hidden),
            )
            self.query = nn.Parameter(torch.randn(1, 1, hidden) * 0.02)
            self.attn = nn.MultiheadAttention(hidden, heads, batch_first=True)
            self.norm = nn.LayerNorm(hidden)

        def forward(self, ents, present):
            # ents: (B, K, slot_dim)   present: (B, K) 1/0
            h = self.encode(ents)
            q = self.query.expand(h.shape[0], -1, -1)
            pad_mask = present < 0.5                      # True == ignore
            # A row with nothing present would make softmax produce NaN across
            # a fully-masked set. Let such rows attend to slot 0 and then zero
            # the result, which is well-defined and costs one multiply.
            empty = pad_mask.all(dim=1)
            pad_mask = pad_mask.clone()
            pad_mask[empty, 0] = False
            pooled, _w = self.attn(q, h, h, key_padding_mask=pad_mask,
                                   need_weights=False)
            pooled = pooled.squeeze(1)
            pooled = pooled * (~empty).unsqueeze(-1).to(pooled.dtype)
            return self.norm(pooled)

    class BotPolicy(nn.Module):
        """The policy. One forward pass serves a whole server's bots."""

        def __init__(self, hidden=256, ent_hidden=128, gru_hidden=256,
                     n_weapons=16):
            super().__init__()
            self.hidden = hidden
            self.gru_hidden = gru_hidden
            self.n_weapons = n_weapons

            self.context = nn.Sequential(
                nn.Linear(CONTEXT_DIM, hidden), nn.SiLU(),
                nn.Linear(hidden, hidden), nn.SiLU(),
            )
            self.entities = EntityAttention(ENT_SLOT_DIM, ent_hidden)
            self.fuse = nn.Sequential(
                nn.Linear(hidden + ent_hidden, hidden), nn.SiLU(),
            )
            self.film = FiLM(schema.INTENT_DIM, hidden)
            self.core = nn.GRUCell(hidden, gru_hidden)
            self.trunk = nn.Sequential(nn.LayerNorm(gru_hidden), nn.SiLU())

            # Heads. Continuous view deltas are a squashed Gaussian: the mean is
            # what we use at inference, the log-std is what PPO needs later.
            self.view_mean = nn.Linear(gru_hidden, 2)
            self.view_logstd = nn.Parameter(torch.full((2,), -1.0))
            self.move = nn.Linear(gru_hidden, 2)          # forward, side
            self.buttons = nn.Linear(gru_hidden, len(schema.BUTTON_NAMES))
            self.weapon = nn.Linear(gru_hidden, n_weapons)
            self.value = nn.Linear(gru_hidden, 1)

            # Small init on the action heads so a fresh net twitches rather
            # than spinning: a randomly-initialised policy that slams the view
            # delta to its clamp every tick looks broken and trains badly.
            for head in (self.view_mean, self.move):
                nn.init.uniform_(head.weight, -1e-3, 1e-3)
                nn.init.zeros_(head.bias)

        def split(self, obs):
            """Observation -> (context, entity slots, present mask, intent)."""
            ctx = torch.cat([obs[:, s:s + n] for g, s, n in GROUPS
                             if g in CONTEXT_GROUPS], dim=1)
            ents = obs[:, ENT_START:ENT_START + GROUP_DIMS["ent"]]
            ents = ents.view(-1, schema.MAX_ENTITIES, ENT_SLOT_DIM)
            present = ents[:, :, 0]                       # e*_present is first
            intent_start = next(s for g, s, _n in GROUPS if g == "intent")
            intent = obs[:, intent_start:intent_start + schema.INTENT_DIM]
            return ctx, ents, present, intent

        def forward(self, obs, hx=None):
            ctx, ents, present, intent = self.split(obs)
            h = self.fuse(torch.cat([self.context(ctx),
                                     self.entities(ents, present)], dim=1))
            h = self.film(h, intent)
            if hx is None:
                hx = obs.new_zeros(obs.shape[0], self.gru_hidden)
            hx = self.core(h, hx)
            z = self.trunk(hx)
            return {
                "view_mean": self.view_mean(z),
                "view_logstd": self.view_logstd.expand_as(self.view_mean(z)),
                "move": self.move(z),
                "buttons": self.buttons(z),
                "weapon": self.weapon(z),
                "value": self.value(z).squeeze(-1),
            }, hx

        @torch.no_grad()
        def act(self, obs, hx=None, deterministic=True):
            """Inference path: observation batch -> action tensors + new state.

            Returns already-bounded values. Bounding here as well as in the
            adapter is deliberate belt-and-braces: the adapter is the one that
            must not be bypassed, but a policy that emits in-range numbers is
            far easier to debug than one whose output only looks sane after
            someone else clamps it.
            """
            out, hx = self.forward(obs, hx)
            view = torch.tanh(out["view_mean"])
            if not deterministic:
                std = out["view_logstd"].exp()
                view = torch.tanh(out["view_mean"] + std * torch.randn_like(std))
            pitch = view[:, 0] * schema.MAX_PITCH_DELTA_DEG
            yaw = view[:, 1] * schema.MAX_YAW_DELTA_DEG
            move = torch.tanh(out["move"])
            buttons = (torch.sigmoid(out["buttons"]) > 0.5)
            weapon = out["weapon"].argmax(dim=1)
            return {
                "pitch": pitch, "yaw": yaw,
                "forward": move[:, 0], "side": move[:, 1],
                "buttons": buttons, "weapon": weapon,
                "value": out["value"],
            }, hx

    def build(**kw):
        return BotPolicy(**kw)

    def param_count(m):
        return sum(p.numel() for p in m.parameters())

    def estimate_flops(m, batch=1):
        """Rough forward FLOPs (2 per MAC) — enough to reason about headroom."""
        total = 0
        for mod in m.modules():
            if isinstance(mod, nn.Linear):
                total += 2 * mod.in_features * mod.out_features
            elif isinstance(mod, nn.GRUCell):
                total += 2 * 3 * (mod.input_size + mod.hidden_size) * mod.hidden_size
        # The per-entity encoder runs once per slot, not once per bot.
        for mod in m.entities.encode.modules():
            if isinstance(mod, nn.Linear):
                total += 2 * mod.in_features * mod.out_features * (schema.MAX_ENTITIES - 1)
        return total * batch

else:  # pragma: no cover - exercised only on hosts without torch
    def build(**kw):
        raise RuntimeError(
            "PyTorch is not installed. The Phase 0 harness runs without it; "
            "the model needs it:  ~/.venvs/gamebots/bin/pip install torch")


# --------------------------------------------------------------------------
# CLI
# --------------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--summary", action="store_true")
    ap.add_argument("--bench", action="store_true")
    ap.add_argument("--batches", default="1,8,32,64,128,256,512,1024")
    ap.add_argument("--hidden", type=int, default=256)
    ap.add_argument("--iters", type=int, default=200)
    args = ap.parse_args()

    if not HAVE_TORCH:
        print("PyTorch not installed — use ~/.venvs/gamebots/bin/python",
              file=sys.stderr)
        return 2

    print(f"schema {schema.SCHEMA_HASH:#010x}  obs_dim={schema.OBS_DIM}")
    print(f"groups: " + ", ".join(f"{g}[{s}:{s+n}]" for g, s, n in GROUPS))
    print(f"context_dim={CONTEXT_DIM}  entity_slot_dim={ENT_SLOT_DIM} "
          f"x{schema.MAX_ENTITIES}  intent_dim={schema.INTENT_DIM}")

    m = build(hidden=args.hidden)
    print(f"\nparameters: {param_count(m):,}")
    print(f"forward FLOPs/decision: ~{estimate_flops(m)/1e6:.2f} M")

    if args.summary:
        print()
        print(m)

    if args.bench:
        if not torch.cuda.is_available():
            print("\nno CUDA device available", file=sys.stderr)
            return 1
        dev = torch.device("cuda")
        name = torch.cuda.get_device_name(0)
        print(f"\ndevice: {name}  torch {torch.__version__}  "
              f"cuda {torch.version.cuda}")
        m = m.to(dev).eval()
        for dtype in (torch.float16, torch.float32):
            m2 = m.to(dtype)
            print(f"\n--- {str(dtype).split('.')[-1]} ---")
            print(f"{'batch':>7} {'ms/step':>9} {'us/bot':>8} "
                  f"{'decisions/s':>13} {'bots@30Hz':>11}")
            for b in [int(x) for x in args.batches.split(",")]:
                obs = torch.randn(b, schema.OBS_DIM, device=dev, dtype=dtype)
                hx = torch.zeros(b, m2.gru_hidden, device=dev, dtype=dtype)
                for _ in range(20):
                    _a, _h = m2.act(obs, hx)
                torch.cuda.synchronize()
                import time as _t
                t0 = _t.perf_counter()
                for _ in range(args.iters):
                    _a, _h = m2.act(obs, hx)
                torch.cuda.synchronize()
                ms = (_t.perf_counter() - t0) / args.iters * 1000
                dps = b / (ms / 1000)
                print(f"{b:>7} {ms:>9.3f} {ms*1000/b:>8.2f} {dps:>13,.0f} "
                      f"{dps/30:>11,.0f}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
