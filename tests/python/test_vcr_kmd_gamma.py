"""vcr-kmd: GDI gamma ramps reach the colour table (clean-room lane, 2026-09-28).

Jedi Academy logged "SetDeviceGammaRamp failed." on .124 (V5 6000): the
display driver never offered GCAPS2_CHANGEGAMMARAMP, so win32k refused every
ramp and the id Tech 3 family (JA, RtCW, ioquake3) ran on software gamma with
r_overBrightBits forced to 0 - the dark picture. The conversion is run by
tests/native/test_vcr_kmd_gamma.c; these pin the glue that cannot run on the
host: the capability, the entry point, the depth rule and the kill switch.
"""
import re
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
KMD = REPO / "voodoo-cleanroom" / "vcr-kmd"
VCRDD = (KMD / "display" / "vcrdd.c").read_text()
G2D = (KMD / "display" / "vcrdd_2d.c").read_text()
MP = (KMD / "miniport" / "vcrmp.c").read_text()
HW = (KMD / "miniport" / "vcrmp_hw.c").read_text()
IOCTL = (KMD / "include" / "vcr_ioctl.h").read_text()


def func(src, sig):
    i = src.index(sig)
    return src[i:src.index("\n}\n", i)]


def test_the_capability_is_offered_at_direct_colour_depths_only():
    dev = func(VCRDD, "static void fill_devinfo(")
    assert re.search(r"if \(vcr_gamma_depth_ok\(pd->bpp\)\)\s+di->flGraphicsCaps2 \|= GCAPS2_CHANGEGAMMARAMP;", dev)


def test_the_entry_point_is_registered_at_its_own_slot():
    table = VCRDD[VCRDD.index("static DRVFN g_drvfn[] = {"):]
    table = table[:table.index("};")]
    entries = re.findall(r"\{ (INDEX_\w+),", table)
    assert "INDEX_DrvIcmSetDeviceGammaRamp" in entries
    # every g_drvfn[n].pfn assignment must fill the slot whose INDEX matches
    enable = func(VCRDD, "BOOL APIENTRY DrvEnableDriver(")
    for n, fn in re.findall(r"g_drvfn\[(\d+)\]\.pfn = \(PFN\)(\w+);", enable):
        assert entries[int(n)] == "INDEX_" + fn, (n, fn, entries[int(n)])


def test_the_ramp_goes_through_the_miniports_clut_write():
    fn = func(VCRDD, "BOOL APIENTRY DrvIcmSetDeviceGammaRamp(")
    assert "iFormat != IGRF_RGB_256WORDS" in fn
    assert "pd->gamma_off" in fn and "!vcr_gamma_depth_ok(pd->bpp)" in fn
    assert "vcr_gamma_entry(" in fn
    assert "IOCTL_VIDEO_SET_COLOR_REGISTERS" in fn
    assert "return rc == 0;" in fn
    # the miniport takes a colour-register write at any depth: no bpp gate
    assert "bpp" not in func(HW, "VP_STATUS VcrHwSetClut(")


def test_default_on_and_diag_gdigamma_0_turns_it_off():
    assert re.search(r"#define VCR_INFO_F_NO_GDIGAMMA\s+0x4000", IOCTL)
    fill = func(MP, "static void fill_info(")
    assert '(VcrDiagGet(L"GdiGamma", 1) ? 0 : VCR_INFO_F_NO_GDIGAMMA)' in fill
    assert "pd->gamma_off = (info.flags & VCR_INFO_F_NO_GDIGAMMA) ? 1 : 0;" in G2D


def test_a_mode_set_still_starts_from_identity():
    """A game's ramp must not outlive its mode: every mode set reloads the
    identity table (the Glide release's RESTORE_MODE included)."""
    assert "clut_identity(x);" in HW
