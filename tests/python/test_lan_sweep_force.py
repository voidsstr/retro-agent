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


def test_a_dialog_the_close_raised_is_recorded_and_named_in_the_verdict():
    """2026-10-02, .124: UE1 on GlideDrv answered the sweep's WM_CLOSE with
    "Critical Error" (Assertion failed: RenDev, EndFullscreen <- WM_KILLFOCUS)
    and sat on it until the force - and the sweep, which only looked for
    dialogs while sampling, called it a plain forced close."""
    mod = _load()
    one = inspect.getsource(mod.run_one)
    grace = one.index("settled = await settle(grace)")
    probe = one.index('rec["dialogs_on_close"]')
    keys = one.index('await key_to(box, set(left), "ALT+F4", rec)')
    force = one.index('rec["forced"] = sorted(set(left.values()))')
    assert grace < probe < keys < force          # after the WM_CLOSE, before keys or force
    assert "h not in dlg0" in one[probe:keys] and 'h not in rec["dialogs"]' in one[probe:keys]
    assert '"dialog while closing: "' in one


def test_the_games_own_quit_goes_first_while_it_has_the_keyboard():
    """2026-10-03: Descent 3, Quake II and Unreal Gold were all FOCUSED at 30 and
    60 s; the sweep's WM_CLOSE destroyed their windows and the focus fell back to
    the agent console, so the later keys were withheld and UE1 crashed in
    EndFullscreen. A proven console quit now runs first, through key_to."""
    mod = _load()
    assert mod.clean_quit_keys(["Unreal.exe"]) == ("TILDE", "TEXT:exit", "RETURN")
    assert mod.clean_quit_keys(["helper.exe", "QUAKE2.EXE"]) == ("TILDE", "TEXT:quit", "RETURN")
    for unproven in ("DeusEx.exe", "glh2.exe", "quake3.exe", "main.exe", "sof2.exe"):
        assert mod.clean_quit_keys([unproven]) is None, unproven
    one = inspect.getsource(mod.run_one)
    quit_at = one.index("keys = clean_quit_keys(left.values())")
    close_at = one.index('await box.exec_(f"cmd /c taskkill /pid {p} 2>nul", timeout=30)')
    assert quit_at < close_at
    assert "if not await key_to(box, set(left), k, rec):" in one[quit_at:close_at]
    # each sample now says who had the keyboard before the close
    assert 'rec["samples"].append({"at": at, "alive": alive, "stats": lc.shot_stats(data),' in one
    assert '"foreground": fgr' in one


def test_a_withheld_key_says_whether_the_game_ever_had_the_keyboard():
    mod = _load()
    one = inspect.getsource(mod.run_one)
    assert "the WM_CLOSE took the game's window and its keyboard" in one
    assert "close keys withheld - game not focused" in one   # still said when it never had it
