#!/usr/bin/env python3
"""Push the OldUnreal 469e UT99 WindowsXP client patch to one or more retro
agents and run it unattended.

The patch ZIP is staged on the SMB share at:
    \\\\192.168.1.122\\files\\Game Updates\\UT99-Multiplayer\\
        OldUnreal-UTPatch469e-WindowsXP-x86.zip (103 MB)

For each target agent this script:
  1. Maps the share as drive Y: via NETMAP
  2. Copies the zip to C:\\WINDOWS\\TEMP\\
  3. Uses the agent's built-in zip extraction via a PowerShell one-liner
     (XP has Expand-Archive starting with PowerShell 5, or we fall back to
     Windows Compressed Folders via Shell.Application in WSH)
  4. Runs the extracted setup executable with silent flags so it overlays onto
     C:\\UT\\ in-place.

Usage:
    cd /path/to/retro-agent
    python3 scripts/game-servers/push-ut99-xp-patch.py 192.168.1.143 [...]

Cross-compatibility note: 469e clients can join 436/451/469 servers and
vice versa, so this upgrade is non-disruptive for the fleet.
"""

import asyncio
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "..", "..", "client"))
try:
    from retro_protocol import RetroConnection  # type: ignore
except ImportError:
    # Fallback: source copy in nsc-assistant
    sys.path.insert(0, "/home/voidsstr/development/nsc-assistant")
    from shared.retro_protocol import RetroConnection  # type: ignore


SMB_UNC = r"\\192.168.1.122\files"
SMB_USER = "admin"
SMB_PASS = "password"
PATCH_NAME = "OldUnreal-UTPatch469e-WindowsXP-x86.zip"
PATCH_SMB_PATH = rf"Y:\Game Updates\UT99-Multiplayer\{PATCH_NAME}"
PATCH_TEMP_ZIP = rf"C:\WINDOWS\TEMP\{PATCH_NAME}"
PATCH_EXTRACT_DIR = r"C:\WINDOWS\TEMP\ut99_469e"
UT99_INSTALL_DIR = r"C:\UT"


async def _run(cmd: str, conn: RetroConnection, timeout: float = 60.0) -> str:
    """Wrap command_text with a clear error prefix."""
    try:
        return await conn.command_text(cmd, timeout=timeout)
    except Exception as e:
        return f"ERR: {e}"


async def apply_patch(agent_ip: str, secret: str = "retro-agent-secret") -> None:
    print(f"\n=== {agent_ip} ===")
    c = RetroConnection(agent_ip, 9898)
    await c.connect(secret, timeout=15.0)
    try:
        # Verify UT is installed
        probe = await _run(rf'EXEC dir "{UT99_INSTALL_DIR}\System"', c, 15.0)
        if "File Not Found" in probe or "cannot find" in probe.lower():
            print(f"  {UT99_INSTALL_DIR}\\System not found — skipping (UT99 not installed)")
            return

        # Map share
        r = await _run(
            rf'NETMAP {SMB_UNC} Y: {SMB_USER} {SMB_PASS}', c, 15.0
        )
        print(f"  map: {r[:80].strip()}")

        # Copy patch locally
        print("  copying patch to TEMP (...100 MB, may take ~30 s)")
        r = await _run(
            rf'EXEC copy /Y "{PATCH_SMB_PATH}" "{PATCH_TEMP_ZIP}"',
            c, timeout=180.0,
        )
        if "copied" not in r.lower():
            print(f"  copy failed: {r[:200]}")
            return

        # Extract via PowerShell Expand-Archive (XP SP3 with PS 2.0 lacks it,
        # so fall back to Shell.Application COM object which is always present)
        print("  extracting patch…")
        extract_cmd = (
            rf'EXEC cmd /c mkdir "{PATCH_EXTRACT_DIR}" 2>nul & '
            rf'cscript //Nologo //E:JScript "C:\WINDOWS\TEMP\unzip.js" '
            rf'"{PATCH_TEMP_ZIP}" "{PATCH_EXTRACT_DIR}"'
        )
        # Stage a tiny JScript unzipper (Shell.Application)
        jscript = (
            b"var sh=new ActiveXObject('Shell.Application');"
            b"var src=sh.NameSpace(WScript.Arguments(0));"
            b"var dst=sh.NameSpace(WScript.Arguments(1));"
            b"dst.CopyHere(src.Items(), 4|16);\r\n"
        )
        await c.send_command(r"UPLOAD C:\WINDOWS\TEMP\unzip.js",
                             binary_payload=jscript)
        r = await _run(extract_cmd, c, timeout=600.0)

        # The zip unpacks as a setup folder — find the installer .exe
        r = await _run(rf'EXEC dir /b /s "{PATCH_EXTRACT_DIR}\\*.exe"', c, 30.0)
        setup = next(
            (ln.strip() for ln in r.splitlines()
             if ln.strip().lower().endswith(".exe") and "setup" in ln.lower()),
            None,
        )
        if not setup:
            # The OldUnreal WindowsXP-x86 zip unpacks to a directory of files
            # that should overlay onto the UT99 install root. Copy them in
            # place — the patch ships with the System subdir structure intact.
            print("  no setup.exe — overlaying files onto %s" % UT99_INSTALL_DIR)
            r = await _run(
                rf'EXEC xcopy /E /Y /Q "{PATCH_EXTRACT_DIR}\\*" "{UT99_INSTALL_DIR}\\"',
                c, timeout=300.0,
            )
            print(f"  xcopy: {r.strip().splitlines()[-1][:100] if r.strip() else 'ok'}")
        else:
            print(f"  running {setup}")
            # OldUnreal installer supports /S for silent
            status, resp = await c.send_command(f'LAUNCH "{setup}" /S')
            print(f"  launch: {resp[:120]!r}")

        print(f"  patch applied to {UT99_INSTALL_DIR}")
    finally:
        await c.close()


async def main() -> None:
    if len(sys.argv) < 2:
        print(f"Usage: {sys.argv[0]} <agent-ip> [<agent-ip> ...]", file=sys.stderr)
        sys.exit(1)
    for ip in sys.argv[1:]:
        try:
            await apply_patch(ip)
        except Exception as e:
            print(f"  {ip}: {e}")


if __name__ == "__main__":
    asyncio.run(main())
