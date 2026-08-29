"""Source-level tests for the Quake III engine adapter.

The adapter compiles into ioquake3's game module, so it cannot be unit-tested
without the whole engine. What CAN be checked here is the class of mistake that
actually bit while building it — all of which are silent at runtime:

  * hand-counted offsets into the observation (the Python side already read
    `visible` out of the middle of `rel_vel` once);
  * the generated header drifting from the schema;
  * the upstream patch losing a hook, which leaves a module that loads, prints
    its banner, and never drives anything;
  * the build omitting a translation unit — that produced a `.so` which linked
    fine and then failed `dlopen` with "undefined symbol: vec3_origin", after
    which **the engine silently fell back to the QVM and the bots looked
    completely normal**. That is the worst failure mode in the system: a
    successful-looking build doing nothing.

Run: pytest tests/python/test_gamebots_quake3.py
"""

import importlib.util
import re
import subprocess
import sys
from pathlib import Path

import pytest

_REPO = Path(__file__).resolve().parent.parent.parent
_GB = _REPO / "scripts" / "gamebots"
_Q3 = _GB / "adapters" / "quake3"


def _load(name):
    spec = importlib.util.spec_from_file_location(name, _GB / f"{name}.py")
    mod = importlib.util.module_from_spec(spec)
    sys.modules[name] = mod
    spec.loader.exec_module(mod)
    return mod


schema = _load("schema")


# --- the entity-slot macros the adapter walks -----------------------------

def _header_defines():
    text = (_GB / "gamebots_schema.h").read_text()
    return {m.group(1): int(m.group(2))
            for m in re.finditer(r"^#define\s+(GB_\w+)\s+(-?\d+)\s*$",
                                 text, re.M)}


def test_entity_macros_match_the_schema():
    """The adapter walks slots as `base + i * GB_ENT_SLOT_STRIDE`, then indexes
    sub-fields by macro. If these drift from the field table it reads adjacent
    floats and the bot behaves oddly with nothing logged."""
    d = _header_defines()
    off = {f[1]: f[2] for f in schema.FIELD_TABLE}
    e0 = off["e0_present"]
    assert d["GB_ENT_SLOT_STRIDE"] == off["e1_present"] - e0
    assert d["GB_ENT_PRESENT"] == 0
    assert d["GB_ENT_TEAMMATE"] == off["e0_is_teammate"] - e0
    assert d["GB_ENT_DIR"] == off["e0_dir"] - e0
    assert d["GB_ENT_DIST"] == off["e0_dist_norm"] - e0
    assert d["GB_ENT_RELVEL"] == off["e0_rel_vel"] - e0
    assert d["GB_ENT_HEALTH"] == off["e0_health_frac"] - e0
    assert d["GB_ENT_VISIBLE"] == off["e0_visible"] - e0


def test_every_slot_really_has_the_same_stride():
    """The macros describe slot 0 and are reused for all of them."""
    off = {f[1]: f[2] for f in schema.FIELD_TABLE}
    stride = off["e1_present"] - off["e0_present"]
    for i in range(schema.MAX_ENTITIES):
        assert off[f"e{i}_present"] == off["e0_present"] + i * stride
        assert off[f"e{i}_visible"] - off[f"e{i}_present"] == \
            off["e0_visible"] - off["e0_present"]


def test_adapter_uses_macros_not_hand_counted_offsets():
    src = (_Q3 / "gb_adapter.c").read_text()
    assert "GB_ENT_SLOT_STRIDE" in src
    assert "GB_ENT_VISIBLE" in src
    # `base + 7` style indexing is exactly the bug this guards against.
    assert not re.search(r"base\s*\+\s*\d+\s*\]", src), \
        "hand-counted entity offset in the Quake III adapter"


# --- the upstream patch --------------------------------------------------

def test_the_patch_installs_all_three_hooks():
    """A module missing a hook still loads and still prints its banner. It just
    never drives a bot, and the only symptom is bots that play like botlib."""
    patch = (_Q3 / "ai_main.patch").read_text()
    for hook in ("GB_Init", "GB_FrameBegin", "GB_ApplyAction"):
        assert f"+{hook}" in patch.replace("\t", "") or hook in patch, hook


def test_apply_runs_after_botupdateinput():
    """Ordering is the fallback design: botlib fills a complete usercmd first,
    so "the policy declined" degrades to the stock bot rather than to a bot
    standing still. If GB_ApplyAction moved above BotUpdateInput, botlib would
    overwrite us and the adapter would appear to do nothing."""
    patch = (_Q3 / "ai_main.patch").read_text()
    upd = patch.index("BotUpdateInput(botstates[i], time, elapsed_time);")
    app = patch.index("GB_ApplyAction(i, botstates[i]->viewangles")
    assert upd < app


def test_the_adapter_is_off_by_default():
    """Installing the module must change nothing until someone opts in — this
    goes into a game server people play on."""
    src = (_Q3 / "gb_adapter.c").read_text()
    m = re.search(r'trap_Cvar_Register\(&gb_enable,\s*"gb_enable",\s*"(\w+)"', src)
    assert m and m.group(1) == "0", "gb_enable must default to 0"


def test_apply_action_is_a_no_op_when_disabled_or_unanswered():
    src = (_Q3 / "gb_adapter.c").read_text()
    body = src[src.index("void GB_ApplyAction"):]
    assert "!gb_enable.integer" in body
    assert "!gb_have_action[clientNum]" in body


def test_only_bots_are_driven():
    """SVF_BOT is the guard that stops us rewriting a human's usercmd."""
    src = (_Q3 / "gb_adapter.c").read_text()
    assert "SVF_BOT" in src


# --- the build ------------------------------------------------------------

def test_build_includes_the_qcommon_sources():
    """Omitting q_math.c/q_shared.c builds a .so that links, fails dlopen with
    'undefined symbol: vec3_origin', and the engine then falls back to the QVM
    WITHOUT saying the module was ignored."""
    sh = (_Q3 / "build.sh").read_text()
    assert "q_math.c" in sh and "q_shared.c" in sh


def test_build_verifies_the_module_actually_loads():
    sh = (_Q3 / "build.sh").read_text()
    assert "ctypes.CDLL" in sh, "build does not dlopen-check the module"
    assert "vmMain" in sh


def test_build_guards_against_upstream_source_drift():
    """If ioq3 adds a game source we do not compile, we would link a module
    quietly missing that file's symbols."""
    sh = (_Q3 / "build.sh").read_text()
    assert "basegame.cmake" in sh
    assert "does not build" in sh


def test_build_regenerates_the_header_from_the_schema():
    sh = (_Q3 / "build.sh").read_text()
    assert "--emit-header" in sh


@pytest.mark.skipif(not (_Q3 / "build" / "ioq3").is_dir(),
                    reason="ioq3 source not cloned (gitignored)")
def test_the_source_list_still_matches_upstream():
    cmake = _Q3 / "build" / "ioq3" / "cmake" / "basegame.cmake"
    if not cmake.is_file():
        pytest.skip("basegame.cmake not present")
    block = re.search(r"set\(GAME_SOURCES(.*?)\)", cmake.read_text(), re.S)
    upstream = set(re.findall(r"game/([a-z_0-9]+\.c)", block.group(1)))
    ours = set(re.findall(r"([a-z_0-9]+\.c)", (_Q3 / "build.sh").read_text()))
    missing = upstream - ours
    assert not missing, f"ioq3 game sources not in build.sh: {sorted(missing)}"
