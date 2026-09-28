"""The HP P1120 rows of vcr-kmd's timing table (2026-09-27), and its monitor INF.

The HP P1120 on .124 (EDID HWP22D3: H 30-121 kHz, V 48-160 Hz, 300 MHz - what
HP's manual and XP's own monitor3.inf say too) accepted far more than the table
offered: 640x480 and 800x600 stopped at 120 Hz, 1024x768 at 100, 1280x1024 at
85, nothing past 1920x1440. common/vcr_modes.c now carries a section of
standard timings for it - VESA DMT where the standard has the mode, else VESA
GTF - and tools/vesa_timings.py is the generator. This pins:

  * every row of that section IS what the generator computes (a hand-edited
    number cannot slip in and still claim to be GTF or DMT),
  * the generator's GTF is the VESA formula (X.org's `gtf` utility, the same
    formula, agrees - checked live when the utility is installed, and against
    modelines captured from it otherwise),
  * the host-side gate (tools/mode_sweep.py, the driver's own mode math via
    tools/modecalc.c) passes every mode for the P1120 and still refuses the
    rows the Sony cannot take,
  * inf/vcrp1120.inf states the EDID's ranges and the largest mode the table
    offers the monitor.

The native suites hold the rest: tests/native/test_vcr_kmd_edid.c (the mode
list per monitor, programmed rates without slack) and test_vcr_kmd_modes.c
(the 2X width rule that keeps 1800x1440 out).
"""
import re
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
KMD = REPO / "voodoo-cleanroom" / "vcr-kmd"
TOOLS = KMD / "tools"
EDID_NATIVE = REPO / "tests" / "native" / "test_vcr_kmd_edid.c"
INF = KMD / "inf" / "vcrp1120.inf"
sys.path.insert(0, str(TOOLS))

import mode_sweep  # noqa: E402
import vesa_timings as vt  # noqa: E402


def _c_array(src, name):
    m = re.search(rf"\b{name}\s*\[\s*\d*\s*\]\s*=\s*\{{(.*?)\}};", src, re.S)
    assert m, name
    return bytes(int(x, 16) for x in re.findall(r"0x([0-9a-fA-F]{2})", m.group(1)))


P1120_EDID = _c_array(EDID_NATIVE.read_text(), "k_hp_p1120")

# the section as the native suite lists it (test_vcr_kmd_edid.c k_p1120_rows)
ADDED = {(640, 480, 70), (640, 480, 140), (640, 480, 150), (640, 480, 160),
         (800, 600, 70), (800, 600, 140), (800, 600, 150), (800, 600, 160),
         (1024, 768, 120), (1024, 768, 140), (1152, 864, 120),
         (1280, 960, 70), (1280, 960, 100), (1280, 1024, 70), (1280, 1024, 100),
         (1400, 1050, 60), (1400, 1050, 70), (1400, 1050, 75), (1400, 1050, 85),
         (1400, 1050, 100),
         (1792, 1344, 60), (1792, 1344, 70), (1792, 1344, 75), (1792, 1344, 85),
         (1856, 1392, 60), (1856, 1392, 70), (1856, 1392, 75),
         (1920, 1440, 70), (2048, 1536, 60), (2048, 1536, 65)}


def key(r):
    return r["w"], r["h"], r["refresh"]


def test_the_section_is_exactly_what_the_generator_computes():
    old, section = vt.table_rows()
    assert len(old) == 70, "the rows before the section keep their indices"
    plan = vt.p1120_candidates()
    assert [key(r) for r in section] == [key(r) for r, _ in plan]
    for got, (want, src) in zip(section, plan):
        assert got == want, (src, got, want)
    assert {key(r) for r in section} == ADDED
    # no row is in the table twice: vcr_timing_find returns the first match
    keys = [key(r) for r in old + section]
    assert len(keys) == len(set(keys))


def test_the_sources_are_dmt_where_the_standard_has_the_mode():
    srcs = {key(r): s for r, s in vt.p1120_candidates()}
    dmt = {k for k, s in srcs.items() if s.startswith("DMT")}
    assert dmt == set(vt.DMT)
    # one row was lowered to the PLL step inside 160 Hz; nothing else moved
    moved = {k for k, s in srcs.items() if "PLL" in s}
    assert moved == {(640, 480, 160)}
    r = next(r for r, _ in vt.p1120_candidates() if key(r) == (640, 480, 160))
    assert vt.gtf(640, 480, 160)["pixclk_khz"] == 72852 and r["pixclk_khz"] == 72784
    assert vt.pll_calc(72852) == 72892 and vt.pll_calc(72784) == 72784
    assert vt.rates(r, vt.pll_calc(72852))[1] > 160.0         # GTF's own clock: 160.085 Hz
    assert vt.rates(r, vt.pll_calc(r["pixclk_khz"]))[1] <= 160.0
    # nothing past the monitor, by the numbers the chip will run
    for r, _ in vt.p1120_candidates():
        hk, vz = vt.rates(r, vt.pll_calc(r["pixclk_khz"]))
        assert 30 <= hk <= 121 and 48 <= vz <= 160 and vt.pll_calc(r["pixclk_khz"]) <= 300000


def test_no_1800x1440_it_is_not_whole_2x_characters():
    """HP rates the P1120 to 1800x1440@80. Every 1800x1440 timing inside its
    range has htotal > 261 characters (2X on the VSA-100), and 1800 px is
    112.5 two-X characters: the CRTC would show 1792 columns."""
    for hz in (60, 70, 75, 80):
        r = vt.gtf(1800, 1440, hz)
        assert sum((r["w"], r["hfp"], r["hsync"], r["hbp"])) // 8 > 261
        assert r["w"] % 16 == 8
    assert not any(r["w"] == 1800 for r in vt.table_rows()[1])


# `gtf W H HZ` from X.org (the VESA formula), captured 2026-09-27 on the dev host
XORG_GTF = {
    (2048, 1536, 60): (266.95, 2048, 2200, 2424, 2800, 1536, 1537, 1540, 1589),
    (2048, 1536, 65): (291.77, 2048, 2208, 2432, 2816, 1536, 1537, 1540, 1594),
    (1024, 768, 120): (139.05, 1024, 1104, 1216, 1408, 768, 769, 772, 823),
    (1024, 768, 140): (164.20, 1024, 1104, 1216, 1408, 768, 769, 772, 833),
    (800, 600, 160): (116.41, 800, 864, 952, 1104, 600, 601, 604, 659),
    (640, 480, 160): (72.85, 640, 680, 752, 864, 480, 481, 484, 527),
    (1792, 1344, 85): (295.52, 1792, 1928, 2128, 2464, 1344, 1345, 1348, 1411),
    (1920, 1440, 70): (277.02, 1920, 2072, 2280, 2640, 1440, 1441, 1444, 1499),
    (1280, 1024, 100): (190.96, 1280, 1376, 1520, 1760, 1024, 1025, 1028, 1085),
    (1152, 864, 120): (176.01, 1152, 1240, 1368, 1584, 864, 865, 868, 926),
    (1800, 1440, 80): (299.95, 1800, 1944, 2144, 2488, 1440, 1441, 1444, 1507),
}


def modeline(r):
    return (round(r["pixclk_khz"] / 1000.0, 2), r["w"], r["w"] + r["hfp"],
            r["w"] + r["hfp"] + r["hsync"], r["w"] + r["hfp"] + r["hsync"] + r["hbp"],
            r["h"], r["h"] + r["vfp"], r["h"] + r["vfp"] + r["vsync"],
            r["h"] + r["vfp"] + r["vsync"] + r["vbp"])


def test_gtf_is_the_vesa_formula():
    for (w, h, hz), want in XORG_GTF.items():
        got = modeline(vt.gtf(w, h, hz))
        assert abs(got[0] - want[0]) < 0.011 and got[1:] == want[1:], (w, h, hz, got, want)


def test_gtf_agrees_with_xorg_gtf_on_every_gtf_row():
    exe = shutil.which("gtf")
    if not exe:
        pytest.skip("X.org `gtf` not installed - checked against the captured modelines only")
    for r, src in vt.p1120_candidates():
        if not src.startswith("GTF"):
            continue
        out = subprocess.run([exe, str(r["w"]), str(r["h"]), str(r["refresh"])],
                             capture_output=True, text=True, check=True).stdout
        m = re.search(r'Modeline "\S+"\s+([\d.]+)\s+([\d ]+?)\s+-HSync', out)
        assert m, out
        nums = tuple(int(x) for x in m.group(2).split())
        want = (float(m.group(1)),) + nums
        got = modeline(vt.gtf(r["w"], r["h"], r["refresh"]))
        assert abs(got[0] - want[0]) < 0.011 and got[1:] == want[1:], (key(r), got, want)


P1120_INFO = {"cmd": "info", "ok": True, "edid_ok": 1, "mon_filter": 1, "mon_src": 1,
              "monitor": "HWP22D3 HP P1120", "mon_h_khz": [30, 121], "mon_v_hz": [48, 160],
              "mon_max_pixclk_khz": 300000, "edid": P1120_EDID.hex()}


@pytest.fixture(scope="module")
def real_calc():
    if not (shutil.which("gcc") or shutil.which("cc")):
        pytest.skip("no host C compiler: tools/modecalc.c not built - the real-mode-math "
                    "check did NOT run")
    import golden_compare
    return golden_compare.ours()


def test_the_host_gate_passes_every_mode_for_the_p1120(real_calc):
    rng, why = mode_sweep.monitor_gate(P1120_INFO)
    assert why is None and rng["h_khz"] == (30, 121) and rng["v_hz"] == (48, 160)
    assert not mode_sweep.out_of_range(list(real_calc), rng, real_calc)
    # the new rows are there at every depth
    for w, h, hz in ADDED:
        for b in (8, 16, 32):
            assert f"{w}x{h}x{b}@{hz}" in real_calc


def test_the_host_gate_still_refuses_the_sonys_out_of_range_rows(real_calc):
    sony_edid = _c_array(EDID_NATIVE.read_text(), "k_sony_cpd_g200")
    sony = dict(P1120_INFO, monitor="SNY1270 CPD-G200", mon_h_khz=[30, 96], mon_v_hz=[48, 120],
                mon_max_pixclk_khz=260000, edid=sony_edid.hex())
    rng, why = mode_sweep.monitor_gate(sony)
    assert why is None
    out = mode_sweep.out_of_range([f"{w}x{h}x16@{hz}" for w, h, hz in ADDED], rng, real_calc)
    inside = {(640, 480, 70), (800, 600, 70), (1280, 960, 70), (1280, 1024, 70),
              (1400, 1050, 60), (1400, 1050, 70), (1400, 1050, 75), (1400, 1050, 85),
              (1792, 1344, 60), (1856, 1392, 60)}
    assert set(out) == {f"{w}x{h}x16@{hz}" for w, h, hz in ADDED - inside}


def _inf():
    return INF.read_text()


def test_the_inf_is_a_monitor_inf_for_hwp22d3():
    s = _inf()
    assert re.search(r"^Class\s*=\s*Monitor\s*$", s, re.M)
    assert "{4D36E96E-E325-11CE-BFC1-08002BE10318}" in s
    assert re.search(r"=\s*HWP22D3\.Install,\s*Monitor\\HWP22D3\s*$", s, re.M)
    # the identity the EDID carries: bytes 8-11 -> HWP / 0x22D3
    i = (P1120_EDID[8] << 8) | P1120_EDID[9]
    pnp = "".join(chr(64 + ((i >> s_) & 31)) for s_ in (10, 5, 0))
    assert f"{pnp}{P1120_EDID[11]:02X}{P1120_EDID[10]:02X}" == "HWP22D3"
    # registry only: nothing to copy, nothing a reboot would be needed for
    assert "CopyFiles" not in s and "SourceDisksFiles" not in s
    assert s.isascii()


def test_the_inf_states_the_edids_range_and_the_largest_mode_offered():
    s = _inf()
    fd = P1120_EDID.index(bytes([0, 0, 0, 0xfd]))
    vmin, vmax, hmin, hmax = P1120_EDID[fd + 5:fd + 9]
    assert (vmin, vmax, hmin, hmax) == (48, 160, 30, 121)
    m = re.search(r'^HKR,"MODES\\(\d+),(\d+)",Mode1,,"([\d.]+)-([\d.]+),([\d.]+)-([\d.]+),\+,\+"',
                  s, re.M)
    assert m
    w, h = int(m.group(1)), int(m.group(2))
    assert (float(m.group(3)), float(m.group(4))) == (hmin, hmax)
    assert (float(m.group(5)), float(m.group(6))) == (vmin, vmax)
    assert re.search(rf'^HKR,,MaxResolution,,"{w},{h}"\s*$', s, re.M)
    # the cap is the largest mode the table offers inside the monitor's range
    rows = vt.table_rows()[0] + vt.table_rows()[1]
    fits = [r for r in rows if "D" not in r["flags"] and vt.inside(r)]
    biggest = max(fits, key=lambda r: r["w"] * r["h"])
    assert (w, h) == (biggest["w"], biggest["h"]) == (2048, 1536)
    # and it wipes what XP's monitor3.inf left (MaxResolution 1800,1440)
    assert re.search(r"^HKR,MODES\s*$", s, re.M) and re.search(r"^HKR,,MaxResolution\s*$", s, re.M)
