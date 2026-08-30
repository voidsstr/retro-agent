"""Tests for the fleet LAN patch-level audit (scripts/gamepatch/audit.py).

Every case here encodes a wrong answer this tool actually gave on its first
runs against the live fleet on 2026-08-29, and what the hardware then proved.
The tool's whole value is telling "this client cannot join" apart from "I could
not read this client", so the tests are mostly about that distinction.
"""
import os
import sys

import asyncio

import pytest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "scripts",
                                "gamepatch"))
import audit  # noqa: E402


# --------------------------------------------------------------------------
# The three states must stay three states.
# --------------------------------------------------------------------------

def test_marker_rejects_a_state_outside_the_three():
    with pytest.raises(ValueError):
        audit.Marker("q3", "C:\\x", "broken")


def test_an_unknown_must_say_why():
    """An unknown with no reason is indistinguishable from a shrug.

    The point of the third state is to send someone to the right place; an
    unknown that cannot say what stopped it is no better than a mismatch.
    """
    with pytest.raises(ValueError):
        audit.Marker("q3", "C:\\x", audit.UNKNOWN)
    audit.Marker("q3", "C:\\x", audit.UNKNOWN, why="DOWNLOAD timed out")


# --------------------------------------------------------------------------
# DIRLIST returns a BARE JSON ARRAY.
# --------------------------------------------------------------------------

class FakeConn:
    def __init__(self, texts=None, blobs=None):
        self.texts, self.blobs = texts or {}, blobs or {}

    async def command_text(self, cmd, timeout=None):
        if cmd not in self.texts:
            raise RuntimeError(f"no canned reply for {cmd!r}")
        v = self.texts[cmd]
        if isinstance(v, Exception):
            raise v
        return v

    async def command_binary(self, cmd, timeout=None):
        if cmd not in self.blobs:
            raise RuntimeError("Cannot open file: error 2")
        v = self.blobs[cmd]
        if isinstance(v, Exception):
            raise v
        return v


def test_dirlist_accepts_a_bare_array():
    asyncio.run(_test_dirlist_accepts_a_bare_array())


async def _test_dirlist_accepts_a_bare_array():
    """The agent answers DIRLIST with a top-level list, not {"entries": ...}.

    The first version called .get() on it. A list has no .get, the
    AttributeError was not caught by the ValueError handler, and every single
    q3/q2/unreal check on the fleet came back "unknown" -- 112 of them, which
    looked like a dead fleet rather than one wrong method call.
    """
    conn = FakeConn(texts={"DIRLIST C:\\g": '[{"name":"pak0.pk3","is_dir":false}]'})
    names, why = await audit._dirlist(conn, "C:\\g")
    assert names == ["pak0.pk3"], why


def test_dirlist_still_accepts_the_wrapped_form():
    asyncio.run(_test_dirlist_still_accepts_the_wrapped_form())


async def _test_dirlist_still_accepts_the_wrapped_form():
    conn = FakeConn(texts={"DIRLIST C:\\g": '{"entries":[{"name":"a.pk3"}]}'})
    names, _ = await audit._dirlist(conn, "C:\\g")
    assert names == ["a.pk3"]


def test_unparseable_dirlist_is_not_an_empty_directory():
    asyncio.run(_test_unparseable_dirlist_is_not_an_empty_directory())


async def _test_unparseable_dirlist_is_not_an_empty_directory():
    """Garbage must not read as "the directory is empty"."""
    conn = FakeConn(texts={"DIRLIST C:\\g": "<html>404</html>"})
    names, why = await audit._dirlist(conn, "C:\\g")
    assert names is None and why


# --------------------------------------------------------------------------
# GoldSrc: protocol decides, not PatchVersion equality.
# --------------------------------------------------------------------------

def test_a_1125_client_is_ok_against_a_1127_server():
    asyncio.run(_test_a_1125_client_is_ok_against_a_1127_server())


async def _test_a_1125_client_is_ok_against_a_1127_server():
    """PROVED ON HARDWARE, and it is why this is not a string comparison.

    .145 runs cstrike PatchVersion 1.1.2.5; the server logs itself as
    "48/1.1.2.7". Launched against it, the client's own console recorded
    "Connection accepted by 192.168.1.132:27018". Comparing the two version
    strings for equality reported 56 mismatches across the fleet, none real.
    """
    conn = FakeConn(blobs={
        "DOWNLOAD C:\\cs\\cstrike\\steam.inf": b"PatchVersion=1.1.2.5\nProductName=cstrike\n"})
    got = await audit.check_goldsrc(conn, "cs16", "C:\\cs", {"goldsrc": "protocol 48"})
    assert [m.state for m in got] == [audit.OK]


def test_a_won_tree_with_no_steam_inf_anywhere_is_a_real_mismatch():
    asyncio.run(_test_a_won_tree_with_no_steam_inf_anywhere_is_a_real_mismatch())


async def _test_a_won_tree_with_no_steam_inf_anywhere_is_a_real_mismatch():
    """The one genuine finding of the first fleet-wide audit.

    steam.inf did not exist before Steam, so a tree without one anywhere is a
    WON-era engine (protocol 46/47) and cannot reach a protocol-48 server.
    Verified: .145's C:\\Sierra\\Half-Life runs `hl.exe -game ts -window`
    happily, but adding `+connect <ts server>` makes the process disappear
    within 25s having never joined.
    """
    conn = FakeConn(blobs={})          # neither ts/steam.inf nor valve/steam.inf
    got = await audit.check_goldsrc(conn, "ts", "C:\\Sierra\\Half-Life",
                                    {"goldsrc": "protocol 48"})
    assert [m.state for m in got] == [audit.MISMATCH]
    assert "WON" in got[0].found


def test_a_mod_inherits_the_engine_version_from_valve():
    asyncio.run(_test_a_mod_inherits_the_engine_version_from_valve())


async def _test_a_mod_inherits_the_engine_version_from_valve():
    """A third-party mod ships no steam.inf; the engine's governs."""
    conn = FakeConn(blobs={
        "DOWNLOAD C:\\hl\\valve\\steam.inf": b"PatchVersion=1.1.2.7\n"})
    got = await audit.check_goldsrc(conn, "ts", "C:\\hl", {"goldsrc": "protocol 48"})
    assert [m.state for m in got] == [audit.OK]


# --------------------------------------------------------------------------
# Quake III: pak coverage is information, not a verdict.
# --------------------------------------------------------------------------

def test_a_client_missing_pak7_and_pak8_still_passes():
    asyncio.run(_test_a_client_missing_pak7_and_pak8_still_passes())


async def _test_a_client_missing_pak7_and_pak8_still_passes():
    """.145 has only pak0..pak6 and joined the 1.36 server as "BOX145".

    Requiring the full retail pak0..pak8 set called that box broken while it
    was demonstrably playing on the server.
    """
    entries = ",".join('{"name":"pak%d.pk3","is_dir":false}' % i for i in range(7))
    conn = FakeConn(texts={"DIRLIST C:\\q3\\baseq3": "[" + entries + "]"})
    got = await audit.check_q3(conn, "quake3", "C:\\q3", {})
    assert [m.state for m in got] == [audit.OK]


def test_an_empty_baseq3_is_unknown_not_a_mismatch():
    asyncio.run(_test_an_empty_baseq3_is_unknown_not_a_mismatch())


async def _test_an_empty_baseq3_is_unknown_not_a_mismatch():
    conn = FakeConn(texts={"DIRLIST C:\\q3\\baseq3": "[]"})
    got = await audit.check_q3(conn, "quake3", "C:\\q3", {})
    assert [m.state for m in got] == [audit.UNKNOWN]


# --------------------------------------------------------------------------
# UT99: the client's marker, not the Linux server's.
# --------------------------------------------------------------------------

def test_ut99_is_identified_by_the_oldunreal_stamp_package():
    asyncio.run(_test_ut99_is_identified_by_the_oldunreal_stamp_package())


async def _test_ut99_is_identified_by_the_oldunreal_stamp_package():
    """The fleet is 469c and was reported as 436/451 on every box.

    The first check looked for VulkanDrv/XOpenGLDrv/SDLDrv -- the LINUX
    server's renderers, which a Windows client never has. The real marker is
    the stamp package OldUnreal469c.u, present on both platforms.
    """
    names = ["Core.dll", "UnrealTournament.exe", "OldUnreal469c.u", "D3D9Drv.dll"]
    conn = FakeConn(texts={"DIRLIST C:\\ut\\System":
                           "[" + ",".join('{"name":"%s"}' % n for n in names) + "]"})
    got = await audit.check_unreal(conn, "ut99", "C:\\ut", {"unreal": "469"})
    assert got[0].state == audit.OK and got[0].found == "OldUnreal469c"


def test_a_436_tree_is_unknown_because_it_may_be_a_kept_reference_copy():
    asyncio.run(_test_a_436_tree_is_unknown_because_it_may_be_a_kept_reference_copy())


async def _test_a_436_tree_is_unknown_because_it_may_be_a_kept_reference_copy():
    """C:\\Games\\UT436 is named for its version and is old on purpose.

    Every box carrying one ALSO has a 469c C:\\Games\\UnrealTournament. Calling
    the old tree a mismatch sends someone to fix a directory nobody plays on.
    """
    names = ["Core.dll", "UnrealTournament.exe"]
    conn = FakeConn(texts={"DIRLIST C:\\Games\\UT436\\System":
                           "[" + ",".join('{"name":"%s"}' % n for n in names) + "]"})
    got = await audit.check_unreal(conn, "ut99", "C:\\Games\\UT436", {"unreal": "469"})
    assert got[0].state == audit.UNKNOWN


def test_a_case_collision_is_flagged():
    asyncio.run(_test_a_case_collision_is_flagged())


async def _test_a_case_collision_is_flagged():
    """Two files differing only in case are ONE file on a Windows client.

    The live UT99 server carries both Botpack.u (the current 469 release) and
    a BotPack.u matching no OldUnreal release at all. Which one survives a
    copy to an XP box is arbitrary, and the wrong one is a version mismatch at
    connect time.
    """
    names = ["Botpack.u", "BotPack.u", "OldUnreal469c.u"]
    conn = FakeConn(texts={"DIRLIST C:\\ut\\System":
                           "[" + ",".join('{"name":"%s"}' % n for n in names) + "]"})
    got = await audit.check_unreal(conn, "ut99", "C:\\ut", {"unreal": "469"})
    assert any(m.state == audit.MISMATCH and "case-collision" in m.game for m in got)


# --------------------------------------------------------------------------
# Scoping and per-box verdicts.
# --------------------------------------------------------------------------

def test_only_games_we_host_a_server_for_are_audited():
    """An engine is not a game.

    `q3` covers Jedi Academy and SoF2, whose paks live in base/, not baseq3;
    `unreal` covers Unreal Gold. Auditing those against our Quake III and UT99
    servers reported healthy installs as broken clients that were never going
    to connect to those servers anyway.
    """
    assert "jka" not in audit.HOSTED["q3"]
    assert "sof2" not in audit.HOSTED["q3"]
    assert "unreal" not in audit.HOSTED["unreal"]   # Unreal Gold, not UT99
    assert "ut99" in audit.HOSTED["unreal"]
    # No valve (Half-Life deathmatch) server runs on this host.
    assert "halflife" not in audit.HOSTED["goldsrc"]


def test_a_box_is_judged_by_its_best_install_of_each_game():
    old = audit.Marker("ut99", "C:\\Games\\UT436", audit.UNKNOWN, why="old tree")
    cur = audit.Marker("ut99", "C:\\Games\\UnrealTournament", audit.OK,
                       found="OldUnreal469c")
    best = audit.per_game_best([old, cur])
    assert len(best) == 1 and best[0].state == audit.OK


def test_a_mismatch_never_hides_behind_an_unknown():
    """Ranking is ok > unknown > mismatch, but only ok may mask a mismatch.

    A box whose only readable install is broken must still report broken.
    """
    bad = audit.Marker("ts", "C:\\Sierra\\Half-Life", audit.MISMATCH, found="WON")
    unk = audit.Marker("ts", "C:\\other", audit.UNKNOWN, why="unreadable")
    best = audit.per_game_best([bad, unk])
    assert len(best) == 1 and best[0].state == audit.UNKNOWN
