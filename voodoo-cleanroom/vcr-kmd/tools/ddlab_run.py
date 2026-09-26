#!/usr/bin/env python3
"""ddlab_run.py - run ddlab.exe (tools/ddlab.c) on a box or the VM test bed.

    ddlab_run.py 127.0.0.1 --port 19910 caps
    ddlab_run.py 192.168.1.124 flip --res 800x600 --bpp 16 --frames 120

Uploads out/ddlab.exe to C:\\vcr\\ddlab\\, runs it through `start /wait` (a
normal desktop window - exclusive mode needs one) under EXECW, and prints the
RESULT json from the flushed log.
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

DIR = r"C:\vcr\ddlab"


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
    await call(a, rf"UPLOAD {DIR}\ddlab.exe", (KMD / "out" / "ddlab.exe").read_bytes())
    log = rf"{DIR}\{a.mode}.log"
    await call(a, rf'EXEC cmd /c del /f /q "{log}"')
    args = f"{a.mode} --res {a.res} --bpp {a.bpp} --frames {a.frames} --log {log}"
    await call(a, f'EXECW {a.timeout} cmd /c start "ddlab" /wait "{DIR}\\ddlab.exe" {args}',
               timeout=a.timeout + 40)
    text = (await call(a, f"DOWNLOAD {log}")).decode("latin1", "replace")
    res = None
    for ln in text.splitlines():
        if ln.startswith("RESULT "):
            res = json.loads(ln[7:])
    if res is None:
        res = {"mode": a.mode, "error": "no RESULT line", "log_tail": text.splitlines()[-6:]}
    print(json.dumps(res))
    if a.verbose:
        print(text)
    return 0 if "error" not in res and not any(res.get(k) for k in ("mismatch", "lock_fail", "bad_copy", "bad_fill", "bad_scroll")) else 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("host")
    ap.add_argument("mode", choices=("caps", "flip", "blt"))
    ap.add_argument("--port", type=int, default=9898)
    ap.add_argument("--res", default="640x480")
    ap.add_argument("--bpp", type=int, default=16)
    ap.add_argument("--frames", type=int, default=120)
    ap.add_argument("--timeout", type=int, default=120)
    ap.add_argument("-v", "--verbose", action="store_true")
    a = ap.parse_args()
    sys.exit(asyncio.run(main_async(a)))


if __name__ == "__main__":
    main()
