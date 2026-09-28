"""The Unreal Gold 226 server on the dev host (2026-09-28).

The fleet's Unreal Gold server used to be OldUnreal 227k, which no staged
client could join: the staged UnrealGold tree is 226 Final, and a 226 client
refuses 227 packages ("Package 'UnrealI' version mismatch", measured on .124).
It was replaced by the staged tree's own System\\UCC.exe under Wine in docker
(scripts/game-servers/unrealgold/). These tests pin the pieces that made that
work, each of which was a trap or a measured requirement:

  * the server ini is DERIVED from the staged client ini, and changes only
    the port, the dead master uplinks, the admin password and the name;
  * the ports stay 7807/7808, so favourites and retro-gameindex stay valid;
  * the container blocks on `wineserver -w` (a bare `wine` returns at once);
  * the 227k unit is kept for rollback and is never enabled beside the 226 one.
"""

import importlib.util
import os
import re

import pytest

_HERE = os.path.dirname(os.path.abspath(__file__))
_REPO = os.path.abspath(os.path.join(_HERE, "..", ".."))
_UG = os.path.join(_REPO, "scripts", "game-servers", "unrealgold")
_UNITS = os.path.join(_REPO, "scripts", "game-servers", "units")


def _load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


msi = _load("make_server_ini", os.path.join(_UG, "make_server_ini.py"))


def _read(*parts):
    with open(os.path.join(*parts), encoding="utf-8") as fh:
        return fh.read()


# A trimmed copy of the staged System\Unreal.ini: the sections the generator
# touches, with the staged tree's real ServerActors lines and CRLF endings.
STAGED = "\r\n".join([
    "[URL]",
    "Protocol=unreal",
    "Map=Index.unr",
    "Port=7777",
    "Class=UPak.UPakFemaleOne",
    "",
    "[Engine.GameEngine]",
    "CacheSizeMegs=4",
    "ServerActors=IpDrv.UdpBeacon",
    "ServerActors=IpServer.UdpServerQuery",
    "ServerActors=IpServer.UdpServerUplink MasterServerAddress=master0.gamespy.com MasterServerPort=27900",
    "ServerActors=IpServer.UdpServerUplink MasterServerAddress=www.epicgames.com MasterServerPort=27900",
    "ServerActors=IpServer.UdpServerUplink MasterServerAddress=master.telefragged.com MasterServerPort=27500",
    "ServerPackages=UPak",
    "",
    "[IpDrv.UdpBeacon]",
    "DoBeacon=True",
    "BeaconPort=7776",
    "",
    "[Engine.GameInfo]",
    "bLowGore=False",
    "",
    "[UBrowserAll]",
    "ListFactories[0]=UBrowser.UBrowserSubsetFact,SupersetTag=UBrowserLAN",
    "",
])


def _ini(text):
    """{section: [lines]} -- enough to assert on."""
    out, cur = {}, None
    for line in text.replace("\r\n", "\n").split("\n"):
        m = re.match(r"^\[([^\]]+)\]$", line)
        if m:
            cur = m.group(1)
            out[cur] = []
        elif cur and line.strip():
            out[cur].append(line)
    return out


def test_server_ini_moves_the_game_port_to_7807():
    """7777 is UT2004 on this host. UdpServerQuery takes the next free port,
    so 7807 puts the query on 7808 -- where every favourite already points."""
    out = _ini(msi.server_ini(STAGED))
    assert "Port=7807" in out["URL"]
    assert "Port=7777" not in out["URL"]


def test_server_ini_drops_the_dead_masters_and_keeps_lan_discovery():
    out = _ini(msi.server_ini(STAGED))
    actors = [l for l in out["Engine.GameEngine"] if l.startswith("ServerActors=")]
    assert actors == ["ServerActors=IpDrv.UdpBeacon",
                      "ServerActors=IpServer.UdpServerQuery"]
    assert "ServerPackages=UPak" in out["Engine.GameEngine"]


def test_server_ini_names_the_server_and_sets_the_fleet_admin_password():
    out = _ini(msi.server_ini(STAGED))
    assert "ServerName=NSC Retro Fleet Arena (Unreal Gold)" in out["Engine.GameReplicationInfo"]
    assert "ShortName=NSC Unreal" in out["Engine.GameReplicationInfo"]
    assert "AdminPassword=retroadmin" in out["Engine.GameInfo"]
    assert "bLowGore=False" in out["Engine.GameInfo"]


def test_server_ini_leaves_everything_else_byte_for_byte():
    """The server must run what the clients run; only four things differ."""
    out = msi.server_ini(STAGED)
    assert "\r\n" in out and "\n" not in out.replace("\r\n", "")
    removed = set(STAGED.split("\r\n")) - set(out.split("\r\n"))
    assert removed == {
        "Port=7777",
        "ServerActors=IpServer.UdpServerUplink MasterServerAddress=master0.gamespy.com MasterServerPort=27900",
        "ServerActors=IpServer.UdpServerUplink MasterServerAddress=www.epicgames.com MasterServerPort=27900",
        "ServerActors=IpServer.UdpServerUplink MasterServerAddress=master.telefragged.com MasterServerPort=27500",
    }


def test_server_ini_is_idempotent():
    once = msi.server_ini(STAGED)
    assert msi.server_ini(once) == once


def test_server_ini_refuses_a_tree_that_would_be_invisible():
    """No UdpServerQuery = nothing answers on 7808 = the wall says DOWN for a
    server that is running. Fail at install time instead."""
    broken = STAGED.replace("ServerActors=IpServer.UdpServerQuery\r\n", "")
    with pytest.raises(ValueError, match="UdpServerQuery"):
        msi.server_ini(broken)


def test_the_container_blocks_on_wineserver():
    """`wine X.exe` returns as soon as wineserver owns the process; a unit
    whose ExecStart is only that reads `active` while nothing hosts."""
    entry = _read(_UG, "entry.sh")
    assert re.search(r"^wine UCC\.exe server .*&\s*$", entry, re.M | re.S)
    assert re.search(r"^wineserver -w\s*$", entry, re.M)
    assert "-ini=UnrealServer.ini" in entry
    assert 'UG_PORT="${UG_PORT:-7807}"' in entry


def test_the_container_is_on_the_host_network_as_the_tree_owner():
    run = _read(_UG, "run-ug-server.sh")
    assert "--net=host" in run, "the fleet reaches it by LAN address and beacon"
    assert '--user "$(id -u):$(id -g)"' in run, (
        "as root, UCC leaves root-owned logs/ini in ~/unrealgold-server")
    assert re.search(r"--name \"\$NAME\"", run) and 'NAME="${NAME:-ugsrv}"' in run


def test_the_unit_runs_the_226_server_and_stops_its_container():
    unit = _read(_UNITS, "unrealgold-server.service")
    assert "ExecStart=/home/voidsstr/unrealgold-server/_run/run-ug-server.sh" in unit
    assert "ExecStop=/usr/bin/docker stop -t 10 ugsrv" in unit
    assert "ucc-bin-amd64" not in unit.split("[Service]")[1]
    assert "WantedBy=default.target" in unit


def test_the_227k_unit_is_kept_for_rollback_and_installed_disabled():
    """Both bind 7807/7808/7775. install.sh must disable the old one, and the
    old one must still be the exact 227k command so a rollback is one step."""
    old = _read(_UNITS, "unrealgold227-server.service")
    assert "ucc-bin-amd64 server DmDeck16.unr" in old
    assert "ROLLBACK ONLY" in old
    inst = _read(_UG, "install.sh")
    assert "disable --now unrealgold227-server" in inst
    assert "enable unrealgold-server" in inst


def test_install_verifies_the_copy_by_count_and_bytes():
    """A copy's exit code has lied on this project before (xcopy, gvfs)."""
    inst = _read(_UG, "install.sh")
    assert "files/bytes" in inst and "exit 1" in inst


def test_healthcheck_checks_the_unreal_gold_version_too():
    hc = _read(_REPO, "scripts", "game-servers", "healthcheck.py")
    assert re.search(r'\("unrealgold-server",.*unreal226\)', hc)
