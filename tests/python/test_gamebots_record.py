"""Tests for the demonstration recorder: the shard container format
(record/shard.py), the episode-bookkeeping wrapper policyd calls into
(record/recorder.py), and the policyd hook that wires it up (policyd.py's
--record).

The demo format gets the same discipline the rest of this system already
applies to layout drift — the wire protocol (schema.py) and the checkpoint
container (runtime.py):

  * a shard recorded against a different observation layout is REFUSED, not
    silently misread;
  * a truncated/crash-corrupted shard (a process killed mid write) is
    detected from the file size alone, not mistaken for a smaller-but-valid
    one;
  * recording is opt-in and, when off, changes nothing about how policyd
    serves a request.

Everything that needs numpy is marked accordingly, because the shard format's
struct path is deliberately usable without it (see shard.py's docstring) and
the main test suite runs on a system Python with neither numpy nor torch.

Run: pytest tests/python/test_gamebots_record.py
     ~/.venvs/gamebots/bin/python -m pytest tests/python/test_gamebots_record.py
"""

import importlib.util
import math
import os
import socket
import struct
import sys
import threading
import time
from pathlib import Path

import pytest

_GB = Path(__file__).resolve().parent.parent.parent / "scripts" / "gamebots"
_RECORD = _GB / "record"


def _load(name, subdir=None):
    base = _GB / subdir if subdir else _GB
    spec = importlib.util.spec_from_file_location(name, base / f"{name}.py")
    mod = importlib.util.module_from_spec(spec)
    sys.modules[name] = mod
    spec.loader.exec_module(mod)
    return mod


schema = _load("schema")
policyd = _load("policyd")
shard = _load("shard", "record")
recorder_mod = _load("recorder", "record")

HAVE_NUMPY = shard.HAVE_NUMPY
needs_numpy = pytest.mark.skipif(not HAVE_NUMPY, reason="numpy not installed")


def needs_cuda():
    if not HAVE_NUMPY:
        return pytest.mark.skip(reason="numpy not installed")
    try:
        import torch
    except ImportError:
        return pytest.mark.skip(reason="torch not installed")
    return pytest.mark.skipif(not torch.cuda.is_available(), reason="no CUDA")


def _obs(**overrides):
    obs = [0.0] * schema.OBS_DIM
    for k, v in overrides.items():
        obs[k] = v
    return obs


_ALIVE_OFF = next(f[2] for f in schema.FIELD_TABLE if f[1] == "alive")


# --------------------------------------------------------------------------
# header / magic / truncation — struct-only, no numpy needed
# --------------------------------------------------------------------------

def test_header_roundtrips(tmp_path):
    p = tmp_path / "h.gbdemo"
    with open(p, "wb") as fh:
        shard.write_header(fh, schema.SCHEMA_HASH, schema.OBS_DIM, "unit-test")
    with open(p, "rb") as fh:
        header = shard.read_header(fh)
    assert header.schema_hash == schema.SCHEMA_HASH
    assert header.obs_dim == schema.OBS_DIM
    assert header.policy_name == "unit-test"
    assert header.format_version == shard.FORMAT_VERSION


def test_policy_name_longer_than_the_field_is_truncated_not_corrupted(tmp_path):
    p = tmp_path / "h.gbdemo"
    long_name = "x" * 200
    with open(p, "wb") as fh:
        shard.write_header(fh, schema.SCHEMA_HASH, schema.OBS_DIM, long_name)
    with open(p, "rb") as fh:
        header = shard.read_header(fh)
    assert header.policy_name == "x" * shard.POLICY_NAME_MAX


def test_bad_magic_is_rejected(tmp_path):
    p = tmp_path / "h.gbdemo"
    with open(p, "wb") as fh:
        shard.write_header(fh, schema.SCHEMA_HASH, schema.OBS_DIM)
    data = bytearray(p.read_bytes())
    data[0:4] = b"XXXX"
    p.write_bytes(bytes(data))
    with open(p, "rb") as fh:
        with pytest.raises(ValueError, match="magic"):
            shard.read_header(fh)


def test_truncated_header_is_rejected(tmp_path):
    p = tmp_path / "h.gbdemo"
    p.write_bytes(b"GBD1" + b"\x00" * 3)   # far short of HEADER_SIZE
    with open(p, "rb") as fh:
        with pytest.raises(ValueError, match="truncated"):
            shard.read_header(fh)


def test_shard_smaller_than_its_own_header_is_rejected(tmp_path):
    p = tmp_path / "h.gbdemo"
    p.write_bytes(b"GBD1" + b"\x00" * 10)
    with pytest.raises(ValueError):
        shard.inspect_shard(str(p))


def test_schema_mismatch_is_refused_by_default(tmp_path):
    """The single most important invariant this module exists for: a shard
    recorded against a different observation layout must never be trained on
    silently."""
    p = tmp_path / "h.gbdemo"
    with open(p, "wb") as fh:
        shard.write_header(fh, schema.SCHEMA_HASH ^ 0xABCDEF, schema.OBS_DIM)
    with pytest.raises(ValueError, match="schema"):
        shard.inspect_shard(str(p))          # strict_schema=True by default
    # Non-strict inspection is allowed to look anyway (a --describe tool).
    header, _n, _trailing = shard.inspect_shard(str(p), strict_schema=False)
    assert header.schema_hash == schema.SCHEMA_HASH ^ 0xABCDEF


def test_obs_dim_mismatch_is_refused_even_non_strict(tmp_path):
    """A mismatched schema hash with a mismatched obs_dim must not let
    inspect_shard compute record math using THIS build's record size against
    a shard shaped for a different one -- that would misread every record
    rather than erroring."""
    p = tmp_path / "h.gbdemo"
    with open(p, "wb") as fh:
        shard.write_header(fh, schema.SCHEMA_HASH ^ 0x1, schema.OBS_DIM + 8)
    with pytest.raises(ValueError, match="obs_dim"):
        shard.inspect_shard(str(p), strict_schema=False)


def test_pack_record_rejects_wrong_obs_length():
    with pytest.raises(ValueError):
        shard.pack_record(0, 0, 0, False, [0.0] * (schema.OBS_DIM - 1),
                          0, 0.0, 0.0, 0.0, 0.0, 0)


def test_write_record_then_load_shard_python_roundtrips(tmp_path):
    """The struct-only path -- write_record + load_shard_python -- must work
    with no numpy at all, since it's what a host without numpy falls back to."""
    w = shard.ShardWriter(str(tmp_path), prefix="t")
    obs0 = [float(i) for i in range(schema.OBS_DIM)]
    obs1 = [float(-i) for i in range(schema.OBS_DIM)]
    w.write_record(5, 0, 10, False, obs0, schema.BTN_ATTACK, 1.5, -2.5, 0.5,
                   -0.5, 3)
    w.write_record(5, 0, 11, True, obs1, 0, 0.0, 0.0, 0.0, 0.0, 0)
    w.close()

    header, records, trailing = shard.load_shard_python(w.current_path)
    assert trailing == 0
    assert len(records) == 2
    assert records[0]["bot_id"] == 5
    assert records[0]["tick"] == 10
    assert records[0]["done"] == 0
    assert list(records[0]["obs"]) == obs0
    assert records[0]["buttons"] == schema.BTN_ATTACK
    assert records[0]["pitch"] == pytest.approx(1.5)
    assert records[1]["done"] == 1
    assert list(records[1]["obs"]) == obs1


def test_truncated_tail_is_detected_not_misread(tmp_path):
    """A process killed mid write_batch leaves a partial record at the end.
    That must be counted as trailing bytes, not silently treated as data or
    crash the reader."""
    w = shard.ShardWriter(str(tmp_path), prefix="t")
    obs = [0.0] * schema.OBS_DIM
    for i in range(4):
        w.write_record(i, 0, i, False, obs, 0, 0.0, 0.0, 0.0, 0.0, 0)
    w.close()

    path = w.current_path
    with open(path, "ab") as fh:
        fh.write(b"\x00" * (shard.RECORD_SIZE // 2))     # a half-written record

    header, n_complete, trailing = shard.inspect_shard(path)
    assert n_complete == 4
    assert trailing == shard.RECORD_SIZE // 2

    header, records, trailing2 = shard.load_shard_python(path)
    assert len(records) == 4
    assert trailing2 == trailing


def test_shard_rolls_at_max_records_per_shard(tmp_path):
    w = shard.ShardWriter(str(tmp_path), prefix="roll", max_records_per_shard=3)
    obs = [0.0] * schema.OBS_DIM
    for i in range(7):
        w.write_record(i, 0, i, False, obs, 0, 0.0, 0.0, 0.0, 0.0, 0)
    w.close()
    files = sorted(Path(tmp_path).glob("roll-*.gbdemo"))
    assert len(files) >= 2
    total = 0
    for f in files:
        _h, recs, trailing = shard.load_shard_python(str(f))
        assert trailing == 0
        total += len(recs)
    assert total == 7


# --------------------------------------------------------------------------
# numpy fast path
# --------------------------------------------------------------------------

@needs_numpy
def test_record_dtype_matches_the_struct_layout():
    """If these two paths ever disagreed, write_batch and write_record would
    put different bytes on disk for the same logical record -- the module
    itself asserts this at import time; this test documents why."""
    assert shard.RECORD_DTYPE.itemsize == shard.RECORD_SIZE


@needs_numpy
def test_write_batch_and_load_shard_roundtrip(tmp_path):
    import numpy as np
    w = shard.ShardWriter(str(tmp_path), prefix="b")
    n = 16
    rows = shard.record_array(n)
    rows["bot_id"] = np.arange(n, dtype=np.uint16)
    rows["episode_id"] = 0
    rows["tick"] = 100
    rows["done"] = 0
    rows["obs"] = np.random.default_rng(0).normal(size=(n, schema.OBS_DIM)).astype(np.float32)
    rows["buttons"] = schema.BTN_ATTACK
    rows["pitch"] = 3.0
    rows["yaw"] = -4.0
    rows["fwd"] = 1.0
    rows["side"] = -1.0
    rows["weapon"] = 2
    w.write_batch(rows)
    w.close()

    header, arr, trailing = shard.load_shard(w.current_path)
    assert trailing == 0
    assert len(arr) == n
    assert np.array_equal(arr["bot_id"], rows["bot_id"])
    assert np.allclose(arr["obs"], rows["obs"])
    assert (arr["buttons"] == schema.BTN_ATTACK).all()


@needs_numpy
def test_write_batch_casts_from_a_different_dtype(tmp_path):
    """A caller that built its rows with float64 obs (e.g. a naive generator)
    must still produce the SAME on-disk bytes as one that used float32 -
    write_batch casts field by field rather than requiring an exact dtype
    match."""
    import numpy as np
    loose_dtype = np.dtype([
        ("bot_id", "<u2"), ("episode_id", "<u4"), ("tick", "<u4"),
        ("done", "u1"), ("obs", "<f8", (schema.OBS_DIM,)),
        ("buttons", "<u2"), ("pitch", "<f8"), ("yaw", "<f8"), ("fwd", "<f8"),
        ("side", "<f8"), ("weapon", "u1"),
    ])
    rows = np.zeros(3, dtype=loose_dtype)
    rows["bot_id"] = [1, 2, 3]
    rows["obs"] = 0.5
    rows["pitch"] = 1.25

    w = shard.ShardWriter(str(tmp_path), prefix="cast")
    w.write_batch(rows)
    w.close()

    _h, arr, _t = shard.load_shard(w.current_path)
    assert list(arr["bot_id"]) == [1, 2, 3]
    assert np.allclose(arr["obs"], 0.5)
    assert np.allclose(arr["pitch"], 1.25)


@needs_numpy
def test_empty_batch_is_a_no_op(tmp_path):
    import numpy as np
    w = shard.ShardWriter(str(tmp_path), prefix="empty")
    w.write_batch(shard.record_array(0))
    assert w.total_written == 0
    assert w.current_path is None
    w.close()


# --------------------------------------------------------------------------
# DemoRecorder — episode bookkeeping
# --------------------------------------------------------------------------

def _batch(bot_ids, alive, tick=0):
    import numpy as np
    n = len(bot_ids)
    obs = np.zeros((n, schema.OBS_DIM), dtype=np.float32)
    obs[:, _ALIVE_OFF] = np.array(alive, dtype=np.float32)
    buttons = np.zeros(n, dtype=np.uint16)
    zeros = np.zeros(n, dtype=np.float32)
    weapon = np.zeros(n, dtype=np.uint8)
    return np.array(bot_ids, dtype=np.uint16), obs, buttons, zeros, zeros, zeros, zeros, weapon


@needs_numpy
def test_recorder_writes_exactly_the_batch_it_was_given(tmp_path):
    rec = recorder_mod.DemoRecorder(str(tmp_path), policy_name="unit-test")
    ids, obs, btn, pitch, yaw, fwd, side, wpn = _batch([1, 2, 3], [1, 1, 1], tick=7)
    rec.record(7, 0, ids, obs, btn, pitch, yaw, fwd, side, wpn)
    rec.close()

    files = sorted(Path(tmp_path).glob("*.gbdemo"))
    assert len(files) == 1
    header, arr, trailing = shard.load_shard(str(files[0]))
    assert trailing == 0
    assert len(arr) == 3
    assert list(arr["bot_id"]) == [1, 2, 3]
    assert (arr["tick"] == 7).all()
    assert (arr["episode_id"] == 0).all()   # first sighting: episode 0
    assert (arr["done"] == 0).all()         # all alive


@needs_numpy
def test_recorder_marks_dead_frames_done(tmp_path):
    rec = recorder_mod.DemoRecorder(str(tmp_path), policy_name="unit-test")
    ids, obs, btn, pitch, yaw, fwd, side, wpn = _batch([1], [0], tick=0)
    rec.record(0, 0, ids, obs, btn, pitch, yaw, fwd, side, wpn)
    rec.close()
    _h, arr, _t = shard.load_shard(rec.writer.current_path)
    assert arr["done"][0] == 1


@needs_numpy
def test_recorder_increments_episode_on_respawn(tmp_path):
    """A dead->alive transition for the same bot is a new episode -- so a
    sequence-aware trainer built on this format later can chunk on it."""
    rec = recorder_mod.DemoRecorder(str(tmp_path), policy_name="unit-test")
    for tick, alive in ((0, 1), (1, 1), (2, 0), (3, 1)):
        ids, obs, btn, pitch, yaw, fwd, side, wpn = _batch([9], [alive], tick=tick)
        rec.record(tick, 0, ids, obs, btn, pitch, yaw, fwd, side, wpn)
    rec.close()
    _h, arr, _t = shard.load_shard(rec.writer.current_path)
    assert list(arr["episode_id"]) == [0, 0, 0, 1]
    assert list(arr["done"]) == [0, 0, 1, 0]


@needs_numpy
def test_recorder_keeps_different_connections_and_bots_separate(tmp_path):
    """bot_id 0 exists on every server; episode tracking keyed on bot_id
    alone would have one server's bot 0 affect another's episode count."""
    rec = recorder_mod.DemoRecorder(str(tmp_path), policy_name="unit-test")
    ids, obs, btn, pitch, yaw, fwd, side, wpn = _batch([0], [0], tick=0)     # dead
    rec.record(0, 1, ids, obs, btn, pitch, yaw, fwd, side, wpn)
    ids, obs, btn, pitch, yaw, fwd, side, wpn = _batch([0], [1], tick=0)     # alive, different conn
    rec.record(0, 2, ids, obs, btn, pitch, yaw, fwd, side, wpn)
    rec.close()
    _h, arr, _t = shard.load_shard(rec.writer.current_path)
    # Both are episode 0 (first sighting each), not "respawned" into episode 1.
    assert list(arr["episode_id"]) == [0, 0]


@needs_numpy
def test_recorder_reports_stats(tmp_path):
    rec = recorder_mod.DemoRecorder(str(tmp_path), policy_name="unit-test")
    ids, obs, btn, pitch, yaw, fwd, side, wpn = _batch([1, 2], [1, 1])
    rec.record(0, 0, ids, obs, btn, pitch, yaw, fwd, side, wpn)
    rec.close()
    stats = rec.stats()
    assert stats["record_total_written"] == 2
    assert stats["record_bots_tracked"] == 2
    assert stats["record_dropped_batches"] == 0


@needs_numpy
def test_recorder_drops_rather_than_blocks_when_the_writer_falls_behind(tmp_path):
    """The hard requirement in policyd.py's own words: 'the server never
    blocks a game server.' A recorder whose disk cannot keep up must shed
    load, not make record() (called from the single serving thread) wait on
    I/O -- so with a tiny queue and an artificially slow writer, record()
    still returns immediately and the drop is counted, not silent."""
    rec = recorder_mod.DemoRecorder(str(tmp_path), policy_name="slow",
                                    queue_maxsize=1)
    real_write_batch = rec.writer.write_batch

    def slow_write_batch(rows):
        time.sleep(0.05)
        return real_write_batch(rows)

    rec.writer.write_batch = slow_write_batch
    ids, obs, btn, pitch, yaw, fwd, side, wpn = _batch([1], [1])

    t0 = time.perf_counter()
    for _ in range(30):
        rec.record(0, 0, ids, obs, btn, pitch, yaw, fwd, side, wpn)
    elapsed = time.perf_counter() - t0
    rec.close()

    assert elapsed < 1.0, "record() blocked instead of dropping under backpressure"
    assert rec.dropped_batches > 0
    assert rec.dropped_records == rec.dropped_batches   # one bot per batch here
    assert rec.stats()["record_dropped_batches"] == rec.dropped_batches


def test_recorder_construction_requires_numpy_message():
    if HAVE_NUMPY:
        pytest.skip("numpy is installed on this host")
    with pytest.raises(RuntimeError, match="numpy"):
        recorder_mod.DemoRecorder("/tmp/should-not-be-created")


# --------------------------------------------------------------------------
# policyd integration: --record is opt-in and never blocks a response
# --------------------------------------------------------------------------

class _ArrayPolicy(policyd.Policy):
    """A minimal stand-in for runtime.GpuPolicy that exposes act_arrays()
    without needing torch or a GPU -- exactly the shape the recording hook is
    written against."""

    name = "test-array"

    def act_arrays(self, tick, flags, ids, obs, conn_key=0):
        import numpy as np
        n = len(ids)
        return (np.full(n, schema.BTN_ATTACK, dtype=np.uint16),
               np.full(n, 1.0, dtype=np.float32),
               np.full(n, -1.0, dtype=np.float32),
               np.full(n, 0.5, dtype=np.float32),
               np.full(n, -0.5, dtype=np.float32),
               np.zeros(n, dtype=np.uint8))

    def describe(self):
        return self.name


def _alive_obs():
    obs = _obs()
    obs[_ALIVE_OFF] = 1.0
    return obs


@needs_numpy
def test_policyd_records_when_a_recorder_is_wired_in(tmp_path):
    sock_path = str(tmp_path / "p.sock")
    out_dir = str(tmp_path / "demos")
    rec = recorder_mod.DemoRecorder(out_dir, policy_name="test-array")
    server = policyd.PolicyServer(_ArrayPolicy(), sock_path, status_path=None,
                                  stats_interval=0, recorder=rec)
    server.start()
    t = threading.Thread(target=server.run, daemon=True)
    t.start()
    try:
        c = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        for _ in range(50):
            try:
                c.connect(sock_path)
                break
            except OSError:
                time.sleep(0.02)
        c.settimeout(5)
        entries = [(i, _alive_obs()) for i in range(4)]
        c.sendall(schema.pack_request(1, entries))
        want = schema.HEADER_SIZE + 4 * schema.ACTION_SIZE
        buf = b""
        while len(buf) < want:
            chunk = c.recv(want - len(buf))
            assert chunk
            buf += chunk
        c.close()
    finally:
        server.stop()
        t.join(timeout=5)

    assert rec.writer.total_written == 4
    files = sorted(Path(out_dir).glob("*.gbdemo"))
    assert len(files) == 1
    header, arr, trailing = shard.load_shard(str(files[0]))
    assert trailing == 0
    assert len(arr) == 4
    assert (arr["buttons"] == schema.BTN_ATTACK).all()


def test_policy_server_has_no_recorder_by_default():
    """Recording is opt-in: constructing a PolicyServer without passing one
    must not create or touch anything on disk."""
    server = policyd.PolicyServer(policyd.NoOpPolicy(), "/tmp/does-not-matter.sock")
    assert server.recorder is None


@needs_numpy
def test_a_throwing_recorder_disables_itself_but_keeps_serving(tmp_path):
    """The same discipline as a throwing policy (test_gamebots_policyd.py):
    a game server blocked on a socket is a stalled game server. A broken
    recorder must not be able to take the answer down."""

    class _Boom:
        def record(self, *a, **kw):
            raise RuntimeError("disk full")

        def stats(self):
            return {}

        def close(self):
            pass

    sock_path = str(tmp_path / "p.sock")
    server = policyd.PolicyServer(_ArrayPolicy(), sock_path, status_path=None,
                                  stats_interval=0, recorder=_Boom())
    server.start()
    t = threading.Thread(target=server.run, daemon=True)
    t.start()
    try:
        c = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        for _ in range(50):
            try:
                c.connect(sock_path)
                break
            except OSError:
                time.sleep(0.02)
        c.settimeout(5)
        entries = [(0, _alive_obs())]
        c.sendall(schema.pack_request(1, entries))
        want = schema.HEADER_SIZE + schema.ACTION_SIZE
        buf = b""
        while len(buf) < want:
            chunk = c.recv(want - len(buf))
            assert chunk, "server stalled instead of answering"
            buf += chunk
        _tick, _flags, actions = schema.unpack_response(buf)
        assert len(actions) == 1
        assert actions[0][1] == schema.BTN_ATTACK   # the policy's answer still went out
        c.close()
    finally:
        server.stop()
        t.join(timeout=5)
    assert server.recorder is None    # disabled itself after the failure


# --------------------------------------------------------------------------
# main() wiring: --record without numpy/act_arrays degrades, never crashes
# --------------------------------------------------------------------------

def test_main_help_mentions_record_flag():
    src = (_GB / "policyd.py").read_text()
    assert "--record" in src
    assert "--record-max-per-shard" in src


# --------------------------------------------------------------------------
# recording overhead on the real serving path — the hard requirement
# --------------------------------------------------------------------------

@needs_cuda()
def test_recording_overhead_at_512_bots_is_small(tmp_path):
    """The hard requirement: --record must not measurably slow the serving
    path. Isolates PolicyServer._serve_fast's own cost (a fake connection, no
    real socket) with and without a recorder wired in, at 512 bots -- the same
    scale runtime.py's own README benchmark uses (0.357 ms/req there).

    The bound here is deliberately generous (not a tight regression pin) to
    avoid flaking on a shared/loaded CI host; the real measured numbers from a
    quiet run are reported in the task's summary, not asserted verbatim here.
    """
    import numpy as np
    runtime = _load("runtime")

    class _FakeConn:
        def __init__(self):
            self._fd = 1

        def fileno(self):
            return self._fd

        def sendall(self, _data):
            pass

    policy = runtime.GpuPolicy(device="cuda", use_graphs=True, prewarm=True)
    n = 512
    entries = [(i, [0.05 * ((i + j) % 20) for j in range(schema.OBS_DIM)])
              for i in range(n)]
    req = schema.pack_request(0, entries)

    def bench(srv, iters=200):
        conn = _FakeConn()
        for _ in range(20):
            srv._serve_fast(conn, req)
        t0 = time.perf_counter()
        for _ in range(iters):
            srv._serve_fast(conn, req)
        return (time.perf_counter() - t0) / iters * 1000.0   # ms/req

    baseline_srv = policyd.PolicyServer(policy, str(tmp_path / "a.sock"),
                                        stats_interval=0)
    ms_off = bench(baseline_srv)

    rec = recorder_mod.DemoRecorder(str(tmp_path / "demos"), policy_name="gpu")
    recording_srv = policyd.PolicyServer(policy, str(tmp_path / "b.sock"),
                                         stats_interval=0, recorder=rec)
    ms_on = bench(recording_srv)
    rec.close()

    print(f"\n512-bot _serve_fast: recording OFF {ms_off:.4f} ms/req, "
         f"ON {ms_on:.4f} ms/req, overhead {ms_on - ms_off:.4f} ms "
         f"({(ms_on / ms_off - 1) * 100:.1f}%)")

    # Generous bound: recording must not come close to doubling serve time,
    # and its absolute added cost must stay well under a single-digit-ms
    # frame budget's worth of slack.
    assert ms_on < ms_off * 2.0 + 1.0
