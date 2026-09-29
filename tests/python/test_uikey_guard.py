"""UIKEY must not be able to shut a fleet box down (agent 1.91.0, 2026-09-28).

A sweep closing Aliens vs Predator on .124 sent ALT+F4 while the game was alive
but not focused. The key reached the desktop - "Shut Down Windows" - the
console-quit RETURN that followed confirmed it, and the box restarted. The pure
decision is agent/shared/uiguard.h (tests/native/test_uiguard.c); these tests
pin that the agent USES it before any key leaves, that WINLIST tells a tool
where keys would land, and that the sweep asks before it types.
"""
import asyncio
import importlib.util
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
INPUT_C = os.path.join(REPO, "agent", "src", "input.c")


def _src():
    return open(INPUT_C, encoding="latin-1").read()


def _func(src, name):
    i = src.index("void %s(" % name)
    j = src.index("\n}\n", i)
    return src[i:j]


def test_uikey_checks_the_guard_before_any_key_is_sent():
    body = _func(_src(), "handle_uikey")
    g = body.index("uig_check(")
    for sender in ("send_text_input(", "keybd_event(", "send_key_press_ex("):
        assert sender in body, sender
        assert g < body.index(sender), "%s runs before the guard" % sender
    # a refusal answers with an error and returns - it is not logged and ignored
    refusal = body[g:body.index("}", body.index("if (r != UIG_OK)"))]
    assert "send_error_response" in refusal and "return;" in refusal


def test_winlist_reports_the_focused_window_and_each_owner():
    body = _func(_src(), "handle_winlist")
    assert 'json_key(&j, "foreground")' in body
    assert '"pid"' in body
    assert "GetWindowThreadProcessId" in _src()


def _load_sweep():
    sys.path.insert(0, os.path.join(REPO, "scripts", "benchmarks"))
    sys.path.insert(0, REPO)
    spec = importlib.util.spec_from_file_location(
        "lan_sweep_ut", os.path.join(REPO, "scripts", "benchmarks", "lan_sweep.py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


class FakeBox:
    def __init__(self, winlist):
        self.winlist = winlist
        self.sent = []

    async def cmd(self, command, timeout=60.0):
        if command == "WINLIST":
            return 0, self.winlist
        if command.startswith("UIKEY "):
            self.sent.append(command[6:])
            return 0, "OK"
        raise AssertionError(command)


def _key(winlist, pids):
    sweep = _load_sweep()
    box, rec = FakeBox(winlist), {}
    ok = asyncio.run(sweep.key_to(box, set(pids), "ALT+F4", rec))
    return ok, box.sent, rec


def test_the_sweep_sends_a_key_only_to_the_games_own_window():
    ok, sent, rec = _key('{"windows":[],"foreground":{"hwnd":"1","pid":424,'
                         '"class":"AvP","title":"AvP"}}', {424})
    assert ok and sent == ["ALT+F4"] and not rec


def test_the_124_case_withholds_the_key():
    """the game is alive (pid 424) but the DESKTOP has the focus"""
    ok, sent, rec = _key('{"windows":[],"foreground":{"hwnd":"2","pid":1520,'
                         '"class":"Progman","title":"Program Manager"}}', {424})
    assert not ok and sent == []
    assert "Progman" in rec["keys_skipped"][0]


def test_an_agent_that_cannot_name_the_focus_gets_no_keys():
    ok, sent, rec = _key('{"windows":[]}', {424})
    assert not ok and sent == [] and "unknown" in rec["keys_skipped"][0]
