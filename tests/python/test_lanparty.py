#!/usr/bin/env python3
"""scripts/fleet/lanparty.py - the whole-fleet multiplayer round tool.

What these pin, each a way a party run could report success while proving
nothing:

  * a title file that fails to parse must be an ERROR, never a silent fall back
    to the seven built-in titles (the rest would then read as "untested");
  * a per-box launcher variant (the Win98 box's COMMAND.COM .bat, a Voodoo
    variant) must win over the title's '*' launcher, and a box with no entry at
    all must not be launched;
  * {name} and {host} are substituted and every line ends CRLF;
  * the packet witness counts only packets aimed at THE server port on THE
    host - another game's traffic in the same capture is not a join;
  * `kill` reaches every process name any title runs, the hosting box's server
    included, so a round cannot leave a game running into the next one.
"""
import importlib.util, json, os, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
spec = importlib.util.spec_from_file_location("lanparty", os.path.join(REPO, "scripts", "fleet", "lanparty.py"))
lp = importlib.util.module_from_spec(spec)
spec.loader.exec_module(lp)


def _titles(obj):
    fd, p = tempfile.mkstemp(suffix=".json")
    with os.fdopen(fd, "w") as f:
        json.dump(obj, f)
    return p


def test_builtins_survive_without_a_title_file():
    g = lp.load_titles("/nonexistent/lanparty_titles.json")
    assert {"q3", "q2", "cs", "ut99", "ut2004", "ssam-tfe", "ssam-tse"} <= set(g)


def test_a_malformed_title_file_is_an_error_not_a_fallback():
    fd, p = tempfile.mkstemp(suffix=".json")
    with os.fdopen(fd, "w") as f:
        f.write("{ not json")
    try:
        lp.load_titles(p)
    except ValueError:
        return
    raise AssertionError("a broken title file silently fell back to the built-ins")


def test_json_titles_merge_and_underscore_keys_are_comments():
    p = _titles({"_note": "x", "d3": {"title": "Descent 3", "exe": ["main.exe"], "bats": [{"boxes": ["*"], "bat": "x"}]}})
    g = lp.load_titles(p)
    assert "d3" in g and "_note" not in g and "q3" in g


def test_per_box_variant_beats_star_and_a_box_without_an_entry_is_skipped():
    t = {"bats": [{"boxes": ["*"], "bat": "XP"}, {"boxes": ["192.168.1.243"], "bat": "W98"}]}
    assert lp.bat_for(t, 243) == "W98"
    assert lp.bat_for(t, 110) == "XP"
    only = {"bats": [{"boxes": ["243"], "bat": "W98"}]}
    assert lp.bat_for(only, 110) is None
    assert lp.applicable(only, [110, 243]) == [243]


def test_render_substitutes_and_ends_every_line_crlf():
    b = lp.render_bat("@echo off\nstart x +name {name} +connect {host}", 171, "192.168.1.123").decode()
    assert "+name Fleet171" in b and "+connect 192.168.1.123" in b
    assert b.endswith("\r\n") and "\n" not in b.replace("\r\n", "")


def test_host_ip_is_the_hosting_box_for_peer_titles():
    assert lp.host_ip_of({"host": {"box": "123"}}) == "192.168.1.123"
    assert lp.host_ip_of({}) == lp.HOST


def test_packet_count_only_counts_the_server_port_on_the_host():
    lines = [
        "12:00:00.1 IP 192.168.1.110.27005 > 192.168.1.132.27961: UDP, length 40",
        "12:00:00.2 IP 192.168.1.110.27005 > 192.168.1.132.27961: UDP, length 40",
        "12:00:00.3 IP 192.168.1.123.27005 > 192.168.1.132.27961: UDP, length 40",
        "12:00:00.4 IP 192.168.1.124.27005 > 192.168.1.132.27910: UDP, length 40",   # another server
        "12:00:00.5 IP 192.168.1.133.27005 > 192.168.1.240.27961: UDP, length 40",   # not the host
    ]
    assert lp.count_packets(lines, "192.168.1.132", 27961) == {"192.168.1.110": 2, "192.168.1.123": 1}


def test_kill_list_covers_every_title_and_the_host_server():
    p = _titles({"halo": {"exe": ["halo.exe"], "host": {"box": "123", "exe": ["haloded.exe"], "bat": "x"},
                          "bats": [{"boxes": ["*"], "bat": "y"}]}})
    names = lp.all_game_exes(lp.load_titles(p))
    for n in ("halo.exe", "haloded.exe", "quake3.exe", "hl.exe", "seriousSam.exe".lower()):
        assert n in names


# ---- 2026-11-11 readiness rounds: Win98 route, graceful close, rosters that name boxes

def test_a_single_bat_title_is_never_handed_to_the_win98_box():
    # the built-in titles carry one cmd.exe launcher; COMMAND.COM cannot run it
    assert lp.bat_for({"bat": "@echo off\nstart \"\" x.exe"}, 243) is None
    assert lp.bat_for({"bat": "x"}, 110) == "x"


def test_win98_launcher_is_an_8_3_batch_started_by_start_exe():
    p = lp.bat_path("Quake2Win9x", 243)
    assert p == "C:\\FLEETMP\\QUAKE2WI.BAT"
    assert lp.launch_cmd(243, p) == "LAUNCH C:\\WINDOWS\\COMMAND\\START.EXE C:\\FLEETMP\\QUAKE2WI.BAT"
    assert lp.bat_path("Quake2Win9x", 110) == "C:\\fleetmp\\Quake2Win9x.bat"
    assert lp.launch_cmd(110, "C:\\fleetmp\\x.bat") == 'LAUNCH "C:\\fleetmp\\x.bat"'


def test_process_names_compare_on_the_basename_because_win9x_reports_paths():
    assert lp.base("C:\\GAMES\\QUAKE2WIN9X\\QUAKE2.EXE") == "quake2.exe"
    assert lp.base("quake2.exe") == "quake2.exe"


GOLDSRC_STATUS = """hostname:  Retro Fleet CS
version : 48/1.1.2.7/Stdio 10211 secure
#      name userid uniqueid frag time ping loss adr
# 1 "Brandon" 2 BOT   0 12:34   0    0
# 2 "(1)Jucator - www.bcs16.ro" 5 STEAM_ID_LAN   0 00:40  12    0 192.168.1.110:27005
#3 "Fleet123" 6 4294967295 1 0:30 10 0 192.168.1.123:27005
# 4 "Fleet240" 1 4294967295 0 1:00 0 0 loopback
"""


def test_goldsrc_status_separates_bots_and_carries_each_box_address():
    rows = lp.parse_goldsrc_status(GOLDSRC_STATUS)
    assert ("Brandon", 0, True, None) in rows
    # this repack ignores +name - the ADDRESS still says which box it is
    assert ("(1)Jucator - www.bcs16.ro", 12, False, "192.168.1.110") in rows
    assert ("Fleet240", 0, False, None) in rows          # listen-server host: loopback
    assert lp.on_server(110, rows) and lp.on_server(123, rows) and lp.on_server(240, rows)
    assert not lp.on_server(133, rows)


def test_a_bot_is_never_counted_as_a_box_on_the_server():
    assert not lp.on_server(110, [("Fleet110", 0, True, None)])
    assert not lp.on_server(110, [("__info__", 3, 16)])


def test_tribes2_info_reply_counts_humans_and_strips_colour_codes():
    def s(x):
        return bytes([len(x)]) + x
    content = "2\nStorm\t0\nInferno\t0\n2\n\x10\x0eNova-9\t0\t0\n\x10\x0cFleet110\t0\t0".encode("latin1")
    pkt = (bytes([0x14, 0x02]) + b"KEYS" + s(b"Classic") + s(b"Capture the Flag") + s(b"Katabatic")
           + bytes([1, 2, 32, 1]) + b"\x00\x00" + s(b"info") + bytes([len(content) & 255, len(content) >> 8]) + content)
    rows = lp.parse_t2_info(pkt)
    assert ("Nova-9", None, True, None) in rows
    assert ("Fleet110", None, False, None) in rows
    assert ("__info__", 1, 32) in rows
    assert lp.parse_t2_info(b"\x10junk") is None


class FakeConn:
    """Answers PROCLIST from a script of process lists and records every command."""
    def __init__(self, lists, fg_pid=None):
        self.lists, self.sent, self.fg = list(lists), [], fg_pid

    async def command_text(self, cmd):
        self.sent.append(cmd)
        if cmd == "PROCLIST":
            return json.dumps(self.lists.pop(0) if len(self.lists) > 1 else self.lists[0])
        if cmd == "WINLIST":
            return json.dumps({"foreground": {"pid": self.fg}})
        return "OK"


def _run(coro):
    import asyncio
    return asyncio.new_event_loop().run_until_complete(coro)


def test_nt_close_is_wm_close_first_and_force_only_what_survives(monkeypatch):
    async def nosleep(_):
        return None
    monkeypatch.setattr(lp.asyncio, "sleep", nosleep)
    game = [{"pid": 7, "name": "quake2.exe"}, {"pid": 9, "name": "explorer.exe"}]
    c = FakeConn([game, game, []])            # still running after one poll, gone after the next
    out = _run(lp.kill_games(c, 124, ["quake2.exe"]))
    assert c.sent[1] == 'EXEC cmd /c taskkill /im "quake2.exe"'      # no /f
    assert not any(x.startswith("PROCKILL") for x in c.sent)
    assert out == ["quake2.exe (closed)"]
    c = FakeConn([game])                       # never goes: forced after the grace
    out = _run(lp.kill_games(c, 110, ["quake2.exe"], grace=4))
    assert "PROCKILL 7" in c.sent and "PROCKILL 9" not in c.sent
    assert out == ["quake2.exe (forced)"]


def test_win98_close_uses_the_console_quit_when_the_game_has_the_focus(monkeypatch):
    async def nosleep(_):
        return None
    monkeypatch.setattr(lp.asyncio, "sleep", nosleep)
    game = [{"pid": 5, "name": "E:\\GAMES\\QUAKE2WIN9X\\QUAKE2.EXE"}]
    c = FakeConn([game, []], fg_pid=5)
    out = _run(lp.kill_games(c, 243, ["quake2.exe"], quit9x=["TILDE", "TEXT:quit", "RETURN"]))
    assert ["UIKEY TILDE", "UIKEY TEXT:quit", "UIKEY RETURN"] == [x for x in c.sent if x.startswith("UIKEY")]
    assert out == ["console quit"] and not any("taskkill" in x for x in c.sent)
    c = FakeConn([game], fg_pid=99)            # not focused: no keys typed into another window
    out = _run(lp.kill_games(c, 243, ["quake2.exe"], quit9x=["TILDE"]))
    assert not any(x.startswith("UIKEY") for x in c.sent)
    assert "PROCKILL 5" in c.sent and "LAUNCH C:\\RETRO_AGENT\\GLRESET9.EXE" in c.sent


def test_d3_pilot_refuses_a_library_file_whose_callsign_moved(tmp_path, monkeypatch):
    d = tmp_path / "Descent3"
    d.mkdir()
    (d / "sdf.plt").write_bytes(b"\x2b\x00\x00\x00sdf\x00" + b"\x01" * 20)
    monkeypatch.setattr(lp, "LIB", str(tmp_path))
    plt = lp.gen_file("d3pilot", 110)
    assert plt[4:13] == b"Fleet110\x00" and plt[13:] == b"\x01" * 20 and len(plt) == 28 + 5
    (d / "sdf.plt").write_bytes(b"\x2b\x00\x00\x00xyz\x00" + b"\x01" * 20)
    try:
        lp.gen_file("d3pilot", 110)
    except ValueError:
        return
    raise AssertionError("built a pilot from a file whose layout changed")


def test_the_generated_title_file_parses_and_every_box_gets_one_launcher():
    g = lp.load_titles()
    for key, t in g.items():
        if "bats" not in t:
            continue
        for b in t["bats"]:
            for o in b["boxes"]:
                assert lp.bat_for(t, int(o)) is not None, (key, o)
                lp.render_bat(lp.bat_for(t, int(o)), int(o), lp.host_ip_of(t))   # ASCII, placeholders
        if "243" in [str(x) for x in t.get("boxes", [])] and "bats" in t:
            assert lp.bat_for(t, 243) is not None, key
