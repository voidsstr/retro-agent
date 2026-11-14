"""The favourites push must not write into a modern Windows host's games.

The agent leaves Windows 10/11 hosts alone (hostpolicy.c), but the favourites
push in scripts/gameindex/sync.py runs on the dev host and swept every live
agent with no OS filter. On 2026-09-24 it had written 15 servers into
WHITEBEAST's (Win11) own C:\\UnrealTournament\\System\\UnrealTournament.ini.

Run: pytest tests/python/test_gameindex_modern_host.py
"""
import asyncio
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts" / "gameindex"))
import sync  # noqa: E402


class FakeConn:
    def __init__(self, hwprofile=None, fail=False, reg=None, reg_fail=False):
        self.hwprofile, self.fail, self.asked = hwprofile, fail, []
        self.reg, self.reg_fail = reg if reg is not None else {"values": []}, reg_fail

    async def command_text(self, cmd, timeout=30):
        self.asked.append(cmd)
        if self.fail:
            raise RuntimeError("no answer")
        if cmd.startswith("REGREAD"):
            if self.reg_fail:
                raise RuntimeError("no answer")
            return json.dumps(self.reg)
        return json.dumps(self.hwprofile)


def check(greeting, conn):
    return asyncio.run(sync.unmanaged_modern_host(conn, greeting))


def test_retro_boxes_are_never_queried():
    for g in ("RETRO N5R5L9 Win4.10", "RETRO NSC-C543 Win5.1", "RETRO DELL Win6.1"):
        c = FakeConn()
        assert check(g, c) is None, g
        assert c.asked == [], f"{g}: a retro box must not be asked for HWPROFILE"


def test_agent_that_says_unmanaged_is_skipped():
    c = FakeConn({"host_policy": {"modern": True, "managed": False}})
    assert "not managed" in check("RETRO WHITEBEAST Win6.2", c)
    assert c.asked == ["HWPROFILE"]


def test_managed_override_is_respected():
    c = FakeConn({"host_policy": {"modern": True, "managed": True}})
    assert check("RETRO WHITEBEAST Win6.2", c) is None


def test_old_agent_on_6_2_is_treated_as_modern():
    # 1.81.1 on Win11 greets with the GetVersionEx shim value and has no field
    c = FakeConn({"os": {"version": "6.2.9200"}})
    assert "too old" in check("RETRO WHITEBEAST Win6.2", c)


def test_no_answer_is_not_permission_to_write():
    c = FakeConn(fail=True)
    assert check("RETRO WHITEBEAST Win10.0", c)


def test_push_favorites_consults_the_policy_before_writing():
    src = (Path(sync.__file__)).read_text()
    body = src[src.index("async def push_favorites("):]
    body = body[:body.index("\n# --- one pass")]
    assert body.index("modern_host_scope(") < body.index('UPLOAD {path}'), \
        "the host-policy check must come before any UPLOAD"


# --- games-only modern hosts (ModernGamesOnly=1, .139) ----------------------

def scope(greeting, conn):
    return asyncio.run(sync.modern_host_scope(conn, greeting))


GAMES_ONLY = {"host_policy": {"modern": True, "managed": False, "games_only": True}}


def test_games_only_box_may_write_inside_games_dir_only():
    why, root = scope("RETRO DESKTOP-CH9EU7O Win6.2", FakeConn(GAMES_ONLY))
    assert why is None and root == "C:\\Games"
    assert sync.inside_games_root("C:\\Games\\Quake2\\baseq2\\autoexec.cfg", root)
    assert sync.inside_games_root("c:/games/Quake2/baseq2/autoexec.cfg", root)
    assert not sync.inside_games_root(
        "C:\\Users\\x\\AppData\\Roaming\\Quake3\\servercache.dat", root)
    assert not sync.inside_games_root("C:\\GamesOther\\a.cfg", root)


def test_games_only_box_honours_gamesdir():
    reg = {"values": [{"name": "GamesDir", "type": "REG_SZ", "data": "D:\\Retro\\"}]}
    why, root = scope("RETRO X Win10.0", FakeConn(GAMES_ONLY, reg=reg))
    assert why is None and root == "D:\\Retro"


def test_games_only_with_unreadable_registry_writes_nothing():
    why, root = scope("RETRO X Win10.0", FakeConn(GAMES_ONLY, reg_fail=True))
    assert why and root is None


def test_plain_unmanaged_box_is_still_skipped():
    why, root = scope("RETRO WHITEBEAST Win6.2",
                      FakeConn({"host_policy": {"modern": True, "managed": False}}))
    assert "not managed" in why and root is None


def test_push_favorites_filters_targets_by_games_root():
    src = (Path(sync.__file__)).read_text()
    body = src[src.index("async def push_favorites("):]
    body = body[:body.index("\n# --- one pass")]
    assert body.index("inside_games_root(") < body.index('UPLOAD {path}')


def test_games_only_never_reaches_the_appdata_servercache():
    # 2026-11-13: the first games-only pass on .139 wrote Team Arena's
    # servercache.dat into C:\\Users\\<user>\\AppData - push_servercache ran
    # outside the per-title loop that checked the games folder.
    src = (Path(sync.__file__)).read_text()
    body = src[src.index("async def push_favorites("):]
    body = body[:body.index("\n# --- one pass")]
    call = body.index("await push_servercache(")
    assert body.rindex("if games_root:", 0, call) < call
