#!/usr/bin/env python3
"""golden_timings.py - timing-table rows decoded from a vendor golden capture.

Every mode the vendor driver offered on this box is a timing its monitor is
known to accept. For the ones our table lacks, this decodes the exact timing
back out of the captured CRTC (with the vendor's sync-one-unit-early
convention), misc (polarity), CR09 (doublescan) and pllCtrl0 (dot clock), and
prints `vcr_timings[]` rows. Run it, paste, and golden_compare.py must then
report the new modes identical.

    golden_timings.py golden/amigamerlin-3.1-r11_192.168.1.124.json [--all]
"""
import argparse
import json
import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent


def pll_khz(v):
    n, m, k = (v >> 8) & 0xff, (v >> 2) & 0x3f, v & 3
    return (14318 * (n + 2) // (m + 2)) >> k


def full_after(start, low, bits):
    """Smallest value > start whose low `bits` bits equal `low`."""
    mask = (1 << bits) - 1
    v = (start & ~mask) | low
    while v <= start:
        v += 1 << bits
    return v


def decode(cap):
    c = bytes.fromhex(cap["vga"]["crtc"])
    misc = int(cap["vga"]["misc"], 16)
    io = [int(x, 16) for x in cap["io"]]
    e1a, e1b = c[0x1a], c[0x1b]
    if io[0x4c // 4] & 1:
        return None                                  # 2X: horizontal is halved
    ht = c[0] | ((e1a & 1) << 8)
    hd = c[1] | ((e1a & 4) << 6)
    hs = c[4] | ((e1a & 0x40) << 2)
    he = full_after(hs, (c[5] & 0x1f) | ((e1a & 0x80) >> 2), 6)
    vt = c[6] | ((c[7] & 1) << 8) | ((c[7] & 0x20) << 4) | ((e1b & 1) << 10)
    vd = c[0x12] | ((c[7] & 2) << 7) | ((c[7] & 0x40) << 3) | ((e1b & 4) << 8)
    vs = c[0x10] | ((c[7] & 4) << 6) | ((c[7] & 0x80) << 2) | ((e1b & 0x40) << 4)
    ve = full_after(vs, c[0x11] & 0x0f, 4)
    htot, hdisp = (ht + 5) * 8, (hd + 1) * 8
    hss, hse = (hs + 1) * 8, (he + 1) * 8
    vtot, vdisp, vss, vse = vt + 2, vd + 1, vs + 1, ve + 1
    dbl = bool(c[9] & 0x80)
    w, h, rest = cap["tag"].split("x")
    bpp, hz = rest.split("@")
    rows = vdisp // 2 if dbl else vdisp
    flags = (["N"] if misc & 0x40 else []) + (["V"] if misc & 0x80 else []) + (["D"] if dbl else [])
    return {"w": int(w), "h": int(h), "hz": int(hz), "khz": pll_khz(io[0x40 // 4]),
            "hfp": hss - hdisp, "hsync": hse - hss, "hbp": htot - hse,
            "vfp": vss - vdisp, "vsync": vse - vss, "vbp": vtot - vse,
            "flags": " | ".join(flags) or "0", "ok": hdisp == int(w) and rows == int(h)}


def ours():
    text = (HERE.parent / "common" / "vcr_modes.c").read_text()
    return {(int(a), int(b), int(c)) for a, b, c in
            re.findall(r"\{\s*(\d+),\s*(\d+),\s*(\d+),\s*\d+,", text)}


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("golden")
    ap.add_argument("--all", action="store_true", help="also modes we already have")
    a = ap.parse_args(argv)
    g = json.load(open(a.golden))
    have = ours()
    seen = set()
    for cap in g["captures"]:
        if not cap.get("ok") or "vga" not in cap:
            continue
        t = decode(cap)
        if not t or not t["ok"]:
            continue
        key = (t["w"], t["h"], t["hz"])
        if key in seen or (key in have and not a.all):
            continue
        seen.add(key)
        print(f"    {{ {t['w']:4d},{t['h']:4d}, {t['hz']:3d}, {t['khz']:6d}, {t['hfp']:3d}, "
              f"{t['hsync']:3d}, {t['hbp']:3d}, {t['vfp']:2d}, {t['vsync']}, {t['vbp']:2d}, "
              f"{t['flags']} }},")
    return 0


if __name__ == "__main__":
    sys.exit(main())
