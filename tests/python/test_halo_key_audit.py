"""A shared Halo key is only a FAULT between boxes that have Halo installed.

WHY THIS EXISTS
---------------
Halo PC allows one simultaneous player per CD key, so two boxes sharing a key
cannot be in the same game - the host throws the second one out with the
generic "Your CD Key is invalid".  scripts/halo/audit_keys.py exists to catch
that before somebody spends an afternoon on "the key is wrong".

The first version of the audit flagged .133 and .143 as a duplicate.  Neither
box has halo.exe on it - the capability gate refuses the title there, no SSE2 -
so the shared value is a leftover from a fleet-wide install.reg push and cannot
affect any game.  Reporting it as a licensing fault is exactly the mistake this
project keeps paying for: a measurement artefact reported as a defect.  So the
audit now separates the two, and this test pins that distinction.
"""
import importlib.util
import os

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
TOOL = os.path.join(REPO, "scripts", "halo", "audit_keys.py")


def _load():
    spec = importlib.util.spec_from_file_location("halo_audit_keys", TOOL)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def test_two_boxes_with_halo_sharing_a_key_is_a_fault():
    live, stale = _load().classify_duplicates(
        {"aaaa": [("192.168.1.123", True), ("192.168.1.145", True)]})
    assert "aaaa" in live
    assert not stale


def test_two_boxes_WITHOUT_halo_sharing_a_key_is_only_stale_state():
    """The .133 / .143 case measured 2026-09-01."""
    live, stale = _load().classify_duplicates(
        {"dc77": [("192.168.1.133", False), ("192.168.1.143", False)]})
    assert not live, "a box with no halo.exe cannot be in a game - not a fault"
    assert "dc77" in stale


def test_one_box_with_halo_and_one_without_is_not_a_fault_either():
    """Only ONE of them can be in a game, so the key is never contended."""
    live, stale = _load().classify_duplicates(
        {"bbbb": [("192.168.1.240", True), ("192.168.1.143", False)]})
    assert not live
    assert "bbbb" in stale


def test_three_boxes_two_of_them_playing_is_a_fault():
    live, _stale = _load().classify_duplicates(
        {"cccc": [("192.168.1.240", True), ("192.168.1.246", True),
                  ("192.168.1.143", False)]})
    assert "cccc" in live


def test_a_key_held_by_one_box_is_never_reported():
    live, stale = _load().classify_duplicates({"dddd": [("192.168.1.123", True)]})
    assert not live and not stale


def test_the_audit_never_prints_a_key_or_the_blob():
    """The report identifies a key ONLY by fingerprint - a leak here would put
    a licence key in a terminal, a log and this session's transcript."""
    src = open(TOOL, encoding="utf-8").read()
    assert "hexdigest()[:10]" in src, "fingerprints must be truncated hashes"
    # the raw blob is hashed, never printed
    assert 'print(' in src
    for bad in ("print(hexs", "print(key", "% key", "format(key"):
        assert bad not in src, "audit_keys.py must never print a key: %r" % bad
