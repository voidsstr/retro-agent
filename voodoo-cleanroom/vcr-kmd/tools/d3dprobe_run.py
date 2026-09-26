#!/usr/bin/env python3
"""d3dprobe_run.py - run d3dprobe.exe (tools/d3dprobe.c) on a box or a test bed.

    d3dprobe_run.py 127.0.0.1 --port 19920 caps
    d3dprobe_run.py 127.0.0.1 --port 19920 render [--full --res 800x600 --bpp 16]
    d3dprobe_run.py 192.168.1.124 perf --full --res 800x600 --frames 300

Uploads out/d3dprobe.exe to C:\\vcr\\d3dprobe\\, runs it through `start /wait`
(a normal desktop window - a device needs one) under EXECW, and prints the
RESULT json from the flushed log. Exit 1 when a check failed or no RESULT.
"""
import argparse
import asyncio
import json
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
KMD = HERE.parent
sys.path.insert(0, str(KMD.parents[1]))
from client.retro_protocol import RetroConnection  # noqa: E402

DIR = r"C:\vcr\d3dprobe"


async def call(a, cmd, payload=None, timeout=60):
    c = RetroConnection(a.host, a.port)
    await c.connect("retro-agent-secret", timeout=20)
    try:
        if payload is not None:
            st, d = await c.send_command(cmd, binary_payload=payload, timeout=timeout)
        else:
            st, d = await c.send_command(cmd, timeout=timeout)
        return d
    finally:
        await c.close()


async def main_async(a):
    await call(a, rf"MKDIR {DIR}")
    await call(a, rf"UPLOAD {DIR}\d3dprobe.exe", (KMD / "out" / "d3dprobe.exe").read_bytes())
    log = rf"{DIR}\{a.mode}.log"
    await call(a, rf'EXEC cmd /c del /f /q "{log}"')
    args = f"{a.mode} --res {a.res} --bpp {a.bpp} --frames {a.frames} --log {log}"
    if a.full:
        args += " --full"
    if a.novsync:
        args += " --novsync"
    if a.tests:
        args += f" --tests {a.tests}"
    await call(a, f'EXECW {a.timeout} cmd /c start "d3dprobe" /wait "{DIR}\\d3dprobe.exe" {args}',
               timeout=a.timeout + 40)
    text = (await call(a, f"DOWNLOAD {log}")).decode("latin1", "replace")
    res = None
    for ln in text.splitlines():
        if ln.startswith("RESULT "):
            res = json.loads(ln[7:])
    if res is None:
        res = {"mode": a.mode, "error": "no RESULT line", "log_tail": text.splitlines()[-8:]}
    print(json.dumps(res))
    if a.verbose:
        print(text)
    return 0 if "error" not in res and not res.get("fail") else 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("host")
    ap.add_argument("mode", choices=("caps", "render", "perf"))
    ap.add_argument("--port", type=int, default=9898)
    ap.add_argument("--res", default="640x480")
    ap.add_argument("--bpp", type=int, default=16)
    ap.add_argument("--frames", type=int, default=200)
    ap.add_argument("--full", action="store_true")
    ap.add_argument("--novsync", action="store_true")
    ap.add_argument("--tests", default="")
    ap.add_argument("--timeout", type=int, default=180)
    ap.add_argument("-v", "--verbose", action="store_true")
    a = ap.parse_args()
    sys.exit(asyncio.run(main_async(a)))


if __name__ == "__main__":
    main()
