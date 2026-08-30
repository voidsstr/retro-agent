"""Halo: Combat Evolved - the DigitalProductID the game reads at startup.

FIX BEING GUARDED (2026-08-30).  halo.exe 1.10 refuses to start unless
HKLM\\Software\\Microsoft\\Microsoft Games\\Halo\\DigitalProductID holds the
164-byte Microsoft structure.  The retail setup writes it; we do not have the
retail setup (the media on the share is a post-install tree), so
`scripts/halo/make_dpid.py` builds it from the licence-holder's own key.

Every assertion here is a condition halo.exe 1.10 really applies, read out of
its own code at 0x0057f3f0:

    cbData        == 0xA4          the value must be exactly 164 bytes
    blob[0..3]    == 0xA4
    blob[4..5]    == 3             version major - NOT 0, which is what a
                                   naive "all-zero header" blob would carry
    blob[6..7]    == 0
    blob[8..]     product-ID string, bytes 6..8 == "OEM" selecting one of two
                  digit-index tables that must find DIGITS at their indices
    blob[0x34]    the 15-byte base-24 encoding of the key

No product key appears in this file, and none may ever be added to it: the keys
live in the Azure vault and the literal lives only in the staged install.reg on
the share. Even the throwaway key below is BUILT from the alphabet rather than
written out, because a 5x5 literal is a product key by shape and
tests/python/test_no_committed_secrets.py -- which guards the whole repo, so
this file does not duplicate it -- fails on one wherever it appears.
"""
import os
import struct
import sys

import pytest

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
sys.path.insert(0, os.path.join(REPO, "scripts", "halo"))

make_dpid = pytest.importorskip("make_dpid")

# A syntactically valid key made only of the base-24 alphabet, and NOT a real
# one. It is BUILT rather than written out: a 5x5 literal in this repo is a
# product key by shape, and tests/python/test_no_committed_secrets.py is right
# to fail on one wherever it appears, including here.
def _grouped(chars):
    return "-".join(chars[i:i + 5] for i in range(0, 25, 5))


FAKE = _grouped(make_dpid.ALPHABET[:24] + make_dpid.ALPHABET[0])


def test_alphabet_is_microsofts():
    assert make_dpid.ALPHABET == "BCDFGHJKMPQRTVWXY2346789"
    assert len(make_dpid.ALPHABET) == 24
    # the excluded letters are the ambiguous ones - A E I L N O S U Z and 0 1 5
    for c in "AEILNOSUZ015":
        assert c not in make_dpid.ALPHABET


def test_codec_round_trips():
    raw = make_dpid.encode_key(FAKE)
    assert len(raw) == 15
    assert make_dpid.decode_key(raw) == FAKE


def test_codec_is_big_endian_base24_in_a_little_endian_integer():
    zero, one = make_dpid.ALPHABET[0], make_dpid.ALPHABET[1]
    # the LAST character is the least significant digit
    assert make_dpid.encode_key(_grouped(zero * 24 + one)).hex() == \
        "010000000000000000000000000000"
    # ...and the second-to-last is worth 24 of it
    assert make_dpid.encode_key(_grouped(zero * 23 + one + zero)).hex() == \
        "180000000000000000000000000000"


def test_rejects_a_key_that_is_not_a_key():
    with pytest.raises(ValueError):
        make_dpid.encode_key("TOO-SHORT")
    with pytest.raises(ValueError):
        # the ambiguous letters Microsoft excluded are not in the alphabet
        make_dpid.encode_key(_grouped("A" * 25))


def test_blob_passes_every_gate_halo_applies():
    b = make_dpid.build(FAKE)
    assert len(b) == 0xA4
    assert struct.unpack_from("<I", b, 0)[0] == 0xA4
    # version major 3 - the old-buggy value would be 0 and halo.exe rejects it
    assert struct.unpack_from("<HH", b, 4) == (3, 0)
    assert struct.unpack_from("<HH", b, 4) != (0, 0)
    pid = b[8:0x20].split(b"\0")[0].decode("ascii")
    assert pid.startswith(make_dpid.HALO_MPC + "-")
    assert pid[6:9] == "OEM"                     # picks halo.exe's OEM table
    for i in (12, 13, 14, 15, 18, 19, 20, 21, 22):   # that table's indices
        assert pid[i].isdigit(), pid
    assert b[0x24:0x34].split(b"\0")[0].decode("ascii") == make_dpid.HALO_SKU
    assert b[0x34:0x43] == make_dpid.encode_key(FAKE)
    assert make_dpid.decode_key(b[0x34:0x43]) == FAKE


def test_product_identifiers_are_halos_own():
    # read out of the mgspid.dll Microsoft shipped inside the game tree
    assert make_dpid.HALO_MPC == "69771"
    assert make_dpid.HALO_SKU == "Z08-00030"
    assert make_dpid.HALO_OEM == "OEM-1208613"


def test_blob_is_deterministic():
    assert make_dpid.build(FAKE) == make_dpid.build(FAKE)


def test_reg_stanza_is_regedit4_binary_and_round_trips():
    import re
    b = make_dpid.build(FAKE)
    text = make_dpid.reg_lines(b)
    # REG_BINARY, and NOT hex(3)/hex(4) - Win9x regedit only understands `hex:`
    assert text.startswith('"DigitalProductID"=hex:')
    assert "hex(" not in text
    got = bytes.fromhex(re.sub(r"[^0-9a-f]", "", text.split("hex:", 1)[1]))
    assert got == b
