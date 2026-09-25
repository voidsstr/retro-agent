"""The gate's `glide` capability: a PRESENT 3dfx device with its driver installed.

Added 2026-09-24 for the Voodoo GLQuake shortcut on .243 (Win98, Voodoo 2 behind
a Cirrus 2D card). The shortcut must appear only where a Voodoo can actually run
it, and feature levels cannot see a Voodoo 2 - the active adapter is the 2D card.

Run: pytest tests/python/test_glide_capability.py
"""
import re
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
SRC = REPO / "agent" / "src"


def _fn(src, name):
    i = src.index(name + "(")
    return src[i:src.index("\n}\n", i)]


def test_detector_needs_a_driver_and_a_present_device_on_both_roots():
    body = _fn((SRC / "hwextra.c").read_text(), "int hwextra_glide_installed")
    assert '"SYSTEM\\\\CurrentControlSet\\\\Enum\\\\PCI"' in body and '"Enum\\\\PCI"' in body
    assert '"Driver"' in body, "a Voodoo with no driver installed cannot run Glide"
    assert "ven != VEN_3DFX" in body
    # a pulled card leaves its Enum key (Driver and all) behind: ask Config Manager
    assert 'GetProcAddress(cm, "CM_Locate_DevNodeA")' in body
    assert "locate(&dn, id, 0) != 0" in body
    assert not re.search(r"(?<![\w\"])CM_Locate_DevNodeA\s*\(", body), \
        "cfgmgr32 must be resolved dynamically - a static import kills the exe on Win9x"


def test_hwprofile_sets_and_reports_the_capability():
    s = (SRC / "hwprofile.c").read_text()
    assert "p->caps |= GG_CAP_GLIDE" in s
    assert 'json_kv_bool(&j, "glide"' in s and '"glide_evidence"' in s


def test_python_mirror_knows_glide():
    import sys
    sys.path.insert(0, str(REPO / "scripts" / "gamegate"))
    import rules
    assert rules.CAPABILITIES["glide"] == rules.CAP_GLIDE == 0x0002
    h = (REPO / "agent" / "shared" / "gamegate.h").read_text()
    assert "#define GG_CAP_GLIDE      0x0002u" in h
    assert rules.CAPABILITY_REMEDY[rules.CAP_GLIDE] in h, "remedy text must match the C header"
