#!/usr/bin/env python3
"""vesa_timings.py - where the table's standard timings come from.

Two sources, and nothing else, for a timing the table did not decode from a
vendor capture (tools/golden_timings.py does those):

  * VESA DMT (Display Monitor Timing standard 1.0 r13): DMT below holds the
    rows the P1120 section uses, copied from the standard's own tables. A DMT
    row wins wherever the standard has the mode.
  * VESA GTF (Generalized Timing Formula 1.1), default parameters
    (C = 40 %, M = 600 %/kHz, K = 128, J = 20 %: C' = 30, M' = 300), non-
    interlaced, no margins - gtf() below, step for step. GTF is the formula a
    CRT of 1998-2001 is built to: the P1120's EDID (1.2) lists 1800x1440@80 as
    a standard timing, which EDID 1.2 defines as the GTF timing, and HP's
    manual says the monitor is GTF compliant. Cross-checked against X.org's
    `gtf` utility (the same formula): tests/python/test_vcr_kmd_p1120_modes.py.

CVT is deliberately not used: its reduced-blanking variant is for flat panels
(a CRT needs its horizontal flyback time), and for the rest the monitor states
GTF.

    vesa_timings.py gtf 2048 1536 60        # one GTF row
    vesa_timings.py dmt 1792 1344 75        # one DMT row
    vesa_timings.py p1120                   # the rows common/vcr_modes.c's
                                            # HP P1120 section must carry
"""
import re
import sys
from pathlib import Path

# ---- VESA GTF 1.1 -----------------------------------------------------------------

CELL = 8                # character cell, pixels
MIN_PORCH = 1           # lines
V_SYNC = 3              # lines
H_SYNC_PCT = 8.0        # of the total line
MIN_VSYNC_BP_US = 550.0
C_PRIME = 30.0          # (C - J) * K / 256 + J, C = 40, J = 20, K = 128
M_PRIME = 300.0         # K / 256 * M, M = 600


def gtf(w, h, hz):
    """The GTF timing for w x h at hz (vertical refresh specified), as a
    vcr_timings[] row: dict with w, h, refresh, pixclk_khz (to the nearest
    kHz), hfp, hsync, hbp, vfp, vsync, vbp, flags ("N": -hsync +vsync, GTF's
    polarity)."""
    w = int(round(w / CELL)) * CELL
    h_period_est = (1.0 / hz - MIN_VSYNC_BP_US / 1e6) / (h + MIN_PORCH) * 1e6   # us
    v_sync_bp = int(round(MIN_VSYNC_BP_US / h_period_est))
    total_lines = h + v_sync_bp + MIN_PORCH
    v_field_est = 1.0 / h_period_est / total_lines * 1e6
    h_period = h_period_est / (hz / v_field_est)
    duty = C_PRIME - M_PRIME * h_period / 1000.0
    h_blank = int(round(w * duty / (100.0 - duty) / (2 * CELL))) * 2 * CELL
    total = w + h_blank
    h_sync = int(round(H_SYNC_PCT / 100.0 * total / CELL)) * CELL
    return {"w": w, "h": h, "refresh": hz,
            "pixclk_khz": int(round(total / h_period * 1000)),
            "hfp": h_blank // 2 - h_sync, "hsync": h_sync, "hbp": h_blank // 2,
            "vfp": MIN_PORCH, "vsync": V_SYNC, "vbp": v_sync_bp - V_SYNC, "flags": "N"}


# ---- VESA DMT 1.0 r13 (the rows the P1120 section uses) ---------------------------
# (w, h, Hz): (pixel clock kHz, hfp, hsync, hbp, vfp, vsync, vbp, flags)
# flags: "N" hsync negative, "V" vsync negative (DMT's sync polarities)
DMT = {
    (1400, 1050, 60): (121750, 88, 144, 232, 3, 4, 32, "N"),
    (1400, 1050, 75): (156000, 104, 144, 248, 3, 4, 42, "N"),
    (1400, 1050, 85): (179500, 104, 152, 256, 3, 4, 48, "N"),
    (1792, 1344, 60): (204750, 128, 200, 328, 1, 3, 46, "N"),
    (1792, 1344, 75): (261000, 96, 216, 352, 1, 3, 69, "N"),
    (1856, 1392, 60): (218250, 96, 224, 352, 1, 3, 43, "N"),
    (1856, 1392, 75): (288000, 128, 224, 352, 1, 3, 104, "N"),
}


def dmt(w, h, hz):
    r = DMT.get((w, h, hz))
    if r is None:
        return None
    k, hfp, hs, hbp, vfp, vs, vbp, fl = r
    return {"w": w, "h": h, "refresh": hz, "pixclk_khz": k, "hfp": hfp, "hsync": hs,
            "hbp": hbp, "vfp": vfp, "vsync": vs, "vbp": vbp, "flags": fl}


def standard(w, h, hz):
    """DMT where the standard has the mode, else GTF: (row, "DMT" | "GTF")."""
    r = dmt(w, h, hz)
    return (r, "DMT") if r else (gtf(w, h, hz), "GTF")


def totals(r):
    return (r["w"] + r["hfp"] + r["hsync"] + r["hbp"],
            r["h"] * (2 if "D" in r["flags"] else 1) + r["vfp"] + r["vsync"] + r["vbp"])


def rates(r, khz=None):
    """(H kHz, V Hz) from a row's own numbers (or at the dot clock khz)."""
    ht, vt = totals(r)
    hk = (r["pixclk_khz"] if khz is None else khz) / ht
    return hk, hk * 1000.0 / vt


# ---- the clock the driver will program: common/vcr_modes.c vcr_pll_calc ----------

FREF_X100 = 1431818     # 14.31818 MHz, in units of 10 Hz


def pll_khz(n, m, k):
    return (FREF_X100 * (n + 2) // ((m + 2) * 100)) >> k


def pll_calc(khz):
    """vcr_pll_calc, line for line: the frequency (kHz) the PLL will run for a
    table clock of khz."""
    k = 1 if khz > 150000 else 2 if khz > 65000 else 3
    best, best_err = None, None
    for m in range(1, 64):
        if khz > 36000 and m > 10:
            break
        if khz > 200000 and m > 5:
            break
        np2 = (khz * (m + 2) * (1 << k) * 100 + FREF_X100 // 2) // FREF_X100
        if np2 > 257 or np2 < 2:
            continue
        div = (m + 2) * 100 * (1 << k)
        f = (FREF_X100 * np2 + div // 2) // div
        err = abs(f - khz)
        if best_err is None or err < best_err:
            best, best_err = (np2 - 2, m, k), err
    return pll_khz(*best)


def fit_pll(r, rng):
    """r, with its clock lowered - if and only if the clock the PLL would run
    puts the PROGRAMMED rates past rng - to the highest PLL frequency that
    keeps them inside. A formula lands ON a limit (GTF's 640x480@160 is
    160.000 Hz) and the PLL's step then lands a hair past it (160.085 Hz);
    the driver's half-unit slack would pass that, the monitor's word does
    not. Returns (row, lowered?)."""
    ht, vt = totals(r)
    cap = min(rng["pixclk_khz"], rng["h_khz"][1] * ht, rng["v_hz"][1] * ht * vt / 1000.0)
    if pll_calc(r["pixclk_khz"]) <= cap:
        return r, False
    t = int(cap)
    while pll_calc(t) > cap:
        t -= 1
    return dict(r, pixclk_khz=pll_calc(t)), True


# ---- the HP P1120 section of common/vcr_modes.c -----------------------------------
# What the monitor states - its EDID (read from .124 on 2026-09-27) and HP's
# manual agree: H 30-121 kHz, V 48-160 Hz, a dot clock under 300 MHz.
P1120 = {"h_khz": (30, 121), "v_hz": (48, 160), "pixclk_khz": 300000}

# The refreshes a mode list offers - VESA's 60/70/75/85 and the high rates a
# 121 kHz / 160 Hz tube reaches (72 Hz is VESA's for 640x480 and 800x600 only,
# which the table already has) - at the 4:3 and 5:4 resolutions of a 21-inch
# tube. 1800x1440 (HP's rated maximum) is NOT here: every 1800x1440 timing
# inside the range needs the VSA-100's 2X mode, 2X halves the horizontal CRTC,
# and 1800 is 112.5 sixteen-pixel characters - the CRTC would blank the last 8
# columns (vcr_mode_check refuses a 2X timing whose width is not whole 2X
# characters).
P1120_HZ = (60, 70, 75, 85, 100, 120, 140, 150, 160)
P1120_RES = ((640, 480), (800, 600), (1024, 768), (1152, 864), (1280, 960), (1280, 1024),
             (1400, 1050), (1600, 1200), (1792, 1344), (1856, 1392), (1920, 1440),
             (2048, 1536))
# 2048x1536 stops below 70 Hz at 300 MHz; DMT's own 1600x1200 series steps
# through 65 Hz, and so does this one
P1120_EXTRA = ((2048, 1536, 65),)
# the comment that opens the section of common/vcr_modes.c holding these rows
P1120_MARKER = "HP P1120 (HWP22D3)"

TABLE = Path(__file__).resolve().parent.parent / "common" / "vcr_modes.c"
ROW = re.compile(r"\{\s*(\d+),\s*(\d+),\s*(\d+),\s*(\d+),\s*(\d+),\s*(\d+),\s*(\d+),"
                 r"\s*(\d+),\s*(\d+),\s*(\d+),\s*([^}]*)\}")
FIELDS = ("w", "h", "refresh", "pixclk_khz", "hfp", "hsync", "hbp", "vfp", "vsync", "vbp")


def table_rows(path=TABLE):
    """vcr_timings[] as (the rows before the P1120 section, the rows in it)."""
    src = Path(path).read_text()
    start = src.index("const vcr_timing vcr_timings[] = {")
    body = src[start:src.index("};", start)]
    cut = body.find(P1120_MARKER)
    parts = (body, "") if cut < 0 else (body[:cut], body[cut:])
    out = []
    for part in parts:
        rows = []
        for m in ROW.finditer(part):
            fl = "".join(f for f in ("N", "V", "D") if re.search(r"\b" + f + r"\b", m.group(11)))
            rows.append(dict(zip(FIELDS, map(int, m.groups()[:10])), flags=fl))
        out.append(rows)
    return out[0], out[1]


def inside(r, rng=P1120):
    hk, vz = rates(r)
    e = 1e-3                            # a clock to the kHz lands a hair either side of 160 Hz
    return (rng["h_khz"][0] - e <= hk <= rng["h_khz"][1] + e
            and rng["v_hz"][0] - e <= vz <= rng["v_hz"][1] + e
            and r["pixclk_khz"] <= rng["pixclk_khz"])


def p1120_candidates(path=TABLE):
    """Every (w, h, Hz) of the sets above that is a standard timing inside the
    P1120's range by the formula's own numbers, and that the table does not
    already carry from another source (its DMT rows, the vendor's 1600x1200
    shape that stays 1X). The driver's own PROGRAMMED clock is checked against
    the EDID by tests/native/test_vcr_kmd_edid.c."""
    have = {(r["w"], r["h"], r["refresh"]) for r in table_rows(path)[0]}
    want = [(w, h, hz) for w, h in P1120_RES for hz in P1120_HZ] + list(P1120_EXTRA)
    out = []
    for w, h, hz in sorted(want):
        if (w, h, hz) in have:
            continue
        r, src = standard(w, h, hz)
        if inside(r):
            r, lowered = fit_pll(r, P1120)
            out.append((r, src + (", clock to the PLL step inside the range" if lowered else "")))
    return out


def row_text(r, note=""):
    return ("    {{ {w:4d},{h:4d}, {refresh:3d}, {pixclk_khz:6d}, {hfp:3d}, {hsync:3d}, {hbp:3d}, "
            "{vfp:2d}, {vsync:d}, {vbp:3d}, {fl} }},".format(
                fl=" | ".join(r["flags"]) or "0", **r)
            + (f"   /* {note} */" if note else ""))


def main(argv):
    if len(argv) >= 5 and argv[1] in ("gtf", "dmt", "std"):
        w, h, hz = map(int, argv[2:5])
        r = {"gtf": gtf, "dmt": dmt, "std": lambda *a: standard(*a)[0]}[argv[1]](w, h, hz)
        if r is None:
            print("no such DMT mode", file=sys.stderr)
            return 1
        hk, vz = rates(r)
        print(row_text(r, f"{hk:.2f} kHz {vz:.2f} Hz"))
        return 0
    if len(argv) == 2 and argv[1] == "p1120":
        for r, src in p1120_candidates():
            hk, vz = rates(r)
            print(row_text(r, f"{src} {hk:.2f} kHz {vz:.2f} Hz"))
        return 0
    print(__doc__, file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
