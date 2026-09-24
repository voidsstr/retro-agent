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
    def __init__(self, hwprofile=None, fail=False):
        self.hwprofile, self.fail, self.asked = hwprofile, fail, []

    async def command_text(self, cmd, timeout=30):
        self.asked.append(cmd)
        if self.fail:
            raise RuntimeError("no answer")
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
    assert body.index("unmanaged_modern_host(") < body.index('UPLOAD {path}'), \
        "the host-policy check must come before any UPLOAD"
