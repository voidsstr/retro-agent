"""Source-level tests for the Quake II engine adapter.

Like the Quake III adapter, this compiles into a real engine's game module
(and, uniquely here, into the engine binary itself) and cannot be
unit-tested without the whole thing. What CAN be checked here is the class
of mistake that actually bit while building it -- all silent at runtime:

  * hand-counted offsets into the observation (the project's running
    superstition -- see test_gamebots_quake3.py for the two times this
    happened for real);
  * the generated header drifting from the schema;
  * the upstream patch losing a hook, which leaves a module that loads,
    prints its banner, and never drives a bot;
  * the build omitting a translation unit;
  * the one Quake-II-specific failure mode with real teeth: a fake client
    left at client_t.state == cs_free crashes the WHOLE SERVER via an
    unguarded reliable-message buffer the instant it picks up an item or
    dies (verified on a live test server, see README.md "The crash"). The
    fix is a second patch, to the ENGINE, not the game module -- and it is
    not optional, so this file checks it exists and guards the right two
    functions, not just that build.sh happens to run something extra.

Run: pytest tests/python/test_gamebots_quake2.py
"""

import importlib.util
import re
import sys
from pathlib import Path

import pytest

_REPO = Path(__file__).resolve().parent.parent.parent
_GB = _REPO / "scripts" / "gamebots"
_Q2 = _GB / "adapters" / "quake2"


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
    """The adapter walks slots as `base + i * GB_ENT_SLOT_STRIDE`, then
    indexes sub-fields by macro. If these drift from the field table it
    reads adjacent floats and the bot behaves oddly with nothing logged --
    exactly the bug that shipped once already in this project (the scripted
    policy reading `visible` out of the middle of `rel_vel`)."""
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


def test_adapter_uses_macros_not_hand_counted_offsets():
    src = (_Q2 / "gb_adapter.c").read_text()
    assert "GB_ENT_SLOT_STRIDE" in src
    assert "GB_ENT_VISIBLE" in src
    # `base + 7` style indexing is exactly the bug this guards against --
    # it has happened twice already in this project (see the plan's risk
    # table and the Quake III adapter's own test for this).
    assert not re.search(r"base\s*\+\s*\d+\s*\]", src), \
        "hand-counted entity offset in the Quake II adapter"


def test_adapter_does_not_use_gclient_or_svclient_reserved_names_wrong():
    """`world` is a macro in header/local.h ((&g_edicts[0])) -- a parameter
    or local reusing that name fails to compile with an error pointing at
    the macro body, not the adapter. Documented in the README; checked here
    so it can't quietly come back if the file is edited again."""
    src = (_Q2 / "gb_adapter.c").read_text()
    assert not re.search(r"\bvec3_t\s+world\b", src)
    assert "worldvec" in src


# --- the usercmd-hook patch (g_main.patch) --------------------------------

def test_the_patch_installs_all_hooks():
    """A module missing a hook still loads and still prints its banner --
    it just never spawns or drives a bot, and (unlike Quake III, which
    still has botlib) there is no fallback behaviour to notice either."""
    patch = (_Q2 / "g_main.patch").read_text()
    for hook in ("GB_Init", "GB_Shutdown", "GB_RunFrame"):
        assert hook in patch, hook


def test_run_frame_hooks_before_the_frame_advances():
    """GB_RunFrame() has to run as the FIRST statement of G_RunFrame(),
    mirroring where a real client's ClientThink() already happened (on the
    network read path, before the engine calls G_RunFrame() at all). If it
    lands after level.framenum/level.time advance, bots would be driven
    against next frame's clock instead of this one's -- a subtle,
    silent-forever mismatch."""
    patch = (_Q2 / "g_main.patch").read_text()
    run_frame_call = patch.index("+\tGB_RunFrame();")
    framenum_advance = patch.index("level.framenum++;")
    assert run_frame_call < framenum_advance


def test_the_adapter_is_off_by_default_two_ways():
    """Installing the module must change nothing until someone opts in --
    and here that is TWO switches, not one: gb_bots (spawn anything at all)
    and gb_enable (let the policy touch what's spawned)."""
    src = (_Q2 / "gb_adapter.c").read_text()
    m_bots = re.search(r'gi\.cvar\("gb_bots",\s*"(\w+)"', src)
    m_enable = re.search(r'gi\.cvar\("gb_enable",\s*"(\w+)"', src)
    assert m_bots and m_bots.group(1) == "0", "gb_bots must default to 0"
    assert m_enable and m_enable.group(1) == "0", "gb_enable must default to 0"


def test_run_frame_is_a_no_op_with_zero_bots():
    """gb_bots 0 must cost nothing -- no gi.trace calls, no ClientConnect,
    nothing -- which is what makes 'installing this changes nothing'
    actually true rather than aspirational."""
    src = (_Q2 / "gb_adapter.c").read_text()
    body = src[src.index("void GB_RunFrame"):]
    assert "if (nbots == 0)\n\t\treturn;" in body


def test_fallback_used_when_disabled_or_unanswered():
    """No engine bot AI exists to fall back to (see README) -- so the
    fallback here is OUR code, gated the same way Quake III gates its
    real botlib fallback: only overridden when the policy actually
    answered for that specific bot this frame."""
    src = (_Q2 / "gb_adapter.c").read_text()
    body = src[src.index("for (i = 0; i < nbots; i++) {\n\t\tedict_t"):]
    assert "gb_have_action[bot_list[i]]" in body
    assert "GB_CmdFromAction" in body
    assert "GB_FallbackCmd" in body


def test_respawn_is_forced_independent_of_the_policy():
    """Nothing else drives a fake client's input -- without this, a dead
    bot (fallback OR an unanswering policy) sits at the death screen
    forever. Checked in BOTH command builders, not just one."""
    src = (_Q2 / "gb_adapter.c").read_text()
    fallback = src[src.index("static void GB_FallbackCmd"):
                    src.index("static void GB_CmdFromAction")]
    from_action = src[src.index("static void GB_CmdFromAction"):]
    assert "GB_WantsRespawn" in fallback
    assert "GB_WantsRespawn" in from_action


def test_only_documented_buttons_are_wired():
    """The action schema is shared across engines; Quake II only implements
    a subset of it (no secondary fire, reload, walk-toggle or zoom in
    vanilla baseq2 -- see README). Attack/jump/crouch must be wired;
    the undocumented-as-working ones should not be silently invented."""
    src = (_Q2 / "gb_adapter.c").read_text()
    body = src[src.index("static void GB_CmdFromAction"):]
    assert "GB_BTN_ATTACK" in body
    assert "GB_BTN_JUMP" in body
    assert "GB_BTN_CROUCH" in body
    assert "GB_BTN_ATTACK2" not in body, \
        "Quake II has no secondary fire -- do not wire GB_BTN_ATTACK2"
    assert "GB_BTN_RELOAD" not in body, \
        "Quake II has no reload -- do not wire GB_BTN_RELOAD"


# --- the safety patch (sv_fakeclient_safety.patch) ------------------------

def test_safety_patch_exists_and_guards_both_leak_points():
    """A fake client never leaves client_t.state == cs_free (no network
    handshake ever runs for it). Two stock functions write into a cs_free
    client's message buffer with no state check at all, and
    SV_SendClientMessages() never drains a cs_free client -- so ANY
    reliable per-client message aimed at a bot (a pickup, an obituary, a
    respawn effect) accumulates until SZ_GetSpace() calls Com_Error and
    takes the whole server down. Verified on a live server: ~20 seconds
    from the first bot death without this patch. Both leak points must be
    guarded, not just the more obvious one (PF_Unicast) -- SV_ClientPrintf
    is reached directly by gi.cprintf(), bypassing PF_Unicast entirely, and
    stock baseq2 calls gi.cprintf() for every single item pickup."""
    patch = (_Q2 / "sv_fakeclient_safety.patch").read_text()
    assert "sv_send.c" in patch and "sv_game.c" in patch
    assert patch.count("cs_free") >= 2
    assert "SV_ClientPrintf" in patch or "cl->state == cs_free" in patch
    assert "PF_Unicast" in patch or "client->state == cs_free" in patch


def test_safety_patch_is_applied_before_the_two_functions_change_behaviour():
    """The guard must return/skip BEFORE the function does any work -- a
    guard placed after the message is already queued does nothing."""
    patch = (_Q2 / "sv_fakeclient_safety.patch").read_text()
    # In SV_ClientPrintf: the guard must precede vsnprintf/the writes.
    csend = patch[patch.index("--- src/server/sv_send.c"):
                  patch.index("--- src/server/sv_game.c")]
    guard_if = csend.index("+\tif (cl->state == cs_free)")
    va_start = csend.index("va_start(argptr, fmt);")
    assert guard_if < va_start

    # In PF_Unicast: the guard must precede the reliable/unreliable writes.
    cgame = patch[patch.index("--- src/server/sv_game.c"):]
    guard_if2 = cgame.index("+\tif (client->state == cs_free)")
    first_write = cgame.index("if (reliable)")
    assert guard_if2 < first_write


# --- the build --------------------------------------------------------

def test_build_regenerates_the_header_from_the_schema():
    sh = (_Q2 / "build.sh").read_text()
    assert "--emit-header" in sh


def test_build_guards_against_upstream_source_drift():
    """If yquake2 adds a game source we do not compile, we would link a
    module quietly missing that file's symbols."""
    sh = (_Q2 / "build.sh").read_text()
    assert "GAME_OBJS_" in sh
    assert "does not build" in sh


def test_build_includes_the_common_shared_sources():
    """common/shared/*.c is linked into game.so via GAME_OBJS_ but is not
    pulled in by including header/local.h -- omit it and the module links
    fine, then fails dlopen with an undefined symbol, exactly the
    q_math.c/q_shared.c trap the Quake III build script documents."""
    sh = (_Q2 / "build.sh").read_text()
    assert "common/shared/shared.c" in sh

    assert "common/shared/flash.c" in sh
    assert "common/shared/rand.c" in sh


def test_build_verifies_the_module_actually_loads():
    sh = (_Q2 / "build.sh").read_text()
    assert "ctypes.CDLL" in sh, "build does not dlopen-check the module"
    assert "GetGameAPI" in sh


def test_build_also_builds_the_safety_patched_engine():
    """game_ai.so alone is not a safe deployment -- see README "The crash".
    build.sh must also produce a patched q2ded, and must apply the safety
    patch before doing so."""
    sh = (_Q2 / "build.sh").read_text()
    assert "sv_fakeclient_safety.patch" in sh
    assert "q2ded_gamebots" in sh
    assert "release/q2ded" in sh


def test_readme_documents_the_crash_and_the_fix():
    """This is the single most important finding in this adapter -- a
    build.sh that produces q2ded_gamebots but a README that doesn't say why
    would leave the next person deploying the unsafe stock engine."""
    readme = (_Q2 / "README.md").read_text()
    assert "cs_free" in readme
    assert "SZ_GetSpace" in readme
    assert "q2ded_gamebots" in readme


@pytest.mark.skipif(not (_Q2 / "build" / "yquake2").is_dir(),
                    reason="yquake2 source not cloned (gitignored)")
def test_the_source_list_still_matches_upstream():
    makefile = _Q2 / "build" / "yquake2" / "Makefile"
    if not makefile.is_file():
        pytest.skip("Makefile not present")
    text = makefile.read_text()
    m = re.search(r"GAME_OBJS_ = \\\n(.*?)\n\n", text, re.S)
    if not m:
        pytest.skip("GAME_OBJS_ block not found (upstream Makefile changed shape)")
    upstream_game = set(re.findall(r"src/game/([a-zA-Z0-9_/]+\.o)", m.group(1)))
    upstream_common = set(re.findall(r"src/(common/shared/[a-zA-Z0-9_]+)\.o",
                                     m.group(1)))
    sh = (_Q2 / "build.sh").read_text()
    ours_game = set(re.findall(r"([a-zA-Z0-9_/]+\.c)", sh))
    missing_game = {f[:-2] + ".o" for f in upstream_game
                    if f[:-2] + ".c" not in ours_game}
    missing_common = {f for f in upstream_common if f + ".c" not in sh}
    assert not missing_game, f"yquake2 game sources not in build.sh: {sorted(missing_game)}"
    assert not missing_common, f"yquake2 common/shared sources not in build.sh: {sorted(missing_common)}"
