#!/usr/bin/env python3
r"""
push_3dfxctl.py - publish the 3dfx Control Panel binary to the fleet share.

Stages 3dfxctl.exe to
    \\192.168.1.122\files\Utility\Retro Automation\3dfx\3dfxctl.exe
THROUGH an online fleet agent that has the share mapped writable as Z: (the same
"publish via a fleet agent" method as provisioning/push_onboard.py). From the
share any box can pull it, or deploy it straight to a box with --deploy.

Usage:
  python3 push_3dfxctl.py <online-agent-ip>            # publish to the share
  python3 push_3dfxctl.py <voodoo-box-ip> --deploy     # ALSO copy onto that box
                                                        # (C:\RETRO_AGENT + Start Menu shortcut)

Requires the target agent online. --deploy works even if Z: is down (uploads the
local binary directly). Publishing to the share needs Z: mapped writable.
"""
import argparse, asyncio, os, sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
_REPO = HERE.parents[2]                      # scripts/3dfx/3dfxctl -> repo root
sys.path.insert(0, str(_REPO))
from client.retro_protocol import RetroConnection  # noqa: E402

SECRET = os.environ.get("RETRO_AGENT_SECRET", "retro-agent-secret")
PORT   = int(os.environ.get("RETRO_AGENT_PORT", "9898"))
EXE    = HERE / "3dfxctl.exe"
SHARE_DIR = r"Z:\Utility\Retro Automation\3dfx"
STAGE  = r"C:\RETRO_AGENT\stage"
BOXDIR = r"C:\RETRO_AGENT"


async def txt(conn, cmd, t=120):
    return await conn.command_text(cmd, timeout=t)


async def amain(args):
    if not EXE.exists():
        sys.exit(f"missing {EXE} - run `make` first")
    data = EXE.read_bytes()
    conn = RetroConnection(args.host, PORT)
    await conn.connect(SECRET, timeout=15)
    print(f"connected: {conn.hostname} {conn.os_version}  ({len(data)} bytes)")
    try:
        await txt(conn, f'EXEC cmd /c md "{STAGE}" 2>nul')
        status, resp = await conn.send_command(f"UPLOAD {STAGE}\\3dfxctl.exe",
                                               binary_payload=data, timeout=120)
        if status == 0xFF:
            sys.exit("UPLOAD failed: " + resp.decode("ascii", "replace"))
        print("uploaded to stage.")

        # publish to the share (best effort - reconnect Z: first)
        await txt(conn, r'EXEC cmd /c net use Z: \\192.168.1.122\files /persistent:yes 2>nul')
        zc = await txt(conn, r'EXEC cmd /c if exist Z:\ (echo ZOK) else (echo ZDOWN)')
        if "ZOK" in zc:
            await txt(conn, f'EXEC cmd /c md "{SHARE_DIR}" 2>nul')
            out = await txt(conn, f'EXEC cmd /c copy /Y "{STAGE}\\3dfxctl.exe" "{SHARE_DIR}\\3dfxctl.exe"')
            print(f"published to share: {out.strip()[:80]}")
            print(await txt(conn, f'EXEC cmd /c dir "{SHARE_DIR}"'))
        else:
            print("share Z: is DOWN - skipped share publish (rerun when \\\\192.168.1.122 is online).")

        if args.deploy:
            out = await txt(conn, f'EXEC cmd /c copy /Y "{STAGE}\\3dfxctl.exe" "{BOXDIR}\\3dfxctl.exe"')
            print(f"deployed to box: {out.strip()[:80]}")
            # Start Menu shortcut (All Users) so users can find it.  Upload the
            # VBS as a FILE -- an echo'd one-liner breaks on the '&' path joins
            # (cmd treats them as command separators).
            vbs = ('Set s = CreateObject("WScript.Shell")\r\n'
                   'Set l = s.CreateShortcut(s.SpecialFolders("AllUsersPrograms") & "\\3dfx Control Panel.lnk")\r\n'
                   'l.TargetPath = "C:\\RETRO_AGENT\\3dfxctl.exe"\r\n'
                   'l.Save\r\n')
            await conn.send_command(f"UPLOAD {STAGE}\\mk3dfxlnk.vbs",
                                    binary_payload=vbs.encode("latin-1"), timeout=30)
            await txt(conn, f'EXEC cmd /c cscript //nologo "{STAGE}\\mk3dfxlnk.vbs" & del "{STAGE}\\mk3dfxlnk.vbs"')
            chk = await txt(conn, r'EXEC cmd /c if exist "%ALLUSERSPROFILE%\Start Menu\Programs\3dfx Control Panel.lnk" (echo LNKOK) else (echo LNKMISS)')
            print("Start Menu shortcut:", "created" if "LNKOK" in chk else "FAILED (" + chk.strip()[:40] + ")")
    finally:
        await conn.close()


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("host", help="online agent IP (with Z: mapped for share publish)")
    ap.add_argument("--deploy", action="store_true", help="also copy onto that box + Start Menu shortcut")
    asyncio.run(amain(ap.parse_args()))
