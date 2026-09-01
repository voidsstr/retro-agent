#!/usr/bin/env python3
"""A SafeDisc version you can emulate is only HALF an answer: the IMAGE has to
carry the protection too.

WHAT THIS ENCODES (measured on hardware, .143, 2026-09-01)
-----------------------------------------------------------
Comanche 4 was audited as a clean, separable payload - and it is: `C4setup\\` on
the FLT/IGG disc is the loose retail tree, `C4setup\\C4.EXE` (md5
c0ad730a79f8d48ec06cffdd3bdf26af) is the INTACT wrapped retail binary, and
`Crack\\c4.exe` is a decrypted dump of it that was never used. It was hand-copied
to `.143`, its image mounted with DAEMON Tools 3.47, and it **renders** - the
NovaLogic splash comes up - and then says:

    "Cannot locate the CD-ROM. Please insert the correct CD-ROM, select OK and
     restart application"

That did not change with DAEMON Tools emulation turned ON (all four options
check-marked and the driver blob `d347bus\\Cfg\\khjeh` observed to change), nor
with the disc's own `DRVMGT.DLL` / `00000001.TMP` copied into the game directory
the way a real install puts them.

The reason is not the SafeDisc VERSION. `C4.EXE` is **SafeDisc 2.40.011**, which
is in the generation DAEMON Tools 3.x targets. The reason is the MEDIA:

    FLT-COM4.BIN                                358,557 sectors,   0 bad EDC
    SystemShock2/_disc/System Shock 2 (USA).bin 284,667 sectors, 793 bad EDC
    MaxPayne/_disc/MaxPayne.bin                 357,635 sectors, 600 bad EDC

SafeDisc 2 authenticates the disc by reading sectors that MUST FAIL. The two
staged titles that WORK on this fleet under DAEMON Tools carry hundreds of such
sectors; the Comanche 4 re-master carries none, because it is a regenerated data
rip with the protection region stripped - which is precisely why that release
shipped a Crack folder.

**DAEMON Tools' SafeDisc emulation REPLAYS a protection the image must still
carry. It cannot invent one.**

WHY THE CONTROL MATTERS MORE THAN THE FINDING. "0 bad sectors" is an observation
until something proves the measurement can see a protection region at all. That
is what the System Shock 2 case below is for, and it is the reason this test
scans a real staged image rather than only a synthetic one. Without it this
whole conclusion would rest on a scanner nobody had ever seen return non-zero -
the exact shape of failure this repo keeps paying for.
"""
import os
import struct
import sys

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.normpath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, os.path.join(REPO, "scripts", "fleet"))

import discprotect  # noqa: E402  (path set above; this is the true source)

LIB = "/mnt/retro-share/Files/Games-Library"

#: Bounded on purpose. The images live on a CIFS share and System Shock 2's
#: protection region is at sectors 819-10058, so 12,000 sectors (28 MB) reaches
#: it with room to spare. A bounded scan can CONFIRM protection and can never
#: refute it - see scan_image()'s docstring.
WINDOW = 12000


# --------------------------------------------------------------------------
# 1. The EDC itself. No share, no hardware - if this is wrong every other
#    conclusion in this file is worthless.
# --------------------------------------------------------------------------
def _mode1_sector(user, lba=0):
    """Build a well-formed Mode-1 sector with a correct EDC."""
    sec = bytearray(discprotect.RAW_SECTOR)
    sec[0:12] = discprotect.SYNC
    m, s, f = lba // (60 * 75), (lba // 75) % 60, lba % 75
    sec[12] = (m // 10) * 16 + m % 10
    sec[13] = (s // 10) * 16 + s % 10
    sec[14] = (f // 10) * 16 + f % 10
    sec[15] = 1                                   # mode 1
    sec[16:16 + len(user)] = user
    struct.pack_into("<I", sec, discprotect.EDC_OFFSET,
                     discprotect.edc(bytes(sec[:discprotect.EDC_COVERS])))
    return bytes(sec)


def test_edc_of_an_empty_buffer_is_zero():
    assert discprotect.edc(b"") == 0


def test_a_well_formed_sector_verifies():
    assert discprotect.sector_edc_ok(_mode1_sector(b"CD001" + b"\x00" * 100))


def test_one_flipped_bit_makes_the_edc_disagree():
    """This is the whole mechanism: a deliberately-corrupt sector reads back
    with an EDC that does not match its own bytes."""
    good = bytearray(_mode1_sector(b"payload"))
    good[100] ^= 0x01
    assert not discprotect.sector_edc_ok(bytes(good))


def test_a_wrong_length_buffer_is_an_error_not_a_false_pass():
    """A short read must raise, never quietly answer 'fine'."""
    with pytest.raises(ValueError):
        discprotect.sector_edc_ok(b"\x00" * 2048)


# --------------------------------------------------------------------------
# 2. The SafeDisc version, read RELATIVE to the marker.
# --------------------------------------------------------------------------
def test_version_is_read_at_marker_plus_0x20_not_a_fixed_offset():
    blob = bytearray(b"\xcc" * 0x800)
    at = 0x333                                     # deliberately not 0xfd4
    blob[at:at + len(discprotect.SAFEDISC_MARKER)] = discprotect.SAFEDISC_MARKER
    struct.pack_into("<III", blob, at + 0x20, 2, 40, 11)
    tmp = os.path.join(os.environ.get("TMPDIR", "/tmp"), "discprotect_probe.bin")
    with open(tmp, "wb") as fh:
        fh.write(blob)
    try:
        assert discprotect.safedisc_version(tmp) == (2, 40, 11)
    finally:
        os.unlink(tmp)


def test_a_binary_with_no_marker_answers_none():
    tmp = os.path.join(os.environ.get("TMPDIR", "/tmp"), "discprotect_clean.bin")
    with open(tmp, "wb") as fh:
        fh.write(b"MZ" + b"\x00" * 4096)
    try:
        assert discprotect.safedisc_version(tmp) is None
    finally:
        os.unlink(tmp)


# --------------------------------------------------------------------------
# 3. An .iso is a THIRD state, not "no protection".
# --------------------------------------------------------------------------
def test_a_2048_byte_iso_reports_that_it_CANNOT_carry_the_protection():
    """Blue Shift.iso is 307,726,336 B = 150,257 x 2048 exactly. Reporting that
    as "0 bad sectors" would read as "the image is clean", when the truth is
    that the format has no EDC field at all and the question is unanswerable
    from it. Absent and unrepresentable are different facts."""
    tmp = os.path.join(os.environ.get("TMPDIR", "/tmp"), "discprotect_fake.iso")
    with open(tmp, "wb") as fh:
        fh.write(b"\x00" * (2048 * 4))
    try:
        rep = discprotect.scan_image(tmp)
        assert rep.kind == "iso"
        assert rep.carries_protection is False
        assert "CANNOT carry" in str(rep)
    finally:
        os.unlink(tmp)


# --------------------------------------------------------------------------
# 4. THE CONTROL. A staged SafeDisc title that really works on this fleet must
#    have bad-EDC sectors in its image - otherwise the scanner above has never
#    been seen to fire and the Comanche 4 conclusion is unsupported.
# --------------------------------------------------------------------------
def test_a_working_staged_safedisc_image_really_does_carry_bad_sectors():
    img = os.path.join(LIB, "SystemShock2", "_disc", "System Shock 2 (USA).bin")
    if not os.path.isdir(LIB):
        pytest.skip("SKIPPED LOUDLY: %s is not mounted - the protection-region "
                    "control did NOT run, so nothing here has confirmed the "
                    "scanner can return non-zero" % LIB)
    if not os.path.isfile(img):
        pytest.skip("SKIPPED LOUDLY: %s absent - control NOT verified" % img)
    rep = discprotect.scan_image(img, max_sectors=WINDOW)
    assert rep.kind == "raw", "the staged image stopped being a 2352-byte dump"
    assert rep.carries_protection, (
        "System Shock 2's staged image has no bad-EDC sectors in its first %d. "
        "That title is SafeDisc and it WORKS on this fleet under DAEMON Tools, "
        "so either the image was replaced with a clean rip (in which case the "
        "title is about to stop working) or this scanner is broken - and if it "
        "is broken, the Comanche 4 refusal it supports is unsupported."
        % WINDOW)


def test_claude_md_records_that_emulation_cannot_invent_a_protection():
    """The doc is what the next agent reads before spending a day mounting an
    image that can never satisfy its own exe."""
    with open(os.path.join(REPO, "CLAUDE.md"), encoding="utf-8",
              errors="replace") as fh:
        text = fh.read()
    assert "cannot invent one" in text, (
        "CLAUDE.md no longer states that SafeDisc emulation replays a "
        "protection the IMAGE must carry - which is the half of the rule that "
        "the version check does not cover")
