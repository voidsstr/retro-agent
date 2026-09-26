#!/usr/bin/env python3
"""cursor_golden.py - what the installed driver's hardware cursor looks like.

A screenshot cannot show a hardware cursor (the video processor overlays it at
scanout; it is never in the frame buffer GDI reads), so the cursor is checked
the way the modes were: by capturing the vendor driver's registers and pattern
bytes and requiring ours to be identical.

    cursor_golden.py 192.168.1.124 --label amigamerlin-3.1-r11
    cursor_golden.py 192.168.1.124 --label vcrkmd --compare amigamerlin-3.1-r11

Moves the pointer to a known spot (one click on empty desktop, --at X,Y), loads
vcrprobe.sys, reads vidProcCfg, hwCurPatAddr, hwCurLoc, hwCurC0/C1 from the
master's memBase0 and the 1 KB pattern from video memory, and prints the
pattern decoded as a picture (per row: 8 bytes AND plane, 8 bytes XOR plane,
MSB = leftmost - the layout include/vcr_cursor.h assumes; if that assumption
were wrong the picture would not be an arrow).
"""
import argparse
import asyncio
import json
import struct
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
KMD = HERE.parent
REPO = KMD.parents[1]
sys.path.insert(0, str(REPO / "scripts" / "benchmarks"))
import v56k_bench as vb  # noqa: E402

TOOL = r"C:\vcr\vcrctl.exe"


def jl(text):
    for ln in reversed(text.strip().splitlines()):
        if ln.startswith("{"):
            try:
                return json.loads(ln)
            except json.JSONDecodeError:
                return None
    return None


def picture(pat):
    rows = []
    for y in range(64):
        r = pat[y * 16:(y + 1) * 16]
        a = int.from_bytes(r[:8], "big")
        x = int.from_bytes(r[8:], "big")
        line = ""
        for i in range(64):
            ab = (a >> (63 - i)) & 1
            xb = (x >> (63 - i)) & 1
            line += {(0, 0): "#", (0, 1): "o", (1, 0): ".", (1, 1): "~"}[(ab, xb)]
        rows.append(line.rstrip("."))
    while rows and not rows[-1]:
        rows.pop()
    return rows


async def main_async(a):
    box = vb.Box(a.host)
    await box.upload(r"C:\vcr\vcrprobe.sys", (KMD / "out" / "vcrprobe.sys").read_bytes())
    await box.upload(TOOL, (KMD / "out" / "vcrctl.exe").read_bytes())
    await box.exec_(r"sc create vcrprobe type= kernel start= demand binPath= C:\vcr\vcrprobe.sys")
    await box.exec_("sc start vcrprobe")
    x, y = (int(v) for v in a.at.split(","))
    await box.text(f"UICLICK {x} {y}")
    await asyncio.sleep(1)
    pci = jl(await box.exec_(rf"{TOOL} probe-pci 3 0 0")) or {}
    cfg = bytes.fromhex(pci.get("cfg", ""))
    bar0 = struct.unpack_from("<I", cfg, 0x10)[0] & ~0xF
    bar1 = struct.unpack_from("<I", cfg, 0x14)[0] & ~0xF
    io = (jl(await box.exec_(rf"{TOOL} probe-mem {bar0:x} 256")) or {}).get("dwords") or []
    reg = {n: io[o // 4] for n, o in (("vidProcCfg", 0x5c), ("hwCurPatAddr", 0x60),
                                       ("hwCurLoc", 0x64), ("hwCurC0", 0x68),
                                       ("hwCurC1", 0x6c))} if io else {}
    addr = int(reg.get("hwCurPatAddr", "0"), 16)
    mem = jl(await box.exec_(rf"{TOOL} probe-mem {bar1 + addr:x} 1024")) or {}
    pat = b"".join(struct.pack("<I", int(d, 16)) for d in mem.get("dwords") or [])
    await box.exec_("sc stop vcrprobe")
    await box.exec_("sc delete vcrprobe")
    out = {"host": a.host, "label": a.label, "taken": time.strftime("%Y-%m-%dT%H:%M:%S"),
           "pointer_at": [x, y], "bar1": f"{bar1:08x}", "regs": reg, "pattern": pat.hex()}
    p = KMD / "golden" / f"cursor_{a.label}_{a.host}.json"
    p.write_text(json.dumps(out, indent=1))
    print(json.dumps({k: out[k] for k in ("label", "pointer_at", "regs")}))
    for ln in picture(pat)[:40]:
        print("   |" + ln)
    if a.compare:
        ref = json.loads((KMD / "golden" / f"cursor_{a.compare}_{a.host}.json").read_text())
        same_pat = ref["pattern"] == out["pattern"]
        diffs = {k: (ref["regs"].get(k), v) for k, v in out["regs"].items()
                 if k != "hwCurPatAddr" and ref["regs"].get(k) != v}
        print(json.dumps({"pattern_identical": same_pat, "register_differences": diffs}))
        return 0 if same_pat and not diffs else 1
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("host")
    ap.add_argument("--label", required=True)
    ap.add_argument("--at", default="900,600", help="where to put the pointer (empty desktop)")
    ap.add_argument("--compare")
    a = ap.parse_args()
    sys.exit(asyncio.run(main_async(a)))


if __name__ == "__main__":
    main()
