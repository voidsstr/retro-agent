"""dos_win9x_bench.py must survive a Win9x agent that goes deaf for the length
of the DOS job it LAUNCHed (agents before 1.89.1: the DOS child shares the
agent's console VM). Measured on .243 2026-09-28: the first run's .END poll
timed out and the tool died while Doom was still timing demo1 - and the score
(5026 gametics in 2439 realtics) was sitting in C:\\BENCH\\RES afterwards."""
import asyncio
import importlib.util
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
BENCH = ROOT / "scripts" / "benchmarks"


def load():
    sys.path.insert(0, str(BENCH))
    spec = importlib.util.spec_from_file_location("dos_win9x_bench", BENCH / "dos_win9x_bench.py")
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m


def test_a_timed_out_poll_reconnects_and_keeps_waiting(monkeypatch):
    m = load()
    calls = {"end_polls": 0, "opens": 0}

    async def fake_cmd(c, text, **kw):
        if text.startswith("DOWNLOAD") and text.endswith(".END"):
            calls["end_polls"] += 1
            if calls["end_polls"] == 1:
                raise TimeoutError("the agent is stalled behind the DOS job")
            return 1, b"END"
        if text.startswith("DOWNLOAD") and text.endswith(".TXT"):
            return 1, b"...\ntimed 5026 gametics in 2439 realtics\n"
        return 0, b"OK"

    async def no_sleep(_):
        return None

    class FakeConn:
        async def close(self):
            pass

    async def fake_open(self, deadline):
        calls["opens"] += 1
        self.c = FakeConn()
        return self.c

    monkeypatch.setattr(m, "cmd", fake_cmd)
    monkeypatch.setattr(m.asyncio, "sleep", no_sleep)
    monkeypatch.setattr(m.Link, "open", fake_open)
    link = m.Link("192.0.2.1")
    link.c = FakeConn()
    res = asyncio.run(m.run_job(link, "J0R0", "C:\\DOOM", "DOOM.EXE -timedemo demo1", 300))
    assert res.get("fps") == 72.1, res
    assert res.get("agent_stalled_polls") == 1, "a stall is recorded, not hidden"
    assert calls["opens"] == 1 and link.stalls == 1
