#!/usr/bin/env python3
r"""
build_driver.py - build the fxD3D host display driver on a DDK-provisioned box.

Packages the fxD3D source subset (preserving the driver/ d3dhal/ glide-sdk/
layout the SOURCES relative paths need), uploads it over the agent, runs the DDK
`build` remotely, and downloads the resulting fxd3ddd.dll. This is the
"fleet builds its own driver" path - the box provisioned by provision_ddk.py is
the compiler.

Usage:
  python3 build_driver.py <target-ip> [--bld chk|fre] [--dest C:\\build\\fxd3d]
                                      [--out ./out]

Requires: the target already provisioned (provision_ddk.py). The build is the
-DHAVE_DDK path of driver/nt/enable.c; the portable core + glue are the same
files verified on the Linux host.
"""

import argparse
import asyncio
import io
import os
import sys
import zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
_REPO = HERE.parent.parent
FX = _REPO / "scripts" / "3dfx"
sys.path.insert(0, str(_REPO))
from client.retro_protocol import RetroConnection  # noqa: E402

SECRET = os.environ.get("RETRO_AGENT_SECRET", "retro-agent-secret")
PORT = int(os.environ.get("RETRO_AGENT_PORT", "9898"))
UNZIP_JS = _REPO / "provisioning" / "retro_unzip.js"

# The exact source subset the DDK SOURCES file references, packaged with the
# scripts/3dfx-relative layout preserved (driver/nt/SOURCES uses ..\ and ..\..\).
PACKAGE = [
    "driver/nt/SOURCES", "driver/nt/enable.c", "driver/nt/dispdrv.def",
    "driver/ddi_glue.c", "driver/ddi_glue.h",
    "d3dhal/d3dhal_state.c", "d3dhal/d3dhal_tex.c",
    "d3dhal/d3dhal_prim.c", "d3dhal/d3dhal_ddi.c",
    "d3dhal/glidebackend.h", "d3dhal/include/fxd3d.h",
]


def build_zip():
    """Zip the source subset + all glide-sdk headers, paths relative to 3dfx/."""
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w", zipfile.ZIP_DEFLATED) as z:
        for rel in PACKAGE:
            p = FX / rel
            if not p.exists():
                raise SystemExit(f"missing source: {p}")
            z.write(p, rel)
        for h in (FX / "glide-sdk" / "include").glob("*.h"):
            z.write(h, f"glide-sdk/include/{h.name}")
    return buf.getvalue()


async def ex(conn, cmd, timeout=120):
    try:
        return await conn.command_text("EXEC " + cmd, timeout=timeout)
    except Exception as e:  # noqa: BLE001
        return f"__ERR__ {e}"


async def upload(conn, data: bytes, remote: str, label=""):
    status, resp = await conn.send_command(f"UPLOAD {remote}", binary_payload=data, timeout=180)
    if status == 0xFF:
        raise RuntimeError(f"UPLOAD {remote} failed: {resp.decode('ascii','replace')}")
    print(f"  uploaded {label or remote} ({len(data)} bytes)")


async def amain(args):
    dest = args.dest.rstrip("\\")
    conn = RetroConnection(args.host, PORT)
    await conn.connect(SECRET, timeout=15)
    print(f"connected: {conn.hostname} {conn.os_version}")
    try:
        chk = await ex(conn, r'cmd /c if exist C:\DDK\build_fxd3d.bat (echo Y) else (echo N)')
        if "Y" not in chk.upper():
            print("target not provisioned (C:\\DDK\\build_fxd3d.bat missing) - "
                  "run provision_ddk.py first")
            return 2

        # 1. package + upload sources
        print("packaging fxD3D sources...")
        zdata = build_zip()
        await ex(conn, f'cmd /c rd /s /q "{dest}" 2>nul & md "{dest}"', timeout=60)
        await upload(conn, zdata, f"{dest}\\src.zip", "source package")
        if UNZIP_JS.exists():
            await upload(conn, UNZIP_JS.read_bytes(), r"C:\WINDOWS\TEMP\retro_unzip.js", "unzip shim")
        print("extracting sources on target...")
        await ex(conn, f'cscript //nologo C:\\WINDOWS\\TEMP\\retro_unzip.js "{dest}\\src.zip" "{dest}"',
                 timeout=120)

        # 2. build remotely
        print(f"building ({args.bld} WXP) - this runs the DDK on the box...")
        out = await ex(conn, f'cmd /c C:\\DDK\\build_fxd3d.bat "{dest}" {args.bld} 2>&1', timeout=600)
        print("---- remote build output ----")
        print(out[-3000:])
        print("-----------------------------")
        code = None
        for line in out.splitlines():
            if "BUILD_EXIT=" in line:
                code = line.split("BUILD_EXIT=")[-1].strip()
        if code not in ("0", None) and code is not None:
            print(f"build reported BUILD_EXIT={code} (nonzero). See output above.")

        # 3. retrieve outputs
        outdir = Path(args.out); outdir.mkdir(parents=True, exist_ok=True)
        got = []
        for name in ("fxd3ddd.dll", "fxd3dmp.sys"):
            try:
                data = await conn.command_binary(f"DOWNLOAD C:\\build\\out\\{name}", timeout=120)
                (outdir / name).write_bytes(data)
                got.append(f"{name} ({len(data)} bytes)")
            except Exception:  # noqa: BLE001
                pass
        if got:
            print(f"\nretrieved: {', '.join(got)} -> {outdir}/")
            print("Next: stage the driver + fxd3d.inf on the share and install via PnP.")
            return 0
        print("\nno driver binary produced - inspect the build output above.")
        return 1
    finally:
        await conn.close()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("host", help="DDK-provisioned target box IP")
    ap.add_argument("--bld", default="chk", choices=["chk", "fre"])
    ap.add_argument("--dest", default=r"C:\build\fxd3d")
    ap.add_argument("--out", default=str(HERE / "out"))
    asyncio.run(amain(ap.parse_args()))


if __name__ == "__main__":
    main()
