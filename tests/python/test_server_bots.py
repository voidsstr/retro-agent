"""The fleet bot convention on the dedicated game servers (2026-10-01).

The rule: **3 bots, "Hard", backfilling** - an empty server shows 3 bots and
each human who joins replaces one, wherever the engine can do that. Every
engine has its own difficulty scale, and copying a number from one to another
is the mistake this file exists to stop (CS:S 3 is EXPERT, not hard; jk_botti
counts 1 as the BEST). "Hard" is the named Hard where a scale has one
(YaPB 3, CS:S 2, Unreal 2, Quake 2) and otherwise the 4th of 5 rungs from the
easiest (Quake III/OA/JKA 4, 3zb2 7 of 0..9, KTX 15 of 0..20, Tribes 2 0.75).

These tests read the COMMITTED configs and install scripts - the files that
put the bots on the host - and pin both the numbers and the traps that were
paid for while installing them:

  * a semicolon in a Quake 2 or mvdsv cfg COMMENT splits the line, and the
    rest runs as a command (Q2 "Unknown command it", mvdsv "security hole");
  * 3zb2 upstream finds its data through the "game" cvar, which would switch
    every client into a 3zb2 gamedir it does not have;
  * Yamagi loads game.so beside its own binary first, so the 3zb2 game.so is
    only used when the unit runs a copy of the binary from the server tree;
  * one jk_botti stock bot name is a slur.

Run: pytest tests/python/test_server_bots.py
"""

import importlib.util
import os
import re

_HERE = os.path.dirname(os.path.abspath(__file__))
_GS = os.path.abspath(os.path.join(_HERE, "..", "..", "scripts", "game-servers"))


def _read(*parts):
    with open(os.path.join(_GS, *parts), encoding="latin-1") as fh:
        return fh.read()


def _cvars(text, setter=r"(?:seta?\s+)?"):
    """`[set|seta] name value` -> {name: value}, comments dropped, LAST wins."""
    out = {}
    for line in text.splitlines():
        line = line.split("//", 1)[0].strip()
        m = re.match(r"^" + setter + r"([A-Za-z_][\w]*)\s+\"?([^\"]*?)\"?\s*$", line)
        if m:
            out[m.group(1)] = m.group(2)
    return out


def _comment_lines(text):
    return [l for l in text.splitlines() if l.strip().startswith("//")]


# --- Counter-Strike: Source ----------------------------------------------

def test_css_fills_to_three_at_valve_hard():
    cv = _cvars(_read("css", "server.cfg"))
    assert cv["bot_quota"] == "3"
    assert cv["bot_quota_mode"] == "fill"     # backfill, not 3 fixed bots
    assert cv["bot_difficulty"] == "2"        # 0 easy 1 normal 2 HARD 3 expert


def test_css_binds_the_fleet_address_when_the_host_holds_it():
    """With the wired NIC on .132+.196 and Wi-Fi up on .129, the route source
    was Wi-Fi: srcds bound .129 and every probe of .132:27025 read DOWN."""
    s = _read("css", "run-css-server.sh")
    assert "192.168.1.132" in s
    assert s.index("FLEET_IP") < s.index("ip -4 route get")


# --- id Tech 3 family -------------------------------------------------------

def test_team_arena_backfills_three_hardcore_bots():
    cv = _cvars(_read("q3ta", "server.cfg"))
    assert cv["bot_enable"] == "1"
    assert cv["bot_minplayers"] == "3"
    assert cv["g_spSkill"] == "4"


def test_jedi_academy_backfills_three_bots():
    cv = _cvars(_read("jka", "server.cfg"))
    assert cv["bot_enable"] == "1"
    assert cv["bot_minplayers"] == "3"
    assert cv["g_spSkill"] == "4"


# --- Quake 2: 3zb2 ----------------------------------------------------------

def test_quake2_cfg_autospawns_the_fleet_botlist():
    cv = _cvars(_read("quake2", "server.cfg"))
    assert cv["botlist"] == "fleet"
    assert cv["autospawn"] == "1"
    assert cv["zb_path"] == "3zb2"
    assert cv["maplist"] == "q2dmx"            # 3zb2 ignores sv_maplist
    assert cv["zigmode"] == "0"                # pak6.pak content stays unused


def test_no_semicolon_in_a_quake2_comment():
    """The command buffer splits at ';' before it strips '//': the comment's
    tail ran as a command ("Unknown command it")."""
    bad = [l for l in _comment_lines(_read("quake2", "server.cfg")) if ";" in l]
    assert bad == []


def test_3zb2_fleet_botlist_is_three_hard_autospawned_bots():
    rows = [l for l in _read("quake2", "3zb2-fleet-bots.cfg").splitlines()
            if l.startswith("\\\\")]
    assert len(rows) == 3
    for row in rows:
        fields = [f.strip() for f in row[2:].split("\\")]
        params = [int(x) for x in fields[3:19]]
        assert len(params) == 16
        assert params[1] == 7 and params[4] == 7 and params[6] == 7  # aim/combat/reaction
        assert fields[-1] == "1"                                      # autospawn flag
        assert fields[1] in ("male", "female", "cyborg")              # retail models only


def test_3zb2_patch_keeps_clients_in_baseq2():
    p = _read("quake2", "3zb2-fleet.patch")
    assert '+\tgamepath = gi.cvar ("zb_path", "3zb2", 0);' in p
    assert '-\tgamepath = gi.cvar ("game", "0", CVAR_NOSET);' in p
    added = [l for l in p.splitlines() if l.startswith("+") and not l.startswith("+++")]
    assert not any("\\\\" in l for l in added)     # no Windows paths left


def test_quake2_unit_runs_a_binary_copy_from_the_server_tree():
    d = _read("quake2", "quake2-server-3zb2.conf")
    lines = [l for l in d.splitlines() if l.startswith("ExecStart")]
    assert lines[0] == "ExecStart="               # clear, then replace
    assert "/q2-server/quake2 " in lines[1]
    assert "/usr/lib/yamagi-quake2/quake2" not in lines[1]
    assert "stdbuf -oL" in lines[1]


# --- Quake 1: FrikBot X -----------------------------------------------------

def test_quake1_fleet_qc_backfills_three_at_quake_hard():
    q = _read("quake1", "fleet.qc")
    assert re.search(r"float FLEET_BOTS\s*=\s*3;", q)
    assert re.search(r"float FLEET_SKILL\s*=\s*2;", q)   # Quake's own Hard
    assert "KickABot()" in q and "BotConnect(" in q       # both directions


def test_quake1_runs_the_fbx_gamedir_and_no_exits():
    unit = _read("units", "quake1-server.service")
    assert "-game fbx" in unit
    cv = _cvars(_read("quake1", "server.cfg"), setter="")
    assert cv["noexit"] == "1"
    assert cv["rcon_password"] == "retroadmin"


def test_frikbot_build_wires_the_hooks_install_txt_lists():
    b = _read("quake1", "build-frikbot.sh")
    for hook in ("BotInit();", "BotFrame();", "FleetBotFrame();", "BotPreFrame()",
                 "BotPostFrame()", "ClientInRankings();", "ClientDisconnected();"):
        assert hook in b, hook
    assert "[ \\t]*" in b      # the builtin regex must not cross a newline


# --- QuakeWorld: KTX frogbots -----------------------------------------------

def test_quakeworld_ktx_backfills_to_three_at_skill_15():
    cv = _cvars(_read("quakeworld", "server.cfg"))
    assert cv["k_fb_enabled"] == "1"
    assert cv["k_fb_autoadd_limit"] == "3"
    assert cv["k_fb_autoremove_at"] == "3"
    assert cv["k_fb_skill"] == "15"
    assert cv["k_defmap"] == "dm4"            # "start" has no waypoints


def test_no_semicolon_in_a_quakeworld_comment():
    """mvdsv refused the tail of a comment line after ';' ("security hole")."""
    bad = [l for l in _comment_lines(_read("quakeworld", "server.cfg")) if ";" in l]
    assert bad == []


# --- Half-Life Deathmatch: jk_botti -----------------------------------------

def test_jk_botti_defaults_are_three_and_hard_on_its_inverted_scale():
    s = _read("hldm", "install-jk_botti.sh")
    assert 'MAXBOTS="${2:-3}"' in s
    assert 'SKILL="${3:-2}"' in s            # 1 is the BEST on jk_botti's scale
    assert "min_bots 0" in s and "max_bots {maxbots}" in s


def test_jk_botti_install_drops_the_slur_name():
    assert "/FragFag/d" in _read("hldm", "install-jk_botti.sh")


# --- Unreal Gold 226 --------------------------------------------------------

def test_unreal_gold_server_ini_turns_on_three_hard_bots():
    spec = importlib.util.spec_from_file_location(
        "make_server_ini", os.path.join(_GS, "unrealgold", "make_server_ini.py"))
    msi = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(msi)
    staged = "\r\n".join(["[URL]", "Port=7777", "", "[Engine.GameEngine]",
                          "ServerActors=IpDrv.UdpBeacon",
                          "ServerActors=IpServer.UdpServerQuery", ""])
    out = msi.server_ini(staged)
    assert "[UnrealShare.DeathMatchGame]\r\nbMultiPlayerBots=True\r\nInitialBots=3" in out
    assert "[UnrealShare.BotInfo]\r\nDifficulty=2" in out    # 0 Easy .. 2 Hard .. 3 Unreal
    assert msi.server_ini(out) == out
