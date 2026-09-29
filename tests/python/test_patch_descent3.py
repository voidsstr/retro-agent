"""Descent 3 1.4 pilot patch - provisioning/patches/descent3/apply.py.

The defect: the launcher's -Width/-Height switch the display to 1920x1080 but
the in-flight view stays a 640x480 box, because SetScreenMode(SM_GAME) sizes
the game window from the PILOT file (game.cpp -> InitGameScreen, which clamps to
the display) and the staged sdf.plt saved 640x480. Measured 2026-09-29 on .195
and .240; the fix (4096x4096 in the pilot) verified on .240 with a test pilot
that is this build's output with the name bytes changed (HW_VERIFIED).

Pure-logic tests build synthetic pilot files; share tests SKIP LOUDLY when the
library is not mounted.

The module is loaded under its OWN name ("descent3_apply"), never as a bare
`import apply`: several patch builders ship an apply.py, and tests/run_all.sh
runs one pytest process, so a bare import hands whichever test runs second the
FIRST test's module out of sys.modules (measured 2026-09-29 against
test_patch_idtech2.py: both orders failed).
"""
import hashlib
import importlib.util
import json
import os
import struct
import sys

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
APPLY = os.path.join(HERE, "..", "..", "provisioning", "patches", "descent3", "apply.py")
_spec = importlib.util.spec_from_file_location("descent3_apply", APPLY)
d3 = importlib.util.module_from_spec(_spec)
sys.modules["descent3_apply"] = d3
_spec.loader.exec_module(d3)

LIBRARY = "/mnt/retro-share/Files/Games-Library"
ORIGINAL_MD5 = "b03f1b0f19e6d430f3d64b8c426f4da6"
PATCHED_MD5 = "a42937534d704dce2374df0a0deea849"
RVW_VERIFIED_MD5 = "21a07b5456ca988d23a8d6c6b1a1ecc9"   # the pilot run on .240


def make_pilot(name="sdf", version=0x2A, w=640, h=480, ship="Pyro-GL",
               strings=("", "", "", "", ""), tail=b"\x00\x02" + b"Pilot Training\x00" + b"\x00" * 64):
    """Build a pilot header exactly as pilot::write lays it out."""
    b = struct.pack("<i", version)
    b += name.encode() + b"\0" + ship.encode() + b"\0"
    n = 5 if version >= d3.PFV_AUDIOTAUNT3N4 else 3
    for s in strings[:n]:
        b += s.encode() + b"\0"
    b += struct.pack("<H", 0xFFFF) + b"\x01"          # picture_id, difficulty
    if version >= d3.PFV_PROFANITY:
        b += b"\x00"
    if version >= d3.PFV_AUDIOTAUNTS:
        b += b"\x01"
    b += b"\x02" + struct.pack("<HH", 0x0040, 0x3FBE)  # hud_mode, hud_stat, graphical
    b += struct.pack("<ii", w, h)
    if version >= d3.PFV_REARVIEWINFO:
        b += b"\x00\x00"
    return b + tail


def exp_for(data, name="sdf", version=0x2A):
    p = d3.parse_pilot(data)
    return {"md5": hashlib.md5(data).hexdigest(), "size": len(data), "version": version,
            "name": name, "w_off": p["w_off"], "h_off": p["h_off"], "old_w": 640, "old_h": 480,
            "old_bytes": struct.pack("<ii", 640, 480)}


def md5(b):
    return hashlib.md5(b).hexdigest()


# ---------------------------------------------------------------- pure logic

def test_loaded_under_its_own_name_not_as_a_bare_apply_module():
    assert d3.__name__ == "descent3_apply"
    assert sys.modules.get("apply") is not d3


def test_synthetic_sdf_pilot_has_the_staged_offsets():
    p = d3.parse_pilot(make_pilot())
    assert (p["w_off"], p["h_off"]) == (0x1F, 0x23)
    assert (p["game_window_w"], p["game_window_h"]) == (640, 480)
    assert p["name"] == "sdf" and p["ship"] == "Pyro-GL" and p["hud_mode"] == 2


def test_the_recorded_offsets_match_the_expectation_table():
    e = d3.PILOTS["sdf.plt"]
    assert (e["w_off"], e["h_off"], e["version"]) == (0x1F, 0x23, 0x2A)
    assert e["old_bytes"] == struct.pack("<ii", 640, 480)
    assert (e["md5"], e["patched_md5"]) == (ORIGINAL_MD5, PATCHED_MD5)
    assert (d3.TARGET_W, d3.TARGET_H) == (4096, 4096)
    assert d3.HW_VERIFIED["md5"] == RVW_VERIFIED_MD5


def test_offsets_move_with_the_name_length_so_the_parser_not_a_constant_decides():
    p = d3.parse_pilot(make_pilot(name="fleet"))
    assert (p["w_off"], p["h_off"]) == (0x21, 0x25)


def test_patch_changes_exactly_the_eight_window_bytes():
    src = make_pilot()
    out = d3.patch_pilot(src, exp_for(src))
    assert len(out) == len(src)
    diff = [i for i in range(len(src)) if src[i] != out[i]]
    assert min(diff) >= 0x1F and max(diff) <= 0x26
    assert out[0x1F:0x27] == bytes.fromhex("0010000000100000")
    q = d3.parse_pilot(out)
    assert (q["game_window_w"], q["game_window_h"]) == (4096, 4096)


def test_old_buggy_value_is_640x480_and_fixed_value_is_4096():
    """Both states named, so a regression to either is visible."""
    src = make_pilot()
    assert d3.parse_pilot(src)["game_window_w"] == 640
    assert d3.parse_pilot(d3.patch_pilot(src, exp_for(src)))["game_window_w"] == 4096


def test_revert_is_the_exact_inverse_of_patch():
    src = make_pilot()
    e = exp_for(src)
    assert d3.revert_pilot(d3.patch_pilot(src, e), e) == src
    with pytest.raises(d3.PilotError):          # an unpatched file has nothing to revert
        d3.revert_pilot(src, e)


def test_a_recorded_patched_md5_that_does_not_match_is_refused():
    src = make_pilot()
    e = exp_for(src)
    e["patched_md5"] = "0" * 32
    with pytest.raises(d3.PilotError):
        d3.patch_pilot(src, e)


def test_refuses_version_0x2B_a_pilot_the_game_has_already_rewritten():
    src = make_pilot(version=0x2B)
    p = d3.parse_pilot(src)          # the parser understands it...
    assert (p["w_off"], p["game_window_w"]) == (0x1F, 640)
    e = exp_for(src)
    e["version"] = 0x2B
    with pytest.raises(d3.PilotError):   # ...but the patch was only proven on 0x2A
        d3.patch_pilot(src, e)


@pytest.mark.parametrize("kw", [{"w": 800, "h": 600}, {"w": 640, "h": 400}])
def test_refuses_unexpected_old_values(kw):
    src = make_pilot(**kw)
    e = exp_for(make_pilot())
    with pytest.raises(d3.PilotError):
        d3.patch_pilot(src, e)


def test_refuses_when_the_parse_lands_elsewhere():
    src = make_pilot(name="fleet")
    e = exp_for(make_pilot())        # expects 0x1F
    e["name"] = "fleet"
    with pytest.raises(d3.PilotError):
        d3.patch_pilot(src, e)


def test_refuses_truncated_and_garbage():
    with pytest.raises(d3.PilotError):
        d3.parse_pilot(b"\x2a\x00\x00\x00sdf")               # unterminated name
    with pytest.raises(d3.PilotError):
        d3.parse_pilot(b"\xff" * 64)                         # implausible version
    whole = make_pilot(tail=b"")
    for cut in range(len(whole) - 1, 0x10, -1):              # every cut inside the header
        with pytest.raises(d3.PilotError):
            d3.parse_pilot(whole[:cut])


def test_state_of_three_states():
    src = make_pilot()
    e = exp_for(src)
    assert d3.state_of(src, e) == "original"
    assert d3.state_of(d3.patch_pilot(src, e), e) == "patched"
    assert d3.state_of(make_pilot(w=1920, h=1080), e).startswith("unknown")


def test_patched_means_this_patch_on_this_original_not_any_4096_pilot():
    """A pilot that merely carries 4096x4096 - another pilot, or one someone
    edited - must not pass as published."""
    src = make_pilot()
    e = exp_for(src)
    other = bytearray(d3.patch_pilot(src, e))
    other[-1] ^= 0xFF                                        # a byte outside the window
    st = d3.state_of(bytes(other), e)
    assert st.startswith("unknown") and "differs from the original" in st
    with pytest.raises(d3.PilotError):
        d3.original_of(bytes(other), e)


def test_find_pilots_is_case_insensitive(tmp_path):
    (tmp_path / "SDF.PLT").write_bytes(b"x")
    (tmp_path / "sub").mkdir()
    (tmp_path / "sub" / "other.Plt").write_bytes(b"x")
    (tmp_path / "default keyboard.pld").write_bytes(b"x")
    assert d3.find_pilots(str(tmp_path)) == ["SDF.PLT", "sub/other.Plt"]
    assert d3.expected_for("SDF.PLT")[0] == "sdf.plt"
    assert d3.locate(str(tmp_path), "sdf.plt") == "SDF.PLT"


def test_an_unmounted_library_fails_loudly(tmp_path):
    with pytest.raises(SystemExit) as ei:
        d3.cmd_check(str(tmp_path / "not-mounted"))
    assert "not mounted" in str(ei.value)


# ------------------------------------------------ build / check / publish on a fake library

@pytest.fixture
def fake(tmp_path, monkeypatch):
    """A fake library holding a synthetic ORIGINAL sdf.plt, wired into PILOTS."""
    lib = tmp_path / "lib"
    (lib / "Descent3").mkdir(parents=True)
    src = make_pilot()
    e = exp_for(src)
    e["patched_md5"] = md5(d3.patch_pilot(src, e))
    monkeypatch.setitem(d3.PILOTS, "sdf.plt", e)
    (lib / "Descent3" / "sdf.plt").write_bytes(src)
    return {"lib": lib, "src": src, "e": e, "out": tmp_path / "out",
            "patched": d3.patch_pilot(src, e),
            "backup": lib / "_patches" / "Descent3" / d3.BACKUP_DIR / "sdf.plt"}


def test_check_and_build_from_the_original(fake):
    assert d3.cmd_check(str(fake["lib"])) == 0
    assert d3.cmd_build(str(fake["lib"]), str(fake["out"])) == 0
    built = (fake["out"] / "Descent3" / "sdf.plt").read_bytes()
    assert built == fake["patched"]
    assert (fake["out"] / "originals" / "Descent3" / "sdf.plt").read_bytes() == fake["src"]
    m = json.loads((fake["out"] / "manifest.json").read_text())
    o = m["outputs"][0]
    assert (o["rel"], o["md5"], o["original_md5"]) == ("sdf.plt", md5(built), fake["e"]["md5"])
    assert o["backup_share_path"] == "Files/Games-Library/_patches/Descent3/originals-2026-09-29/sdf.plt"


def test_build_is_idempotent_and_still_works_once_the_share_is_patched(fake):
    """After --publish the live file is the patched one: the build must reproduce
    the SAME output and the SAME original, not refuse."""
    d3.cmd_build(str(fake["lib"]), str(fake["out"]))
    first = (fake["out"] / "Descent3" / "sdf.plt").read_bytes()
    (fake["lib"] / "Descent3" / "sdf.plt").write_bytes(fake["patched"])    # "published"
    out2 = fake["out"].parent / "out2"
    assert d3.cmd_build(str(fake["lib"]), str(out2)) == 0
    assert (out2 / "Descent3" / "sdf.plt").read_bytes() == first
    assert (out2 / "originals" / "Descent3" / "sdf.plt").read_bytes() == fake["src"]
    m = json.loads((out2 / "manifest.json").read_text())
    assert "reconstructed" in m["outputs"][0]["built_from"]


def test_build_refuses_a_staged_pilot_in_an_unknown_state(fake):
    (fake["lib"] / "Descent3" / "sdf.plt").write_bytes(make_pilot(w=1920, h=1080))
    with pytest.raises(SystemExit):
        d3.cmd_build(str(fake["lib"]), str(fake["out"]))


def test_check_states_of_a_published_share(fake, capsys):
    (fake["lib"] / "Descent3" / "sdf.plt").write_bytes(fake["patched"])
    assert d3.cmd_check(str(fake["lib"])) == 0                   # no backup: WARN, not FAIL
    assert "WARN no backup" in capsys.readouterr().out
    fake["backup"].parent.mkdir(parents=True)
    fake["backup"].write_bytes(fake["src"])
    assert d3.cmd_check(str(fake["lib"])) == 0
    assert "backup present" in capsys.readouterr().out
    fake["backup"].write_bytes(fake["patched"])                   # a "backup" that is not the original
    assert d3.cmd_check(str(fake["lib"])) == 1


def test_check_fails_on_a_pilot_it_does_not_know_and_on_a_missing_one(fake):
    (fake["lib"] / "Descent3" / "other.plt").write_bytes(make_pilot(name="other"))
    assert d3.cmd_check(str(fake["lib"])) == 1
    (fake["lib"] / "Descent3" / "other.plt").unlink()
    (fake["lib"] / "Descent3" / "sdf.plt").unlink()
    assert d3.cmd_check(str(fake["lib"])) == 1


class FakeShare:
    """The share as publish sees it: md5 per share path, and a put that lands
    (or, with drop=<path>, silently does not - the gvfs failure mode)."""

    def __init__(self, files=None, drop=(), rc=0):
        self.files, self.drop, self.rc, self.puts = dict(files or {}), set(drop), rc, []

    def md5(self, rel):
        return self.files.get(rel)

    def put(self, local, dest, dry_run):
        self.puts.append(dest)
        if self.rc == 0 and not dry_run and dest not in self.drop:
            self.files[dest] = md5(open(local, "rb").read())
        return self.rc


LIVE = "Files/Games-Library/Descent3/sdf.plt"
BACKUP = "Files/Games-Library/_patches/Descent3/originals-2026-09-29/sdf.plt"


def _publish(fake, monkeypatch, share, dry_run=False):
    monkeypatch.setattr(d3, "_put", share.put)
    monkeypatch.setattr(d3, "_share_md5", share.md5)
    return d3.cmd_publish(str(fake["out"]), dry_run=dry_run)


def test_publish_backs_up_first_then_puts_and_is_idempotent(fake, monkeypatch):
    d3.cmd_build(str(fake["lib"]), str(fake["out"]))
    share = FakeShare({LIVE: fake["e"]["md5"]})
    assert _publish(fake, monkeypatch, share) == 0
    assert share.puts == [BACKUP, LIVE]                           # backup THEN live, one each
    assert (share.files[LIVE], share.files[BACKUP]) == (md5(fake["patched"]), fake["e"]["md5"])
    share.puts.clear()
    assert _publish(fake, monkeypatch, share) == 0 and share.puts == []   # second run: no-op


def test_publish_dry_run_puts_in_order_and_writes_nothing(fake, monkeypatch):
    d3.cmd_build(str(fake["lib"]), str(fake["out"]))
    share = FakeShare({LIVE: fake["e"]["md5"]})
    assert _publish(fake, monkeypatch, share, dry_run=True) == 0
    assert share.puts == [BACKUP, LIVE] and share.files == {LIVE: fake["e"]["md5"]}


def test_publish_on_an_already_patched_share_only_adds_a_missing_backup(fake, monkeypatch):
    d3.cmd_build(str(fake["lib"]), str(fake["out"]))
    share = FakeShare({LIVE: md5(fake["patched"])})
    assert _publish(fake, monkeypatch, share) == 0 and share.puts == [BACKUP]
    share.puts.clear()
    assert _publish(fake, monkeypatch, share) == 0 and share.puts == []


def test_publish_refuses_an_unknown_share_file_or_backup(fake, monkeypatch):
    d3.cmd_build(str(fake["lib"]), str(fake["out"]))
    share = FakeShare({LIVE: "0" * 32})
    assert _publish(fake, monkeypatch, share) == 1 and share.puts == []
    share = FakeShare({LIVE: fake["e"]["md5"], BACKUP: "1" * 32})
    assert _publish(fake, monkeypatch, share) == 1 and share.puts == []


def test_publish_stops_when_the_backup_put_fails(fake, monkeypatch):
    d3.cmd_build(str(fake["lib"]), str(fake["out"]))
    share = FakeShare({LIVE: fake["e"]["md5"]}, rc=3)
    assert _publish(fake, monkeypatch, share) == 3 and share.puts == [BACKUP]


@pytest.mark.parametrize("dropped", [BACKUP, LIVE])
def test_publish_verifies_each_put_through_the_share(fake, monkeypatch, dropped):
    """A put that 'succeeds' but never lands (gvfs has done exactly this here)
    must fail - and a lost backup must stop before the live file is touched."""
    d3.cmd_build(str(fake["lib"]), str(fake["out"]))
    share = FakeShare({LIVE: fake["e"]["md5"]}, drop=[dropped])
    assert _publish(fake, monkeypatch, share) == 3
    assert share.puts == ([BACKUP] if dropped == BACKUP else [BACKUP, LIVE])


def test_publish_refuses_local_files_that_are_not_the_build(fake, monkeypatch):
    d3.cmd_build(str(fake["lib"]), str(fake["out"]))
    share = FakeShare({LIVE: fake["e"]["md5"]})
    local = fake["out"] / "Descent3" / "sdf.plt"
    good = local.read_bytes()
    local.write_bytes(fake["src"])                                # tampered / stale
    assert _publish(fake, monkeypatch, share) == 1 and share.puts == []
    local.write_bytes(good)
    (fake["out"] / "originals" / "Descent3" / "sdf.plt").write_bytes(good)
    assert _publish(fake, monkeypatch, share) == 1 and share.puts == []


# ---------------------------------------------------------------- the share

def _need_share():
    if not os.path.isdir(LIBRARY):
        pytest.skip("SKIPPED LOUDLY: %s is not mounted - the staged Descent3 pilot was NOT checked"
                    % LIBRARY)


def test_staged_tree_holds_exactly_the_pilots_we_patch():
    _need_share()
    tree = d3.tree_dir(LIBRARY)
    assert [p.lower() for p in d3.find_pilots(tree)] == ["sdf.plt"]


def test_staged_pilot_is_the_original_or_already_patched():
    _need_share()
    tree = d3.tree_dir(LIBRARY)
    data = open(os.path.join(tree, d3.locate(tree, "sdf.plt")), "rb").read()
    st = d3.state_of(data, d3.PILOTS["sdf.plt"])
    assert st in ("original", "patched"), st
    assert md5(data) in (ORIGINAL_MD5, PATCHED_MD5)


def test_building_from_the_share_gives_the_pilot_proven_on_hardware(tmp_path):
    """The build's output, renamed to the test pilot 'rvw', is byte-for-byte the
    file that opened the level full screen at 1920x1080 on .240."""
    _need_share()
    assert d3.cmd_build(LIBRARY, str(tmp_path)) == 0
    built = (tmp_path / "Descent3" / "sdf.plt").read_bytes()
    assert md5(built) == PATCHED_MD5
    assert md5((tmp_path / "originals" / "Descent3" / "sdf.plt").read_bytes()) == ORIGINAL_MD5
    assert built[4:8] == b"sdf\0"
    rvw = built[:4] + b"rvw\0" + built[8:]
    assert md5(rvw) == RVW_VERIFIED_MD5


def test_check_on_the_share_passes():
    _need_share()
    assert d3.cmd_check(LIBRARY) == 0
