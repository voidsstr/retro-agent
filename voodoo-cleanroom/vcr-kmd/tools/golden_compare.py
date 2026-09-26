#!/usr/bin/env python3
"""golden_compare.py - our mode math against a vendor golden capture.

Compiles tools/modecalc.c (the miniport's own common/vcr_modes.c) for the
host, then for every mode both sides have, compares what the vendor driver
programmed (golden_capture.py, IO registers + the VGA file from vcrprobe) with
what ours would program: pixel clock, 2X, pixel format, screen size, sync
polarity and every CRTC register (masking the bits that do not affect timing).

    golden_compare.py golden/amigamerlin-3.1-r11_192.168.1.124.json [--mode 1024x768x16@85]

A difference is not automatically a bug in ours - the vendor may use a
different (GTF) timing at the same nominal refresh; the report says which
registers differ so the timing can be read off.
"""
import argparse
import json
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent


def ours():
    cc = shutil.which("gcc") or shutil.which("cc")
    with tempfile.TemporaryDirectory() as d:
        exe = Path(d) / "modecalc"
        subprocess.run([cc, "-O1", "-o", str(exe), str(HERE / "modecalc.c")], check=True)
        out = subprocess.run([str(exe)], check=True, capture_output=True, text=True).stdout
    return {j["mode"]: j for j in map(json.loads, out.splitlines())}


def pll_khz(v):
    n, m, k = (v >> 8) & 0xff, (v >> 2) & 0x3f, v & 3
    return (14318 * (n + 2) // (m + 2)) >> k


def timing_from_crtc(c, e1a, e1b):
    """Decode htotal/vtotal/... (in chars/lines) from a VGA CRTC dump."""
    ht = c[0] | ((e1a & 1) << 8)
    hd = c[1] | ((e1a & 4) << 6)
    hs = c[4] | ((e1a & 0x40) << 2)
    vt = c[6] | ((c[7] & 1) << 8) | ((c[7] & 0x20) << 4) | ((e1b & 1) << 10)
    vd = c[0x12] | ((c[7] & 2) << 7) | ((c[7] & 0x40) << 3) | ((e1b & 4) << 8)
    vs = c[0x10] | ((c[7] & 4) << 6) | ((c[7] & 0x80) << 2) | ((e1b & 0x40) << 4)
    return {"htotal_chars": ht + 5, "hdisp_chars": hd + 1, "hsync_char": hs,
            "vtotal": vt + 2, "vdisp": vd + 1, "vsync_line": vs}


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("golden")
    ap.add_argument("--mode")
    a = ap.parse_args(argv)
    g = json.load(open(a.golden))
    mine = ours()
    same = diff = 0
    for cap in g["captures"]:
        m = cap.get("tag")
        if not cap.get("ok") or "vga" not in cap or m not in mine or (a.mode and m != a.mode):
            continue
        io = [int(x, 16) for x in cap["io"]]
        vga = cap["vga"]
        vc = bytes.fromhex(vga["crtc"])
        o = mine[m]
        oc = bytes.fromhex(o["crtc"])
        vendor_khz = pll_khz(io[0x40 // 4])
        problems = []
        if abs(vendor_khz - o["khz"]) > max(50, o["khz"] // 200):
            problems.append(f"pixclk vendor {vendor_khz} ours {o['khz']}")
        if (io[0x4c // 4] & 1) != (o["dacmode"] & 1):
            problems.append(f"2X vendor {io[0x4c // 4] & 1} ours {o['dacmode'] & 1}")
        if io[0x98 // 4] & 0xffffff != int(o["screensize"], 16):
            problems.append(f"screensize vendor {io[0x98 // 4]:08x} ours {o['screensize']}")
        vm, om = int(vga["misc"], 16), int(o["misc"], 16)
        if (vm & 0xc0) != (om & 0xc0):
            problems.append(f"sync polarity misc vendor {vm:02x} ours {om:02x}")
        crtc_diff = [f"CR{i:02x} {vc[i]:02x}/{oc[i]:02x}" for i in range(25)
                     if i not in (0x0e, 0x0f, 0x0c, 0x0d, 0x0a, 0x0b, 0x13, 0x14, 0x17, 0x18)
                     and vc[i] != oc[i]]
        for idx, key in ((0x1a, "crtc1a"), (0x1b, "crtc1b")):
            if vc[idx] != int(o[key], 16):
                crtc_diff.append(f"CR{idx:02x} {vc[idx]:02x}/{o[key]}")
        if crtc_diff:
            problems.append("CRTC vendor/ours: " + " ".join(crtc_diff))
        vt = timing_from_crtc(vc, vc[0x1a], vc[0x1b])
        ot = timing_from_crtc(oc, int(o["crtc1a"], 16), int(o["crtc1b"], 16))
        if problems:
            diff += 1
            print(f"{m:>18}: DIFFERS")
            for p in problems:
                print(f"      {p}")
            if vt != ot:
                print(f"      vendor timing {vt}")
                print(f"      our    timing {ot}")
        else:
            same += 1
            print(f"{m:>18}: identical")
    print(f"{same} identical, {diff} differ")
    return 0


if __name__ == "__main__":
    sys.exit(main())
