"""When two published records claim one IP, the NEWEST must win.

WHY THIS EXISTS
---------------
docs/fleet-inventory.md is generated because a hand-maintained table was wrong
about most of the fleet and TWICE missed a graphics card being swapped. On
2026-09-01 the generated one missed a third, for a different reason.

Hardware moves between boxes here constantly, and a retired machine's published
record never goes away. Two records claimed 192.168.1.124:

    NSC-9871C0E9964   reported_at 2003-04-02   NVIDIA GeForce2 GTS
    NSC-AB862B3CF23   reported_at 2026-09-01   3dfx Voodoo5      <- the live one

match_records() took the FIRST match in directory order, so the alphabetically
earlier name won and the document showed a card that was not in the machine.

THE TIE-BREAK MUST BE THE FILE'S MTIME, NOT reported_at. Box clocks on this
fleet are not trustworthy - those two records were stamped 2003 and 2004 by
machines whose CMOS had drifted or reset, and .124's clock was found set to
29 July 2004 while the host said September 2026. The share's mtime is written
by this host, which is the same reasoning the module docstring already gives
for judging staleness.
"""
import importlib.util
import os

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SRC = os.path.join(REPO, "scripts", "fleet", "inventory.py")


def _mod():
    spec = importlib.util.spec_from_file_location("inv_under_test", SRC)
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m


def _rec(name, ip, mtime, gpu):
    return {"file": name + ".json", "path": "/x/" + name, "mtime": mtime,
            "data": {"hostname": name,
                     # record_ips() reads iface["ipv4"], a LIST - not "ip".
                     # A fixture with the wrong shape matches nothing and the
                     # test passes for the wrong reason.
                     "network": {"interfaces": [{"ipv4": [ip]}]},
                     "video_cards": [{"name": gpu}]}}


def test_the_newest_record_wins_an_ip_collision():
    inv = _mod()
    old = _rec("NSC-9871C0E9964", "192.168.1.124", 1000.0, "NVIDIA GeForce2 GTS")
    new = _rec("NSC-AB862B3CF23", "192.168.1.124", 2000.0, "3dfx Voodoo5")
    # directory order puts the STALE one first, which is what used to win
    for records in ([old, new], [new, old]):
        by_entry, _orphans = inv.match_records(
            [("192.168.1.124", "whatever", "")], list(records))
        assert by_entry, "nothing matched - check the fixture shape"
        rec = list(by_entry.values())[0]
        name = (rec.get("data") or {}).get("hostname")
        assert name == "NSC-AB862B3CF23", (
            "the 2026 record must win over the 2003 one regardless of "
            "directory order; got %r" % name)


def test_the_tiebreak_uses_file_mtime_not_the_boxes_own_clock():
    src = open(SRC, encoding="utf-8").read()
    i = src.index("candidates = [")
    window = src[i:i + 400]
    assert 'records[i]["mtime"]' in window, (
        "tie-break on the FILE's mtime - reported_at comes from the box, and "
        "two boxes on this fleet stamp their records 2003 and 2004")
    assert "reported_at" not in window


def test_the_reasoning_survives_in_the_comment():
    """Delete the why and someone restores the `break` as a simplification."""
    src = open(SRC, encoding="utf-8").read()
    i = src.index("candidates = [")
    before = src[max(0, i - 1200):i]
    assert "192.168.1.124" in before
    assert "mtime" in before and "clock" in before.lower()
