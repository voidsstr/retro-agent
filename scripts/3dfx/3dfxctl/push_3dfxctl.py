#!/usr/bin/env python3
r"""
push_3dfxctl.py - put the 3dfx Control Panel on a box, and prove it landed.

    python3 push_3dfxctl.py <ip> --deploy            # install on that box (+ share copy)
    python3 push_3dfxctl.py <ip> --deploy --no-share # install only
    python3 push_3dfxctl.py <ip>                     # share copy only (via that box's Z:)
    python3 push_3dfxctl.py <ip> --report            # just run the panel's /report there

--deploy, through the box's retro agent:
  1. refuses if a 3dfxctl.exe is running there (it may be holding a paced,
     temporary display mode; close it on the box first - never kill it);
  2. UPLOADs the exe to C:\RETRO_AGENT\stage\, copies it to
     C:\RETRO_AGENT\3dfxctl.exe, and DOWNLOADs it back to compare md5 - the
     post-condition, not the copy's return;
  3. makes "3dfx Control Panel" shortcuts in the All Users Start Menu and on
     the All Users desktop (icon from the exe) and checks both files exist.
     NB: GAMESYNC sweeps every desktop .lnk it did not write itself (it keeps
     only the game icons, "Retro Agent" and "Retro Chat"), so the DESKTOP
     shortcut lasts until the next GAMESYNC run; the Start Menu one stays;
  4. runs `3dfxctl.exe /report` there and prints what the panel sees (the
     driver stack, every setting, every override) - DOWNLOAD, never `type`.

Nothing here changes a display mode or a driver setting.
"""
import argparse
import asyncio
import hashlib
import json
import os
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
_REPO = HERE.parents[2]                      # scripts/3dfx/3dfxctl -> repo root
sys.path.insert(0, str(_REPO))
from client.retro_protocol import RetroConnection  # noqa: E402

SECRET = os.environ.get("RETRO_AGENT_SECRET", "retro-agent-secret")
PORT = int(os.environ.get("RETRO_AGENT_PORT", "9898"))
EXE = HERE / "3dfxctl.exe"
SHARE_DIR = r"Z:\Utility\Retro Automation\3dfx"
STAGE = r"C:\RETRO_AGENT\stage"
BOXDIR = r"C:\RETRO_AGENT"
TARGET = BOXDIR + r"\3dfxctl.exe"
REPORT = BOXDIR + r"\3dfxctl-report.txt"
LNK_NAME = "3dfx Control Panel"


async def txt(conn, cmd, t=120):
    return await conn.command_text(cmd, timeout=t)


async def panel_running(conn):
    procs = await txt(conn, "PROCLIST")
    try:
        data = json.loads(procs)
        rows = data.get("processes", data) if isinstance(data, dict) else data
        return [p for p in rows if str(p.get("name", "")).lower() == "3dfxctl.exe"]
    except (ValueError, AttributeError):
        return ["3dfxctl.exe" in procs.lower()] if "3dfxctl.exe" in procs.lower() else []


async def report(conn):
    await conn.send_command(f'EXEC cmd /c del "{REPORT}" 2>nul', timeout=30)
    # unquoted: EXEC runs it under cmd /c, whose quote stripping breaks a
    # line that starts and ends with a quote (neither path has a space)
    await txt(conn, f"EXEC {TARGET} /report {REPORT}", t=60)
    data = await conn.command_binary(f"DOWNLOAD {REPORT}", timeout=60)
    print("---- 3dfxctl /report ----")
    print(data.decode("latin-1", "replace"))


async def amain(args):
    if not EXE.exists():
        sys.exit(f"missing {EXE} - run `make` first")
    data = EXE.read_bytes()
    md5 = hashlib.md5(data).hexdigest()
    conn = RetroConnection(args.host, PORT)
    await conn.connect(SECRET, timeout=15)
    print(f"connected: {conn.hostname} {conn.os_version}  (3dfxctl.exe {len(data)} bytes, md5 {md5})")
    try:
        if args.report:
            await report(conn)
            return
        await txt(conn, f'EXEC cmd /c md "{STAGE}" 2>nul')
        status, resp = await conn.send_command(f"UPLOAD {STAGE}\\3dfxctl.exe",
                                               binary_payload=data, timeout=120)
        if status == 0xFF:
            sys.exit("UPLOAD failed: " + resp.decode("ascii", "replace"))
        print("uploaded to stage.")

        if not args.no_share:
            await txt(conn, r'EXEC cmd /c net use Z: \\192.168.1.122\files /persistent:yes 2>nul')
            zc = await txt(conn, r'EXEC cmd /c if exist Z:\ (echo ZOK) else (echo ZDOWN)')
            if "ZOK" in zc:
                await txt(conn, f'EXEC cmd /c md "{SHARE_DIR}" 2>nul')
                await txt(conn, f'EXEC cmd /c copy /Y "{STAGE}\\3dfxctl.exe" "{SHARE_DIR}\\3dfxctl.exe"')
                d = await txt(conn, f'EXEC cmd /c dir "{SHARE_DIR}\\3dfxctl.exe"')
                ok = f"{len(data):,}" in d or str(len(data)) in d
                print(f"share copy: {'OK - size matches' if ok else 'NOT VERIFIED'}\n{d.strip()}")
            else:
                print("share Z: is down - share copy skipped (rerun with the NAS online).")

        if args.deploy:
            running = await panel_running(conn)
            if running:
                sys.exit("a 3dfxctl.exe is running on the box - close the panel there first "
                         "(it may hold a paced, temporary display mode; do not kill it)")
            await txt(conn, f'EXEC cmd /c copy /Y "{STAGE}\\3dfxctl.exe" "{TARGET}"')
            back = await conn.command_binary(f"DOWNLOAD {TARGET}", timeout=120)
            got = hashlib.md5(back).hexdigest()
            if got != md5:
                sys.exit(f"{TARGET} on the box has md5 {got}, not {md5} - NOT deployed")
            print(f"deployed: {TARGET} md5 {got} (read back)")
            # the shortcuts: a VBS uploaded as a FILE - an echo'd one-liner breaks on '&'
            vbs = ('Set s = CreateObject("WScript.Shell")\r\n'
                   'For Each f In Array("AllUsersPrograms", "AllUsersDesktop")\r\n'
                   f'  Set l = s.CreateShortcut(s.SpecialFolders(f) & "\\{LNK_NAME}.lnk")\r\n'
                   f'  l.TargetPath = "{TARGET}"\r\n'
                   f'  l.WorkingDirectory = "{BOXDIR}"\r\n'
                   f'  l.IconLocation = "{TARGET},0"\r\n'
                   '  l.Description = "3dfx Control Panel - vsync, SLI/AA, gamma, 2D, refresh, clock"\r\n'
                   '  l.Save\r\n'
                   'Next\r\n')
            await conn.send_command(f"UPLOAD {STAGE}\\mk3dfxlnk.vbs",
                                    binary_payload=vbs.encode("latin-1"), timeout=30)
            await txt(conn, f'EXEC cmd /c cscript //nologo "{STAGE}\\mk3dfxlnk.vbs" & '
                            f'del "{STAGE}\\mk3dfxlnk.vbs"')
            for where in (r"%ALLUSERSPROFILE%\Start Menu\Programs", r"%ALLUSERSPROFILE%\Desktop"):
                chk = await txt(conn, f'EXEC cmd /c if exist "{where}\\{LNK_NAME}.lnk" '
                                      f'(echo LNKOK) else (echo LNKMISS)')
                print(f"shortcut {where}: {'present' if 'LNKOK' in chk else 'MISSING'}")
            await report(conn)
    finally:
        await conn.close()


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("host", help="the box's IP (the agent must answer)")
    ap.add_argument("--deploy", action="store_true", help="install on that box")
    ap.add_argument("--no-share", action="store_true", help="skip the share copy")
    ap.add_argument("--report", action="store_true", help="only run the panel's /report there")
    asyncio.run(amain(ap.parse_args()))
