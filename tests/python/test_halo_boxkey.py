"""A box's own Halo key must survive GAMESYNC (2026-09-28).

Halo allows one simultaneous player per key, so scripts/halo/assign_keys.py
gives every box its own DigitalProductID. GAMESYNC merges a title's install.reg
each time it walks the title, and Halo's carries the library's single key:
.145 and .240, given distinct keys on 2026-09-01, were measured back on the
SAME key on 2026-09-28 - the two could not be in one game, with nothing saying
so. Now assign_keys.py also keeps the box's key outside the game tree, the
staged Play Halo.bat re-applies it at every launch, and audit_keys.py reports
the key the box actually PLAYS on. These tests pin the three together.
"""
import hashlib
import importlib.util
import os
import sys

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
HALO = os.path.join(REPO, "scripts", "halo")
sys.path.insert(0, HALO)
import boxkey  # noqa: E402


def _load(path, name):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


SF = _load(os.path.join(REPO, "scripts", "fleet", "stage-fleetres.py"), "stage_fleetres_hk")
AUDIT = _load(os.path.join(HALO, "audit_keys.py"), "halo_audit_hk")

BLOB = bytes(range(164))                     # stands in for a 164-byte DPID


def _reg(blob):
    hx = ",".join("%02x" % b for b in blob)
    lines, cur = [], ""
    for part in hx.split(","):               # regedit's own 25-per-line wrap
        cur += part + ","
        if len(cur) > 70:
            lines.append(cur)
            cur = ""
    body = "\\\r\n  ".join(lines + [cur.rstrip(",")])
    return ('REGEDIT4\r\n\r\n[HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Microsoft Games\\Halo]\r\n'
            '"ProductId"="x"\r\n"DigitalProductID"=hex:' + body + '\r\n')


def test_the_launcher_and_the_scripts_name_the_same_file():
    assert SF.HALO_BOXKEY_BAT == "%ALLUSERSPROFILE%\\" + boxkey.BOXKEY_REL


def test_the_halo_launcher_reapplies_the_box_key_before_starting_the_game():
    post = SF.TITLES["Halo"]["post"]
    pb = [p for p in post if p["marker"] == "HALO_BOXKEY"]
    assert len(pb) == 1
    pb = pb[0]
    assert pb["file"] == "Play Halo.bat"
    assert pb["before"] == 'start "" halo.exe'
    text = "\n".join(pb["lines"])
    assert "HALO_BOXKEY" in text                     # the idempotency marker
    assert 'reg import "%s"' % SF.HALO_BOXKEY_BAT in text
    # regedit /s would raise a UAC prompt at every launch on a Windows 7 box
    assert "regedit" not in text
    # a box with no key of its own must SAY so rather than share silently
    assert "NO PER-BOX HALO KEY" in text
    # cmd.exe expands %VAR% inside ( ) blocks at parse time, and a ( or ) in
    # a generated value has cost this project three times
    assert "(" not in text and ")" not in text


def test_a_reg_file_fingerprints_like_the_registry_value():
    """audit_keys.py hashes REGREAD's "01 02 ..." form; the box-local file is
    a .reg with continuation lines. The same blob must give the same print,
    or the audit could never see that two boxes share a key."""
    regread = " ".join("%02x" % b for b in BLOB)
    hexs = regread.replace(" ", "").lower()
    from_registry = hashlib.sha256(hexs.encode()).hexdigest()[:10]
    assert boxkey.dpid_fp_from_reg(_reg(BLOB)) == from_registry
    assert boxkey.dpid_fp(regread) == from_registry
    other = boxkey.dpid_fp_from_reg(_reg(bytes(reversed(BLOB))))
    assert other != from_registry


def test_a_reg_file_without_the_value_has_no_fingerprint():
    assert boxkey.dpid_fp_from_reg('REGEDIT4\r\n\r\n[HKEY_CURRENT_USER\\x]\r\n"FIRSTRUN"=dword:1\r\n') is None


def test_the_box_local_key_is_the_one_the_box_plays_on():
    fp, src = AUDIT.effective_key("aaaa", "bbbb")
    assert fp == "bbbb" and "aaaa" in src      # registry = library key until launch
    fp, src = AUDIT.effective_key("bbbb", "bbbb")
    assert fp == "bbbb" and src == "box-local"


def test_a_registry_only_key_is_reported_as_temporary():
    fp, src = AUDIT.effective_key("aaaa", None)
    assert fp == "aaaa"
    assert "NO box-local copy" in src and "GAMESYNC" in src


def test_no_key_at_all_is_no_key():
    assert AUDIT.effective_key(None, None) == (None, "")


def test_assign_writes_and_reads_back_the_box_local_copy():
    src = open(os.path.join(HALO, "assign_keys.py"), encoding="utf-8").read()
    assert "boxkey_path(c)" in src
    assert '"UPLOAD " + path' in src and '"DOWNLOAD " + path' in src
    assert "back == blob" in src, "the copy must be verified by its bytes"


def test_the_staged_launcher_carries_the_block():
    """Share-side: what the fleet actually runs. SKIPS loudly off the LAN."""
    bat = "/mnt/retro-share/Files/Games-Library/Halo/Play Halo.bat"
    if not os.path.isfile(bat):
        pytest.skip("share not mounted - the STAGED launcher is NOT verified")
    flat = open(bat, encoding="latin-1").read().replace("\r\n", "\n")
    block = "\n".join(SF.halo_boxkey())
    assert block in flat, "Play Halo.bat on the share lacks the HALO_BOXKEY block"
    assert flat.index(block) < flat.index('start "" halo.exe')


ASSIGN = _load(os.path.join(HALO, "assign_keys.py"), "halo_assign_hk")


def test_the_shipped_map_gives_every_box_its_own_key():
    mp = ASSIGN.load_map(ASSIGN.MAP_DEFAULT)
    names = list(mp.values())
    assert len(names) == len(set(names))
    # the library key is what every UNASSIGNED box plays on; -2 is REJECTED
    assert "fleet-gamekey-halo-pc" not in names
    assert "fleet-gamekey-halo-pc-2" not in names
    assert "NSC-C543575F526" in mp          # .124, the V5 6000 box (2026-09-28)


def test_a_map_naming_one_key_twice_is_refused(tmp_path):
    f = tmp_path / "m.txt"
    f.write_text("A\tfleet-gamekey-halo-pc-4\nb\tfleet-gamekey-halo-pc-4\n")
    with pytest.raises(SystemExit):
        ASSIGN.load_map(str(f))


def test_the_map_is_keyed_case_insensitively(tmp_path):
    f = tmp_path / "m.txt"
    f.write_text("dell\tfleet-gamekey-halo-pc-7\n")
    assert ASSIGN.load_map(str(f)) == {"DELL": "fleet-gamekey-halo-pc-7"}


def test_the_audit_audits_the_roster_not_a_stale_list():
    assert AUDIT.DEFAULT_BOXES == AUDIT.roster_boxes()
    assert "192.168.1.197" in AUDIT.DEFAULT_BOXES   # ADMIN-PC (was .246, then .195)


def test_halo_skips_its_intro_movies_only_where_vcr_kmd_drives_the_screen():
    """The Bink intros ran >13 minutes on .124's vcr-kmd (2026-09-28)."""
    post = [p for p in SF.TITLES["Halo"]["post"] if p["marker"] == "HALO_NOVIDEO"]
    assert len(post) == 1
    text = "\n".join(post[0]["lines"])
    assert 'sc query %s 2>nul | find /i "RUNNING"' % SF.VCRKMD_SERVICE in text
    assert "set HALO_NOVIDEO=\n" in text + "\n"      # cleared first: never inherited
    # the start line carries it, and the block must not sit inside the span a
    # HALO_BOXKEY refresh replaces (its first line up to the start line)
    assert post[0]["before"] != 'start "" halo.exe'
    fixes = dict(SF.TITLES["Halo"]["fix"]["Play Halo.bat"])
    assert fixes['start "" halo.exe -vidmode'] == 'start "" halo.exe %HALO_NOVIDEO% -vidmode'
