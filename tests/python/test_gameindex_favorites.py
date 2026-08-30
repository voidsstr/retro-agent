r"""The favourites writers for the engines beyond Quake -- and the per-title
policy that decides which of the fleet's own servers a title may be given.

Why this file exists
--------------------
The agent covered the Quake family. The staged library now carries thirty-odd
titles, and extending it turned up four failures that all LOOK like success in
the log -- the pass reports "wrote N servers" every time:

  * **Every local server but one was dropped.** `best_servers` deduped by host
    IP to stop a big internet host eating all 16 slots. All ten fleet servers
    live on 192.168.1.132, so a box was given Quake III *or* OpenArena, CS 1.6
    *or* the no-blood server -- never both.
  * **The path was wrong for half the Quake III family.** Soldier of Fortune II
    and Jedi Academy keep their game data in `base`, not `baseq3`, so the
    writer created a directory the game never reads.
  * **The Unreal path doubled.** The agent indexes a game by where it found the
    executable, and for every Unreal-engine title that is `System\`, so
    appending `System` again produced a path that cannot exist.
  * **Unreal Gold and Deus Ex were about to be handed UT99 servers.** They are
    the same engine and a completely different game.

None of that is visible from the host, which is why it is tested rather than
remembered. The formats themselves were read out of the games' own files --
UBrowser.u's bytecode, XInterface.u's struct, and revSrvBrowser.dll's printf
template -- and the tests assert against those, not against folklore.
"""
import pathlib
import re
import sys

import pytest

REPO = pathlib.Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(REPO / "scripts" / "gameindex"))
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

import db          # noqa: E402
import favorites   # noqa: E402

from test_gameindex_staged_library import (  # noqa: E402
    STAGED_LIBRARY, signature_rows)


@pytest.fixture()
def con(tmp_path):
    c = db.connect(str(tmp_path / "t.db"))
    yield c
    c.close()


def row(addr, gamename, name="a server", players=4, local=0, source="test",
        query_port=0, ping=20):
    return {"addr": addr, "hostname": name, "map": "", "players": players,
            "maxplayers": 16, "ping_ms": ping, "gamename": gamename,
            "passworded": 0, "is_local": local, "source": source,
            "query_port": query_port}


UT99 = [row("192.168.1.132:7797", "ut", "NSC Retro Fleet Arena (UT99)",
            local=1, query_port=7798)]


# --- Unreal engine 1: UT99 / Unreal Gold / Deus Ex ---------------------------

def test_unreal_favourites_use_the_QUERY_port_not_the_game_port():
    # From UBrowser.u: Query() reads option 2 and passes it to FoundServer as
    # the query port, and the fleet's UT99 is 7797 game / 7798 query. Writing
    # the game port here produces a favourite that never pings.
    text, _ = favorites.render("unreal", UT99, "", key="ut99")
    assert r"Favorites[0]=NSC Retro Fleet Arena (UT99)\192.168.1.132\7798\False" \
        in text
    assert "7797" not in text


def test_unreal_query_port_is_carried_not_guessed():
    # UT2004 on the fleet is 7777 game / 7787 query -- +10, not +1. Any code
    # that derives the query port breaks on exactly the server we own.
    r = row("192.168.1.132:7777", "ut2004", query_port=7787)
    assert favorites._query_port(r) == 7787
    # ...and a row from before the column existed still degrades sensibly.
    assert favorites._query_port(row("1.2.3.4:7777", "ut")) == 7778


def test_unreal_writes_favoritecount_and_the_terminator():
    text, _ = favorites.render("unreal", UT99, "", key="ut99")
    assert "FavoriteCount=1" in text
    assert "Favorites[1]=" in text, \
        "SaveFavorites() terminates the list with an empty entry"


def test_unreal_merge_touches_nothing_but_the_favourites_keys():
    existing = ("[WinDrv.WindowsClient]\n"
                "WindowedViewportX=800\n"
                "StartupFullscreen=True\n"
                "\n"
                "[UBrowser.UBrowserFavoritesFact]\n"
                "FavoriteCount=2\n"
                r"Favorites[0]=old one\9.9.9.9\7778\False" "\n"
                r"Favorites[1]=old two\8.8.8.8\7778\False" "\n"
                "\n"
                "[Engine.Engine]\n"
                "GameRenderDevice=OpenGLDrv.OpenGLRenderDevice\n")
    text, _ = favorites.render("unreal", UT99, existing, key="ut99")
    assert "StartupFullscreen=True" in text, "somebody set that; do not eat it"
    assert "GameRenderDevice=OpenGLDrv.OpenGLRenderDevice" in text
    assert "[Engine.Engine]" in text
    assert "9.9.9.9" not in text and "8.8.8.8" not in text, \
        "the old favourites are ours to replace"
    assert text.count("[UBrowser.UBrowserFavoritesFact]") == 1


def test_unreal_leaves_a_backslash_in_a_server_name_from_shifting_the_fields():
    # The game parses the line by splitting on backslashes.
    srv = [row("1.2.3.4:7777", "ut", name=r"evil\name", query_port=7778)]
    text, _ = favorites.render("unreal", srv, "", key="ut99")
    line = [l for l in text.splitlines() if l.startswith("Favorites[0]=")][0]
    assert line.count("\\") == 3, f"exactly three separators, got {line!r}"


# --- UT2004 ------------------------------------------------------------------

def test_ut2k4_carries_both_ports_because_its_struct_has_both():
    srv = [row("192.168.1.132:7777", "ut2004", "NSC Retro Fleet Arena",
               local=1, query_port=7787)]
    text, _ = favorites.render("ut2k4", srv, "", key="ut2004")
    assert ('Favorites=(ServerID=0,IP="192.168.1.132",Port=7777,'
            'QueryPort=7787,ServerName="NSC Retro Fleet Arena")') in text


def test_ut2k4_replaces_its_own_lines_and_keeps_the_rest_of_the_section():
    existing = ("[XInterface.ExtendedConsole]\n"
                "ConsoleHotKey=192\n"
                'Favorites=(ServerID=0,IP="9.9.9.9",Port=7777,QueryPort=7778,'
                'ServerName="old")\n'
                "bSpeechMenuUseLetters=False\n")
    srv = [row("192.168.1.132:7777", "ut2004", local=1, query_port=7787)]
    text, _ = favorites.render("ut2k4", srv, existing, key="ut2004")
    assert "ConsoleHotKey=192" in text
    assert "bSpeechMenuUseLetters=False" in text
    assert "9.9.9.9" not in text


# --- GoldSrc -----------------------------------------------------------------

CS = [row("192.168.1.132:27015", "cstrike", "NSC Retro Fleet Arena (CS 1.6)",
          local=1)]
EMPTY_VDF = '"filters"\n{\n\t"favorites"\n\t{\n\t}\n\n\t"history"\n\t{\n\t}\n\n}\n'


def test_goldsrc_writes_the_four_keys_revsrvbrowser_itself_writes():
    text, _ = favorites.render("goldsrc", CS, EMPTY_VDF, key="cs16")
    for key in ("name", "address", "lastplayed", "appID"):
        assert f'"{key}"' in text, f"revSrvBrowser.dll writes {key}"
    assert '"192.168.1.132:27015"' in text
    assert '"10"' in text, "Counter-Strike 1.6 is appID 10"


def test_goldsrc_leaves_the_history_block_alone():
    # "history" is the person's own list of where they have played.
    with_history = EMPTY_VDF.replace(
        '"history"\n\t{\n\t}',
        '"history"\n\t{\n\t\t"0"\n\t\t{\n\t\t\t"name"\t\t"somewhere else"\n'
        '\t\t\t"address"\t"5.5.5.5:27015"\n\t\t}\n\t}')
    text, _ = favorites.render("goldsrc", CS, with_history, key="cs16")
    assert "somewhere else" in text and "5.5.5.5:27015" in text


def test_goldsrc_refuses_to_rewrite_a_file_it_cannot_parse():
    # A vdf we cannot read back exactly is a vdf we must not replace.
    with pytest.raises(favorites.WouldClobber):
        favorites.render("goldsrc", CS, '"filters" { oh dear', key="cs16")


def test_goldsrc_hash_is_stable_so_it_does_not_rewrite_every_five_minutes():
    # `lastplayed` is a %u timestamp in the game's own format string. Putting
    # a real clock reading there would change the file every pass and rewrite
    # every box forever.
    a = favorites.render("goldsrc", CS, EMPTY_VDF, key="cs16")[1]
    b = favorites.render("goldsrc", CS, EMPTY_VDF, key="cs16")[1]
    assert a == b


def test_the_appid_follows_the_mod_not_the_engine():
    assert favorites.GOLDSRC_APPID["cs16"] == 10
    assert favorites.GOLDSRC_APPID["tfc"] == 20
    assert favorites.GOLDSRC_APPID["dod"] == 30


# --- where the file goes -----------------------------------------------------

def test_the_unreal_system_directory_is_not_appended_twice():
    # The agent reports dir as the directory the EXE was found in, which for
    # every Unreal-engine title is System\. Appending blindly produced
    # ...\System\System\UnrealTournament.ini.
    assert favorites.target_path("unreal", r"C:\Games\UnrealTournament\System",
                                 "ut99") == \
        r"C:\Games\UnrealTournament\System\UnrealTournament.ini"
    # ...and it still works if a box ever reports the game root instead.
    assert favorites.target_path("unreal", r"C:\Games\UnrealTournament",
                                 "ut99") == \
        r"C:\Games\UnrealTournament\System\UnrealTournament.ini"


def test_each_unreal_engine_game_gets_its_own_ini():
    got = {k: favorites.target_path("unreal", rf"C:\Games\X\System", k)
           for k in ("ut99", "unreal", "deusex")}
    assert got["ut99"].endswith("UnrealTournament.ini")
    assert got["unreal"].endswith("Unreal.ini")
    assert got["deusex"].endswith("DeusEx.ini")


def test_the_two_ut99_trees_resolve_to_their_own_files():
    # Both are key ut99; only the directory tells them apart, and both must be
    # written -- the 436 tree exists for the three boxes that cannot run 469e.
    a = favorites.target_path("unreal", r"C:\Games\UnrealTournament\System", "ut99")
    b = favorites.target_path("unreal", r"C:\Games\UnrealTournament436\System", "ut99")
    assert a != b and "436" in b


def test_a_quake3_engine_game_whose_data_dir_is_not_baseq3_is_not_written():
    # SoF2 and Jedi Academy use `base`. Writing baseq3\autoexec.cfg into them
    # created a directory the game never reads -- a favourites file that could
    # never have had any effect, reported as a success.
    for key in ("sof2", "jka", "jk2"):
        pol = favorites.policy_for(key, "q3")
        assert not pol["supported"], key
        assert "fleet" in pol["why"] or "master" in pol["why"], key


# --- the per-title policy ----------------------------------------------------

def test_half_life_is_not_pointed_at_servers_it_cannot_join():
    pol = favorites.policy_for("halflife", "goldsrc")
    assert not pol["supported"]
    assert "46" in pol["why"] and "48" in pol["why"], (
        "the reason must name the protocol mismatch, or the next person "
        "'fixes' this by adding a writer")


def test_a_file_we_only_update_is_never_created():
    # The absence of config\serverbrowser.vdf is the evidence that a build
    # does not use that browser -- a WON Half-Life at C:\Sierra\Half-Life has
    # no revSrvBrowser at all. Same for an .ini holding only a favourites
    # section.
    for key in ("cs16", "ut99", "ut2004", "deusex"):
        assert favorites.policy_for(key)["create"] is False, key
    # autoexec.cfg is the opposite: not existing is its normal state.
    for key in ("quake3", "quake2"):
        assert favorites.policy_for(key)["create"] is True, key


def test_nothing_is_written_into_a_benchmark_harness():
    # The user stopped this service because a foreign write mid-test makes a
    # result unattributable. C:\q3bench is a real Quake III install.
    assert favorites.SKIP_DIRS.search(r"C:\q3bench")
    assert favorites.SKIP_DIRS.search(r"D:\Benchmarks\Quake3")
    assert not favorites.SKIP_DIRS.search(r"C:\Games\Quake3-TeamArena")


def test_every_staged_multiplayer_title_has_an_explicit_answer():
    """No staged title may fall through to the generic 'unknown key' reason.

    This is the coverage assertion the whole task turns on: for each title in
    the staged library, the favourites agent must either WRITE it or say, in
    its own words, why there is nothing honest to write. The generic fallback
    means nobody has looked at that title yet.
    """
    exe_to_key = {r["exe"].lower(): r["key"] for r in signature_rows()}
    unexamined = []
    for title, exe in sorted(STAGED_LIBRARY.items()):
        key = exe_to_key.get(exe.lower())
        assert key, f"{title}: {exe} has no signature"
        pol = favorites.policy_for(key)
        if pol["supported"]:
            continue
        if "no verified favourites mechanism" in pol["why"]:
            unexamined.append(f"{title} (key {key})")
    assert not unexamined, (
        "these staged titles have no explicit favourites answer: "
        + ", ".join(unexamined))


def test_the_reasons_are_reasons_and_not_placeholders():
    for key, why in favorites.UNWRITABLE.items():
        assert len(why) > 25, f"{key}: {why!r} does not explain anything"


def test_engines_for_keys_finds_what_the_agent_calls_nothing():
    # The agent reports Deus Ex with engine "-", so without this the pass
    # never fetches Unreal servers for a box whose only Unreal title is Deus
    # Ex.
    assert "unreal" in favorites.engines_for_keys(["deusex"])
    assert favorites.engines_for_keys(["starcraft"]) == []


# --- server selection --------------------------------------------------------

def test_our_own_servers_are_not_deduped_against_each_other(con):
    # All ten fleet servers share one IP. Deduping by host gave a box exactly
    # one of them.
    db.upsert_servers(con, "goldsrc", [
        row("192.168.1.132:27015", "cstrike", local=1),
        row("192.168.1.132:27016", "cstrike", local=1),
        row("192.168.1.132:27017", "ts", local=1)])
    got = {r["addr"] for r in db.best_servers(con, "goldsrc")}
    assert len(got) == 3, f"all three of ours must survive, got {got}"


def test_a_title_is_never_given_our_server_for_a_different_mod(con):
    db.upsert_servers(con, "goldsrc", [
        row("192.168.1.132:27015", "cstrike", local=1),
        row("192.168.1.132:27017", "ts", local=1)])
    cs = [r["addr"] for r in db.best_servers(con, "goldsrc", accepts={"cstrike"})]
    assert cs == ["192.168.1.132:27015"], \
        "a Counter-Strike client joining the Specialists server is rejected"


def test_the_filter_reaches_seeded_servers_too(con):
    # Unreal Gold and Deus Ex are the same ENGINE as UT99 and a different
    # game. The seed list is ours, so we know what is in it.
    db.upsert_servers(con, "unreal", [
        row("139.162.235.20:7777", "ut", players=17, source="seed"),
        row("192.168.1.132:7797", "ut", local=1, query_port=7798)])
    assert db.best_servers(con, "unreal", accepts={"unreal"}) == [], \
        "Unreal Gold cannot join a UT99 server"
    assert len(db.best_servers(con, "unreal", accepts={"ut"})) == 2


def test_the_filter_does_not_reach_a_masters_output(con):
    # We have no reliable mod taxonomy for the internet, so a permissive list
    # is better than one silently emptied.
    db.upsert_servers(con, "q3", [
        row("8.8.8.8:27960", "cpma", players=12, source="q3master")])
    assert len(db.best_servers(con, "q3", accepts={"baseq3"})) == 1


def test_the_rendered_order_does_not_move_when_player_counts_do(con):
    """Selection is by liveliness; OUTPUT is by address.

    Ordering the file by player count means it changes whenever anyone joins
    a server anywhere in the world, the applied-hash check never matches, and
    every box is rewritten every five minutes -- the exact cost the whole
    "only if it changed" design exists to avoid. Measured on .171 before this
    was fixed: two passes ninety seconds apart rewrote Quake III and both UT99
    trees purely from reordering.
    """
    db.upsert_servers(con, "q3", [row("3.3.3.3:27960", "baseq3", players=5),
                                  row("1.1.1.1:27960", "baseq3", players=9),
                                  row("2.2.2.2:27960", "baseq3", players=7)])
    first = [r["addr"] for r in db.best_servers(con, "q3")]
    # the same servers, every player count shuffled
    db.upsert_servers(con, "q3", [row("3.3.3.3:27960", "baseq3", players=9),
                                  row("1.1.1.1:27960", "baseq3", players=5),
                                  row("2.2.2.2:27960", "baseq3", players=6)])
    assert [r["addr"] for r in db.best_servers(con, "q3")] == first == \
        ["1.1.1.1:27960", "2.2.2.2:27960", "3.3.3.3:27960"]


def test_ours_still_comes_first_whatever_its_address(con):
    db.upsert_servers(con, "q3", [row("1.1.1.1:27960", "baseq3", players=30),
                                  row("192.168.1.132:27961", "baseq3",
                                      players=0, local=1)])
    assert db.best_servers(con, "q3")[0]["addr"] == "192.168.1.132:27961"


def test_query_port_survives_a_round_trip_through_the_db(con):
    db.upsert_servers(con, "unreal", [
        row("192.168.1.132:7797", "ut", local=1, query_port=7798)])
    assert db.best_servers(con, "unreal")[0]["query_port"] == 7798


def test_a_database_made_before_query_port_existed_is_migrated(tmp_path):
    # CREATE TABLE IF NOT EXISTS does nothing to a table that already exists,
    # so without a migration the column never reaches the fleet's live DB.
    import sqlite3
    path = str(tmp_path / "old.db")
    old = sqlite3.connect(path)
    old.execute("CREATE TABLE servers (engine TEXT NOT NULL, addr TEXT NOT NULL,"
                " hostname TEXT, map TEXT, players INTEGER, maxplayers INTEGER,"
                " ping_ms INTEGER, gamename TEXT, passworded INTEGER,"
                " is_local INTEGER, source TEXT, first_seen TEXT,"
                " last_seen TEXT, PRIMARY KEY (engine, addr))")
    old.commit()
    old.close()
    con = db.connect(path)
    cols = {r["name"] for r in con.execute("PRAGMA table_info(servers)")}
    assert "query_port" in cols
    db.upsert_servers(con, "unreal", [row("1.2.3.4:7777", "ut", query_port=7778)])
    con.close()
