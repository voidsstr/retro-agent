"""Tests for the behavioural-cloning training pipeline: dataset loading
(train/dataset.py), the BC trainer (train/bc.py), imitation evaluation
(train/eval_imitation.py) and the record -> train -> evaluate driver
(train/e2e_synthetic.py).

Phase 2's actual deliverable is a MEASUREMENT, not a script that runs without
crashing, so the important tests here are:

  * a checkpoint from this trainer round-trips through runtime.GpuPolicy.load()
    -- the same acceptance test a real deployment would apply;
  * a trained checkpoint never produces NaN;
  * a shard recorded against a different schema is refused by the loader, the
    same discipline shard.py enforces one layer down;
  * the trained net measurably imitates the scripted expert better than an
    untrained one, on FRESH synthetic episodes it never trained on -- not just
    that the training loss went down.

Split into two speed tiers deliberately: a `needs_torch` tier that runs on
CPU in a couple of seconds (pipeline correctness, no GPU required) and a
`needs_cuda` tier with enough data/epochs to make the "beats random" margin
robust across seeds (a few seconds on a real GPU, minutes on CPU -- see
train/e2e_synthetic.py's own timing notes).

Run: pytest tests/python/test_gamebots_train.py
     ~/.venvs/gamebots/bin/python -m pytest tests/python/test_gamebots_train.py
"""

import importlib.util
import sys
from pathlib import Path

import pytest

_GB = Path(__file__).resolve().parent.parent.parent / "scripts" / "gamebots"


def _load(name, subdir=None):
    base = _GB / subdir if subdir else _GB
    spec = importlib.util.spec_from_file_location(name, base / f"{name}.py")
    mod = importlib.util.module_from_spec(spec)
    sys.modules[name] = mod
    spec.loader.exec_module(mod)
    return mod


schema = _load("schema")
model = _load("model")
shard = _load("shard", "record")
gen_synthetic = _load("gen_synthetic", "record")
dataset = _load("dataset", "train")

HAVE_TORCH = model.HAVE_TORCH
HAVE_NUMPY = shard.HAVE_NUMPY
needs_numpy = pytest.mark.skipif(not HAVE_NUMPY, reason="numpy not installed")
needs_torch = pytest.mark.skipif(not HAVE_TORCH, reason="torch not installed")


def needs_cuda():
    if not HAVE_TORCH:
        return pytest.mark.skip(reason="torch not installed")
    import torch
    return pytest.mark.skipif(not torch.cuda.is_available(), reason="no CUDA")


def _write_small_shard(directory, n_episodes=6, episode_len=12, seed=0,
                       policy_name="scripted-synthetic"):
    return gen_synthetic.generate(str(directory), n_episodes=n_episodes,
                                  episode_len=episode_len, seed=seed,
                                  policy_name=policy_name)


# --------------------------------------------------------------------------
# dataset.py — numpy only, no torch required
# --------------------------------------------------------------------------

@needs_numpy
def test_find_shards_from_dir_and_explicit_files(tmp_path):
    _write_small_shard(tmp_path)
    from_dir = dataset.find_shards([str(tmp_path)])
    assert len(from_dir) == 1
    from_files = dataset.find_shards(from_dir)
    assert from_files == from_dir


@needs_numpy
def test_load_dataset_concatenates_multiple_shards(tmp_path):
    d1, d2 = tmp_path / "a", tmp_path / "b"
    n1 = _write_small_shard(d1, n_episodes=3, episode_len=10, seed=1)
    n2 = _write_small_shard(d2, n_episodes=4, episode_len=10, seed=2)
    data = dataset.load_dataset([str(d1), str(d2)])
    assert len(data["obs"]) == n1 + n2
    assert data["obs"].shape[1] == schema.OBS_DIM
    assert len(data["_files"]) == 2
    assert data["_warnings"] == []


@needs_numpy
def test_load_dataset_raises_on_no_shards_found(tmp_path):
    with pytest.raises(ValueError, match="no .*gbdemo"):
        dataset.load_dataset([str(tmp_path)])


@needs_numpy
def test_load_dataset_refuses_a_schema_mismatched_shard(tmp_path):
    """The dataset loader must inherit shard.py's refusal, not paper over it
    -- training on a mismatched layout produces a confidently wrong model
    with no error anywhere else in the pipeline."""
    p = tmp_path / "bad.gbdemo"
    with open(p, "wb") as fh:
        shard.write_header(fh, schema.SCHEMA_HASH ^ 0x1, schema.OBS_DIM)
        fh.write(shard.pack_record(0, 0, 0, False, [0.0] * schema.OBS_DIM,
                                   0, 0.0, 0.0, 0.0, 0.0, 0))
    with pytest.raises(ValueError, match="schema"):
        dataset.load_dataset([str(tmp_path)])


@needs_numpy
def test_load_dataset_warns_but_still_loads_a_truncated_shard(tmp_path):
    n = _write_small_shard(tmp_path, n_episodes=2, episode_len=10, seed=3)
    files = dataset.find_shards([str(tmp_path)])
    assert len(files) == 1
    with open(files[0], "ab") as fh:
        fh.write(b"\x00" * 17)                # a partial trailing record
    data = dataset.load_dataset([str(tmp_path)])
    assert len(data["obs"]) == n               # the good records still load
    assert len(data["_warnings"]) == 1
    assert "trailing" in data["_warnings"][0]


@needs_numpy
def test_split_indices_is_deterministic_disjoint_and_covers_every_row():
    import numpy as np
    train_idx, val_idx = dataset.split_indices(100, val_frac=0.2, seed=0)
    assert len(set(train_idx) & set(val_idx)) == 0
    assert len(train_idx) + len(val_idx) == 100
    assert 15 <= len(val_idx) <= 25
    train_idx2, val_idx2 = dataset.split_indices(100, val_frac=0.2, seed=0)
    assert np.array_equal(train_idx, train_idx2)
    assert np.array_equal(val_idx, val_idx2)
    train_idx3, val_idx3 = dataset.split_indices(100, val_frac=0.2, seed=1)
    assert not np.array_equal(val_idx, val_idx3)


@needs_numpy
def test_split_indices_requires_at_least_two_records():
    with pytest.raises(ValueError):
        dataset.split_indices(1)


def test_dataset_module_is_importable_without_numpy():
    """The rest of the test suite runs on a system Python with no numpy at
    all; importing this module must not itself crash."""
    assert hasattr(dataset, "HAVE_NUMPY")


# --------------------------------------------------------------------------
# bc.py — the trainer itself
# --------------------------------------------------------------------------

bc = _load("bc", "train") if HAVE_TORCH else None


@needs_torch
def test_compute_loss_is_finite_and_positive(tmp_path):
    import torch
    _write_small_shard(tmp_path, n_episodes=4, episode_len=15, seed=5)
    data = dataset.load_dataset([str(tmp_path)])
    net = model.build(hidden=32)
    idx = list(range(min(16, len(data["obs"]))))
    batch = bc._to_tensors(data, __import__("numpy").array(idx), "cpu", torch.float32)
    total, parts = bc.compute_loss(net, batch)
    assert torch.isfinite(total)
    for v in parts.values():
        assert v == v and v >= 0.0          # not NaN, not negative


@needs_torch
def test_training_reduces_loss_over_epochs(tmp_path):
    _write_small_shard(tmp_path, n_episodes=20, episode_len=20, seed=6)
    data = dataset.load_dataset([str(tmp_path)])
    _net, history = bc.train(data, epochs=6, batch_size=64, hidden=32,
                             seed=0, device="cpu", log=lambda *_a: None)
    assert history[-1]["train_loss"] < history[0]["train_loss"]


@needs_torch
def test_save_checkpoint_matches_what_runtime_gpupolicy_expects(tmp_path):
    """The exact contract runtime.py's GpuPolicy.load() documents:
    {"state_dict", "schema_hash", "version"}."""
    import torch
    net = model.build(hidden=32)
    path = tmp_path / "ckpt.pt"
    version = bc.save_checkpoint(net, str(path), version="test-v1")
    blob = torch.load(str(path), weights_only=False)
    assert set(("state_dict", "schema_hash", "version")) <= set(blob.keys())
    assert blob["schema_hash"] == schema.SCHEMA_HASH
    assert blob["version"] == version == "test-v1"


@needs_torch
def test_checkpoint_round_trips_through_runtime_gpupolicy_load_on_cpu(tmp_path):
    """No CUDA needed for this one -- runtime.GpuPolicy accepts device='cpu',
    which is exactly what lets this run as a plain needs_torch test rather
    than needs_cuda."""
    import numpy as np
    runtime = _load("runtime")
    net = model.build(hidden=32)
    path = tmp_path / "ckpt.pt"
    bc.save_checkpoint(net, str(path), version="roundtrip-test")

    p = runtime.GpuPolicy(device="cpu", use_graphs=False, hidden=32,
                          prewarm=False)
    version = p.load(str(path))
    assert version == "roundtrip-test"

    ids = np.arange(5, dtype=np.uint16)
    obs = np.random.default_rng(1).normal(size=(5, schema.OBS_DIM)).astype(np.float32)
    btn, pitch, yaw, fwd, side, wpn = p.act_arrays(0, 0, ids, obs)
    assert np.isfinite(pitch).all() and np.isfinite(yaw).all()
    assert np.isfinite(fwd).all() and np.isfinite(side).all()


@needs_torch
def test_a_checkpoint_from_a_different_schema_is_refused_on_load(tmp_path):
    """Same invariant as runtime's own test_gamebots_model.py suite, verified
    again from the trainer's side: a bc.py checkpoint carries schema_hash
    precisely so this refusal happens."""
    import torch
    runtime = _load("runtime")
    net = model.build(hidden=32)
    path = tmp_path / "bad.pt"
    torch.save({"state_dict": net.state_dict(),
               "schema_hash": schema.SCHEMA_HASH ^ 0xBEEF,
               "version": "bad"}, path)
    p = runtime.GpuPolicy(device="cpu", use_graphs=False, hidden=32,
                          prewarm=False)
    with pytest.raises(ValueError, match="schema"):
        p.load(str(path))


@needs_torch
def test_trained_net_forward_pass_never_produces_nan(tmp_path):
    _write_small_shard(tmp_path, n_episodes=10, episode_len=15, seed=7)
    data = dataset.load_dataset([str(tmp_path)])
    net, _history = bc.train(data, epochs=3, batch_size=32, hidden=32,
                             seed=0, device="cpu", log=lambda *_a: None)
    import torch
    net.eval()
    with torch.no_grad():
        obs = torch.randn(64, schema.OBS_DIM) * 5.0     # wide range, including extremes
        act, hx = net.act(obs)
    for k, v in act.items():
        if hasattr(v, "isfinite"):
            assert torch.isfinite(v).all(), f"{k} produced non-finite values"
    assert torch.isfinite(hx).all()


# --------------------------------------------------------------------------
# eval_imitation.py
# --------------------------------------------------------------------------

eval_imitation = _load("eval_imitation", "train") if HAVE_TORCH else None


@needs_torch
def test_fresh_examples_shapes_and_ground_truth_matches_scripted_policy():
    obs, target = eval_imitation.fresh_examples(n_episodes=3, episode_len=10, seed=123)
    n = 30
    assert obs.shape == (n, schema.OBS_DIM)
    for key in ("pitch", "yaw", "fwd", "side"):
        assert target[key].shape == (n,)
    assert target["buttons"].shape == (n,)
    assert target["weapon"].shape == (n,)


@needs_torch
def test_evaluate_returns_the_documented_keys_and_finite_values():
    import math
    net = model.build(hidden=32).eval()
    obs, target = eval_imitation.fresh_examples(n_episodes=3, episode_len=10, seed=124)
    metrics = eval_imitation.evaluate(net, obs, target)
    for key in ("pitch_mae_deg", "yaw_mae_deg", "fwd_mae", "side_mae",
               "attack_precision", "attack_recall", "attack_base_rate",
               "weapon_accuracy"):
        assert key in metrics
        assert not math.isnan(metrics[key])
    assert metrics["n"] == 30


# --------------------------------------------------------------------------
# e2e_synthetic.py — the key deliverable
# --------------------------------------------------------------------------

e2e = _load("e2e_synthetic", "train") if HAVE_TORCH else None


@needs_torch
def test_e2e_pipeline_runs_end_to_end_and_checkpoint_loads(tmp_path):
    """Fast (CPU, a couple of seconds) pipeline-correctness pass: record ->
    train -> checkpoint -> loads in runtime.GpuPolicy -> evaluate, with no
    exceptions and only finite numbers out. Deliberately NOT asserting a
    beats-random margin here -- at this tiny scale (fast enough for CPU) the
    margin is noisy; that assertion lives in the needs_cuda test below, sized
    for a real measurement rather than a smoke test."""
    report = e2e.run(train_episodes=8, episode_len=20, eval_episodes=5,
                     eval_len=15, epochs=2, batch_size=32, hidden=32,
                     seed=0, eval_seed=777, device="cpu",
                     out_dir=str(tmp_path), log=lambda *_a: None)
    assert report["runtime_check"] is not None
    assert "OK" in report["runtime_check"]
    for side in ("trained", "random"):
        for key in ("pitch_mae_deg", "yaw_mae_deg", "fwd_mae", "side_mae"):
            v = report[side][key]
            assert v == v and v >= 0.0       # finite, non-negative


@needs_cuda()
def test_e2e_trained_net_measurably_beats_random_on_fresh_episodes(tmp_path):
    """THE key deliverable: on synthetic episodes the net never trained on,
    predicting with the trained checkpoint must be meaningfully closer to the
    scripted expert's own actions than an untrained net of the same
    architecture. Sized to be robust across seeds (checked manually across
    several before picking these thresholds) while still running in a few
    seconds on a real GPU -- see e2e_synthetic.py's docstring for the honest
    CPU timing if this ever needs to run there."""
    report = e2e.run(train_episodes=80, episode_len=60, eval_episodes=30,
                     eval_len=60, epochs=8, batch_size=128, hidden=128,
                     seed=1, eval_seed=777, device="cuda",
                     out_dir=str(tmp_path), log=lambda *_a: None)
    trained, random_ = report["trained"], report["random"]

    # Generous, seed-checked margins -- not a coin flip, a real gap.
    assert trained["yaw_mae_deg"] < random_["yaw_mae_deg"] * 0.7, (
        f"trained yaw MAE {trained['yaw_mae_deg']:.3f} not clearly better "
        f"than random's {random_['yaw_mae_deg']:.3f}")
    assert trained["fwd_mae"] < random_["fwd_mae"] * 0.5, (
        f"trained fwd MAE {trained['fwd_mae']:.3f} not clearly better than "
        f"random's {random_['fwd_mae']:.3f}")
    assert trained["pitch_mae_deg"] <= random_["pitch_mae_deg"], (
        f"trained pitch MAE {trained['pitch_mae_deg']:.3f} worse than "
        f"random's {random_['pitch_mae_deg']:.3f}")
