"""lan_sweep: a forced close is checked by its POST-CONDITION.

2026-10-01: `taskkill /f` returned while Carmageddon 2 lived on; the sweep
called it CHECK and ran 48 more titles while the leftover spun one thread at
100% CPU for 14 hours on .124, starving every later measurement. A survivor
now gets the agent's own PROCKILL by PID; whatever is still there FAILS the
title and stops the sweep."""
import asyncio
import importlib.util
import inspect
import json
import os
import sys

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def _load():
    sys.path.insert(0, os.path.join(REPO, "scripts", "benchmarks"))
    sys.path.insert(0, REPO)
    spec = importlib.util.spec_from_file_location(
        "lan_sweep_force_ut", os.path.join(REPO, "scripts", "benchmarks", "lan_sweep.py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


class FakeBox:
    """taskkill 'succeeds' and kills nothing; PROCKILL kills only if allowed."""
    def __init__(self, prockill_works):
        self.alive = {1096: "CARMA2_HW.EXE"}
        self.prockill_works = prockill_works
        self.sent = []

    async def cmd(self, command, timeout=60.0):
        if command == "PROCLIST":
            return 0, json.dumps([{"pid": p, "name": n} for p, n in self.alive.items()])
        return 0, "OK"

    async def text(self, command, timeout=60.0):
        self.sent.append(command)
        if command.startswith("PROCKILL") and self.prockill_works:
            self.alive.pop(int(command.split()[1]), None)
        return "OK"

    async def exec_(self, command, timeout=60.0):
        self.sent.append(command)
        return "SUCCESS: The process with PID 1096 has been terminated."


def _run(mod, box, monkeypatch):
    async def no_sleep(_):
        return None
    monkeypatch.setattr(mod.asyncio, "sleep", no_sleep)
    return asyncio.run(mod.force_close_verified(box, {1096: "CARMA2_HW.EXE"}))


def test_a_survivor_gets_the_agents_prockill(monkeypatch):
    mod = _load()
    box = FakeBox(prockill_works=True)
    assert _run(mod, box, monkeypatch) == []
    assert "PROCKILL 1096" in box.sent


def test_a_process_that_survives_everything_is_reported(monkeypatch):
    mod = _load()
    box = FakeBox(prockill_works=False)
    assert _run(mod, box, monkeypatch) == ["CARMA2_HW.EXE"]


def test_a_survivor_fails_the_title_and_stops_the_sweep():
    mod = _load()
    one = inspect.getsource(mod.run_one)
    assert 'rec["survived_force"] = await force_close_verified(box, left)' in one
    assert '"FAIL: "' in one and "STILL RUNNING AFTER A FORCED CLOSE" in one
    assert "2>nul" not in one.split('rec["forced"]')[1].split("force_close_verified")[0]
    main = inspect.getsource(mod.amain)
    assert 'rec.get("survived_force")' in main and "return 5" in main
