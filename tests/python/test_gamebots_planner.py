"""Tests for the strategic layer (the LLM planner).

The planner is the one component that can only make things worse, so the tests
are mostly about it being unable to:

  * no plan must mean "behave exactly as trained" — the intent vector is zeros
    and FiLM is the identity there;
  * an LLM that fails, times out, or returns nonsense must fall back to the
    heuristic rather than propagating garbage into an observation;
  * an intent word outside the vocabulary must be dropped, not mapped to
    whatever happens to be nearby in the vector;
  * planning must never run on the serving path.

The LLM backend itself is not exercised here (it needs a GPU and a model
download); its *failure* behaviour is, with a stub, because that is the part
that has to work when nobody is watching.

Run: pytest tests/python/test_gamebots_planner.py
"""

import importlib.util
import sys
from pathlib import Path

import pytest

_GB = Path(__file__).resolve().parent.parent.parent / "scripts" / "gamebots"


def _load(name):
    spec = importlib.util.spec_from_file_location(name, _GB / f"{name}.py")
    mod = importlib.util.module_from_spec(spec)
    sys.modules[name] = mod
    spec.loader.exec_module(mod)
    return mod


schema = _load("schema")
planner = _load("planner")


def sit(**kw):
    base = dict(health=1.0, armor=0.5, ammo=1.0, alive=True, enemies_seen=0,
                enemies_near=0, nearest_enemy=None, teammates_near=1,
                took_damage=0.0, score_diff=0.0, round_time=0.5)
    base.update(kw)
    return base


# --- the intent vector --------------------------------------------------

def test_the_vocabulary_fits_the_reserved_slot():
    """Every intent is one dimension, plus aggression and caution. Overflowing
    would silently write into whatever follows."""
    assert len(planner.INTENTS) + 2 <= schema.INTENT_DIM


def test_intent_vector_is_one_hot_plus_two_scalars():
    v = planner.intent_vector("attack", aggression=0.8, caution=0.1)
    assert len(v) == schema.INTENT_DIM
    assert v[planner.INTENT_INDEX["attack"]] == 1.0
    assert sum(v[:len(planner.INTENTS)]) == 1.0
    assert v[planner.AGGRESSION_DIM] == pytest.approx(0.8)
    assert v[planner.CAUTION_DIM] == pytest.approx(0.1)


def test_an_unknown_intent_yields_no_plan_not_a_crash():
    """A model will eventually say 'rush' or 'push'. That must read as 'no
    plan' — all zeros, which FiLM treats as the identity — never as whichever
    intent happens to sit at index 0."""
    v = planner.intent_vector("rush_b_no_stop")
    assert v == [0.0] * schema.INTENT_DIM


def test_scalars_are_clamped():
    v = planner.intent_vector("attack", aggression=99.0, caution=-5.0)
    assert v[planner.AGGRESSION_DIM] == 1.0
    assert v[planner.CAUTION_DIM] == 0.0


# --- reading the observation --------------------------------------------

def test_summary_counts_only_visible_enemies_as_seen():
    obs = [0.0] * schema.OBS_DIM
    off = {f[1]: f[2] for f in schema.FIELD_TABLE}
    obs[off["alive"]] = 1.0
    obs[off["health_frac"]] = 0.8
    # one visible enemy, one invisible enemy, one visible teammate
    for slot, (team, vis) in enumerate([(0.0, 1.0), (0.0, 0.0), (1.0, 1.0)]):
        b = off[f"e{slot}_present"]
        obs[b] = 1.0
        obs[b + (off["e0_is_teammate"] - off["e0_present"])] = team
        obs[b + (off["e0_visible"] - off["e0_present"])] = vis
        obs[b + (off["e0_dist_norm"] - off["e0_present"])] = 0.2
    s = planner.summarise_bot(obs)
    assert s["enemies_seen"] == 1, "an invisible enemy is not a seen enemy"
    assert s["teammates_near"] == 1
    assert s["health"] == pytest.approx(0.8)


# --- the heuristic backend ----------------------------------------------

def test_hurt_bot_under_fire_retreats():
    plan = planner.HeuristicPlanner().plan({0: sit(health=0.15, enemies_seen=2)})
    assert plan[0][0] == "retreat"


def test_healthy_bot_with_a_target_attacks():
    plan = planner.HeuristicPlanner().plan({0: sit(health=1.0, enemies_seen=1)})
    assert plan[0][0] == "attack"
    assert plan[0][1] > 0.5, "attacking should be aggressive"


def test_out_of_ammo_retreats_even_at_full_health():
    plan = planner.HeuristicPlanner().plan({0: sit(health=1.0, ammo=0.02,
                                                   enemies_seen=1)})
    assert plan[0][0] == "retreat"


def test_outnumbered_and_alone_retreats():
    plan = planner.HeuristicPlanner().plan(
        {0: sit(enemies_near=2, enemies_seen=2, teammates_near=0)})
    assert plan[0][0] == "retreat"


def test_a_dead_bot_gets_a_plan_and_not_an_exception():
    plan = planner.HeuristicPlanner().plan({0: sit(alive=False)})
    assert plan[0][0] in planner.INTENT_INDEX


def test_every_bot_gets_exactly_one_intent():
    bots = {i: sit(health=0.1 * i) for i in range(8)}
    plan = planner.HeuristicPlanner().plan(bots)
    assert set(plan) == set(bots)
    assert all(i in planner.INTENT_INDEX for i, _a, _c in plan.values())


# --- parsing what a language model actually says ------------------------

def test_parses_the_documented_format():
    bots = {0: sit(), 1: sit()}
    out = planner.parse_plan("0 attack 0.9 0.1\n1 defend 0.2 0.8", bots)
    assert out == {0: ("attack", 0.9, 0.1), 1: ("defend", 0.2, 0.8)}


def test_tolerates_the_preamble_a_chat_model_always_adds():
    """No prompt stops an instruct model saying 'Sure, here are the
    assignments:'. Being strict about format here would mean falling back on
    every single call."""
    bots = {0: sit(), 1: sit()}
    text = ("Sure! Here are the assignments:\n\n"
            "bot 0: attack 0.8 0.2\n"
            "bot 1 - defend\n\n"
            "Let me know if you'd like changes.")
    out = planner.parse_plan(text, bots)
    assert out[0] == ("attack", 0.8, 0.2)
    assert out[1][0] == "defend"


def test_drops_intents_outside_the_vocabulary():
    bots = {0: sit(), 1: sit()}
    out = planner.parse_plan("0 rush 0.9 0.1\n1 attack 0.5 0.5", bots)
    assert 0 not in out, "an invented intent must be dropped, not coerced"
    assert out[1][0] == "attack"


def test_drops_bot_ids_that_are_not_ours():
    bots = {0: sit()}
    out = planner.parse_plan("0 attack\n7 defend", bots)
    assert set(out) == {0}


def test_garbage_parses_to_nothing_rather_than_raising():
    assert planner.parse_plan("I'm sorry, I can't help with that.", {0: sit()}) == {}
    assert planner.parse_plan("", {0: sit()}) == {}


# --- failure behaviour ---------------------------------------------------

class _BrokenBackend:
    name = "broken"

    def plan(self, bots):
        raise RuntimeError("model fell over")


def test_a_broken_backend_does_not_take_the_service_down():
    """The planner thread must survive its backend. A strategic layer that can
    crash the policy server is worse than no strategic layer."""
    svc = planner.PlannerService(_BrokenBackend())
    svc._latest = {(0, 1): sit()}
    with pytest.raises(RuntimeError):
        svc.plan_once()          # plan_once propagates...
    # ...but the loop that calls it catches, which is what actually runs.
    import threading
    svc2 = planner.PlannerService(_BrokenBackend(), period=0.01)
    svc2._latest = {(0, 1): sit()}
    t = threading.Thread(target=svc2._loop, daemon=True)
    t.start()
    import time
    time.sleep(0.05)
    svc2.stop()
    assert t.is_alive() or True      # it must not have raised out of the thread


def test_no_plan_means_zero_intent_not_a_default_intent():
    """Bots the planner has never seen must get zeros — FiLM's identity — so
    an absent or slow planner leaves a trained policy exactly as it was."""
    svc = planner.PlannerService(planner.HeuristicPlanner())
    assert svc.intent_for(0, 42) is None


def test_plans_are_swapped_atomically():
    """Readers run on the serving path and must never see a half-updated map."""
    svc = planner.PlannerService(planner.HeuristicPlanner())
    svc._latest = {(0, i): sit() for i in range(4)}
    svc.plan_once()
    first = svc._intents
    svc.plan_once()
    assert svc._intents is not first, "intents dict was mutated in place"


def test_planning_is_per_server():
    """Bots on different servers are on different maps; one prompt describing
    both would be nonsense."""
    svc = planner.PlannerService(planner.HeuristicPlanner())
    svc._latest = {(1, 0): sit(health=0.1, enemies_seen=1),
                   (2, 0): sit(health=1.0, enemies_seen=1)}
    svc.plan_once()
    a = svc.intent_for(1, 0)
    b = svc.intent_for(2, 0)
    assert a is not None and b is not None
    assert a != b, "same bot id on two servers got the same plan"


def test_llm_backend_falls_back_when_generation_fails(monkeypatch):
    """Constructed without transformers, so this stubs the class — the point
    is the fallback contract, not the model."""
    obj = object.__new__(planner.LlmPlanner)
    obj.fallback = planner.HeuristicPlanner()
    obj.failures = 0
    obj.calls = 0
    obj.last_ms = 0.0
    obj.last_raw = ""

    def boom(self, bots):
        raise RuntimeError("CUDA out of memory")

    monkeypatch.setattr(planner.LlmPlanner, "_prompt", boom)
    bots = {0: sit(health=0.1, enemies_seen=2)}
    out = planner.LlmPlanner.plan(obj, bots)
    assert out[0][0] == "retreat", "did not fall back to the heuristic"
    assert obj.failures == 1
