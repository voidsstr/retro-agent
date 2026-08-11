"""Source invariants for the CS 1.6 no-blood mod (scripts/game-servers/cs16-noblood).

The mod works by dropping blood temp entities inside the SVC_TEMPENTITY hook.
That is a sharp tool: the same hook carries *every* visual effect in the game,
so a careless edit either stops suppressing blood or deletes bullet holes,
sparks and smoke along with it. Neither failure throws — the server starts
happily and just looks wrong — so it can only be caught by reading the source.

Verified live on 2026-08-11: Metamod-P + AMXX loaded the compiled plugin and
`noblood_stats` answered. These tests lock in the shape that produced that.
"""

import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
MOD = ROOT / "scripts" / "game-servers" / "cs16-noblood"
SMA = MOD / "plugin" / "noblood.sma"
CFG = MOD / "cfg" / "server.cfg"
VANILLA_CFG = ROOT / "scripts" / "game-servers" / "cs16-vanilla" / "cfg" / "server.cfg"


def _sma():
    assert SMA.is_file(), f"missing plugin source: {SMA}"
    return SMA.read_text()


def test_hooks_the_tempentity_message():
    """Without the SVC_TEMPENTITY registration the plugin loads but does nothing."""
    src = _sma()
    assert re.search(r"#define\s+SVC_TEMPENTITY\s+23", src), \
        "SVC_TEMPENTITY must be 23 (GoldSrc engine message id)"
    assert re.search(r"register_message\s*\(\s*SVC_TEMPENTITY", src), \
        "plugin must register_message(SVC_TEMPENTITY, ...)"


def test_blocks_exactly_the_three_blood_temp_entities():
    """101/103/115 are the whole of 'blood' in GoldSrc - all three, no more."""
    src = _sma()
    for name, num in (("TE_BLOODSTREAM", 101), ("TE_BLOOD", 103), ("TE_BLOODSPRITE", 115)):
        assert re.search(rf"#define\s+{name}\s+{num}\b", src), \
            f"{name} must be defined as {num}"
        assert name in src.split("public on_temp_entity")[1], \
            f"{name} must appear in the temp-entity handler's blocked set"


def test_never_blocks_gunshot_decals():
    """TE_GUNSHOTDECAL (109) is bullet holes, not blood. Blocking it would strip
    every bullet impact from the map and is the most tempting wrong edit here."""
    src = _sma()
    assert "109" not in re.sub(r"^\s*[/*].*$", "", src, flags=re.M), \
        "109 (TE_GUNSHOTDECAL) must never be blocked - that is bullet holes"
    assert "TE_GUNSHOTDECAL" not in src


def test_no_blanket_message_block():
    """set_msg_block on SVC_TEMPENTITY would kill every temp entity in the game
    (sparks, smoke, glass, explosions), not just blood."""
    src = _sma()
    assert "set_msg_block" not in src, \
        "must filter per temp-entity type, never blanket-block SVC_TEMPENTITY"


def test_handler_falls_through_for_everything_else():
    """Anything that is not blood must reach the client untouched."""
    src = _sma()
    handler = src.split("public on_temp_entity")[1]
    assert "PLUGIN_CONTINUE" in handler, \
        "non-blood temp entities must return PLUGIN_CONTINUE"
    assert "PLUGIN_HANDLED" in handler, \
        "blood temp entities must return PLUGIN_HANDLED (dropped)"


def test_exposes_a_verification_hook():
    """noblood_stats/noblood_version are how the mod is proven live over rcon
    without a human staring at a screen. Keep them."""
    src = _sma()
    assert "noblood_stats" in src and "register_srvcmd" in src
    assert "noblood_version" in src and "register_cvar" in src


def test_both_server_cfgs_enable_lan_mode():
    """sv_lan 1 disables Steam auth - it is the only reason the fleet's
    non-Steam BCS 1.6 clients can connect at all."""
    for cfg in (CFG, VANILLA_CFG):
        assert cfg.is_file(), f"missing config: {cfg}"
        assert re.search(r"^\s*sv_lan\s+1\s*$", cfg.read_text(), flags=re.M), \
            f"{cfg.name} must set sv_lan 1"


def test_compiled_plugin_is_committed():
    """The server box has no compiler in its deploy path, so the .amxx ships."""
    amxx = MOD / "dist" / "noblood.amxx"
    assert amxx.is_file(), "dist/noblood.amxx must be committed alongside the source"
    assert amxx.stat().st_size > 200, "compiled plugin looks truncated"
