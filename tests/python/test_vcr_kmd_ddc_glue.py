"""vcr-kmd monitor glue: the EDID read over DDC must happen BEFORE the mode
list is built (FindAdapter), or the list is never filtered and our ICD - which
asks Glide for the highest refresh listed - runs a monitor at rates its EDID
excludes (and, measured on the V5 6000 on 2026-09-26, gives up 4-5 % fill rate
to scanout at 1600x1200). The parser and the filter are pure code tested in
tests/native/test_vcr_kmd_edid.c; this pins the kernel wiring around them.
Verified on .124: DDC reads the Sony CPD-G200's EDID, 210 -> 204 modes, and
the agent's GAMERES sees the monitor again through the child device.
"""
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
KMD = REPO / "voodoo-cleanroom" / "vcr-kmd"


def test_edid_is_read_before_the_mode_list_is_built():
    src = (KMD / "miniport" / "vcrmp.c").read_text()
    assert src.index("VcrMonitorInit(x)") < src.index("vcr_modes_build(&x->caps")


def test_the_monitor_child_carries_the_edid():
    src = (KMD / "miniport" / "vcrmp.c").read_text()
    body = src[src.index("static VP_STATUS NTAPI VcrGetChildDescriptor(PVOID ext,"):]
    body = body[:body.index("\n}\n")]
    assert "ChildIndex == 1" in body and "VcrMonitorChild(" in body
    ddc = (KMD / "miniport" / "vcrmp_ddc.c").read_text()
    assert "*type = Monitor;" in ddc and "VIDEO_ENUM_MORE_DEVICES" in ddc
    assert "VideoPortMoveMemory(desc, x->edid" in ddc


def test_ddc_uses_videoprt_and_the_vendors_pins():
    ddc = (KMD / "miniport" / "vcrmp_ddc.c").read_text()
    assert "VideoPortDDCMonitorHelper(" in ddc and "dc.Size = sizeof dc;" in ddc
    regs = (KMD / "include" / "vcr_regs.h").read_text()
    for name, bit in (("DDC_EN", 18), ("DDC_DCK_OUT", 19), ("DDC_DDA_OUT", 20),
                      ("DDC_DCK_IN", 21), ("DDC_DDA_IN", 22)):
        assert f"VCR_SPP_{name}" in regs and f"(1u << {bit})" in regs.split(f"VCR_SPP_{name}")[1][:40]


def test_the_filter_can_be_switched_off_but_is_on_by_default():
    ddc = (KMD / "miniport" / "vcrmp_ddc.c").read_text()
    assert 'VcrDiagGet(L"EdidFilter", 1)' in ddc and 'VcrDiagGet(L"Ddc", 1)' in ddc
    mk = (KMD / "Makefile").read_text()
    assert "vcrmp_ddc.c" in mk and "common/vcr_edid.c" in mk
