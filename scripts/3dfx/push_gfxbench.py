#!/usr/bin/env python3
"""
push_gfxbench.py - deploy the on-card 3dfx test/benchmark to a fleet machine.

Uploads gfxbench.exe (and, if present, the matching rebuilt glide3x.dll) to a
Voodoo machine over the retro-agent protocol, runs the headless benchmark
sweep, and pulls the CSV back. This is the M1 hardware-validation step of
docs/3dfx-d3d-hal-design.md: it exercises the exact glidebackend seam the D3D
HAL uses, on the real card, using whatever 3dfx driver is installed.

Usage:
  python3 push_gfxbench.py <agent-ip> [--card h5|h3] [--frames N]
                           [--with-glide]   also push out/glide3x_<card>_*.dll
                           [--interactive]  LAUNCH the GUI harness instead of -bench
                           [--dest 'C:\\RETRO_AGENT']

Build gfxbench first:  (cd gfxbench && make CARD=h5)
"""

import argparse
import asyncio
import os
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
_REPO = HERE.parent.parent
sys.path.insert(0, str(_REPO))
from client.retro_protocol import RetroConnection  # noqa: E402

SECRET = os.environ.get("RETRO_AGENT_SECRET", "retro-agent-secret")
PORT = int(os.environ.get("RETRO_AGENT_PORT", "9898"))


async def upload(conn, local: Path, remote: str):
    data = local.read_bytes()
    status, resp = await conn.send_command(f"UPLOAD {remote}", binary_payload=data, timeout=120)
    if status == 0xFF:
        raise RuntimeError(f"UPLOAD {remote} failed: {resp.decode('ascii','replace')}")
    print(f"  uploaded {local.name} -> {remote} ({len(data)} bytes)")


async def amain(args):
    dest = args.dest.rstrip("\\")
    exe = HERE / "gfxbench" / "gfxbench.exe"
    if not exe.exists():
        print(f"FATAL: {exe} not built. Run: (cd gfxbench && make CARD={args.card})")
        return 2

    conn = RetroConnection(args.host, PORT)
    await conn.connect(SECRET, timeout=15)
    print(f"connected: {conn.hostname} {conn.os_version}")
    try:
        await conn.command_text(f'EXEC cmd /c md "{dest}"', timeout=30)
        await upload(conn, exe, f"{dest}\\gfxbench.exe")

        if args.with_glide:
            dll = HERE / "out" / f"glide3x_{args.card}_{'voodoo5' if args.card=='h5' else 'voodoo3'}.dll"
            if dll.exists():
                # push next to the exe AND to the system dir so it wins for our app
                await upload(conn, dll, f"{dest}\\glide3x.dll")
                print("  (pushed rebuilt glide3x.dll next to gfxbench.exe)")
            else:
                print(f"  --with-glide: {dll} not built (run ../build-glide.sh); using installed glide3x")

        if args.interactive:
            print("launching interactive harness (fullscreen on the target)...")
            await conn.command_text(f'LAUNCH {dest}\\gfxbench.exe', timeout=30)
            print("  launched. Use SCREENSHOT/UICLICK via the agent to walk modes.")
            return 0

        csv = f"{dest}\\gfxbench.csv"
        print(f"running benchmark sweep ({args.frames} frames/mode)...")
        out = await conn.command_text(
            f'EXEC cmd /c "{dest}\\gfxbench.exe" -bench -frames {args.frames} -csv "{csv}"',
            timeout=600)
        print("---- gfxbench output ----")
        print(out)
        # pull the CSV back
        try:
            data = await conn.command_binary(f"DOWNLOAD {csv}", timeout=60)
            local_csv = HERE / "gfxbench" / f"results_{args.host.replace('.','_')}.csv"
            local_csv.write_bytes(data)
            print(f"\nresults saved: {local_csv}")
        except Exception as e:  # noqa: BLE001
            print(f"(could not download CSV: {e})")
        return 0
    finally:
        await conn.close()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("host", help="agent IP of the Voodoo machine")
    ap.add_argument("--card", default="h5", choices=["h5", "h3"],
                    help="h5=Voodoo4/5 (default), h3=Voodoo3")
    ap.add_argument("--frames", type=int, default=300)
    ap.add_argument("--with-glide", action="store_true",
                    help="also push the rebuilt glide3x.dll from out/")
    ap.add_argument("--interactive", action="store_true",
                    help="LAUNCH the GUI harness instead of the headless sweep")
    ap.add_argument("--dest", default=r"C:\RETRO_AGENT")
    asyncio.run(amain(ap.parse_args()))


if __name__ == "__main__":
    main()
