"""glquake_win9x_bench.py must keep a run that failed before it produced a
number. Measured on .243 2026-09-28: Quake II's LAUNCH was refused (agent
1.89.1, CreateProcess error 31) and the run came back as a bare {"error"}; the
CSV writer's r["asked"] then raised KeyError and the session's OTHER results
were lost with it. A failed cell is a result too - it becomes status "fail"."""
import asyncio
import importlib.util
import sys
import types
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
BENCH = ROOT / "scripts" / "benchmarks"


def load():
    sys.path.insert(0, str(BENCH))
    spec = importlib.util.spec_from_file_location("glquake_win9x_bench", BENCH / "glquake_win9x_bench.py")
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m


def args(**kw):
    a = dict(game="quake2", ref="gl", exe="GLQUAKE.EXE", gl_driver="3dfxgl", card="Voodoo2",
             notes="", basedir=r"C:\games\Quake2Win9x", bpp=16, timeout=5, poll=0, settle=0)
    a.update(kw)
    return types.SimpleNamespace(**a)


def refused(m, monkeypatch):
    async def fake_cmd(c, text, **kw):
        if text.startswith("LAUNCH"):
            return 0xFF, b"ERR CreateProcess failed: 31"
        return 0, b"OK"

    async def no_sleep(_):
        return None
    monkeypatch.setattr(m, "cmd", fake_cmd)
    monkeypatch.setattr(m.asyncio, "sleep", no_sleep)


def test_a_refused_quake2_launch_still_makes_a_fail_row(monkeypatch):
    m = load()
    refused(m, monkeypatch)
    a = args()
    res = asyncio.run(m.one_run_q2(None, a, 640, 480, "demo1"))
    assert res["demo"] == "demo1" and res["asked"] == "640x480x16", res
    assert "LAUNCH failed" in res["error"]
    row = m.csv_row(res, a, {}, "20260928")
    assert row["status"] == "fail" and row["res"] == "640x480" and "LAUNCH failed" in row["notes"]


def test_a_refused_glquake_launch_still_makes_a_fail_row(monkeypatch):
    m = load()
    refused(m, monkeypatch)
    a = args(game="glquake", basedir=r"C:\games\Quake1")
    res = asyncio.run(m.one_run(None, a, 640, 480, "demo2"))
    assert res["demo"] == "demo2" and res["asked"] == "640x480x16", res
    row = m.csv_row(res, a, {"os": {"product": "Windows 98", "version": "4.10.2222"}}, "20260928")
    assert row["status"] == "fail" and row["os_build"] == "Windows 98 4.10.2222"
