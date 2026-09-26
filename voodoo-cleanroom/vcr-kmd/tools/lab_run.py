#!/usr/bin/env python3
"""lab_run.py - run one of the self-checking lab programs on a box or test bed.

    lab_run.py gdilab 127.0.0.1 --port 19920 [-- --rounds 5]
    lab_run.py ddlab  192.168.1.124 -- blt --res 800x600 --bpp 16

Uploads out/<lab>.exe to C:\\vcr\\<lab>\\, runs it through `start /wait` (a
normal desktop window) under EXECW with --log, and prints the RESULT json
from the flushed log. Exit 1 on an error, a failed check ("fail" > 0) or any
"bad*" count > 0 - the labs report counts, never a bare pass.
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


def verdict(res):
    if "error" in res or res.get("fail"):
        return 1
    return 1 if any(v for k, v in res.items() if k.startswith("bad") and isinstance(v, int)) else 0


async def main_async(a, rest):
    d = rf"C:\vcr\{a.lab}"
    await call(a, rf"MKDIR {d}")
    await call(a, rf"UPLOAD {d}\{a.lab}.exe", (KMD / "out" / f"{a.lab}.exe").read_bytes())
    log = rf"{d}\{a.lab}.log"
    await call(a, rf'EXEC cmd /c del /f /q "{log}"')
    args = " ".join(rest + ["--log", log])
    await call(a, f'EXECW {a.timeout} cmd /c start "{a.lab}" /wait "{d}\\{a.lab}.exe" {args}',
               timeout=a.timeout + 40)
    text = (await call(a, f"DOWNLOAD {log}")).decode("latin1", "replace")
    res = None
    for ln in text.splitlines():
        if ln.startswith("RESULT "):
            res = json.loads(ln[7:])
    if res is None:
        res = {"lab": a.lab, "error": "no RESULT line", "log_tail": text.splitlines()[-8:]}
    print(json.dumps(res))
    if a.verbose:
        print(text)
    return verdict(res)


def main():
    argv = sys.argv[1:]
    rest = []
    if "--" in argv:
        i = argv.index("--")
        argv, rest = argv[:i], argv[i + 1:]
    ap = argparse.ArgumentParser()
    ap.add_argument("lab", help="gdilab | ddlab | d3dprobe | ...")
    ap.add_argument("host")
    ap.add_argument("--port", type=int, default=9898)
    ap.add_argument("--timeout", type=int, default=240)
    ap.add_argument("-v", "--verbose", action="store_true")
    a = ap.parse_args(argv)
    sys.exit(asyncio.run(main_async(a, rest)))


if __name__ == "__main__":
    main()
