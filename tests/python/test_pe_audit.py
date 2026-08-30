"""Regression: the PE audit must FAIL on a Vista-only image and on an
overwritten TimeDateStamp, and must NOT fail merely because the stamp bytes
happen to be printable ASCII.

Both halves come from staging Halo: Combat Evolved (PC) on 2026-08-30.

  * The zip on the share shipped a FAiRLiGHT-cracked halo.exe whose PE
    TimeDateStamp was 0x21544c66 - raw little-endian bytes b'fLT!', decoding to
    1987, impossible for a 2003 binary. That is the finding.

  * In the SAME tree, haloupdate.exe (0x3f59224c = b'L"Y?'), ogg.dll and
    vorbis.dll also have all-printable stamps - with correct 2003 dates, as does
    the genuine Bungie 1.0.10 halo.exe itself (0x53726852 = b'RhrS'). Four
    bytes are printable maybe a quarter of the time, so in any real tree several
    binaries will trip a naive "the stamp looks like text" rule. An earlier pass
    on this project reported exactly that class of phantom finding (citing
    "clean section layout" as tamper evidence, which a genuine Microsoft control
    binary then reproduced byte for byte). So printable-ASCII is recorded as an
    observation and is never on its own a fault.

  * SubsystemVersion >= 6.0 is Vista-only and XP's loader refuses the image
    before a single instruction runs - CLAUDE.md checklist item 8, the fault
    that made SiN Gold unloadable on every XP box.
"""

import importlib.util
import struct
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
TOOL = REPO / "scripts" / "fleet" / "pe-audit.py"

_spec = importlib.util.spec_from_file_location("pe_audit", TOOL)
pe_audit = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(pe_audit)

# Real values measured on the fleet's Halo tree, 2026-08-30.
HALO_FAIRLIGHT_TDS = 0x21544C66  # b'fLT!' -> 1987, impossible
HALOUPDATE_TDS = 0x3F59224C  # b'L"Y?' -> 2003-09-05, fine
HALO_110_TDS = 0x53726852  # 2014-05-13, the genuine Bungie 1.0.10 build


def make_pe(timedatestamp: int, subsys_major: int = 4, subsys_minor: int = 0) -> bytes:
    """Smallest PE32 header the auditor reads: MZ, e_lfanew, COFF, optional."""
    buf = bytearray(0x400)
    buf[0:2] = b"MZ"
    e = 0x80
    struct.pack_into("<I", buf, 0x3C, e)
    buf[e : e + 4] = b"PE\0\0"
    # COFF: machine, numsections, timedatestamp, ...
    struct.pack_into("<HHIIIHH", buf, e + 4, 0x14C, 1, timedatestamp, 0, 0, 0xE0, 0x102)
    opt = e + 24
    struct.pack_into("<H", buf, opt, 0x10B)  # PE32
    struct.pack_into("<HH", buf, opt + 48, subsys_major, subsys_minor)
    struct.pack_into("<H", buf, opt + 68, 2)  # IMAGE_SUBSYSTEM_WINDOWS_GUI
    return bytes(buf)


def audit(data: bytes) -> dict:
    pe = pe_audit.parse_pe(data)
    assert pe is not None, "header should parse as PE32"
    return pe_audit.judge(pe)


def test_overwritten_timestamp_fails():
    """The FAiRLiGHT watermark is caught, and the reason names the date."""
    r = audit(make_pe(HALO_FAIRLIGHT_TDS))
    assert not r["ok"]
    assert any("impossible" in p for p in r["problems"])
    # and the OLD-buggy reading - that this is just a normal binary - is wrong
    assert r["date"] is None


def test_printable_ascii_stamp_with_sane_date_is_not_a_fault():
    """The guard against the phantom finding: printable bytes, real date, ok."""
    r = audit(make_pe(HALOUPDATE_TDS))
    assert r["ok"], r["problems"]
    assert r["ascii_stamp"] is True, "should still be OBSERVED as printable"
    assert r["date"] == "2003-09-05"


def test_genuine_1010_build_passes():
    r = audit(make_pe(HALO_110_TDS))
    assert r["ok"], r["problems"]
    assert r["date"] == "2014-05-13"
    # Sharpest illustration of the false positive: this is the GENUINE
    # Bungie-signed 1.0.10 build and its stamp bytes are b'RhrS' - printable.
    assert r["ascii_stamp"] is True


def test_vista_only_subsystem_fails_and_xp_era_passes():
    """SubsystemVersion 6.0 is refused by XP's loader; 4.0 is fine."""
    bad = audit(make_pe(HALO_110_TDS, subsys_major=6, subsys_minor=0))
    assert not bad["ok"]
    assert any("Vista-only" in p for p in bad["problems"])

    good = audit(make_pe(HALO_110_TDS, subsys_major=4, subsys_minor=0))
    assert good["ok"], good["problems"]


def test_non_pe_is_ignored():
    assert pe_audit.parse_pe(b"not a pe at all" * 40) is None
    assert pe_audit.parse_pe(b"MZ" + b"\0" * 200) is None
