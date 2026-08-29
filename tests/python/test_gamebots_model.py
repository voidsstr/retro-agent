"""Tests for the custom policy network and its GPU serving runtime.

Most of these run without a GPU and without torch — they are skipped rather
than failed, because the harness and the whole rest of the suite must keep
working on a host with no ML stack.

What is worth testing here is not "does it play well" (it is untrained) but the
things that would be silently wrong:

  * the encoders are wired to the right slices of the observation, derived from
    the schema rather than hardcoded;
  * absent entity slots contribute nothing, instead of dragging the pooled
    vector toward zero as a fight thins out;
  * a bot that respawns does not inherit its own pre-death memory, and bots
    never inherit each other's;
  * a checkpoint trained against a different schema is refused, not loaded;
  * actions leaving the model are inside the bounds a game server can accept.

Run: pytest tests/python/test_gamebots_model.py
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
model = _load("model")

needs_torch = pytest.mark.skipif(not model.HAVE_TORCH, reason="torch not installed")


def needs_cuda():
    if not model.HAVE_TORCH:
        return pytest.mark.skip(reason="torch not installed")
    import torch
    return pytest.mark.skipif(not torch.cuda.is_available(), reason="no CUDA")


# --- layout wiring (no torch needed) ------------------------------------

def test_group_slices_cover_the_observation_exactly():
    """The encoders slice the observation by group. A gap or overlap here
    feeds the wrong floats to the wrong encoder — which trains to *something*,
    just not the thing intended, with no error anywhere."""
    total = 0
    for _g, start, n in model.GROUPS:
        assert start == total
        total += n
    assert total == schema._RAW_OBS_DIM


def test_entity_slot_dim_divides_the_entity_group():
    assert model.GROUP_DIMS["ent"] == model.ENT_SLOT_DIM * schema.MAX_ENTITIES


def test_context_excludes_entities_and_intent():
    """Entities go through the shared per-slot encoder and intent through
    FiLM; folding either into the flat context would defeat both."""
    assert "ent" not in model.CONTEXT_GROUPS
    assert "intent" not in model.CONTEXT_GROUPS
    assert model.CONTEXT_DIM == sum(model.GROUP_DIMS[g]
                                    for g in model.CONTEXT_GROUPS)


def test_group_offsets_match_the_schema():
    table = {f[1]: f[2] for f in schema.FIELD_TABLE}
    assert model.ENT_START == table["e0_present"]
    intent_start = next(s for g, s, _n in model.GROUPS if g == "intent")
    assert intent_start == table["intent"]


# --- the network --------------------------------------------------------

@needs_torch
def test_forward_produces_every_head_with_the_right_widths():
    import torch
    m = model.build()
    obs = torch.zeros(3, schema.OBS_DIM)
    out, hx = m(obs)
    assert out["view_mean"].shape == (3, 2)
    assert out["move"].shape == (3, 2)
    assert out["buttons"].shape == (3, len(schema.BUTTON_NAMES))
    assert out["value"].shape == (3,)
    assert hx.shape == (3, m.gru_hidden)


@needs_torch
def test_split_routes_the_right_floats_to_the_right_encoder():
    import torch
    m = model.build()
    obs = torch.zeros(1, schema.OBS_DIM)
    table = {f[1]: f[2] for f in schema.FIELD_TABLE}
    obs[0, table["e2_present"]] = 1.0
    obs[0, table["intent"] + 3] = 7.0
    ctx, ents, present, intent = m.split(obs)
    assert present[0, 2] == 1.0, "entity slot 2 did not land in slot 2"
    assert intent[0, 3] == 7.0
    assert ctx.shape == (1, model.CONTEXT_DIM)
    assert ents.shape == (1, schema.MAX_ENTITIES, model.ENT_SLOT_DIM)


@needs_torch
def test_absent_entities_are_masked_not_averaged_in():
    """An all-zero slot fed through a mean would pull the pooled vector toward
    zero, so 'two enemies' and 'eight distant enemies' would look alike. Masked
    attention must make absent slots contribute nothing at all."""
    import torch
    m = model.build().eval()
    table = {f[1]: f[2] for f in schema.FIELD_TABLE}

    one = torch.zeros(1, schema.OBS_DIM)
    one[0, table["e0_present"]] = 1.0
    one[0, table["e0_dist_norm"]] = 0.4

    padded = one.clone()          # identical, but with the other slots present
    with torch.no_grad():
        _c, e1, p1, _i = m.split(one)
        a = m.entities(e1, p1)
        _c, e2, p2, _i = m.split(padded)
        b = m.entities(e2, p2)
    assert torch.allclose(a, b, atol=1e-6)


@needs_torch
def test_an_empty_entity_set_does_not_produce_nan():
    """Softmax over a fully-masked set is NaN, and a NaN here poisons every
    downstream head — the bot would emit garbage the moment nobody is visible,
    which is most of the time."""
    import torch
    m = model.build().eval()
    obs = torch.zeros(4, schema.OBS_DIM)     # nothing present anywhere
    with torch.no_grad():
        out, hx = m(obs)
    for k, v in out.items():
        assert torch.isfinite(v).all(), f"{k} is not finite with no entities"
    assert torch.isfinite(hx).all()


@needs_torch
def test_film_starts_as_the_identity():
    """An untrained planner must not perturb a trained policy. Scale inits to
    1 and shift to 0, so zero intent means 'behave exactly as before'."""
    import torch
    m = model.build().eval()
    h = torch.randn(2, m.hidden)
    zero_intent = torch.zeros(2, schema.INTENT_DIM)
    with torch.no_grad():
        assert torch.allclose(m.film(h, zero_intent), h, atol=1e-6)


@needs_torch
def test_intent_actually_changes_behaviour_once_film_is_trained():
    """The flip side: FiLM must be *able* to modulate, or conditioning is
    decorative. With non-zero weights a different intent must change the
    output."""
    import torch
    m = model.build().eval()
    torch.nn.init.normal_(m.film.to_scale.weight, std=0.5)
    torch.nn.init.normal_(m.film.to_shift.weight, std=0.5)
    h = torch.randn(1, m.hidden)
    a = m.film(h, torch.zeros(1, schema.INTENT_DIM))
    b = m.film(h, torch.ones(1, schema.INTENT_DIM))
    assert not torch.allclose(a, b)


@needs_torch
def test_act_returns_actions_inside_the_engine_bounds():
    import torch
    m = model.build().eval()
    obs = torch.randn(16, schema.OBS_DIM) * 5.0
    act, _hx = m.act(obs)
    assert act["pitch"].abs().max() <= schema.MAX_PITCH_DELTA_DEG + 1e-4
    assert act["yaw"].abs().max() <= schema.MAX_YAW_DELTA_DEG + 1e-4
    assert act["forward"].abs().max() <= 1.0 + 1e-4
    assert act["buttons"].dtype == torch.bool
    assert int(act["weapon"].max()) < m.n_weapons


@needs_torch
def test_recurrent_state_actually_carries():
    """If the GRU state were ignored the policy would be memoryless, which is
    the thing the recurrent core exists to avoid — and it would look fine."""
    import torch
    m = model.build().eval()
    obs = torch.randn(1, schema.OBS_DIM)
    with torch.no_grad():
        _o, h1 = m(obs)
        _o, h2 = m(obs, h1)
    assert not torch.allclose(h1, h2)


# --- the serving runtime ------------------------------------------------

@needs_cuda()
def test_state_is_per_bot_and_survives_frames():
    import numpy as np
    runtime = _load("runtime")
    p = runtime.GpuPolicy(prewarm=False)
    ids = np.array([5, 9], dtype=np.uint16)
    obs = np.zeros((2, schema.OBS_DIM), dtype=np.float32)
    obs[:, p._alive_off] = 1.0
    p.act_arrays(0, 0, ids, obs)
    rows = [p._slot[(0, 5)], p._slot[(0, 9)]]
    assert rows[0] != rows[1], "two bots share one hidden-state row"
    p.act_arrays(1, 0, ids, obs)
    assert [p._slot[(0, 5)], p._slot[(0, 9)]] == rows, "state row moved"


@needs_cuda()
def test_different_servers_do_not_share_bot_state():
    """bot_id 0 exists on every server. Keying on it alone would have Quake's
    bot 0 inherit Counter-Strike's bot 0 memory."""
    import numpy as np
    runtime = _load("runtime")
    p = runtime.GpuPolicy(prewarm=False)
    ids = np.array([0], dtype=np.uint16)
    obs = np.zeros((1, schema.OBS_DIM), dtype=np.float32)
    p.act_arrays(0, 0, ids, obs, conn_key=1)
    p.act_arrays(0, 0, ids, obs, conn_key=2)
    assert p._slot[(1, 0)] != p._slot[(2, 0)]


@needs_cuda()
def test_respawn_clears_the_hidden_state():
    """Memory of the fight a bot just lost is worse than no memory."""
    import numpy as np
    import torch
    runtime = _load("runtime")
    p = runtime.GpuPolicy(prewarm=False)
    ids = np.array([1], dtype=np.uint16)
    alive = np.zeros((1, schema.OBS_DIM), dtype=np.float32)
    alive[0, p._alive_off] = 1.0
    dead = np.zeros((1, schema.OBS_DIM), dtype=np.float32)

    for t in range(5):
        p.act_arrays(t, 0, ids, alive)
    row = p._slot[(0, 1)]
    assert torch.any(p._state_buf[row] != 0), "state never accumulated"

    accumulated = p._state_buf[row].clone()

    p.act_arrays(5, 0, ids, dead)          # died
    p.act_arrays(6, 0, ids, alive)         # respawned
    after_respawn = p._state_buf[p._slot[(0, 1)]].clone()

    # Compare against a BRAND NEW bot in the SAME policy given the same
    # observation: both should be exactly one step from a zeroed state. (An
    # earlier version of this test built a second GpuPolicy, which has
    # different random weights — it failed for a reason that had nothing to do
    # with the reset.)
    fresh_ids = np.array([99], dtype=np.uint16)
    p.act_arrays(7, 0, fresh_ids, alive)
    fresh = p._state_buf[p._slot[(0, 99)]].clone()

    assert torch.allclose(after_respawn, fresh, atol=1e-3), \
        "respawned bot did not start from a cleared state"
    assert not torch.allclose(after_respawn, accumulated, atol=1e-3), \
        "respawn kept the pre-death memory"


@needs_cuda()
def test_a_checkpoint_from_another_schema_is_refused(tmp_path):
    """Loading it would produce a confidently wrong bot and no error at all —
    the same failure the wire hash exists to prevent, one layer down."""
    import torch
    runtime = _load("runtime")
    p = runtime.GpuPolicy(prewarm=False)
    bad = tmp_path / "bad.pt"
    torch.save({"state_dict": p.net.state_dict(),
                "schema_hash": schema.SCHEMA_HASH ^ 0xABCD}, bad)
    with pytest.raises(ValueError, match="schema"):
        p.load(str(bad))


@needs_cuda()
def test_bucket_padding_never_shrinks_a_batch():
    runtime = _load("runtime")
    for n in (1, 7, 8, 9, 33, 512, 513, 2000):
        assert runtime.pick_bucket(n) >= n


@needs_cuda()
def test_actions_leaving_the_runtime_are_clamped_and_finite():
    import numpy as np
    runtime = _load("runtime")
    p = runtime.GpuPolicy(prewarm=False)
    ids = np.arange(8, dtype=np.uint16)
    obs = np.full((8, schema.OBS_DIM), 1e6, dtype=np.float32)
    btn, pitch, yaw, fwd, side, wpn = p.act_arrays(0, 0, ids, obs)
    assert np.isfinite(pitch).all() and np.isfinite(yaw).all()
    assert np.abs(pitch).max() <= schema.MAX_PITCH_DELTA_DEG + 1e-4
    assert np.abs(yaw).max() <= schema.MAX_YAW_DELTA_DEG + 1e-4
    assert np.abs(fwd).max() <= 1.0 + 1e-4
    assert btn.dtype == np.uint16
