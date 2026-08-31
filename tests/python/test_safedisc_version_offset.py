"""SafeDisc's version is at `marker + 0x20`, never at a fixed file offset.

CLAUDE.md said "file offset `0xfd4`". That was the address the version happened
to land at in the first two binaries measured, not a constant. Measured
2026-08-31 against two staged binaries this repo actually ships:

    Carmageddon2/Carma2_SW.exe   BoG_ at 0x3d4   marker+0x20 -> 1.01.034
    BF1942/Mods/bf1942/Mod.dll   BoG_ at 0xfd4   marker+0x20 -> 2.80.010

Reading 0xfd4 gives 3246392461.3269002208.2332564697 on the first -- garbage
that no sane reader would mistake for a version, which is the *lucky* case. The
dangerous case is garbage that looks plausible.

WHY THE VERSION IS WORTH GETTING RIGHT. It decides the entire plan for a title:
DAEMON Tools 3.47 emulates the **1.x** generation happily -- Carmageddon 2 is
not protection-blocked at all, and was parked as blocked on the assumption that
it was -- and cannot satisfy **2.80**, which is what genuinely walls C&C
Generals and the BF1942 client. A misread version sends someone to install a
kernel driver and reboot boxes that need nothing, or to give up on a title that
already works.

This test asserts the DOC, not a scanner, because the doc is what the next
agent reads before touching a protected title. It skips loudly when the share
is unmounted rather than passing quietly.
"""
import io
import os
import struct

import pytest

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
LIB = "/mnt/retro-share/Files/Games-Library"
MARKER = b"BoG_ *90.0&!!"

CASES = [
    ("Carmageddon2/Carma2_SW.exe", (1, 1, 34)),
    ("BF1942/Mods/bf1942/Mod.dll", (2, 80, 10)),
]


def test_claude_md_documents_the_relative_offset_not_a_fixed_one():
    with open(os.path.join(REPO, "CLAUDE.md"), encoding="utf-8",
              errors="replace") as f:
        text = f.read()
    assert "marker + 0x20" in text, (
        "CLAUDE.md no longer documents the RELATIVE SafeDisc version offset")
    assert "not use a fixed file offset" in text or "do not use a fixed" in text.lower(), (
        "CLAUDE.md must warn against a fixed offset -- that is the actual error")


@pytest.mark.parametrize("rel,expected", CASES)
def test_the_relative_offset_reads_the_right_version(rel, expected):
    path = os.path.join(LIB, rel)
    if not os.path.isdir(LIB):
        pytest.skip("SKIPPED LOUDLY: %s is not mounted, SafeDisc offset "
                    "NOT verified" % LIB)
    if not os.path.exists(path):
        pytest.skip("SKIPPED LOUDLY: %s absent, offset NOT verified" % rel)
    data = io.open(path, "rb").read()
    i = data.find(MARKER)
    assert i >= 0, "%s no longer carries the BoG_ marker" % rel
    got = struct.unpack_from("<III", data, i + 0x20)
    assert got == expected, (
        "%s: marker+0x20 read %s, expected %s. Either the binary changed or "
        "the offset is not what we think." % (rel, got, expected))


def test_a_fixed_0xfd4_is_demonstrably_wrong_somewhere():
    """Prove the old rule is broken rather than merely asserting it.

    Without this, someone could 'simplify' the doc back to a constant and every
    other test here would still pass.
    """
    if not os.path.isdir(LIB):
        pytest.skip("SKIPPED LOUDLY: %s is not mounted" % LIB)
    path = os.path.join(LIB, "Carmageddon2/Carma2_SW.exe")
    if not os.path.exists(path):
        pytest.skip("SKIPPED LOUDLY: Carmageddon2 absent")
    data = io.open(path, "rb").read()
    major, minor, sub = struct.unpack_from("<III", data, 0xfd4)
    assert not (major < 10 and minor < 256 and sub < 1000), (
        "0xfd4 happens to read a plausible version in this binary too -- the "
        "example that proves the fixed offset wrong has stopped proving it; "
        "find another before trusting this file")


def test_the_wrapper_is_not_always_in_the_exe():
    """BF1942's SafeDisc is in a Mod DLL; scanning only the exe reports clean."""
    with open(os.path.join(REPO, "CLAUDE.md"), encoding="utf-8",
              errors="replace") as f:
        text = f.read()
    assert "Mod.dll" in text, (
        "CLAUDE.md no longer records that BF1942's wrapper lives in "
        "Mods\\bf1942\\Mod.dll rather than the executable")
