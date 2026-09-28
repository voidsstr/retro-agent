"""vcr-kmd: vcrdd.dll carries a VERSIONINFO (2026-09-28).

DirectX reports a display driver's file version as the adapter's
DriverVersion; with no resource vcrdd.dll read as 0.0.0.0 - a number games
check. 6.14 is every XP (NT 5.1) display driver's major.minor.
"""
import re
from pathlib import Path

KMD = Path(__file__).resolve().parents[2] / "voodoo-cleanroom" / "vcr-kmd"


def test_the_rc_declares_a_display_driver_version():
    rc = (KMD / "display" / "vcrdd.rc").read_text()
    assert "VS_VERSION_INFO VERSIONINFO" in rc
    assert re.search(r"#define VCR_VER_BIN 6,14,\d+,\d+", rc)
    assert "VFT2_DRV_DISPLAY" in rc and "VFT_DRV" in rc


def test_the_dll_links_it():
    mk = (KMD / "Makefile").read_text()
    assert "$(CROSS)windres -O coff display/vcrdd.rc" in mk
    rule = mk[mk.index("$(OUT)/vcrdd.dll:"):]
    rule = rule[:rule.index("\n\n")]
    assert rule.count("$(OUT)/vcrdd_res.o") >= 2, "a dependency AND on the link line"
