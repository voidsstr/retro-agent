r"""The generated fleet inventory must tell four states apart, and must never
crash on a bad record.

WHY. The hand-maintained "Known Machines" table was wrong about most of the
fleet, and twice a box's graphics card was swapped without the docs noticing.
scripts/fleet/inventory.py replaces it with a document rendered from records
each agent publishes to the share (agent/src/hwpublish.c). That only helps if
its failure modes are legible:

  * "not installed" and "crashed" must never render the same. A box that has
    NEVER published, a box whose record is old, and a box whose record will not
    parse are three different calls to action, and none of them is the same as
    a healthy box.
  * THE FLEET IS POWERED ON DEMAND, so old data is the normal state for several
    boxes at any moment. Stale must therefore be a stamped, explained state -
    not an alarm.
  * A torn copy off SMB must degrade to a reported state, not take the whole
    document down with a traceback. One box writing garbage cannot be allowed
    to hide the other seven.
  * Staleness is judged by the file's mtime on THIS host, never by the
    timestamp inside the record: a retro box's RTC is frequently years out, and
    a record published thirty seconds ago must not read as "last seen 2003".
"""
import json
import os
import subprocess
import sys
import time

import pytest

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SCRIPT = os.path.join(REPO, "scripts", "fleet", "inventory.py")
sys.path.insert(0, os.path.join(REPO, "scripts", "fleet"))

inventory = pytest.importorskip("inventory")


DAY = 86400.0


def sample_record(hostname="P3-DUAL", ip="192.168.1.133", **over):
    rec = {
        "profile_hash": "0123456789abcdef",
        "profile_version": 1,
        "hostname": hostname,
        "agent_version": "1.73.0",
        "reported_at": "2026-08-30 12:00:00",
        "cpu": {"vendor": "GenuineIntel", "brand": "Pentium III 701MHz",
                "family": 6, "model": 11, "stepping": 1, "mhz": 701,
                "mhz_source": "registry", "count": 2, "feature_bits": 7,
                "features": ["fpu", "mmx", "cmov", "sse"]},
        "ram_mb": 255,
        "gpu": {"name": "NVIDIA GeForce4 Ti 4600", "pci_ven": "0x10DE",
                "pci_dev": "0x0250", "hardware_id": "PCI\\VEN_10DE&DEV_0250",
                "vram_mb": 128, "driver_version": "6.14.10.9375",
                "driver_date": "1-1-2005", "feature_level": "dx8",
                "feature_level_num": 8, "source": "EnumDisplayDevices(active)"},
        "os": {"product": "Windows XP", "version": "5.1.2600",
               "service_pack": "Service Pack 3", "level": "xp",
               "level_num": 5},
        "directx": {"version": "9.0c", "major": 9},
        "display": {"width": 640, "height": 480, "bpp": 16,
                    "panel_w": 1280, "panel_h": 1024,
                    "panel_source": "registry", "edid_w": 1280,
                    "edid_h": 1024, "panel_hz": 60, "panel_digital": 0,
                    "panel_name": "Dell"},
        "video_cards": [
            {"instance": "0000", "name": "NVIDIA GeForce4 Ti 4600",
             "pci_ven": "0x10DE", "pci_dev": "0x0250",
             "hardware_id": "PCI\\VEN_10DE&DEV_0250",
             "driver_version": "6.14.10.9375", "driver_date": "1-1-2005",
             "attached_to_desktop": True},
        ],
        "accelerators": [],
        "network": {"interfaces": [
            {"description": "3Com EtherLink", "mac": "00-01-02-03-04-05",
             "ipv4": [ip]}], "source": "GetAdaptersInfo"},
        "disk": [{"root": "C:\\", "free_mb": 897024, "total_mb": 953344}],
    }
    rec.update(over)
    return rec


def write_record(directory, name, data, age_days=0.0):
    os.makedirs(directory, exist_ok=True)
    path = os.path.join(directory, name)
    with open(path, "w", encoding="utf-8") as fh:
        if isinstance(data, str):
            fh.write(data)
        else:
            json.dump(data, fh)
    when = time.time() - age_days * DAY
    os.utime(path, (when, when))
    return path


def write_roster(tmp_path, rows):
    path = os.path.join(str(tmp_path), "roster.txt")
    with open(path, "w", encoding="utf-8") as fh:
        fh.write("# test roster\n")
        for ip, host, note in rows:
            fh.write("%s\t%s\t%s\n" % (ip, host, note))
    return path


# ---------------------------------------------------------------- states

def test_the_four_states_are_distinguishable(tmp_path):
    """current / stale / never seen / unreadable must each render as itself.

    This is the whole point of the file. If a never-published box and a box
    whose record is a fortnight old read the same, the document has recreated
    the problem it exists to solve.
    """
    d = os.path.join(str(tmp_path), "records")
    write_record(d, "FRESH.json",
                 sample_record("FRESH", "192.168.1.1"), age_days=0.1)
    write_record(d, "OLD.json",
                 sample_record("OLD", "192.168.1.2"), age_days=99)
    write_record(d, "TORN.json", '{"hostname": "TORN", "cpu": {', age_days=0.1)
    roster = write_roster(tmp_path, [
        ("192.168.1.1", "FRESH", "the healthy one"),
        ("192.168.1.2", "OLD", "powered off for months"),
        ("192.168.1.3", "GHOST", "never switched on since the publisher shipped"),
        ("192.168.1.4", "TORN", "writes garbage"),
    ])

    ctx = inventory.build(d, roster, stale_days=14)
    states = {ip: s["state"] for ip, s in ctx["states"].items()}

    assert states["192.168.1.1"] == inventory.STATE_CURRENT
    assert states["192.168.1.2"] == inventory.STATE_STALE
    assert states["192.168.1.3"] == inventory.STATE_NEVER
    assert states["192.168.1.4"] == inventory.STATE_UNREADABLE
    # four inputs, four distinct answers
    assert len(set(states.values())) == 4

    doc = inventory.render(ctx["roster"], ctx["matched"], ctx["unrostered"],
                           ctx["now"], 14, d)
    assert "never seen" in doc
    assert "stale, last seen" in doc
    assert "unreadable record" in doc
    # and each says which box it is about
    assert "192.168.1.3" in doc and "192.168.1.4" in doc


def test_stale_is_stamped_with_when_it_was_measured(tmp_path):
    """A fleet powered on demand always has some boxes reporting old data, and
    that is normal rather than broken - so 'stale' has to carry a date, or the
    reader cannot tell a box switched off last week from one that has been
    dark since March."""
    d = os.path.join(str(tmp_path), "records")
    write_record(d, "OLD.json", sample_record("OLD", "192.168.1.2"),
                 age_days=40)
    roster = write_roster(tmp_path, [("192.168.1.2", "OLD", "")])
    ctx = inventory.build(d, roster, stale_days=14)
    doc = inventory.render(ctx["roster"], ctx["matched"], ctx["unrostered"],
                           ctx["now"], 14, d)
    assert "40 days ago" in doc
    assert "normal for a box that has been powered off" in doc
    # every current record is stamped too
    assert ctx["states"]["192.168.1.2"]["measured"] is not None


def test_a_corrupt_or_missing_record_degrades_rather_than_crashing(tmp_path):
    """One torn record must not take the document down. A traceback here would
    hide every other box - the exact failure this file exists to prevent."""
    d = os.path.join(str(tmp_path), "records")
    write_record(d, "TORN.json", "\x00\x01 not json at all", age_days=0.1)
    write_record(d, "ARRAY.json", "[1, 2, 3]", age_days=0.1)
    write_record(d, "EMPTY.json", "", age_days=0.1)
    write_record(d, "GOOD.json", sample_record("GOOD", "192.168.1.9"),
                 age_days=0.1)
    roster = write_roster(tmp_path, [
        ("192.168.1.9", "GOOD", ""),
        ("192.168.1.8", "TORN", ""),
    ])
    ctx = inventory.build(d, roster, stale_days=14)
    doc = inventory.render(ctx["roster"], ctx["matched"], ctx["unrostered"],
                           ctx["now"], 14, d)
    # the healthy box still renders in full
    assert "GeForce4 Ti 4600" in doc
    assert ctx["states"]["192.168.1.9"]["state"] == inventory.STATE_CURRENT
    assert ctx["states"]["192.168.1.8"]["state"] == inventory.STATE_UNREADABLE
    # the junk records are reported, not swallowed
    assert "ARRAY.json" in doc or "EMPTY.json" in doc


def test_a_missing_directory_is_not_an_exception(tmp_path):
    """The share is not always mounted. Every rostered box then reads 'never
    seen', which is honest, and nothing raises."""
    roster = write_roster(tmp_path, [("192.168.1.1", "A", "")])
    ctx = inventory.build(os.path.join(str(tmp_path), "nope"), roster, 14)
    assert ctx["states"]["192.168.1.1"]["state"] == inventory.STATE_NEVER
    inventory.render(ctx["roster"], ctx["matched"], ctx["unrostered"],
                     ctx["now"], 14, "nope")


# ---------------------------------------------------------------- matching

def test_records_are_matched_by_ip_not_by_filename(tmp_path):
    """A computer name is not an identity: boxes on this fleet have been
    renamed, and .124 answers to two different names in different documents.
    The record carries the address it published from, so that is the key."""
    d = os.path.join(str(tmp_path), "records")
    write_record(d, "SOME-OLD-NAME.json",
                 sample_record("SOME-OLD-NAME", "192.168.1.133"), age_days=0.1)
    roster = write_roster(tmp_path, [("192.168.1.133", "P3-DUAL", "")])
    ctx = inventory.build(d, roster, 14)
    assert ctx["states"]["192.168.1.133"]["state"] == inventory.STATE_CURRENT
    assert ctx["unrostered"] == []


def test_an_unrostered_record_is_reported_not_dropped(tmp_path):
    """A new box appearing is information. Dropping it silently is how the
    document quietly stops describing the fleet."""
    d = os.path.join(str(tmp_path), "records")
    write_record(d, "NEWBOX.json",
                 sample_record("NEWBOX", "192.168.1.99"), age_days=0.1)
    roster = write_roster(tmp_path, [("192.168.1.1", "A", "")])
    ctx = inventory.build(d, roster, 14)
    assert [r["file"] for r in ctx["unrostered"]] == ["NEWBOX.json"]
    doc = inventory.render(ctx["roster"], ctx["matched"], ctx["unrostered"],
                           ctx["now"], 14, d)
    assert "no roster entry" in doc
    assert "NEWBOX" in doc


# ---------------------------------------------------------------- two clocks

def test_staleness_uses_the_host_clock_not_the_boxs(tmp_path):
    """A retro box's RTC is frequently years out. A record published thirty
    seconds ago must read as current however wrong the box thinks the date is -
    and the disagreement must be REPORTED rather than silently changing the
    answer."""
    d = os.path.join(str(tmp_path), "records")
    write_record(d, "SKEW.json",
                 sample_record("SKEW", "192.168.1.5",
                               reported_at="2003-04-01 09:00:00"),
                 age_days=0.001)
    roster = write_roster(tmp_path, [("192.168.1.5", "SKEW", "")])
    ctx = inventory.build(d, roster, 14)
    assert ctx["states"]["192.168.1.5"]["state"] == inventory.STATE_CURRENT
    doc = inventory.render(ctx["roster"], ctx["matched"], ctx["unrostered"],
                           ctx["now"], 14, d)
    assert "Clock skew" in doc
    assert "2003-04-01" in doc


# ---------------------------------------------------------------- content

def test_a_record_from_the_future_is_current_not_negative_aged(tmp_path):
    """A record cannot be older than zero. If the file server's clock and this
    host's disagree, an mtime in the future must render as current and the
    skew must be reported - never as a negative age, and never as a reason to
    doubt a record that has just been written.

    This is not hypothetical: .124's own clock runs two hours fast, which is
    how the CopyFile-carries-the-source-timestamp bug was found in the first
    place."""
    d = os.path.join(str(tmp_path), "records")
    write_record(d, "AHEAD.json", sample_record("AHEAD", "192.168.1.7"),
                 age_days=-0.2)          # two hours in the future
    roster = write_roster(tmp_path, [("192.168.1.7", "AHEAD", "")])
    ctx = inventory.build(d, roster, 14)
    assert ctx["states"]["192.168.1.7"]["state"] == inventory.STATE_CURRENT
    doc = inventory.render(ctx["roster"], ctx["matched"], ctx["unrostered"],
                           ctx["now"], 14, d)
    assert "-1 days ago" not in doc and "-0 " not in doc


def test_every_value_is_labelled_as_a_snapshot(tmp_path):
    """The agent version in a record was true when the box wrote it and can be
    false an hour later - and so can every other field. A reader must be able
    to tell 'what .143 SAID at 12:47' from 'what .143 IS', or the document goes
    stale in the reader's head even while the file is correct."""
    d = os.path.join(str(tmp_path), "records")
    write_record(d, "A.json", sample_record("A", "192.168.1.1"), age_days=0.1)
    roster = write_roster(tmp_path, [("192.168.1.1", "A", "")])
    ctx = inventory.build(d, roster, 14)
    doc = inventory.render(ctx["roster"], ctx["matched"], ctx["unrostered"],
                           ctx["now"], 14, d)
    assert "SNAPSHOT" in doc
    assert "As this box reported itself at" in doc
    # the agent version is dated where it is shown, not stated bare
    assert "the version that WROTE this record" in doc
    # and the summary carries it too, so a reader scanning the table sees it
    assert "| Agent |" in doc.split("## Summary")[1].split("---")[0]


def test_the_document_says_it_is_generated(tmp_path):
    """A generated file that does not say so gets hand-edited, and then there
    are two sources of truth again."""
    roster = write_roster(tmp_path, [("192.168.1.1", "A", "")])
    ctx = inventory.build(os.path.join(str(tmp_path), "none"), roster, 14)
    doc = inventory.render(ctx["roster"], ctx["matched"], ctx["unrostered"],
                           ctx["now"], 14, "none")
    assert doc.splitlines()[0].startswith("<!-- GENERATED FILE")
    assert "Do not hand-edit" in doc


def test_the_second_adapter_and_the_hidden_voodoo_are_rendered(tmp_path):
    """The two traps that made the old table wrong.

    .143 renders on a GeForce 6800 with its Voodoo5 5500 as a SECOND adapter,
    and .171's Voodoo 2 is Class=MEDIA so it appears in no display-class scan
    at all - only in the PCI enumerator. A document that shows one card per box
    reproduces exactly the mistake that sized a Voodoo5 test matrix at two
    boxes when only one had the card.
    """
    d = os.path.join(str(tmp_path), "records")
    rec = sample_record("1GHZ", "192.168.1.143")
    rec["video_cards"] = [
        {"instance": "0000", "name": "NVIDIA GeForce 6800",
         "pci_ven": "0x10DE", "pci_dev": "0x0041", "hardware_id": "",
         "driver_version": "6.14.10.8198", "driver_date": "",
         "attached_to_desktop": True},
        {"instance": "0001", "name": "3dfx Voodoo5 5500",
         "pci_ven": "0x121A", "pci_dev": "0x0009", "hardware_id": "",
         "driver_version": "1.04.00", "driver_date": "",
         "attached_to_desktop": False},
    ]
    rec["accelerators"] = [
        {"device_key": "VEN_121A&DEV_0009&SUBSYS_0002121A",
         "pci_ven": "0x121A", "pci_dev": "0x0009",
         "description": "3dfx Voodoo5 5500", "count": 1},
    ]
    write_record(d, "1GHZ.json", rec, age_days=0.1)
    roster = write_roster(tmp_path, [("192.168.1.143", "1GHZ", "")])
    ctx = inventory.build(d, roster, 14)
    doc = inventory.render(ctx["roster"], ctx["matched"], ctx["unrostered"],
                           ctx["now"], 14, d)

    assert "GeForce 6800" in doc and "Voodoo5 5500" in doc
    assert "Video adapters on this box" in doc
    assert "3dfx silicon: 1 card" in doc
    assert "Class=MEDIA" in doc  # says WHY the PCI enumerator is the source


def test_no_glide_silicon_is_stated_positively(tmp_path):
    """.133's V5 6000 is gone, and 'no VEN_121A key at all' is the decisive
    read - a fitted card enumerates there even with no driver bound. Rendering
    that as merely an absent row would leave the reader unsure whether the card
    is missing or just undriven."""
    d = os.path.join(str(tmp_path), "records")
    write_record(d, "P3-DUAL.json", sample_record(), age_days=0.1)
    roster = write_roster(tmp_path, [("192.168.1.133", "P3-DUAL", "")])
    ctx = inventory.build(d, roster, 14)
    doc = inventory.render(ctx["roster"], ctx["matched"], ctx["unrostered"],
                           ctx["now"], 14, d)
    assert "No 3dfx silicon" in doc
    assert "even with no driver bound" in doc


def test_the_persisted_mode_wins_over_the_live_one(tmp_path):
    """.123 and .240 are sitting at 640x480 right now because a game exited
    without restoring. A tool that trusts the live mode pins the box there for
    good; the persisted mode is what the machine is configured to be."""
    d = os.path.join(str(tmp_path), "records")
    write_record(d, "P3-DUAL.json", sample_record(), age_days=0.1)
    roster = write_roster(tmp_path, [("192.168.1.133", "P3-DUAL", "")])
    ctx = inventory.build(d, roster, 14)
    doc = inventory.render(ctx["roster"], ctx["matched"], ctx["unrostered"],
                           ctx["now"], 14, d)
    assert "persisted **1280x1024" in doc
    assert "currently 640x480" in doc


def test_ram_is_reported_as_measured_not_rounded(tmp_path):
    """.133 has 255 MB, one megabyte under the 256 MB floor several 2004 titles
    publish. That is not a rounding artifact - it is the number the capability
    gate sees, so the document must not tidy it into 256."""
    d = os.path.join(str(tmp_path), "records")
    write_record(d, "P3-DUAL.json", sample_record(), age_days=0.1)
    roster = write_roster(tmp_path, [("192.168.1.133", "P3-DUAL", "")])
    ctx = inventory.build(d, roster, 14)
    doc = inventory.render(ctx["roster"], ctx["matched"], ctx["unrostered"],
                           ctx["now"], 14, d)
    assert "255 MB" in doc
    assert "256 MB" not in doc


# ---------------------------------------------------------------- CLI

def test_cli_check_and_json_modes(tmp_path):
    d = os.path.join(str(tmp_path), "records")
    write_record(d, "A.json", sample_record("A", "192.168.1.1"), age_days=0.1)
    roster = write_roster(tmp_path, [("192.168.1.1", "A", ""),
                                     ("192.168.1.2", "B", "")])

    out = subprocess.run(
        [sys.executable, SCRIPT, "--dir", d, "--roster", roster, "--json"],
        capture_output=True, text=True, timeout=120)
    assert out.returncode == 0, out.stderr
    data = json.loads(out.stdout)
    assert data["hosts"]["192.168.1.1"]["state"] == "current"
    assert data["hosts"]["192.168.1.2"]["state"] == "never seen"

    # --check fails while a box is not current, and says which
    out = subprocess.run(
        [sys.executable, SCRIPT, "--dir", d, "--roster", roster,
         "--stdout", "--check"],
        capture_output=True, text=True, timeout=120)
    assert out.returncode == 1
    assert "192.168.1.2" in out.stderr


def test_the_share_copy_is_best_effort_not_fatal(tmp_path):
    """The read-write gvfs mount is per-login-session and can be absent
    headless. Its absence must be REPORTED and must not lose the document -
    whose home is the repo, always present."""
    d = os.path.join(str(tmp_path), "records")
    write_record(d, "A.json", sample_record("A", "192.168.1.1"), age_days=0.1)
    roster = write_roster(tmp_path, [("192.168.1.1", "A", "")])
    out = os.path.join(str(tmp_path), "doc.md")
    # a path under a file is guaranteed unusable as a directory
    blocker = os.path.join(str(tmp_path), "blocker")
    open(blocker, "w").close()

    res = subprocess.run(
        [sys.executable, SCRIPT, "--dir", d, "--roster", roster,
         "--out", out, "--share-copy", os.path.join(blocker, "sub")],
        capture_output=True, text=True, timeout=120)
    assert res.returncode == 0, res.stderr
    assert "share copy skipped" in res.stdout
    # the document itself still landed
    assert os.path.exists(out)
    assert "Fleet hardware inventory" in open(out, encoding="utf-8").read()


def test_the_shipped_roster_parses_and_carries_no_measurements():
    """The roster holds prose a probe cannot discover, and NOTHING measured.
    A CPU or a card in here is the hand-maintained table growing back."""
    rows = inventory.load_roster(
        os.path.join(REPO, "scripts", "fleet", "fleet-roster.txt"))
    assert len(rows) >= 8
    ips = [r[0] for r in rows]
    assert len(ips) == len(set(ips)), "no duplicate hosts"
    for ip, host, _note in rows:
        assert ip.count(".") == 3, ip
        assert host, "every roster row names the box"
