#!/usr/bin/env python3
"""
d3_timetest.py - Descent 3's own benchmark on one box: retail main.exe 1.4
`-timetest <demo>` (the GameGauge mode: play the demo, write fps.txt).

    python3 scripts/benchmarks/d3_timetest.py --host 192.168.1.124 --out <dir>
        [--demo Secret2.dem] [--width 1280 --height 960] [--wait 600]
        [--env FX_GLIDE_SWAPINTERVAL=1] [--extra "-framecap 85"]

It starts the game the way the staged launcher does (cd into the tree, start
main.exe -launched), so main.exe finds its data in the working directory, then
waits for the game to EXIT BY ITSELF and reads fps.txt. It never force-kills a
live fullscreen game - if the bound passes with main.exe still running it says
so and leaves the box for a person. SCREENSHOTS: a GDI capture shows the real
frame where the game renders through Direct3D on XP (.123); where it renders
through Glide (our ICD on .124) GDI reads the desktop surface in the board's
tiled layout - stripes, NOT the game - so there the evidence is fps.txt, the
clean exit and the ICD log. --env sets a variable for the game only
(the agent's own environment is not the desktop's: on .124 it lacks the system
FX_GLIDE_SWAPINTERVAL=1 that a desktop launch inherits, so a run without
--env FX_GLIDE_SWAPINTERVAL=1 measures vsync OFF). Everything it saw goes to --out: fps.txt,
the new part of the 3dfx ICD's own log (C:\\retrogl.log - which board mode and
colour depth the game really opened), GDI screenshots, run.json.
"""
import argparse
import asyncio
import json
import os
import sys
import time
from io import BytesIO
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parents[1]))
from client.retro_protocol import RetroConnection  # noqa: E402

SECRET = "retro-agent-secret"
TREE = r"C:\Games\Descent3"
ICD_LOG = r"C:\retrogl.log"
BAT = r"C:\WINDOWS\TEMP\d3tt.bat"


def launch_bat(demo, width, height, env=(), extra=""):
    """The batch file that starts one timetest, exactly the way the staged
    launcher starts the game: from the tree (main.exe finds its data in the
    working directory), detached, with a stale fps.txt removed first so an old
    result can never be read as this run's."""
    args = f"-launched -nointro -timetest {demo} -Width {width} -Height {height}"
    if extra:
        args += " " + extra
    sets = "".join(f"set {kv}\r\n" for kv in env)
    return ("@echo off\r\n"
            f'cd /d "{TREE}"\r\n'
            "if exist fps.txt del fps.txt\r\n"
            + sets +
            f'start "" main.exe {args}\r\n'), args


async def call(host, cmd, payload=None, raw=False, timeout=60):
    c = RetroConnection(host, 9898)
    await c.connect(SECRET, timeout=15)
    try:
        st, data = await (c.send_command(cmd, timeout=timeout, binary_payload=payload)
                          if payload is not None else c.send_command(cmd, timeout=timeout))
        return data if raw else data.decode("latin-1", "replace")
    finally:
        await c.close()


async def alive(host, image="main.exe"):
    pl = json.loads(await call(host, "PROCLIST"))
    procs = pl if isinstance(pl, list) else pl.get("processes", [])
    return any((p.get("name") or "").lower() == image for p in procs)


async def file_bytes(host, path):
    d = await call(host, f"DOWNLOAD {path}", raw=True, timeout=120)
    return None if d.startswith(b"Cannot open file") else d


async def shot(host, path):
    try:
        bmp = await call(host, "SCREENSHOT 0", raw=True, timeout=90)
        from PIL import Image
        Image.open(BytesIO(bmp)).save(path, optimize=True)
        return True
    except Exception as e:      # a capture failure is recorded, not fatal
        print(f"  screenshot failed: {e}")
        return False


async def amain(a):
    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    rec = {"host": a.host, "demo": a.demo, "width": a.width, "height": a.height,
           "env": a.env, "extra": a.extra, "started": time.strftime("%Y-%m-%d %H:%M:%S")}
    if await alive(a.host):
        raise SystemExit("main.exe is already running - not starting a second one")
    log0 = await file_bytes(a.host, ICD_LOG) or b""
    old_fps = await file_bytes(a.host, TREE + r"\fps.txt")
    # dismiss a screensaver: a game started under one can come up behind it
    fg = json.loads(await call(a.host, "WINLIST")).get("foreground") or {}
    if fg.get("class") == "WindowsScreenSaverClass":
        await call(a.host, "UIKEY SHIFT")
        await asyncio.sleep(3)
    bat, args = launch_bat(a.demo, a.width, a.height, a.env, a.extra)
    await call(a.host, f"UPLOAD {BAT}", payload=bat.encode("latin-1"))
    print(f"[{time.strftime('%H:%M:%S')}] main.exe {args}")
    await call(a.host, f"EXEC cmd /c {BAT}", timeout=60)
    await call(a.host, f"DELETE {BAT}")
    t0 = time.time()
    shots, seen = [30, 60, 120], False
    while time.time() - t0 < a.wait:
        await asyncio.sleep(5)
        el = time.time() - t0
        up = await alive(a.host)
        seen = seen or up
        if shots and el >= shots[0]:
            n = shots.pop(0)
            if up:
                await shot(a.host, out / f"descent3_timetest_{n}s.png")
        if not up and (seen or el > 20):
            break
    el = round(time.time() - t0, 1)
    still = await alive(a.host)
    rec.update(elapsed_s=el, exited_by_itself=not still)
    fps = await file_bytes(a.host, TREE + r"\fps.txt")
    if fps is not None and fps == old_fps:
        fps = None                      # the old file - this run wrote nothing
    rec["fps_txt"] = fps.decode("latin-1", "replace").strip() if fps else None
    if fps:
        (out / "fps.txt").write_bytes(fps)
    log1 = await file_bytes(a.host, ICD_LOG) or b""
    new = log1[len(log0):] if log1.startswith(log0[:4096]) else log1
    if new:                             # no 3dfx ICD on the box: no log
        (out / "retrogl_new.log").write_bytes(new)
    rec["icd_log_lines"] = len(new.splitlines())
    (out / "run.json").write_text(json.dumps(rec, indent=2) + "\n")
    print(json.dumps(rec, indent=2))
    if still:
        print("main.exe is STILL RUNNING - left alone (never force-kill a live fullscreen game)")
        return 3
    return 0 if fps else 2


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--host", default="192.168.1.124")
    ap.add_argument("--out", required=True)
    ap.add_argument("--demo", default="Secret2.dem")
    ap.add_argument("--width", type=int, default=1280)
    ap.add_argument("--height", type=int, default=960)
    ap.add_argument("--wait", type=int, default=600)
    ap.add_argument("--env", action="append", default=[], metavar="NAME=VALUE",
                    help="set for the game only, e.g. FX_GLIDE_SWAPINTERVAL=1")
    ap.add_argument("--extra", default="", help="more main.exe arguments, e.g. '-framecap 85'")
    raise SystemExit(asyncio.run(amain(ap.parse_args())))


if __name__ == "__main__":
    main()
