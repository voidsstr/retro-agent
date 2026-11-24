"""GoldSrc 4554 client.dll-hash patch (provisioning/patches/goldsrc4554/apply.py).

The Specialists 3.0 could not join the fleet's ReHLDS TS server: the 4554
client drops itself on a client.dll MD5 mismatch for non-Valve mods, and
ReHLDS sends no real hash. The fix turns one `je` into `jmp` in hw.dll and
sw.dll (verified in the build VM 2026-11-23). These tests pin the edit both
ways - the original (buggy) byte and the patched byte - on synthetic data, and
against the share when it is mounted.
"""
import hashlib
import importlib.util
import os

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
SPEC = importlib.util.spec_from_file_location(
    "goldsrc4554", os.path.join(HERE, "..", "..", "provisioning", "patches", "goldsrc4554", "apply.py"))
ap = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(ap)


def test_edit_is_one_je_to_jmp_inside_the_measured_context():
    for name, spec in ap.EDITS.items():
        ctx = ap.ctx_bytes(spec)
        rel = spec["je_off"] - spec["ctx_off"]
        assert 0 <= rel < len(ctx), name
        # original: test eax,eax; je  -> the bug path stays reachable
        assert ctx[rel - 2:rel] == b"\x85\xc0", name
        assert ctx[rel] == ap.JE == 0x74, name
        # the context ends with the push of "Your .dll [%s] differs..."
        assert ctx[-5] == 0x68, name
    assert ap.JMP == 0xEB


def test_patch_refuses_unknown_bytes():
    spec = ap.EDITS["hw.dll"]
    with pytest.raises(ap.PatchError):
        ap.patch("hw.dll", b"\0" * spec["size"])
    with pytest.raises(ap.PatchError):
        ap.patch("hw.dll", b"\0" * 10)


LIB = os.path.join(ap.LIB, ap.TITLE)
needs_share = pytest.mark.skipif(not os.path.isfile(os.path.join(LIB, "hw.dll")),
                                 reason="SKIP LOUDLY: library share not mounted")


@needs_share
@pytest.mark.parametrize("name", sorted(ap.EDITS))
def test_share_copy_is_original_or_patched_and_patch_flips_one_byte(name):
    data = open(os.path.join(LIB, name), "rb").read()
    out, state = ap.patch(name, data)
    assert hashlib.md5(out).hexdigest() == ap.EDITS[name]["patched_md5"]
    assert out[ap.EDITS[name]["je_off"]] == ap.JMP
    if state == "original":
        diff = [i for i in range(len(data)) if data[i] != out[i]]
        assert diff == [ap.EDITS[name]["je_off"]]
        assert data[diff[0]] == ap.JE
