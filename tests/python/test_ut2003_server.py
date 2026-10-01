"""The UT2003 2225 server on the dev host (2026-10-01).

`ut2003-server` runs the staged UT2003 tree's own System\\UCC.exe under Wine
in docker (scripts/game-servers/ut2003/). These tests pin what was measured
while standing it up, each of which would otherwise silently break it:

  * a UE2 server holds game, game+1 (native query) AND game+10 (GameSpy), and
    UT2004 owns 7777/7778/7787 -- so the brief's "7787/7788" would have
    collided; the server sits on 7757/7758(/7767);
  * the server ini is DERIVED from the staged client ini and keeps the
    MasterServerUplink actor (the LAN responder lives in it) and
    LANServerPort 10777 (where the clients broadcast);
  * the probe speaks the UE2 NATIVE query and checks the net version
    (121 = UT2003, 128 = UT2004), because a UT2004 reply answers the same
    packet perfectly and admits no UT2003 client;
  * the type-0 player count includes bots;
  * the container blocks on `wineserver -w`.
"""

import importlib.util
import os
import re
import sys

import pytest

_HERE = os.path.dirname(os.path.abspath(__file__))
_REPO = os.path.abspath(os.path.join(_HERE, "..", ".."))
_GS = os.path.join(_REPO, "scripts", "game-servers")
_U3 = os.path.join(_GS, "ut2003")
_UNITS = os.path.join(_GS, "units")


def _load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


msi = _load("ut2003_make_server_ini", os.path.join(_U3, "make_server_ini.py"))
gs = _load("gameservers_for_ut2003", os.path.join(_GS, "gameservers.py"))


def _read(*parts):
    with open(os.path.join(*parts), encoding="utf-8") as fh:
        return fh.read()


# Captured from the live servers on this host, 2026-10-01.
UT2003_INFO = (b'y\x00\x00\x00\x00\x00\x00\x00\x00\x00M\x1e\x00\x00\x00\x00\x00\x00'
               b'\x1fNSC Retro Fleet Arena (UT2003)\x00\x0bDM-Antalus\x00'
               b'\x0cxDeathMatch\x00\x06\x00\x00\x00\x0c\x00\x00\x00'
               b'\x00\x00\x00\x00\x00\x00')
UT2003_RULES = (b'y\x00\x00\x00\x01\x0bservermode\x00\ndedicated\x00\nadminname\x00'
                b'\x00\x0badminemail\x00\x00\x0eServerVersion\x00\x052225\x00'
                b'\ngamestats\x00\x06false\x00\ngoalscore\x00\x0325\x00'
                b'\ntimelimit\x00\x0315\x00\x0bminplayers\x00\x026\x00'
                b'\rtranslocator\x00\x06false\x00\x0bweaponstay\x00\x06false\x00')
UT2004_INFO = (b'\x80\x00\x00\x00\x00\x00\x00\x00\x00\x00a\x1e\x00\x00\x00\x00\x00\x00'
               b'\x16NSC Retro Fleet Arena\x00\nDM-Rankin\x00\x0cxDeathMatch\x00'
               b'\x00\x00\x00\x00\x0c\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00'
               b'\x020\x00\x00\x00')

# A trimmed copy of the staged System\UT2003.ini: the sections the generator
# touches, with CRLF endings like the real file.
STAGED = "\r\n".join([
    "[URL]",
    "Protocol=unreal",
    "Map=Index.ut2",
    "Port=7777",
    "",
    "[Engine.GameEngine]",
    "CacheSizeMegs=128",
    "ServerActors=IpDrv.MasterServerUplink",
    "ServerActors=UWeb.WebServer",
    "ServerPackages=Core",
    "",
    "[Engine.GameReplicationInfo]",
    "ServerName=Another UT2003 Server",
    "ShortName=UT2 Server",
    "",
    "[Engine.AccessControl]",
    "AdminPassword=",
    "",
    "[Engine.GameInfo]",
    "MaxPlayers=32",
    "bEnableStatLogging=true",
    "",
    "[XGame.xDeathMatch]",
    "TimeLimit=0",
    "GoalScore=25",
    "",
    "[IpDrv.MasterServerLink]",
    "LANPort=11777",
    "LANServerPort=10777",
    "MasterServerAddress[0]=utmaster.openspy.net",
    "",
    "[XInterface.MapListDeathMatch]",
    "Maps=DM-Antalus",
    "",
    "[XInterface.ServerBrowser]",
    "bOnlyShowStandard=False",
    "",
])


def _ini(text):
    out, cur = {}, None
    for line in text.replace("\r\n", "\n").split("\n"):
        m = re.match(r"^\[([^\]]+)\]$", line)
        if m:
            cur = m.group(1)
            out[cur] = []
        elif cur and line.strip():
            out[cur].append(line)
    return out


# --- ports -------------------------------------------------------------------

def _ue2_ports(game):
    return {game, game + 1, game + 10}


def test_the_port_clears_ut2004s_three_ports():
    """Measured with `ss -ulpn`: ut2004-server binds 7777, 7778 AND 7787
    (game + 10, its GameSpy port). 7787 -- the obvious pick -- collides."""
    assert msi.GAME_PORT == 7757
    assert not (_ue2_ports(msi.GAME_PORT) & _ue2_ports(7777))
    assert 7787 in _ue2_ports(7777), "the trap this test exists for"


def test_the_port_clears_every_other_server_on_the_host():
    taken = set()
    for row in gs.SERVERS:
        if row["unit"] == "ut2003-server":
            continue
        taken |= {row["port"], row["join"]}
    taken |= set(range(7797, 7807)) | {8777, 7775}   # UT99 uplinks, UG beacon
    assert not (_ue2_ports(msi.GAME_PORT) & taken)


def test_the_server_table_row_queries_game_plus_one():
    row = next(r for r in gs.SERVERS if r["unit"] == "ut2003-server")
    assert row["join"] == 7757 and row["port"] == 7758
    assert row["probe"] == "ut2003" and gs.PROBES["ut2003"] is gs.probe_ut2003
    assert row.get("slow_start_sec", 0) >= 60, "Wine + level load takes ~30 s"


# --- the server ini ------------------------------------------------------------

def test_server_ini_sets_port_uplink_admin_name_and_bots():
    out = _ini(msi.server_ini(STAGED))
    assert "Port=7757" in out["URL"]
    up = out["IpDrv.MasterServerUplink"]
    for k in ("DoUplink=False", "UplinkToGamespy=False", "SendStats=False"):
        assert k in up
    assert "AdminPassword=retroadmin" in out["Engine.AccessControl"]
    assert "ServerName=NSC Retro Fleet Arena (UT2003)" in out["Engine.GameReplicationInfo"]
    assert "bEnableStatLogging=False" in out["Engine.GameInfo"]
    assert "MinPlayers=6" in out["XGame.xDeathMatch"]
    maps = [l.split("=", 1)[1] for l in out["XInterface.MapListDeathMatch"]]
    assert maps == msi.DM_ROTATION and len(maps) == 15


def test_server_ini_keeps_the_lan_responder():
    """The LAN reply lives in MasterServerUplink and answers on 10777 whatever
    DoUplink says -- dropping the actor would make the server invisible."""
    out = _ini(msi.server_ini(STAGED))
    assert "ServerActors=IpDrv.MasterServerUplink" in out["Engine.GameEngine"]
    assert "LANServerPort=10777" in out["IpDrv.MasterServerLink"]


def test_server_ini_refuses_a_tree_that_would_be_invisible():
    with pytest.raises(ValueError, match="MasterServerUplink"):
        msi.server_ini(STAGED.replace("ServerActors=IpDrv.MasterServerUplink\r\n", ""))
    with pytest.raises(ValueError, match="LANServerPort"):
        msi.server_ini(STAGED.replace("LANServerPort=10777", "LANServerPort=10787"))


def test_server_ini_leaves_everything_else_and_is_idempotent():
    once = msi.server_ini(STAGED)
    assert msi.server_ini(once) == once
    assert "\r\n" in once, "CRLF kept"
    out = _ini(once)
    assert out["XInterface.ServerBrowser"] == ["bOnlyShowStandard=False"]
    assert "MasterServerAddress[0]=utmaster.openspy.net" in out["IpDrv.MasterServerLink"]


def test_the_rotation_names_only_stock_dm_maps():
    assert all(m.startswith("DM-") for m in msi.DM_ROTATION)
    inst = _read(_U3, "install.sh")
    assert "rotation map" in inst, "install.sh checks each map exists in the tree"


# --- the probe -------------------------------------------------------------------

def test_parse_the_live_ut2003_info_reply():
    info = gs.parse_ue2_info(UT2003_INFO)
    assert info == {"netver": 121, "port": 7757,
                    "name": "NSC Retro Fleet Arena (UT2003)", "map": "DM-Antalus",
                    "gametype": "xDeathMatch", "players": 6, "max_players": 12}


def test_parse_the_live_rules_reply():
    rules = gs.parse_ue2_rules(UT2003_RULES)
    assert rules["serverversion"] == "2225" and rules["minplayers"] == "6"


def test_a_ut2004_reply_is_not_a_ut2003_server():
    """Same packet, same shape, different build: 128 admits no UT2003 client."""
    assert gs.parse_ue2_info(UT2004_INFO)["netver"] == 128
    assert "UT2004" in gs.ut2003_join_problem(128, "3369")
    assert gs.ut2003_join_problem(121, "2225") is None
    assert gs.ut2003_join_problem(121, None), "unreadable build must not read as joinable"
    assert gs.ut2003_join_problem(121, "2199")


def test_probe_counts_bots_inside_players(monkeypatch):
    """Six bots, no human: the info count is 6, the player list is empty
    (measured: no type-2 reply at all). players stays the total and bots the
    difference -- collect()'s convention -- so humans come out as 0."""
    replies = {0: [UT2003_INFO], 1: [UT2003_RULES], 2: []}
    monkeypatch.setattr(gs, "_ue2_ask", lambda port, t, timeout, host: (replies[t], 5.0))
    out = gs.probe_ut2003(7758)
    assert out["players"] == 6 and out["bots"] == 6
    assert out["version"] == "2225" and "problem" not in out
    assert out["players"] - out["bots"] == 0


def test_probe_reports_ut2004_on_the_ut2003_port_as_a_problem(monkeypatch):
    replies = {0: [UT2004_INFO], 1: [], 2: []}
    monkeypatch.setattr(gs, "_ue2_ask", lambda port, t, timeout, host: (replies[t], 5.0))
    out = gs.probe_ut2003(7758)
    assert out["problem"], "collect() must read this as NOT up"


def test_probe_returns_none_when_nothing_answers(monkeypatch):
    monkeypatch.setattr(gs, "_ue2_ask", lambda *a: ([], None))
    assert gs.probe_ut2003(7758) is None


# --- container, unit, registration -----------------------------------------------

def test_the_container_blocks_on_wineserver():
    entry = _read(_U3, "entry.sh")
    assert re.search(r"^wine UCC\.exe server .*&\s*$", entry, re.M | re.S)
    assert re.search(r"^wineserver -w\s*$", entry, re.M)
    assert "ini=UT2003Server.ini" in entry
    assert 'UT3_PORT="${UT3_PORT:-7757}"' in entry
    assert "xvfb-run" not in entry.split("wine UCC.exe")[1], "console program: no X"


def test_the_container_is_on_the_host_network_as_the_tree_owner():
    run = _read(_U3, "run-ut2003-server.sh")
    assert "--net=host" in run and "--init" in run
    assert '--user "$(id -u):$(id -g)"' in run
    assert 'NAME="${NAME:-ut2003srv}"' in run


def test_the_unit_follows_the_wine_docker_pattern():
    unit = _read(_UNITS, "ut2003-server.service")
    assert "ExecStart=/home/voidsstr/ut2003-server/_run/run-ut2003-server.sh" in unit
    assert "ExecStop=/usr/bin/docker stop -t 10 ut2003srv" in unit
    assert "Restart=always" in unit and "WantedBy=default.target" in unit
    assert "StandardOutput=append:/home/voidsstr/ut2003-server/server.log" in unit


def test_install_verifies_the_copy_by_count_and_bytes():
    inst = _read(_U3, "install.sh")
    assert "files/bytes" in inst and "exit 1" in inst
    assert "enable ut2003-server" in inst


def test_healthcheck_checks_the_net_version():
    hc = _read(_GS, "healthcheck.py")
    assert re.search(r'\("ut2003-server",.*7758, ut2003\)', hc)
    assert "netver != 121" in hc


def test_the_favourites_agent_pins_it_with_the_native_probe():
    """No query_port: masters.probe_server then uses the UE2 native probe on
    game+1 with its netver-121 filter, not a GameSpy probe on a port this
    server never binds."""
    sys.path.insert(0, os.path.join(_REPO, "scripts", "gameindex"))
    try:
        sync = _load("gameindex_sync_for_ut2003",
                     os.path.join(_REPO, "scripts", "gameindex", "sync.py"))
    finally:
        sys.path.pop(0)
    row = next(s for s in sync.LOCAL_SERVERS if s.get("gamename") == "ut2003")
    assert row["engine"] == "ut2k3" and row["port"] == 7757
    assert "query_port" not in row
