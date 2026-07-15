#!/usr/bin/env python3
r"""
provision_ddk.py - deploy the Windows DDK build toolchain to a fleet box.

Turns a fleet Windows box (XP/2000/2003) into a driver-build machine so the
fleet can build its own drivers (the fxD3D host display driver, etc.) - no local
Windows toolchain needed. Idempotent: skips work already done.

WHAT IT DOES (over the retro-agent protocol):
  1. maps the SMB share,
  2. copies the DDK package ZIP off the share (copy /Y - NOT xcopy, which hangs
     on NETMAP'd SMB on XP) and extracts it to C:\WINDDK\3790 with the JScript
     unzip shim,
  3. drops the ddk_setenv.bat wrapper into C:\DDK,
  4. verifies build.exe runs.

WHAT YOU STAGE FIRST (one-time, like the game ZIPs for onboarding):
  the DDK on the share as a single ZIP:
      <share>\Utility\Retro Automation\DDK\winddk-3790.zip
  extracting to yield  C:\WINDDK\3790\bin\build.exe .
  The Windows Server 2003 DDK (build 3790.1830) is the canonical, freely
  redistributable DDK and builds Windows XP display drivers. See README.md for
  where to get it and how to make the ZIP.

Usage:
  python3 provision_ddk.py <target-ip> [--share \\\\192.168.1.122\\files]
                          [--zip 'Utility\\Retro Automation\\DDK\\winddk-3790.zip']
                          [--basedir C:\\WINDDK\\3790] [--force]
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
UNZIP_JS = _REPO / "provisioning" / "retro_unzip.js"


async def ex(conn, cmd, timeout=120):
    try:
        return await conn.command_text("EXEC " + cmd, timeout=timeout)
    except Exception as e:  # noqa: BLE001
        return f"__ERR__ {e}"


async def exists(conn, path):
    out = await ex(conn, f'cmd /c if exist "{path}" (echo YES) else (echo NO)')
    return "YES" in out.upper()


async def upload(conn, local: Path, remote: str):
    data = local.read_bytes()
    status, resp = await conn.send_command(f"UPLOAD {remote}", binary_payload=data, timeout=120)
    if status == 0xFF:
        raise RuntimeError(f"UPLOAD {remote} failed: {resp.decode('ascii','replace')}")
    print(f"  uploaded {local.name} -> {remote}")


async def amain(args):
    drive = "Z:"
    conn = RetroConnection(args.host, PORT)
    await conn.connect(SECRET, timeout=15)
    print(f"connected: {conn.hostname} {conn.os_version}")
    try:
        build_exe = f"{args.basedir}\\bin\\build.exe"
        if not args.force and await exists(conn, build_exe):
            print(f"DDK already present ({build_exe}) - skipping extract (use --force to redo)")
        else:
            # 1. map share
            print(f"mapping share {args.share} -> {drive}")
            await ex(conn, f'cmd /c net use {drive} /delete /y', timeout=30)
            await ex(conn, f'cmd /c net use {drive} "{args.share}" /persistent:no', timeout=40)
            src = f"{drive}\\{args.zip}"
            if not await exists(conn, src):
                print(f"FATAL: DDK package not on share at {src}\n"
                      f"       Stage it first (see README.md).")
                return 3
            # 2. stage the unzip shim + copy the DDK zip local, then extract
            await ex(conn, r'cmd /c md C:\WINDDK', timeout=30)
            if UNZIP_JS.exists():
                await upload(conn, UNZIP_JS, r"C:\WINDOWS\TEMP\retro_unzip.js")
            print("copying DDK package off the share (this is large)...")
            await ex(conn, f'cmd /c copy /Y "{src}" C:\\WINDDK\\winddk.zip', timeout=600)
            print("extracting DDK...")
            await ex(conn,
                     r'cscript //nologo C:\WINDOWS\TEMP\retro_unzip.js '
                     r'C:\WINDDK\winddk.zip C:\WINDDK', timeout=600)
            await ex(conn, r'cmd /c del /q C:\WINDDK\winddk.zip', timeout=30)
            if not await exists(conn, build_exe):
                print(f"FATAL: extract did not yield {build_exe}. "
                      f"Check the ZIP layout (see README.md).")
                return 4

        # 3. drop the env wrapper
        await ex(conn, r'cmd /c md C:\DDK', timeout=30)
        await upload(conn, HERE / "ddk_setenv.bat", r"C:\DDK\ddk_setenv.bat")
        await upload(conn, HERE / "build_fxd3d.bat", r"C:\DDK\build_fxd3d.bat")

        # 4. verify build.exe runs
        print("verifying toolchain...")
        out = await ex(conn, f'cmd /c "{build_exe}" -? 2>&1', timeout=60)
        ok = ("usage" in out.lower()) or ("build" in out.lower() and "options" in out.lower())
        print("---- build.exe -? ----")
        print(out[:400])
        print("----------------------")
        if ok:
            print(f"\nDDK toolchain provisioned on {args.host}. "
                  f"Build drivers with:  python3 build_driver.py {args.host}")
            return 0
        print("WARN: build.exe did not respond as expected; verify the DDK package.")
        return 5
    finally:
        await conn.close()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("host", help="target box IP (an XP/2000/2003 box to build on)")
    ap.add_argument("--share", default=r"\\192.168.1.122\files")
    ap.add_argument("--zip", default=r"Utility\Retro Automation\DDK\winddk-3790.zip")
    ap.add_argument("--basedir", default=r"C:\WINDDK\3790")
    ap.add_argument("--force", action="store_true", help="re-extract even if present")
    asyncio.run(amain(ap.parse_args()))


if __name__ == "__main__":
    main()
