"""box-guardian.py reboots a box whose agent is silent while its kernel answers.

The clock must count silence only while the box is up: on 2026-09-25 it
rebooted .124 mid-boot because the box had been switched off for 15 hours.
"""
import importlib.util
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]


def _guardian():
    spec = importlib.util.spec_from_file_location(
        "box_guardian", REPO / "scripts" / "fleet" / "box-guardian.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def test_a_box_that_was_switched_off_is_not_wedged_the_moment_it_boots():
    g = _guardian()
    c = g.SilenceClock(0)
    t = 0
    for _ in range(900):                 # 15 h off: agent silent, SMB down
        t += 60
        assert c.silent(t, smb_up=False) == 0
    t += 60                              # powered on: SMB up, agent not yet
    assert c.silent(t, smb_up=True) == 60     # the OLD code said 54060
    assert c.silent(t, smb_up=True) < 360     # inside the default grace


def test_a_real_wedge_still_trips_the_grace():
    g = _guardian()
    c = g.SilenceClock(0)
    c.answered(100)
    t = 100
    for _ in range(7):                   # agent dead, kernel up
        t += 60
        s = c.silent(t, smb_up=True)
    assert s == 420 and s >= 360


def test_an_answer_restarts_the_clock():
    g = _guardian()
    c = g.SilenceClock(0)
    assert c.silent(300, smb_up=True) == 300
    c.answered(300)
    assert c.silent(360, smb_up=True) == 60
