#!/usr/bin/env python3
"""mode_sweep.py - set every mode the driver offers and prove each one.

For each mode: ChangeDisplaySettings through `vcrctl setmode`, then the GDI
draw/read-back test (`vcrctl gdi`), then the flight recorder since the last
mode (`vcrctl log <after>`): a mode passes only when the switch reported
success, the CURRENT mode reads back as the one asked for, GDI read back what
it drew, and the driver logged no WARN/ERROR while doing it. A screenshot is
kept per mode for the eye.

Runs against the QEMU test bed (127.0.0.1:19910) and the real card alike.

    mode_sweep.py 127.0.0.1 --port 19910 [--filter 16] [--limit 20] [--shots DIR]
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
        if a.filter:
            modes = [m for m in modes if m.split("@")[0].endswith("x" + a.filter)]
        if a.limit:
            modes = modes[:a.limit]
        print(f"{len(modes)} modes to sweep")
        for m in modes:
            w, h, rest = m.split("x")
            bpp, hz = rest.split("@")
            sm = jline(await run(f"setmode {w} {h} {bpp} {hz}"))
            gdi = jline(await run("gdi")) if sm and sm.get("ok") else None
            logtxt = await run(f"log {after}")
            _, ents = vcrlog.parse_tsv(logtxt)
            if ents:
                after = ents[-1]["seq"]
            bad = [e for e in ents if e["level"] <= 1]
            ok = bool(sm and sm.get("ok") and sm.get("current") == m and gdi and gdi.get("ok")
                      and not bad)
            row = {"mode": m, "ok": ok, "setmode": sm, "gdi": gdi,
                   "log_problems": [vcrlog.format_entry(e, events) for e in bad]}
            results.append(row)
            print(f"  {m:>18}: {'ok' if ok else 'FAIL'}"
                  f"{'' if ok else '  ' + json.dumps({k: row[k] for k in ('setmode', 'gdi')})}")
            for p in row["log_problems"]:
                print("      " + p)
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
    a = ap.parse_args()
    sys.exit(asyncio.run(main_async(a)))


if __name__ == "__main__":
    main()
