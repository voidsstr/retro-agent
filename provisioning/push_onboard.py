#!/usr/bin/env python3
"""
push_onboard.py - publish the onboarding control files to the share.

The agent's onboard_thread reads onboard.cmd + retro_unzip.js (+ optional
retro_theme.reg) from  <share>\\Utility\\Retro Automation\\Onboard\\ . This script
stages them there THROUGH an online fleet agent that has the share mapped with
write access (the Z: drive) - the same "publish via a fleet agent" method used
for the binaries (see CLAUDE.md). It does NOT push the game ZIPs (those are large;
drop them into  <share>\\Games\\  directly).

Usage:
  python3 push_onboard.py <online-agent-ip> [--theme-reg path/to/retro_theme.reg]

Requires the target agent to be online and to have Z: mapped writable.
"""

import argparse
import asyncio
import os
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
_REPO = HERE.parent
sys.path.insert(0, str(_REPO))
from client.retro_protocol import RetroConnection  # noqa: E402

SECRET = os.environ.get("RETRO_AGENT_SECRET", "retro-agent-secret")
PORT = int(os.environ.get("RETRO_AGENT_PORT", "9898"))
SHARE_ONBOARD = r"Z:\Utility\Retro Automation\Onboard"
STAGE = r"C:\RETRO_AGENT\stage"


async def push_file(conn, local: Path, remote_name: str):
    data = local.read_bytes()
    staged = f"{STAGE}\\{remote_name}"
    status, resp = await conn.send_command(f"UPLOAD {staged}", binary_payload=data, timeout=120)
    if status == 0xFF:
        raise RuntimeError(f"UPLOAD {remote_name} failed: {resp.decode('ascii','replace')}")
    # copy from local stage to the share (copy /Y works over NETMAP'd SMB; xcopy does not)
    out = await conn.command_text(f'EXEC cmd /c copy /Y "{staged}" "{SHARE_ONBOARD}\\{remote_name}"',
                                  timeout=120)
    print(f"  {remote_name}: {out.strip()[:80]}")


async def amain(args):
    conn = RetroConnection(args.host, PORT)
    await conn.connect(SECRET, timeout=15)
    print(f"connected: {conn.hostname} {conn.os_version}")
    try:
        await conn.command_text(f'EXEC cmd /c md "{STAGE}"', timeout=30)
        await conn.command_text(f'EXEC cmd /c md "{SHARE_ONBOARD}"', timeout=30)
        files = [(HERE / "onboard.cmd", "onboard.cmd"),
                 (HERE / "onboard_9x.bat", "onboard_9x.bat"),
                 (HERE / "retro_unzip.js", "retro_unzip.js")]
        if args.theme_reg:
            files.append((Path(args.theme_reg), "retro_theme.reg"))
        for local, name in files:
            if not local.exists():
                print(f"  SKIP {name} (missing: {local})")
                continue
            await push_file(conn, local, name)
        # verify
        out = await conn.command_text(f'EXEC cmd /c dir "{SHARE_ONBOARD}"', timeout=60)
        print("\nshare Onboard dir now:\n" + out)
    finally:
        await conn.close()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("host", help="online agent IP with Z: mapped writable")
    ap.add_argument("--theme-reg", help="path to retro_theme.reg to also publish")
    args = ap.parse_args()
    asyncio.run(amain(args))


if __name__ == "__main__":
    main()
