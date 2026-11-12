#!/usr/bin/env python3
"""agentcall.py - one retro-agent call per invocation, one connection, closed
gracefully (the on-box evidence run of 2026-11-12, d3dplan_1112/onbox).

    agentcall.py cmd  "<agent command>" [--timeout S]     print status + text reply
    agentcall.py shot <out.png>                           SCREENSHOT 0 -> PNG
    agentcall.py up   <local file> <remote path>          UPLOAD
    agentcall.py down <remote path> <local file>          DOWNLOAD
    agentcall.py ping                                     protocol PING (not TCP connect)

Host 192.168.1.124:9898 unless --host/--port. Never used while another
connection of ours is open: one connection at a time.
"""
import argparse
import asyncio
import io
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[5]          # the retro-agent worktree
sys.path.insert(0, str(ROOT))
from client.retro_protocol import RetroConnection  # noqa: E402

SECRET = "retro-agent-secret"


async def run(a):
    c = RetroConnection(a.host, a.port)
    await c.connect(SECRET, timeout=15)
    try:
        if a.op == "cmd":
            st, d = await c.send_command(a.args[0], timeout=a.timeout)
            sys.stdout.write(f"[status {st}]\n")
            sys.stdout.write(d.decode("latin1", "replace"))
            if not d.endswith(b"\n"):
                sys.stdout.write("\n")
            return 0 if st != 0xFF else 1
        if a.op == "ping":
            t = time.time()
            st, d = await c.send_command("PING", timeout=a.timeout)
            print(f"[status {st}] {d.decode('latin1', 'replace').strip()} "
                  f"({(time.time() - t) * 1000:.0f} ms)")
            return 0 if b"PONG" in d.upper() else 1
        if a.op == "shot":
            from PIL import Image
            d = await c.command_binary("SCREENSHOT 0", timeout=max(a.timeout, 90))
            img = Image.open(io.BytesIO(d))
            out = Path(a.args[0])
            out.parent.mkdir(parents=True, exist_ok=True)
            img.save(out, optimize=True)
            ext = img.convert("L").getextrema()
            print(f"{out} {img.size[0]}x{img.size[1]} luma-extrema={ext}")
            return 0
        if a.op == "up":
            data = Path(a.args[0]).read_bytes()
            st, d = await c.send_command(f"UPLOAD {a.args[1]}", binary_payload=data,
                                         timeout=a.timeout)
            print(f"[status {st}] {d.decode('latin1', 'replace').strip()} ({len(data)} bytes)")
            return 0 if st != 0xFF else 1
        if a.op == "down":
            st, d = await c.send_command(f"DOWNLOAD {a.args[0]}", timeout=a.timeout)
            if st == 0xFF:          # 0 text / 1 binary are both OK
                print(f"[status {st}] {d.decode('latin1', 'replace').strip()}")
                return 1
            Path(a.args[1]).parent.mkdir(parents=True, exist_ok=True)
            Path(a.args[1]).write_bytes(d)
            print(f"{a.args[1]} {len(d)} bytes")
            return 0
    finally:
        await c.close()
    return 2


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("op", choices=("cmd", "shot", "up", "down", "ping"))
    ap.add_argument("args", nargs="*")
    ap.add_argument("--host", default="192.168.1.124")
    ap.add_argument("--port", type=int, default=9898)
    ap.add_argument("--timeout", type=float, default=60)
    a = ap.parse_args()
    sys.exit(asyncio.run(run(a)))


if __name__ == "__main__":
    main()
