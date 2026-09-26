#!/usr/bin/env python3
"""silicon_battery.py - every vcr-kmd check that can run on a real box, in one
go, after `deploy_box.py install`:

    silicon_battery.py 192.168.1.124 --label vcrkmd-<date> [--golden <vendor golden>]
                       [--vendor-cursor amigamerlin-3.1-r11] [--skip modes,d3d]

Steps (each a subprocess of the existing tool, so each can also run alone):
  info       vcrctl info (the driver is ours and bound)
  modes      mode_sweep.py (--golden: the vendor's modes + register check)
  cursor     cursor_golden.py --compare (the hardware cursor vs the vendor's)
  gdi        gdilab (2D engine: fills, copies, scrolls, clip, engine/CPU order)
  ddraw      ddlab caps/flip/blt at 16 and 32 bpp
  d3d        d3dprobe caps; render fullscreen 16 bpp at 640x480 and 1024x768
  d3dperf    d3dprobe perf fullscreen, no vsync

Every step's JSON (or its tail) goes to evidence/silicon/<label>.jsonl as
{"step", "rc", "result"}. Between steps the agent is PINGed; if it stopped
answering, the battery stops there and says which step it was - the flight
recorder (vcrphases.py --prev after a power cycle) says the rest.
"""
import argparse
import asyncio
import json
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
KMD = HERE.parent
sys.path.insert(0, str(KMD.parents[1]))
from client.retro_protocol import RetroConnection  # noqa: E402

STEPS = ("info", "modes", "cursor", "gdi", "ddraw", "d3d", "d3dperf")


def log(m):
    print(f"[{time.strftime('%H:%M:%S')}] {m}", flush=True)


async def ping(host, port):
    try:
        c = RetroConnection(host, port)
        await c.connect("retro-agent-secret", timeout=15)
        st, d = await c.send_command("PING", timeout=15)
        await c.close()
        return d == b"PONG"
    except Exception:
        return False


async def agent(host, port, cmd, timeout=90):
    c = RetroConnection(host, port)
    await c.connect("retro-agent-secret", timeout=20)
    try:
        st, d = await c.send_command(cmd, timeout=timeout)
        return d.decode("latin1", "replace")
    finally:
        await c.close()


def run(args, timeout):
    t0 = time.time()
    try:
        p = subprocess.run([sys.executable] + args, cwd=KMD, capture_output=True, text=True,
                           timeout=timeout)
        out, rc = p.stdout + p.stderr, p.returncode
    except subprocess.TimeoutExpired as e:
        out, rc = (e.stdout or "") if isinstance(e.stdout, str) else "", -9
    lines = [ln for ln in out.splitlines() if ln.strip()]
    res = []
    for ln in lines:
        if ln.startswith("{"):
            try:
                res.append(json.loads(ln))
            except ValueError:
                pass
    return rc, res or lines[-6:], time.time() - t0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("host")
    ap.add_argument("--port", type=int, default=9898)
    ap.add_argument("--label", required=True)
    ap.add_argument("--golden", help="mode_sweep --golden capture")
    ap.add_argument("--vendor-cursor", default="amigamerlin-3.1-r11")
    ap.add_argument("--skip", default="")
    a = ap.parse_args()
    skip = set(filter(None, a.skip.split(",")))
    out = KMD / "evidence" / "silicon"
    out.mkdir(parents=True, exist_ok=True)
    jl = out / f"{a.label}.jsonl"
    h, pp = a.host, ["--port", str(a.port)]
    plan = {
        "info": None,
        "modes": (["tools/mode_sweep.py", h] + pp + (["--golden", a.golden] if a.golden else []), 3600),
        "cursor": (["tools/cursor_golden.py", h, "--label", a.label, "--compare", a.vendor_cursor], 300),
        "gdi": (["tools/lab_run.py", "gdilab", h] + pp, 600),
        "ddraw": None,
        "d3d": None,
        "d3dperf": (["tools/d3dprobe_run.py", h] + pp + ["perf", "--full", "--novsync", "--res",
                                                         "640x480", "--frames", "300"], 600),
    }
    bad = 0
    for step in STEPS:
        if step in skip:
            continue
        if not asyncio.run(ping(h, a.port)):
            log(f"AGENT SILENT before '{step}' - stopping; the previous step took it down")
            with open(jl, "a") as f:
                f.write(json.dumps({"step": step, "rc": None, "result": "agent silent before this step"}) + "\n")
            return 3
        log(f"== {step}")
        results = []
        if step == "info":
            txt = asyncio.run(agent(h, a.port, r"EXEC C:\vcr\vcrctl.exe info"))
            results.append((0, [txt.strip()[:1500]], 0))
        elif step == "ddraw":
            for bpp in (16, 32):
                for m in ("caps", "flip", "blt"):
                    results.append(run(["tools/ddlab_run.py", h] + pp + [m, "--res", "800x600", "--bpp",
                                                                        str(bpp), "--frames", "60"], 400))
        elif step == "d3d":
            results.append(run(["tools/d3dprobe_run.py", h] + pp + ["caps"], 300))
            for res in ("640x480", "1024x768"):
                results.append(run(["tools/d3dprobe_run.py", h] + pp + ["render", "--full", "--res", res,
                                                                        "--bpp", "16"], 600))
        else:
            results.append(run(*plan[step]))
        for rc, res, dt in results:
            bad += rc != 0
            log(f"   rc {rc} ({dt:.0f}s): {json.dumps(res)[:300]}")
            with open(jl, "a") as f:
                f.write(json.dumps({"step": step, "rc": rc, "seconds": round(dt), "result": res}) + "\n")
    log(f"done: {bad} failing sub-step(s) -> {jl}")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
