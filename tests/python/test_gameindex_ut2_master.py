"""UT2004 and UT2003 internet favourites (2026-09-29).

Until then the favourites agent had NO internet source for either game
("no live UT2004 master"), so a UT2004 box got only the fleet's own server and
a UT2003 box got nothing at all - its target was filed under the UT2004 engine
and filtered for a gamename no server in that bucket carried.

  * discovery: the OpenSpy UT master (utmaster.openspy.net:28902, Epic's TCP
    master protocol as openspy-core code/utmaster implements it) - clients
    "UT2K4CLIENT" 3369 and "CLIENT" 2225;
  * verification: the in-game browser's own query on game port + 1, whose
    reply starts with net version 128 (UT2004) or 121 (UT2003);
  * UT2003's favourites live in a DIFFERENT section from UT2004's:
    [XInterface.Browser_ServerListPageFavorites] in UT2003.ini, not
    [XInterface.ExtendedConsole] (read from UT2003's XInterface.u).
"""
import os
import socket
import struct
import sys
import threading

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "scripts"))
from gameindex import favorites, masters  # noqa: E402

# Captured 2026-09-29: the fleet's own UT2004 (.132:7778) and a live UT2003.
FLEET_UT2004 = (b'\x80\x00\x00\x00\x00\x00\x00\x00\x00\x00a\x1e\x00\x00\x00\x00'
                b'\x00\x00\x16NSC Retro Fleet Arena\x00\nDM-Deck17\x00\x0cxDeathMatch'
                b'\x00\x00\x00\x00\x00\x0c\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00'
                b'\x020\x00\x00\x00')
LIVE_UT2003 = (b"y\x00\x00\x00\x00\x00\x00\x00\x00\x00a\x1e\x00\x00\x00\x00\x00\x00'"
               b"\x1b```Teu\x1b\xff\x01\x01ton\x1b\xff\xba\x01ica\x1b\x01\x7f\x0e V11 (UT2003)"
               b"\x00\x18DM-(Extr)-Stallwart-XXL\x00\x0cxDeathMatch\x00\x06\x00\x00\x00"
               b"\x10\x00\x00\x00\x00\x00\x00\x00\x00\x00")


def _fake_udp(reply):
    return lambda host, port, payload, **kw: (reply, 42)


def test_native_probe_reads_the_fleet_ut2004_reply(monkeypatch):
    monkeypatch.setattr(masters, "_udp", _fake_udp(FLEET_UT2004))
    r = masters._ut2k4_native_probe("192.168.1.132:7777")
    assert r["hostname"] == "NSC Retro Fleet Arena" and r["map"] == "DM-Deck17"
    assert (r["players"], r["maxplayers"], r["gamename"]) == (0, 12, "ut2004")
    assert r["addr"] == "192.168.1.132:7777" and r["query_port"] == 7778


def test_native_probe_reads_a_ut2003_reply_and_strips_colour_codes(monkeypatch):
    monkeypatch.setattr(masters, "_udp", _fake_udp(LIVE_UT2003))
    r = masters._ut2k3_native_probe("146.52.84.66:7777")
    assert r["hostname"] == "Teutonica V11 (UT2003)"
    assert (r["players"], r["maxplayers"], r["gamename"]) == (6, 16, "ut2003")


def test_native_probe_keeps_the_two_games_apart(monkeypatch):
    # A UT2003 server must never land in UT2004's favourites, or the reverse:
    # the client cannot join the other game's servers.
    monkeypatch.setattr(masters, "_udp", _fake_udp(LIVE_UT2003))
    assert masters._ut2k4_native_probe("146.52.84.66:7777") is None
    monkeypatch.setattr(masters, "_udp", _fake_udp(FLEET_UT2004))
    assert masters._ut2k3_native_probe("192.168.1.132:7777") is None


def test_fstring_utf16_and_latin1():
    utf16 = masters._ue_cint(-4) + "Aé€".encode("utf-16-le") + b"\0\0"
    assert masters._ue_read_fstring(utf16, 0)[0] == "Aé€"
    assert masters._ue_read_fstring(masters._ue_fstring("hello"), 0) == ("hello", 7)


def _fake_master(servers, expect_client):
    """A one-connection UT master speaking openspy-core's utmaster protocol."""
    srv = socket.socket()
    srv.bind(("127.0.0.1", 0))
    srv.listen(1)
    seen = {}

    def pkt(c, body):
        c.sendall(struct.pack("<I", len(body)) + body)

    def rpkt(c):
        n = struct.unpack("<I", c.recv(4, socket.MSG_WAITALL))[0]
        return c.recv(n, socket.MSG_WAITALL)

    def run():
        c, _ = srv.accept()
        pkt(c, masters._ue_fstring("111111111"))
        b = rpkt(c)
        _, i = masters._ue_read_fstring(b, 0)
        _, i = masters._ue_read_fstring(b, i)
        seen["client"], i = masters._ue_read_fstring(b, i)
        seen["version"] = struct.unpack("<I", b[i:i + 4])[0]
        ok = seen["client"] == expect_client
        pkt(c, masters._ue_fstring("APPROVED" if ok else "DENIED"))
        if ok and seen["version"] >= 3000:
            rpkt(c)
            pkt(c, masters._ue_fstring("VERIFIED"))
        if ok:
            seen["request"] = rpkt(c)
            pkt(c, struct.pack("<I", len(servers)) + b"\x01")
            for ip, port in servers:
                pkt(c, socket.inet_aton(ip) + struct.pack("<HH", port, port + 1)
                    + masters._ue_fstring("n") + masters._ue_fstring("m")
                    + masters._ue_fstring("g") + b"\x01\x08")
        c.close()
        srv.close()

    threading.Thread(target=run, daemon=True).start()
    return srv.getsockname()[1], seen


def test_master_list_ut2004_client():
    port, seen = _fake_master([("1.2.3.4", 7777), ("5.6.7.8", 7787)], "UT2K4CLIENT")
    got = masters._ut2_master_list("ut2k4", host="127.0.0.1", port=port, timeout=5)
    assert got == ["1.2.3.4:7777", "5.6.7.8:7787"]
    assert seen["version"] == 3369 and seen["request"] == b"\x00\x00"


def test_master_list_ut2003_client():
    port, seen = _fake_master([("9.9.9.9", 7777)], "CLIENT")
    got = masters._ut2_master_list("ut2k3", host="127.0.0.1", port=port, timeout=5)
    assert got == ["9.9.9.9:7777"] and seen["version"] == 2225


def test_master_list_denied_or_unreachable_is_empty_not_an_error():
    port, _ = _fake_master([("1.2.3.4", 7777)], "SOMETHING-ELSE")
    assert masters._ut2_master_list("ut2k4", host="127.0.0.1", port=port, timeout=5) == []
    assert masters._ut2_master_list("ut2k4", host="127.0.0.1", port=1, timeout=2) == []


def test_both_engines_are_discoverable_now():
    for eng in ("ut2k4", "ut2k3"):
        spec = masters.ENGINES[eng]
        assert spec["supported"] and spec["list"] is not None and spec["probe"]


UT2003_INI = ("[Engine.Engine]\r\nRenderDevice=D3DDrv.D3DRenderDevice\r\n\r\n"
              "[xInterface.ExtendedConsole]\r\nConsoleHotKey=192\r\n")


def _row(addr, name, players=3):
    return {"addr": addr, "query_port": 0, "hostname": name, "players": players,
            "maxplayers": 16, "ping_ms": 50, "gamename": "ut2003"}


def test_ut2003_favourites_go_to_its_own_section():
    pol = favorites.policy_for("ut2003")
    assert pol["engine"] == "ut2k3" and pol["filename"] == "UT2003.ini"
    text, _ = favorites.render("ut2k3", [_row("9.9.9.9:7777", "Teutonica")],
                               UT2003_INI, key="ut2003")
    lines = text.splitlines()
    sec = lines.index("[XInterface.Browser_ServerListPageFavorites]")
    assert lines[sec + 1] == ('Favorites=(ServerID=0,IP="9.9.9.9",Port=7777,'
                              'QueryPort=7778,ServerName="Teutonica")')
    # Nothing written under UT2004's section, nothing of the file lost.
    ext = lines.index("[xInterface.ExtendedConsole]")
    assert not lines[ext + 1].startswith("Favorites=")
    assert "RenderDevice=D3DDrv.D3DRenderDevice" in lines
    assert favorites.incumbents("ut2k3", text) == {"9.9.9.9:7777"}
    assert favorites.same_favourites("ut2k3", text, text)


def test_ut2004_target_is_unchanged():
    pol = favorites.policy_for("ut2004")
    assert pol["engine"] == "ut2k4" and pol["filename"] == "UT2004.ini"
    assert favorites.UT2K4_SECTION == "XInterface.ExtendedConsole"
