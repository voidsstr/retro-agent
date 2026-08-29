"""Source-level tests for the UT99 (UnrealScript) engine adapter.

The adapter compiles into `GameBots.u` via `ucc make`, which needs a real
UT99 install and is not available in CI, so — same shape as the Quake III
adapter's tests — what's checked here is the class of mistake that bit while
building this one, all of it silent or misleading at runtime:

  * `GBSchema.uc` drifting from `schema.py` (the C header's problem, same fix:
    generate it, diff it in a test);
  * a bare cross-class `const` creeping back in, which sometimes compiles and
    sometimes doesn't with no visible pattern -- see the adapter README's
    "cross-class const compiler quirk" section for the full account. Every
    schema value must go through `class'GBSchema'.static.Name()`;
  * a bare ternary creeping back in ("Type mismatch in '='" on this compiler,
    intermittently) -- every conditional-assignment in this adapter is
    if/else;
  * the fallback ordering (`Super.Tick()` before any override) regressing,
    which would make a policy failure freeze bots instead of degrading them
    to the built-in AI;
  * the mutator shipping enabled by default.

Run: pytest tests/python/test_gamebots_ut99.py
"""

import importlib.util
import re
import sys
from pathlib import Path

_REPO = Path(__file__).resolve().parent.parent.parent
_GB = _REPO / "scripts" / "gamebots"
_UT = _GB / "adapters" / "ut99"
_CLASSES = _UT / "GameBots" / "Classes"


def _load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    sys.modules[name] = mod
    spec.loader.exec_module(mod)
    return mod


schema = _load("schema", _GB / "schema.py")
gen_gbschema = _load("gen_gbschema_ut99", _UT / "gen_gbschema.py")


def _src(name):
    return (_CLASSES / name).read_text()


# --- GBSchema.uc must be exactly what the generator produces right now ----

def test_gbschema_uc_matches_the_generator():
    """The single most valuable check in this file: if this fails, someone
    changed schema.py (or hand-edited GBSchema.uc) and forgot to regenerate.
    The floats still unpack on the wire either way -- they just mean
    something else, and nothing else here would catch it."""
    committed = _src("GBSchema.uc")
    fresh = gen_gbschema.emit()
    assert committed == fresh, (
        "GameBots/Classes/GBSchema.uc is stale -- regenerate with "
        "`python3 gen_gbschema.py > GameBots/Classes/GBSchema.uc`")


def test_gbschema_uses_functions_not_consts():
    """The whole point of the rewrite documented in the README: a bare
    cross-class `const` compiled in some positions and not others with no
    discoverable pattern. Every value must be a zero-arg static function."""
    src = _src("GBSchema.uc")
    assert re.search(r"^const\s", src, re.M) is None, \
        "GBSchema.uc has a `const` declaration -- must be a static function"
    # Spot-check a handful of the values that mattered most while debugging.
    for name in ("SCHEMA_HASH", "OBS_DIM", "MAX_ENTITIES", "BTN_ATTACK",
                 "ENT_SLOT_STRIDE", "MAX_PITCH_DELTA_DEG"):
        assert re.search(
            rf"static final function \w+ {name}\(\)", src), name


def test_schema_hash_fits_a_signed_int():
    """UnrealScript ints are signed 32-bit. A schema hash >= 2^31 would need
    different handling on both the encode and decode side of GBLink -- this
    just needs to be noticed if it ever happens, not silently mis-sent."""
    assert 0 <= schema.SCHEMA_HASH < (1 << 31), (
        f"SCHEMA_HASH {schema.SCHEMA_HASH:#010x} no longer fits a signed "
        "32-bit int -- GBLink's header packing needs review")


# --- no bare cross-class const anywhere in the adapter --------------------

def test_no_bare_cross_class_const_reference():
    """Every GBSchema.* (and GBMath.*) reference in the adapter must go
    through the static-call form. A bare `GBSchema.NAME` is exactly the
    pattern that compiled intermittently -- see the README."""
    pattern = re.compile(r"(?<!'GBSchema'\.static\.)\bGBSchema\.\w+")
    for fname in ("GBMutator.uc", "GBLink.uc", "GBBot.uc"):
        src = _src(fname)
        # Strip line comments and block text so the (documentation) mentions
        # of the old bug pattern in comments don't false-positive.
        code_lines = []
        for line in src.splitlines():
            stripped = line.split("//", 1)[0]
            code_lines.append(stripped)
        code = "\n".join(code_lines)
        bad = pattern.findall(code)
        assert not bad, f"{fname} has a bare GBSchema.* reference: {bad}"


def test_no_bare_ternary_assignment():
    """A `cond ? a : b` used directly as an assignment RHS or Obs[] element
    intermittently failed to compile ("Type mismatch in '='"). Every such
    spot in this adapter was rewritten to if/else -- this guards against one
    creeping back in."""
    for fname in ("GBMutator.uc", "GBBot.uc", "GBLink.uc", "GBMath.uc"):
        src = _src(fname)
        code = "\n".join(line.split("//", 1)[0] for line in src.splitlines())
        assert " ? " not in code, f"{fname} has a ternary expression"


# --- off by default, and the admin toggle -----------------------------

def test_mutator_disabled_by_default():
    src = _src("GBMutator.uc")
    defaults = src[src.index("defaultproperties"):]
    assert re.search(r"bEnabled\s*=\s*False", defaults), \
        "GBMutator must default to bEnabled=False"


def test_mutate_command_toggles_enable():
    src = _src("GBMutator.uc")
    assert '"gb_enable 1"' in src
    assert '"gb_enable 0"' in src
    assert "StartGB()" in src
    assert "StopGB()" in src


def test_never_intercepts_the_servers_own_bots():
    """This adapter must spawn and own its roster, not reclassify the
    server's addbot/auto-added bots -- the control experiment (and the
    honesty of 'off by default') depends on there being no ambiguity about
    which bots are ours."""
    src = _src("GBMutator.uc")
    assert "AddBot(" not in src


# --- the fallback ordering ------------------------------------------------

def test_gbbot_calls_super_tick_before_applying_action():
    """This IS the fallback discipline: Super.Tick() runs the built-in AI
    first, unconditionally; ApplyAction only OVERWRITES its output, and only
    when a fresh policy answer exists. Reversing this order would mean the
    built-in AI stomps our action every tick, or -- worse -- that a disabled
    mutator still needs special-casing here instead of just not being called."""
    src = _src("GBBot.uc")
    super_idx = src.index("Super.Tick(DeltaTime)")
    apply_idx = src.index("Brain.ApplyAction(")
    assert super_idx < apply_idx


def test_apply_action_is_a_no_op_without_a_fresh_answer():
    src = _src("GBMutator.uc")
    body = src[src.index("function ApplyAction"):]
    body = body[:body.index("\nfunction ", 1)] if "\nfunction " in body[1:] else body
    assert "ActHave[Index] == 0" in body
    assert "return;" in body.split("\n\n")[0] or "return;" in body


def test_action_values_are_clamped_before_use():
    """The policy is not trusted -- same distrust gb_client.c's gb_clamp()
    applies on every other engine. Also the only thing standing between a
    NaN out of GBMath's undecoded-garbage case and the pawn's rotation."""
    src = _src("GBMutator.uc")
    on_action = src[src.index("function OnAction"):]
    assert "!= Pitch" in on_action or "Pitch != Pitch" in on_action, \
        "OnAction must test for NaN explicitly"
    assert "FClamp(Pitch" in on_action
    assert "FClamp(Fwd" in on_action


# --- the wire protocol -- UdpLink, hex-text framed --------------------
#
# History matters here: TcpLink never connects on this build (verified with
# `ss`), and raw binary over UdpLink sends fine but never delivers real
# content back through ReceivedBinary (verified with a known byte pattern
# that never appears in the received buffer). Hex-text over UdpLink is the
# transport that actually works, end to end, live -- see the README.

def test_link_extends_udplink_not_tcplink():
    link = _src("GBLink.uc")
    assert re.search(r"^class GBLink extends UdpLink;", link, re.M), (
        "GBLink must extend UdpLink -- TcpLink never connects on the build "
        "this was verified against, see the README's history section")


def test_link_uses_text_not_binary_send_receive():
    """SendBinary/ReceivedBinary are the pair that sends real bytes but never
    delivers real content back (Count is right, B is garbage) -- this
    adapter must use SendText/ReceivedText instead."""
    link = _src("GBLink.uc")
    assert "SendText(" in link
    assert "event ReceivedText(" in link
    assert "SendBinary(" not in link
    assert "event ReceivedBinary(" not in link


def test_link_binds_a_port_before_sending():
    """UdpLink still needs a local port bound before it can send/receive,
    the same real requirement TcpLink had."""
    link = _src("GBLink.uc")
    init_fn = link[link.index("function Init"):]
    init_fn = init_fn[:init_fn.index("\nfunction ", 1)]
    assert "BindPort()" in init_fn


def test_hex_codec_exists_in_gbmath():
    """The wire payload is hex-encoded ASCII text (2 chars/byte) because
    ReceivedBinary cannot deliver real content on this build -- GBMath must
    provide both directions of that codec."""
    math_src = _src("GBMath.uc")
    assert "static final function int HexChar(" in math_src
    assert "static final function int HexValue(" in math_src


def test_link_has_no_leftover_tcp_connection_state():
    """UDP is connectionless -- EnsureConnected/IsConnected/Open/Opened/
    Closed/bAwaitingResponse/reconnect-cooldown all belonged to the TCP
    design and must not still be here implying a connection that doesn't
    exist for this transport."""
    link = _src("GBLink.uc")
    for leftover in ("EnsureConnected", "bAwaitingResponse",
                     "RECONNECT_COOLDOWN", "function bool IsConnected",
                     "event Opened(", "event Closed("):
        assert leftover not in link, f"GBLink still has TCP-era {leftover!r}"


def test_mutator_judges_staleness_from_last_good_reply():
    """No connection state to ask "are we connected" -- freshness is judged
    from how long it has been since the last good reply, and a stale link
    must relinquish control (clear ActHave) rather than freeze bots on their
    last known action forever."""
    mutator = _src("GBMutator.uc")
    assert "LastGoodReplyTime" in mutator
    assert "ClearActions()" in mutator


def test_policy_port_matches_the_udp_endpoint_convention():
    """27300 is policyd's --udp-listen convention for this adapter (27200
    was the old, dead, TCP-endpoint default)."""
    mutator = _src("GBMutator.uc")
    defaults = mutator[mutator.index("defaultproperties"):]
    assert re.search(r"PolicyPort\s*=\s*27300", defaults)


def test_obs_entry_and_action_sizes_match_schema():
    """WriteBotObs/ParseActions hand-walk byte offsets; if OBS_DIM or
    ACTION_SIZE ever change shape, these need to change with them, and this
    at least pins today's values so a schema change is a visible diff here
    too, not just in GBSchema.uc."""
    assert schema.OBS_DIM == 144
    assert schema.ACTION_SIZE == 24
    assert schema.HEADER_SIZE == 16


# --- build.sh ------------------------------------------------------------

def test_build_regenerates_schema_before_compiling():
    sh = (_UT / "build.sh").read_text()
    assert "gen_gbschema.py" in sh
    assert "ucc make" in sh or "ucc-bin-amd64 make" in sh


def test_build_copies_compiled_output_to_the_install_tree():
    """ucc make's output lands in $HOME/.utpg/System/, not the install
    tree's own System/ -- confirmed empirically, documented in build.sh's
    header. A build script that forgets this copy silently ships nothing."""
    sh = (_UT / "build.sh").read_text()
    assert "GameBots.u" in sh
    assert ".utpg/System" in sh
