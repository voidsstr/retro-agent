#!/usr/bin/env python3
"""golden_capture.py - register dumps from a KNOWN-GOOD driver, per mode.

Uploads vcrctl.exe to a box, proves the HWCEXT mapping works through whatever
3dfx driver is installed (`vcrctl hwc`), then for each mode sets it through
GDI and dumps every IO register plus the CRTC (`vcrctl golden`). The result is
the reference our own mode-set math is compared against
(tools/golden_compare.py) BEFORE our driver programs the chip.

Read-only apart from the mode changes and the CRTC index writes; the box's
own mode is restored at the end.

    golden_capture.py 192.168.1.124 --label amigamerlin-3.1-r11
    golden_capture.py 192.168.1.124 --modes 1024x768x16@85,1600x1200x32@85
"""
import argparse
import asyncio
import json
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[2]
sys.path.insert(0, str(REPO))
from client.retro_protocol import RetroConnection  # noqa: E402

SECRET = "retro-agent-secret"
DEFAULT_DIR = r"C:\vcr"


async def run(c, cmd, timeout=60):
    st, d = await c.send_command(cmd, timeout=timeout)
    return d.decode("ascii", "replace")


def last_json(text):
    for line in reversed(text.strip().splitlines()):
        line = line.strip()
        if line.startswith("{"):
            try:
                return json.loads(line)
            except json.JSONDecodeError:
                pass
    return None


def pick_modes(listed, limit_per_size=None):
    """Every listed mode at 16 bpp, plus 8 and 32 bpp at each size's lowest
    and highest refresh - the axes the mode math varies along."""
    by_size = {}
    for m in listed:
        wh, rest = m.rsplit("x", 1)[0], m
        w, h, bpp_hz = m.split("x")
        bpp, hz = bpp_hz.split("@")
        by_size.setdefault((int(w), int(h)), []).append((int(bpp), int(hz)))
    out = []
    for (w, h), lst in sorted(by_size.items()):
        hzs = sorted({hz for bpp, hz in lst if hz > 1})
        for bpp, hz in sorted(set(lst)):
            if hz <= 1:
                continue
            if bpp == 16 or (bpp in (8, 32) and hz in (hzs[0], hzs[-1])):
                out.append(f"{w}x{h}x{bpp}@{hz}")
    return out


async def main_async(a):
    exe = Path(a.exe)
    c = RetroConnection(a.host, 9898)
    await c.connect(SECRET, timeout=20)
    result = {"host": a.host, "label": a.label, "taken": time.strftime("%Y-%m-%dT%H:%M:%S"),
              "captures": []}
    try:
        await run(c, f"MKDIR {a.dir}")
        data = exe.read_bytes()
        await c.send_command(rf"UPLOAD {a.dir}\vcrctl.exe", binary_payload=data, timeout=60)
        # Verify the post-condition: an UPLOAD to a drive that does not exist
        # (.124 lost its D: in a re-image) answers without error.
        listing = await run(c, f"DIRLIST {a.dir}")
        if f'"size":{len(data)}' not in listing.replace(" ", ""):
            print(f"FAIL: vcrctl.exe did not land in {a.dir}: {listing[:200]}")
            return 2
        tool = rf"{a.dir}\vcrctl.exe"
        probe_loaded = False
        if a.probe:
            # vcrprobe.sys next to the installed driver: the VGA register file
            # (unreachable from user mode) and PCI config of every chip
            pdata = Path(a.probe).read_bytes()
            await c.send_command(rf"UPLOAD {a.dir}\vcrprobe.sys", binary_payload=pdata, timeout=60)
            await run(c, rf"EXEC sc create vcrprobe type= kernel start= demand binPath= {a.dir}\vcrprobe.sys")
            started = await run(c, "EXEC sc start vcrprobe")
            pv = last_json(await run(c, f"EXEC {tool} probe-vga"))
            probe_loaded = bool(pv and pv.get("ok"))
            print(f"vcrprobe: {'loaded' if probe_loaded else 'NOT loaded: ' + started[-200:]}")
            result["probe_vga_desktop"] = pv
            if probe_loaded:
                result["pci"] = []
                for bus, dev, fns in a.pci:
                    for fn in fns:
                        result["pci"].append(last_json(await run(c, f"EXEC {tool} probe-pci {bus} {dev} {fn}")))
        result["hwc"] = last_json(await run(c, f"EXEC {tool} hwc"))
        print("hwc:", json.dumps(result["hwc"]))
        modes = last_json(await run(c, f"EXEC {tool} modes"))
        if not modes:
            print("FAIL: vcrctl modes produced no JSON")
            return 2
        result["modes"] = modes
        original = modes.get("current") if modes else None
        print(f"{len(modes.get('modes', []))} modes listed, current {original}")
        todo = a.modes.split(",") if a.modes else pick_modes(modes.get("modes", []))
        print(f"capturing {len(todo)} modes")
        for m in todo:
            w, h, rest = m.split("x")
            bpp, hz = rest.split("@")
            out = last_json(await run(c, f"EXEC {tool} golden {w} {h} {bpp} {hz}", timeout=60))
            ok = bool(out and out.get("ok"))
            print(f"  {m}: {'ok' if ok else 'FAILED'} {'' if ok else out}")
            result["captures"].append(out or {"cmd": "golden", "ok": False, "mode": m})
        result["restore"] = last_json(await run(c, f"EXEC {tool} restore"))
        print("restore:", result["restore"])
        if a.probe:
            print("vcrprobe unload:", (await run(c, "EXEC sc stop vcrprobe")).strip()[-80:])
            await run(c, "EXEC sc delete vcrprobe")
    finally:
        await c.close()
    outp = Path(a.out) if a.out else HERE.parent / "golden" / f"{a.label}_{a.host}.json"
    outp.parent.mkdir(parents=True, exist_ok=True)
    outp.write_text(json.dumps(result, indent=1))
    print(f"-> {outp}")
    return 0 if all(x.get("ok") for x in result["captures"]) else 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("host")
    ap.add_argument("--label", required=True, help="which driver produced it")
    ap.add_argument("--modes", help="comma list WxHxBPP@HZ (default: a sweep)")
    ap.add_argument("--exe", default=str(HERE.parent / "out" / "vcrctl.exe"))
    ap.add_argument("--dir", default=DEFAULT_DIR)
    ap.add_argument("--out")
    ap.add_argument("--probe", help="vcrprobe.sys to load for the VGA file + PCI config")
    ap.add_argument("--pci", default="3:0:0123,2:0:0",
                    help="bus:dev:fns to dump with the probe (default: the V5 6000's "
                         "four chip functions on bus 3 and its HiNT bridge on bus 2)")
    a = ap.parse_args()
    a.pci = [(int(b), int(d), [int(f) for f in fns])
             for b, d, fns in (x.split(":") for x in a.pci.split(",") if x)]
    sys.exit(asyncio.run(main_async(a)))


if __name__ == "__main__":
    main()
