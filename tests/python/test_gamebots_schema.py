"""Tests for the game-bot observation/action schema.

The schema is the contract between four things that are built separately and
deployed separately: engine adapters (C, per engine), the policy server
(Python), recorded datasets (on disk, outliving both), and trained models. When
those disagree the failure is silent — the floats still unpack, they just mean
different things, and the bot walks into a wall for reasons nobody can see.

So the tests here are mostly about **detecting disagreement loudly**:

  * the layout hash changes when the layout changes, and not when a comment does
  * a mismatched hash is rejected with an actionable message
  * the generated C header matches the checked-in one byte for byte
  * actions from a half-trained net (NaN, inf, wild values) cannot reach a
    game server

Run: pytest tests/python/test_gamebots_schema.py
"""

import importlib.util
import math
import struct
import subprocess
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


# --- layout -----------------------------------------------------------------

def test_observation_is_8_float_aligned():
    """Padded to a multiple of 8 floats so every observation starts 32-byte
    aligned for whatever SIMD or tensor path eventually reads it. Unpadded
    dimensions have a way of becoming permanent."""
    assert schema.OBS_DIM % 8 == 0
    assert schema.OBS_DIM >= schema._RAW_OBS_DIM
    assert schema.OBS_DIM - schema._RAW_OBS_DIM < 8


def test_fields_are_contiguous_and_cover_the_observation():
    """No gaps and no overlaps — a hole in the layout is a field somebody
    forgot to write, read back as whatever was in the buffer before."""
    off = 0
    for _g, _name, start, count, _doc in schema.FIELD_TABLE:
        assert start == off, f"gap or overlap at {_name}: expected {off}, got {start}"
        off += count
    assert off == schema._RAW_OBS_DIM


def test_field_names_are_unique():
    names = [f[1] for f in schema.FIELD_TABLE]
    assert len(names) == len(set(names))


def test_every_entity_slot_has_the_same_shape():
    """A policy indexes entity slots arithmetically; slots of differing width
    would silently misread every slot after the first."""
    widths = []
    for i in range(schema.MAX_ENTITIES):
        fields = [f for f in schema.FIELD_TABLE if f[1].startswith(f"e{i}_")]
        widths.append(sum(f[3] for f in fields))
    assert len(set(widths)) == 1, f"entity slots differ in width: {widths}"


def test_intent_vector_is_reserved_from_the_start():
    """Phase 4 adds the LLM planner, but the intent field exists now: adding it
    later would change the layout hash and invalidate every demonstration
    recorded before then."""
    names = [f[1] for f in schema.FIELD_TABLE]
    assert "intent" in names
    assert schema.INTENT_DIM > 0


# --- the hash ---------------------------------------------------------------

def test_hash_is_stable_across_calls():
    assert schema.schema_hash() == schema.schema_hash() == schema.SCHEMA_HASH


def test_hash_covers_layout_but_not_documentation(monkeypatch):
    """Improving a comment must not invalidate a recorded dataset; renaming or
    resizing a field must."""
    original = schema.FIELD_TABLE

    doc_changed = [(g, n, o, c, d + " (clarified)") for g, n, o, c, d in original]
    monkeypatch.setattr(schema, "FIELD_TABLE", doc_changed)
    assert schema.schema_hash() == schema.SCHEMA_HASH

    renamed = [(g, ("renamed" if i == 0 else n), o, c, d)
               for i, (g, n, o, c, d) in enumerate(original)]
    monkeypatch.setattr(schema, "FIELD_TABLE", renamed)
    assert schema.schema_hash() != schema.SCHEMA_HASH


def test_hash_changes_when_a_field_is_resized(monkeypatch):
    resized = [(g, n, o, (c + 1 if i == 0 else c), d)
               for i, (g, n, o, c, d) in enumerate(schema.FIELD_TABLE)]
    monkeypatch.setattr(schema, "FIELD_TABLE", resized)
    assert schema.schema_hash() != schema.SCHEMA_HASH


# --- wire format ------------------------------------------------------------

def _obs(seed=0.0):
    return [seed + i * 0.001 for i in range(schema.OBS_DIM)]


def test_request_round_trip():
    entries = [(7, _obs(0.1)), (9, _obs(0.2))]
    tick, flags, got = schema.unpack_request(
        schema.pack_request(1234, entries, schema.FLAG_TRAINING))
    assert tick == 1234
    assert flags == schema.FLAG_TRAINING
    assert [g[0] for g in got] == [7, 9]
    for (_bid, sent), (_gid, recv) in zip(entries, got):
        assert recv == pytest.approx(sent, abs=1e-6)


def test_response_round_trip():
    actions = [(3, schema.BTN_ATTACK | schema.BTN_JUMP, 1.5, -2.5, 1.0, -1.0, 4)]
    tick, _flags, got = schema.unpack_response(schema.pack_response(99, actions))
    assert tick == 99
    bid, buttons, pitch, yaw, fwd, side, weapon = got[0]
    assert (bid, weapon) == (3, 4)
    assert buttons & schema.BTN_ATTACK and buttons & schema.BTN_JUMP
    assert (pitch, yaw, fwd, side) == pytest.approx((1.5, -2.5, 1.0, -1.0))


def test_empty_batch_round_trips():
    """A server with no bots this frame still sends a frame."""
    tick, _f, got = schema.unpack_request(schema.pack_request(5, []))
    assert (tick, got) == (5, [])


def test_a_mismatched_schema_hash_is_rejected_with_an_actionable_message():
    """The single most valuable error in the system. An adapter built against a
    stale header is otherwise undetectable: the bytes unpack fine."""
    buf = bytearray(schema.pack_request(1, [(0, _obs())]))
    struct.pack_into("<I", buf, 4, schema.SCHEMA_HASH ^ 0xFFFF)
    with pytest.raises(ValueError) as e:
        schema.unpack_request(bytes(buf))
    msg = str(e.value)
    assert "schema hash mismatch" in msg
    assert "rebuild the adapter" in msg


def test_bad_magic_is_rejected():
    buf = bytearray(schema.pack_request(1, [(0, _obs())]))
    buf[0:4] = b"XXXX"
    with pytest.raises(ValueError, match="bad magic"):
        schema.unpack_request(bytes(buf))


def test_a_truncated_request_is_rejected_not_silently_short():
    full = schema.pack_request(1, [(0, _obs()), (1, _obs())])
    with pytest.raises(ValueError):
        schema.unpack_request(full[:-4])
    with pytest.raises(ValueError):
        schema.unpack_request(full[:2])


def test_wrong_observation_width_is_caught_at_pack_time():
    with pytest.raises(ValueError, match="expected"):
        schema.pack_request(1, [(0, [0.0] * (schema.OBS_DIM - 1))])


def test_response_magic_is_distinct_from_request_magic():
    """Otherwise a reflected or looped-back request would parse as actions."""
    assert schema.REQ_MAGIC != schema.RESP_MAGIC


# --- action safety ----------------------------------------------------------

def test_clamp_bounds_every_axis():
    p, y, f, s = schema.clamp_action(1e6, -1e6, 50.0, -50.0)
    assert p == schema.MAX_PITCH_DELTA_DEG
    assert y == -schema.MAX_YAW_DELTA_DEG
    assert (f, s) == (1.0, -1.0)


def test_clamp_kills_nan():
    """A half-trained net emits NaN long before it emits good play, and NaN
    fails every comparison — so min/max alone would pass it straight through to
    the game server's view angles."""
    for v in (float("nan"),) * 4:
        pass
    p, y, f, s = schema.clamp_action(float("nan"), float("nan"),
                                     float("nan"), float("nan"))
    assert not any(math.isnan(v) for v in (p, y, f, s))
    assert (p, y, f, s) == (0.0, 0.0, 0.0, 0.0)


def test_clamp_kills_infinities():
    p, y, f, s = schema.clamp_action(float("inf"), float("-inf"),
                                     float("inf"), float("-inf"))
    assert all(math.isfinite(v) for v in (p, y, f, s))


def test_clamp_leaves_reasonable_actions_alone():
    vals = (3.0, -4.0, 0.5, -0.25)
    assert schema.clamp_action(*vals) == pytest.approx(vals)


# --- the generated C header -------------------------------------------------

def test_checked_in_header_matches_the_generator():
    """The header adapters compile against is GENERATED. If it is edited by
    hand, or the schema changes without regenerating, the C and Python sides
    disagree — which is precisely the failure the hash exists to catch, except
    it would be caught in production rather than here."""
    checked_in = (_GB / "gamebots_schema.h").read_text()
    assert checked_in == schema.emit_header(), (
        "gamebots_schema.h is stale — regenerate:\n"
        "  python3 scripts/gamebots/schema.py --emit-header "
        "> scripts/gamebots/gamebots_schema.h")


def test_header_carries_the_same_constants_as_python():
    h = (_GB / "gamebots_schema.h").read_text()
    assert f"#define GB_SCHEMA_HASH    0x{schema.SCHEMA_HASH:08x}u" in h
    assert f"#define GB_OBS_DIM        {schema.OBS_DIM}" in h
    assert f"#define GB_MAX_ENTITIES   {schema.MAX_ENTITIES}" in h


def test_header_compiles_and_its_struct_sizes_match_python(tmp_path):
    """The header's static asserts fire at compile time if the C structs and
    the Python struct formats have drifted — which is the one kind of drift
    the runtime hash cannot catch, because both sides would agree on the hash
    while disagreeing on the byte layout."""
    import shutil
    cc = shutil.which("gcc") or shutil.which("cc") or shutil.which("clang")
    if not cc:
        pytest.skip("no C compiler")
    src = tmp_path / "t.c"
    src.write_text(f"""
        #include "gamebots_schema.h"
        #include <stdio.h>
        int main(void) {{
            printf("%zu %zu %zu %u\\n", sizeof(gb_header_t),
                   sizeof(gb_obs_entry_t), sizeof(gb_action_t), GB_SCHEMA_HASH);
            return 0;
        }}
    """)
    exe = tmp_path / "t"
    r = subprocess.run([cc, "-Wall", "-Wextra", "-Werror", "-I", str(_GB),
                        "-o", str(exe), str(src)],
                       capture_output=True, text=True)
    assert r.returncode == 0, f"header does not compile cleanly:\n{r.stderr}"
    out = subprocess.run([str(exe)], capture_output=True, text=True).stdout.split()
    assert int(out[0]) == schema.HEADER_SIZE
    assert int(out[1]) == schema.OBS_ENTRY_SIZE
    assert int(out[2]) == schema.ACTION_SIZE
    assert int(out[3]) == schema.SCHEMA_HASH
