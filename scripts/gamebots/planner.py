#!/usr/bin/env python3
"""The strategic layer — squad tactics at ~2 Hz, on the same GPU.

The policy network is a reflex layer: aim, strafe, peek, reload, thirty times a
second. It is bad at anything that needs a plan, and making it good at that by
training is the expensive, data-hungry road (see the FTW agent's thousands of
parallel matches, which we do not have).

So the plan comes from somewhere else. Following the hierarchical-control
result in docs/game-ai-bots-plan.md §4.3: a language model runs at ~2 Hz per
SQUAD -- not per bot per frame -- reads a compact text summary of the match,
and emits an *intent* per bot. That intent becomes the 16-float slot the schema
has reserved from the start, and FiLM-conditions the policy, so one trained
network plays every role.

Two properties make this cheap enough to be free:

  * **2 Hz per squad, not 30 Hz per bot.** Four squads is eight LLM calls a
    second, against a policy doing fifteen thousand. The planner can be a
    thousand times heavier per call and still cost less.
  * **It lives inside the policy server.** policyd already receives every
    bot's observation every frame, so the planner needs no protocol of its
    own, and the intent is injected into the observation before inference --
    which means engine adapters never learn the planner exists.

And one property makes it safe: **a planner that is slow, broken or absent
leaves the bots exactly as they were.** Intent defaults to zeros, FiLM is
initialised to the identity, so no plan means "behave as trained". A strategic
layer must never be able to make the reflex layer worse.

Backends, in order of what they need:

  heuristic  no dependencies. Rule-based, always available, and the fallback
             whenever the LLM is unavailable or times out.
  llm        a local instruct model on the GPU via transformers.

    python3 planner.py --demo                 # heuristic, no GPU needed
    python3 planner.py --demo --backend llm --model <hf-id>
"""

import argparse
import json
import os
import re
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import schema  # noqa: E402

# The intent vocabulary. Deliberately small and readable: these are the words
# the LLM is asked to choose between, and each maps to one dimension of the
# intent vector. A bigger vocabulary is easy to add later (the vector has room)
# but every entry is something the policy must learn to condition on, so they
# earn their place.
INTENTS = [
    "attack",        # push the enemy, take space
    "defend",        # hold current position
    "retreat",       # break contact, find health
    "flank",         # take an indirect route
    "regroup",       # move toward teammates
    "hunt",          # seek out the enemy, no particular ground
    "objective",     # bomb / flag / control point
    "camp",          # hold still and watch an angle
]
assert len(INTENTS) <= schema.INTENT_DIM, "intent vocabulary exceeds the slot"

INTENT_INDEX = {name: i for i, name in enumerate(INTENTS)}

# Remaining dimensions carry scalars the planner can set alongside the
# one-hot: how hard to commit, and how much risk to accept.
AGGRESSION_DIM = len(INTENTS)
CAUTION_DIM = len(INTENTS) + 1

PLAN_PERIOD_SEC = 0.5          # 2 Hz
LLM_TIMEOUT_SEC = 2.0          # a late plan is worse than the previous plan


def intent_vector(name, aggression=0.5, caution=0.5):
    """One intent as the float slot the policy sees."""
    vec = [0.0] * schema.INTENT_DIM
    idx = INTENT_INDEX.get(name)
    if idx is None:
        return vec                       # unknown word -> no plan, not a crash
    vec[idx] = 1.0
    vec[AGGRESSION_DIM] = max(0.0, min(1.0, aggression))
    vec[CAUTION_DIM] = max(0.0, min(1.0, caution))
    return vec


# --------------------------------------------------------------------------
# reading the match out of the observations policyd already has
# --------------------------------------------------------------------------

_OFF = {f[1]: f[2] for f in schema.FIELD_TABLE}


def summarise_bot(obs):
    """One bot's situation, as plain numbers. Small on purpose: this becomes
    text in a prompt, and a prompt full of 144 floats is a prompt the model
    cannot reason about."""
    ents = []
    for i in range(schema.MAX_ENTITIES):
        b = _OFF[f"e{i}_present"]
        if obs[b] < 0.5:
            continue
        ents.append({
            "teammate": obs[b + (_OFF["e0_is_teammate"] - _OFF["e0_present"])] >= 0.5,
            "dist": round(float(obs[b + (_OFF["e0_dist_norm"] - _OFF["e0_present"])]), 2),
            "visible": obs[b + (_OFF["e0_visible"] - _OFF["e0_present"])] >= 0.5,
        })
    enemies = [e for e in ents if not e["teammate"]]
    mates = [e for e in ents if e["teammate"]]
    return {
        "health": round(float(obs[_OFF["health_frac"]]), 2),
        "armor": round(float(obs[_OFF["armor_frac"]]), 2),
        "ammo": round(float(obs[_OFF["ammo_frac"]]), 2),
        "alive": obs[_OFF["alive"]] >= 0.5,
        "enemies_seen": sum(1 for e in enemies if e["visible"]),
        "enemies_near": sum(1 for e in enemies if e["dist"] < 0.25),
        "nearest_enemy": min((e["dist"] for e in enemies), default=None),
        "teammates_near": sum(1 for e in mates if e["dist"] < 0.35),
        "took_damage": round(float(obs[_OFF["took_damage"]]), 2),
        "score_diff": round(float(obs[_OFF["score_diff_norm"]]), 2),
        "round_time": round(float(obs[_OFF["round_time_frac"]]), 2),
    }


# --------------------------------------------------------------------------
# backends
# --------------------------------------------------------------------------

class HeuristicPlanner:
    """Rules. No model, no GPU, never slow.

    This is not a placeholder for the LLM -- it is the floor the LLM has to
    beat, and the fallback whenever the LLM is unavailable. The cited study
    found LLM+RL statistically TIED with a hand-built behaviour tree on win
    rate, so a decent rule set is a genuinely competitive baseline; the LLM's
    edge is meant to be human-likeness and authorability, not strength.
    """

    name = "heuristic"

    def plan(self, bots):
        out = {}
        for bot_id, s in bots.items():
            if not s["alive"]:
                out[bot_id] = ("regroup", 0.3, 0.5)
                continue
            if s["health"] < 0.3 and s["enemies_seen"]:
                out[bot_id] = ("retreat", 0.1, 0.9)
            elif s["ammo"] < 0.15:
                out[bot_id] = ("retreat", 0.2, 0.8)
            elif s["enemies_near"] >= 2 and s["teammates_near"] == 0:
                out[bot_id] = ("retreat", 0.2, 0.8)
            elif s["enemies_seen"] and s["health"] > 0.6:
                out[bot_id] = ("attack", 0.9, 0.2)
            elif s["enemies_seen"]:
                out[bot_id] = ("attack", 0.6, 0.5)
            elif s["took_damage"] > 0.0:
                out[bot_id] = ("hunt", 0.6, 0.4)
            elif s["score_diff"] < -0.3:
                out[bot_id] = ("hunt", 0.8, 0.2)
            elif s["score_diff"] > 0.3 and s["round_time"] > 0.7:
                out[bot_id] = ("defend", 0.3, 0.8)
            else:
                out[bot_id] = ("hunt", 0.5, 0.5)
        return out


SYSTEM_PROMPT = """You are the squad commander for bots in a deathmatch FPS.
Each turn you get one line per bot describing what it can see. Assign each bot
exactly one intent from this list:

  attack defend retreat flank regroup hunt objective camp

Reply with ONE line per bot, nothing else, in this exact format:

  <bot_id> <intent> <aggression 0-1> <caution 0-1>

Rules: a bot below 0.3 health with enemies visible should usually retreat. Do
not send every bot to the same place; a squad that moves as one blob is easy to
kill. Prefer variety - it is more interesting to play against."""


class LlmPlanner:
    """A local instruct model on the GPU.

    Runs on the same card as the policy, which is the whole point: the policy
    uses about 1% of a 5090, so the strategic layer is free capacity that would
    otherwise idle.

    Falls back to the heuristic on ANY failure -- load error, timeout, garbage
    output. A strategic layer that can break the bots is worse than no
    strategic layer.
    """

    name = "llm"

    def __init__(self, model_id, device="cuda", max_new_tokens=160):
        from transformers import AutoModelForCausalLM, AutoTokenizer
        import torch
        self.torch = torch
        self.model_id = model_id
        self.tok = AutoTokenizer.from_pretrained(model_id)
        self.model = AutoModelForCausalLM.from_pretrained(
            model_id, dtype=torch.bfloat16, device_map=device)
        self.model.eval()
        self.device = device
        self.max_new_tokens = max_new_tokens
        self.fallback = HeuristicPlanner()
        self.failures = 0
        self.calls = 0
        self.last_ms = 0.0
        self.last_raw = ""

    def _prompt(self, bots):
        lines = []
        for bot_id, s in sorted(bots.items()):
            near = "-" if s["nearest_enemy"] is None else f"{s['nearest_enemy']:.2f}"
            lines.append(
                f"bot {bot_id}: health {s['health']:.2f} ammo {s['ammo']:.2f} "
                f"enemies_visible {s['enemies_seen']} nearest {near} "
                f"teammates_near {s['teammates_near']} "
                f"{'TOOK_DAMAGE ' if s['took_damage'] > 0 else ''}"
                f"{'DEAD' if not s['alive'] else ''}")
        return "\n".join(lines)

    def plan(self, bots):
        if not bots:
            return {}
        self.calls += 1
        t0 = time.perf_counter()
        try:
            msgs = [{"role": "system", "content": SYSTEM_PROMPT},
                    {"role": "user", "content": self._prompt(bots)}]
            text = self.tok.apply_chat_template(
                msgs, tokenize=False, add_generation_prompt=True)
            ids = self.tok(text, return_tensors="pt").to(self.device)
            with self.torch.no_grad():
                out = self.model.generate(
                    **ids, max_new_tokens=self.max_new_tokens,
                    do_sample=True, temperature=0.7, top_p=0.9,
                    pad_token_id=self.tok.eos_token_id)
            raw = self.tok.decode(out[0][ids["input_ids"].shape[1]:],
                                  skip_special_tokens=True)
            self.last_raw = raw
            parsed = parse_plan(raw, bots)
            self.last_ms = (time.perf_counter() - t0) * 1000
            if not parsed:
                raise ValueError("model returned no usable assignments")
            # Any bot the model forgot keeps a sensible default rather than
            # losing its plan entirely.
            base = self.fallback.plan(bots)
            base.update(parsed)
            return base
        except Exception as exc:      # noqa: BLE001
            self.failures += 1
            self.last_ms = (time.perf_counter() - t0) * 1000
            self.last_raw = f"<{type(exc).__name__}: {exc}>"
            return self.fallback.plan(bots)


_PLAN_RE = re.compile(
    r"^\s*(?:bot\s*)?(\d+)\s*[:\-]?\s*([a-z]+)"
    r"(?:\s+([01](?:\.\d+)?))?(?:\s+([01](?:\.\d+)?))?",
    re.IGNORECASE)


def parse_plan(text, bots):
    """Pull assignments out of whatever the model said.

    Deliberately forgiving about formatting and strict about content: an
    instruct model will add "Sure, here are the assignments:" no matter how the
    prompt is worded, but an intent outside the vocabulary must be dropped
    rather than silently mapped to something.
    """
    out = {}
    for line in text.splitlines():
        m = _PLAN_RE.match(line.strip())
        if not m:
            continue
        bot_id = int(m.group(1))
        intent = m.group(2).lower()
        if bot_id not in bots or intent not in INTENT_INDEX:
            continue
        agg = float(m.group(3)) if m.group(3) else 0.5
        cau = float(m.group(4)) if m.group(4) else 0.5
        out[bot_id] = (intent, agg, cau)
    return out


# --------------------------------------------------------------------------
# the service that policyd runs
# --------------------------------------------------------------------------

class PlannerService:
    """Runs the planner on its own thread at PLAN_PERIOD_SEC.

    Off the serving path entirely: policyd hands it the latest observations and
    reads back the newest intents whenever it likes. A planner that took a
    second would delay a plan, never a frame.
    """

    def __init__(self, backend=None, period=PLAN_PERIOD_SEC):
        self.backend = backend or HeuristicPlanner()
        self.period = period
        self._latest = {}        # (conn, bot) -> summary
        self._intents = {}       # (conn, bot) -> intent vector
        self._lock = threading.Lock()
        self._stop = threading.Event()
        self._thread = None
        self.plans = 0
        self.last_plan_ms = 0.0

    def observe(self, conn_key, bot_ids, obs):
        """Called by policyd with whatever it just received. Cheap: summarising
        happens here, but only the small dict is kept."""
        with self._lock:
            for i, bid in enumerate(bot_ids):
                self._latest[(conn_key, int(bid))] = summarise_bot(obs[i])

    def intent_for(self, conn_key, bot_id):
        return self._intents.get((conn_key, int(bot_id)))

    def start(self):
        self._thread = threading.Thread(target=self._loop, daemon=True,
                                        name="gamebots-planner")
        self._thread.start()
        return self

    def stop(self):
        self._stop.set()

    def plan_once(self):
        with self._lock:
            snapshot = dict(self._latest)
        if not snapshot:
            return 0
        # One call per server, because a plan is about a squad on a map — bots
        # on different servers share nothing worth reasoning about jointly.
        by_conn = {}
        for (conn, bid), s in snapshot.items():
            by_conn.setdefault(conn, {})[bid] = s
        t0 = time.perf_counter()
        fresh = {}
        for conn, bots in by_conn.items():
            for bid, (intent, agg, cau) in self.backend.plan(bots).items():
                fresh[(conn, bid)] = intent_vector(intent, agg, cau)
        self._intents = fresh          # atomic swap; readers never see a tear
        self.last_plan_ms = (time.perf_counter() - t0) * 1000
        self.plans += 1
        return len(fresh)

    def _loop(self):
        while not self._stop.is_set():
            try:
                self.plan_once()
            except Exception as exc:   # noqa: BLE001
                print(f"planner: {type(exc).__name__}: {exc}", file=sys.stderr)
            self._stop.wait(self.period)

    def stats(self):
        return {
            "planner": self.backend.name,
            "plans": self.plans,
            "last_plan_ms": round(self.last_plan_ms, 2),
            "bots_planned": len(self._intents),
            "llm_failures": getattr(self.backend, "failures", None),
            "llm_calls": getattr(self.backend, "calls", None),
        }


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--backend", default="heuristic",
                    choices=("heuristic", "llm"))
    ap.add_argument("--model", default=os.environ.get(
        "GAMEBOTS_PLANNER_MODEL", "Qwen/Qwen2.5-1.5B-Instruct"))
    ap.add_argument("--demo", action="store_true")
    args = ap.parse_args()

    backend = HeuristicPlanner()
    if args.backend == "llm":
        print(f"loading {args.model} ...")
        backend = LlmPlanner(args.model)

    if not args.demo:
        print(f"intents: {', '.join(INTENTS)}")
        print(f"intent slot: {schema.INTENT_DIM} floats "
              f"({len(INTENTS)} one-hot + aggression + caution)")
        return 0

    # A situation per bot that a plan should visibly react to.
    situations = {
        0: dict(health=0.15, armor=0.0, ammo=0.8, alive=True, enemies_seen=2,
                enemies_near=1, nearest_enemy=0.12, teammates_near=0,
                took_damage=0.3, score_diff=-0.2, round_time=0.5),
        1: dict(health=1.0, armor=0.8, ammo=1.0, alive=True, enemies_seen=1,
                enemies_near=0, nearest_enemy=0.5, teammates_near=1,
                took_damage=0.0, score_diff=0.4, round_time=0.8),
        2: dict(health=0.7, armor=0.2, ammo=0.05, alive=True, enemies_seen=0,
                enemies_near=0, nearest_enemy=None, teammates_near=1,
                took_damage=0.0, score_diff=0.0, round_time=0.3),
        3: dict(health=0.9, armor=0.5, ammo=0.6, alive=False, enemies_seen=0,
                enemies_near=0, nearest_enemy=None, teammates_near=0,
                took_damage=0.0, score_diff=-0.5, round_time=0.9),
    }
    t0 = time.perf_counter()
    plan = backend.plan(situations)
    ms = (time.perf_counter() - t0) * 1000
    print(f"\nbackend={backend.name}  {ms:.1f} ms for {len(situations)} bots\n")
    print(f"{'bot':>4} {'situation':<46} {'intent':<10} {'agg':>5} {'cau':>5}")
    for bid in sorted(situations):
        s = situations[bid]
        desc = (f"hp {s['health']:.2f} ammo {s['ammo']:.2f} "
                f"seen {s['enemies_seen']} near {s['enemies_near']} "
                f"{'DEAD' if not s['alive'] else ''}")
        intent, agg, cau = plan[bid]
        print(f"{bid:>4} {desc:<46} {intent:<10} {agg:>5.2f} {cau:>5.2f}")
    if hasattr(backend, "last_raw") and backend.last_raw:
        print(f"\nmodel said:\n{backend.last_raw.strip()[:400]}")
    print(f"\nintent vector for bot 0: "
          f"{intent_vector(*plan[0])[:len(INTENTS)+2]}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
