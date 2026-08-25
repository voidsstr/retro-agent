"""gameindex — the host half of the game/server favourites pipeline.

Covers the three places this can go quietly wrong:

  * the "only refresh if there are changes" contract, which is the whole point
    of the design and is invisible when it breaks -- a broken hash comparison
    just means every box gets rewritten every five minutes,
  * the favourites renderer, which must MERGE rather than overwrite (someone's
    r_mode/com_maxfps settings live in the same file) and must blank unused
    slots (a stale address otherwise haunts the in-game list forever), and
  * the status-reply parser, which had exactly this bug: it read the wrong
    line and reported "0 alive of 400" for servers that all answered.
"""
import pathlib
import sys

import pytest

REPO = pathlib.Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(REPO / "scripts" / "gameindex"))

import db          # noqa: E402
import favorites   # noqa: E402
import masters     # noqa: E402


@pytest.fixture()
def con(tmp_path):
    c = db.connect(str(tmp_path / "t.db"))
    yield c
    c.close()


def srv(addr, players=4, ping=50, local=0, name=""):
    return {"addr": addr, "hostname": name or f"srv {addr}", "map": "q3dm6",
            "players": players, "maxplayers": 16, "ping_ms": ping,
            "gamename": "baseq3", "passworded": 0, "is_local": local,
            "source": "test"}


# --- the status parser that was silently dropping every Q3 server ------------

def test_q3_style_reply_is_parsed_from_the_right_line():
    # Real shape: header line, THEN the infostring, THEN player lines.
    data = (b"\xff\xff\xff\xffstatusResponse\n"
            b"\\mapname\\pro-q3dm6\\sv_hostname\\Test\\sv_maxclients\\16\n"
            b'5 42 "alpha"\n7 13 "bravo"\n')
    info, players = masters._split_status(data)
    assert info["mapname"] == "pro-q3dm6"
    assert info["sv_hostname"] == "Test"
    assert players == 2, "player LINES are counted, not a cvar"


def test_qw_style_reply_with_infostring_on_the_first_line_still_works():
    data = b"\xff\xff\xff\xffn\\hostname\\QW\\map\\dm4\\maxclients\\16\n1 2 \"x\"\n"
    info, players = masters._split_status(data)
    assert info["hostname"] == "QW" and info["map"] == "dm4"
    assert players == 1


def test_a_reply_with_no_infostring_is_not_mistaken_for_a_live_server():
    assert masters._split_status(b"\xff\xff\xff\xffgarbage\n") == ({}, 0)
    assert masters._split_status(b"") == ({}, 0)


def test_unsupported_engines_say_so_instead_of_returning_empty():
    rows, note = masters.discover("unreal")
    assert rows == []
    assert "unsupported" in note, (
        "'we never looked' must not be reported the same as 'none found'")


# --- favourites rendering ----------------------------------------------------

def test_q3_render_writes_seta_server_slots():
    text, h = favorites.render("q3", [srv("1.2.3.4:27960"), srv("5.6.7.8:27961")])
    assert 'seta server1 "1.2.3.4:27960"' in text
    assert 'seta server2 "5.6.7.8:27961"' in text
    assert len(h) == 16


def test_unused_slots_are_blanked():
    text, _ = favorites.render("q3", [srv("1.2.3.4:27960")])
    assert 'seta server16 ""' in text, (
        "a stale address in an unwritten slot stays in the favourites list "
        "forever otherwise")


def test_existing_settings_are_preserved_and_our_block_replaced():
    existing = ('seta r_mode "4"\n'
                'seta com_maxfps "125"\n'
                f'{favorites.BEGIN}\n'
                'seta server1 "9.9.9.9:27960"\n'
                f'{favorites.END}\n')
    text, _ = favorites.render("q3", [srv("1.2.3.4:27960")], existing)
    assert 'seta r_mode "4"' in text, "someone tuned that; do not eat it"
    assert 'seta com_maxfps "125"' in text
    assert "9.9.9.9" not in text, "the old managed block must be replaced"
    assert text.count(favorites.BEGIN) == 1, "blocks must not accumulate"


def test_loose_seta_server_lines_outside_our_block_are_also_cleared():
    existing = 'seta server3 "6.6.6.6:27960"\nseta r_gamma "1"\n'
    text, _ = favorites.render("q3", [srv("1.2.3.4:27960")], existing)
    assert "6.6.6.6" not in text
    assert 'seta r_gamma "1"' in text


def test_rendering_is_stable_so_an_unchanged_list_produces_an_unchanged_hash():
    servers = [srv("1.2.3.4:27960"), srv("5.6.7.8:27961")]
    a = favorites.render("q3", servers)[1]
    b = favorites.render("q3", servers)[1]
    assert a == b, "an unstable hash would rewrite every box every cycle"


def test_a_different_server_list_changes_the_hash():
    a = favorites.render("q3", [srv("1.2.3.4:27960")])[1]
    b = favorites.render("q3", [srv("9.9.9.9:27960")])[1]
    assert a != b


def test_q2_uses_the_address_book_cvars():
    text, _ = favorites.render("q2", [srv("1.2.3.4:27910")])
    assert 'set adr0 "1.2.3.4:27910"' in text


def test_engine_without_a_writer_reports_why():
    text, why = favorites.render("goldsrc", [srv("1.2.3.4:27015")])
    assert text is None and "verification" in why


def test_target_path_lands_in_the_engine_subdir():
    p = favorites.target_path("q3", "C:\\Program Files\\Quake III Arena")
    assert p == "C:\\Program Files\\Quake III Arena\\baseq3\\autoexec.cfg"
    assert favorites.target_path("q3", "C:\\Quake3\\") == \
        "C:\\Quake3\\baseq3\\autoexec.cfg", "a trailing slash must not double up"


# --- the change-detection contract -------------------------------------------

def test_machine_hash_change_is_what_signals_a_reindex(con):
    assert db.record_machine(con, "10.0.0.1", index_hash="aaaa") is True
    assert db.record_machine(con, "10.0.0.1", index_hash="aaaa") is False, \
        "same hash must NOT report a change, or every pass re-pulls every box"
    assert db.record_machine(con, "10.0.0.1", index_hash="bbbb") is True


def test_applied_hash_round_trips(con):
    assert db.applied_hash(con, "10.0.0.1", "quake3", "C:\\Q3") is None
    db.record_applied(con, "10.0.0.1", "quake3", "C:\\Q3", "deadbeef")
    assert db.applied_hash(con, "10.0.0.1", "quake3", "C:\\Q3") == "deadbeef"


def test_replace_games_removes_what_is_no_longer_installed(con):
    db.replace_games(con, "10.0.0.1", [
        {"key": "quake3", "dir": "C:\\Q3", "engine": "q3"},
        {"key": "quake2", "dir": "C:\\Q2", "engine": "q2"}])
    assert len(db.games_for(con, ip="10.0.0.1")) == 2
    db.replace_games(con, "10.0.0.1", [
        {"key": "quake3", "dir": "C:\\Q3", "engine": "q3"}])
    rows = db.games_for(con, ip="10.0.0.1")
    assert len(rows) == 1 and rows[0]["game_key"] == "quake3", \
        "an uninstalled game must disappear, not linger"


# --- server selection --------------------------------------------------------

def test_local_servers_are_pinned_above_busier_internet_ones(con):
    db.upsert_servers(con, "q3", [
        srv("8.8.8.8:27960", players=30, ping=10),
        srv("192.168.1.132:27961", players=0, ping=1, local=1),
    ])
    best = db.best_servers(con, "q3")
    assert best[0]["addr"] == "192.168.1.132:27961", \
        "ours goes first even when empty - it is the one that is always joinable"


def test_one_host_cannot_eat_every_slot(con):
    # A big host runs eight ports of the same server; without the dedupe it
    # fills all 16 favourites with itself.
    db.upsert_servers(con, "q3", [srv(f"4.4.4.4:2796{i}", players=20 - i)
                                  for i in range(8)] +
                                 [srv("5.5.5.5:27960", players=5)])
    best = db.best_servers(con, "q3")
    hosts = [r["addr"].rsplit(":", 1)[0] for r in best]
    assert len(hosts) == len(set(hosts)), "must be deduped by host IP"
    assert "5.5.5.5" in hosts


def test_empty_internet_servers_are_not_offered(con):
    db.upsert_servers(con, "q3", [srv("8.8.8.8:27960", players=0)])
    assert db.best_servers(con, "q3") == [], \
        "the request was servers WITH players on them"
