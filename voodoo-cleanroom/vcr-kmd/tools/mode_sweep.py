#!/usr/bin/env python3
"""mode_sweep.py - set every mode the driver offers and prove each one.

For each mode: ChangeDisplaySettings through `vcrctl setmode`, then the GDI
draw/read-back test (`vcrctl gdi`), then the flight recorder since the last
mode (`vcrctl log <after>`): a mode passes only when the switch reported
success, the CURRENT mode reads back as the one asked for, GDI read back what
it drew, and the driver logged no WARN/ERROR while doing it. A screenshot is
kept per mode for the eye.

Runs against the QEMU test bed (127.0.0.1:19910) and the real card alike.
With --golden (a golden_capture.py file from the same box):
  - only modes the VENDOR offered are visited: its list is filtered by the
    monitor, so a CRT is never driven outside what it accepts;
  - after each switch our driver's live registers (vcrctl snapshot, read back
    from the chip) must equal the vendor's for that mode: every CRTC timing
    byte, CR1A/CR1B, misc, pllCtrl0, dacMode, vidScreenSize, and vidProcCfg
    minus the tiled-desktop and hardware-cursor bits ours does not use yet.

    mode_sweep.py 127.0.0.1 --port 19910 [--filter 16] [--limit 20] [--shots DIR]
    mode_sweep.py 192.168.1.124 --golden golden/amigamerlin-3.1-r11_192.168.1.124.json
"""
import argparse
import asyncio
import json
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parents[2]))
sys.path.insert(0, str(HERE))
from client.retro_protocol import RetroConnection  # noqa: E402
import vcrlog  # noqa: E402


CRTC_IGNORE = {0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x14}   # cursor/start address
VPC_IGNORE = (1 << 24) | (1 << 27)                          # tiled desktop, hw cursor


def against_golden(snap, cap):
    """Differences between our live registers and the vendor's for one mode."""
    ours = snap["chips"][0]
    oio = [int(x, 16) for x in ours["io"]]
    vio = [int(x, 16) for x in cap["io"]]
    diffs = []
    for off, name, mask in ((0x40, "pllCtrl0", 0xffff), (0x4c, "dacMode", 0x1f),
                            (0x98, "vidScreenSize", 0xffffff),
                            (0x5c, "vidProcCfg", ~VPC_IGNORE & 0xffffffff)):
        if (oio[off // 4] & mask) != (vio[off // 4] & mask):
            diffs.append(f"{name} {oio[off // 4]:08x}/{vio[off // 4]:08x}")
    oc = bytes.fromhex(ours["crtc"])
    vc = bytes.fromhex(cap["vga"]["crtc"])
    for i in list(range(0x19)) + [0x1a, 0x1b]:
        if i not in CRTC_IGNORE and oc[i] != vc[i]:
            diffs.append(f"CR{i:02x} {oc[i]:02x}/{vc[i]:02x}")
    if ours["misc"] != cap["vga"]["misc"]:
        diffs.append(f"misc {ours['misc']}/{cap['vga']['misc']}")
    return diffs


def jline(text):
    for ln in reversed(text.strip().splitlines()):
        if ln.startswith("{"):
            try:
                return json.loads(ln)
            except json.JSONDecodeError:
                return None
    return None


async def main_async(a):
    c = RetroConnection(a.host, a.port)
    await c.connect("retro-agent-secret", timeout=20)
    tool = a.tool
    events = vcrlog.load_events()

    async def run(cmd, timeout=60):
        st, d = await c.send_command(f"EXEC {tool} {cmd}", timeout=timeout)
        return d.decode("ascii", "replace")

    results = []
    try:
        info = jline(await run("info"))
        after = info["log_next_seq"] if info and info.get("ok") else 0
        # XP adds its VGA driver's 4 bpp modes to the list; they are not ours
        modes = [m for m in jline(await run("modes"))["modes"]
                 if int(m.split("@")[0].split("x")[2]) >= 8]
        golden = {}
        if a.golden:
            g = json.load(open(a.golden))
            golden = {c["tag"]: c for c in g["captures"] if c.get("ok") and "vga" in c}
            modes = [m for m in modes if m in golden]
        if a.filter:
            modes = [m for m in modes if m.split("@")[0].endswith("x" + a.filter)]
        if a.limit:
            modes = modes[:a.limit]
        print(f"{len(modes)} modes to sweep")
        for m in modes:
            w, h, rest = m.split("x")
            bpp, hz = rest.split("@")
            # ONE process: a CDS_FULLSCREEN mode reverts when the process that
            # set it exits, so switching in one EXEC and testing in the next
            # tests the desktop mode (every earlier sweep did exactly that).
            mt = jline(await run(f"modetest {w} {h} {bpp} {hz}")) or {}
            sm = {"ok": mt.get("current") == m, "current": mt.get("current")}
            gdi = mt.get("gdi")
            if gdi is not None:
                gdi = dict(gdi, ok=gdi.get("mismatches") == 0)
            logtxt = await run(f"log {after}")
            _, ents = vcrlog.parse_tsv(logtxt)
            if ents:
                after = ents[-1]["seq"]
            bad = [e for e in ents if e["level"] <= 1]
            regdiff = []
            if m in golden and sm.get("ok"):
                snap = mt.get("snapshot")
                regdiff = against_golden(snap, golden[m]) if snap else ["no snapshot"]
            ok = bool(sm and sm.get("ok") and sm.get("current") == m and gdi and gdi.get("ok")
                      and not bad and not regdiff)
            row = {"mode": m, "ok": ok, "setmode": sm, "gdi": gdi, "vs_vendor": regdiff,
                   "log_problems": [vcrlog.format_entry(e, events) for e in bad]}
            results.append(row)
            print(f"  {m:>18}: {'ok' if ok else 'FAIL'}"
                  f"{'' if ok else '  ' + json.dumps({k: row[k] for k in ('setmode', 'gdi')})}")
            for p in row["log_problems"]:
                print("      " + p)
            if regdiff:
                print("      registers (ours/vendor): " + " ".join(regdiff))
            if a.shots:
                data = await c.command_binary("SCREENSHOT 2")
                Path(a.shots).mkdir(parents=True, exist_ok=True)
                (Path(a.shots) / f"{m.replace('@', '_')}.bmp").write_bytes(data)
        print("restore:", jline(await run("restore")))
    finally:
        await c.close()
    if a.out:
        Path(a.out).write_text(json.dumps(results, indent=1))
    failed = [r["mode"] for r in results if not r["ok"]]
    print(f"{len(results) - len(failed)}/{len(results)} modes passed"
          + (f"; failed: {', '.join(failed)}" if failed else ""))
    return 1 if failed else 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("host")
    ap.add_argument("--port", type=int, default=9898)
    ap.add_argument("--tool", default=r"C:\vcr\vcrctl.exe")
    ap.add_argument("--filter", help="only this bpp (8/16/32)")
    ap.add_argument("--limit", type=int)
    ap.add_argument("--shots")
    ap.add_argument("--out")
    ap.add_argument("--golden", help="golden capture: vendor modes only + register check")
    a = ap.parse_args()
    sys.exit(asyncio.run(main_async(a)))


if __name__ == "__main__":
    main()
