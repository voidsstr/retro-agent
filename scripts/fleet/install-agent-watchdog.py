#!/usr/bin/env python3
"""
install-agent-watchdog.py - make a fleet box restart its own retro agent.

THE GAP THIS CLOSES
-------------------
CLAUDE.md states it plainly: nothing supervises the agent. The
`HKLM\\...\\Run\\RetroAgent` value fires only at logon, so any crash takes the
machine off the network until somebody walks to it. That has cost this project
real time repeatedly - a Win9x box twice in one day reading a log, and the
Voodoo 5 6000 host three times in one afternoon, because the AmigaMerlin driver
wedges under repeated Glide context creation and takes the agent's process down
with it.

The wedge itself needs a reboot to clear. But a reboot can be issued REMOTELY
as soon as the agent is answering - so restoring the agent automatically is
what turns "walk to the machine" into "wait three minutes", and it is the
difference between a benchmark sweep that can run unattended and one that
cannot.

WHY A Run-KEY LOOP AND NOT A SCHEDULED TASK
-------------------------------------------
- A scheduled task on XP needs `/ru <user> /rp <password>` to run as the
  console account. That puts the password in argv, in the task store, and in
  every transcript of the command. The account password here is a documented
  fleet convention rather than a secret, but writing it into argv when nothing
  requires it is still gratuitous.
- The agent must run IN THE CONSOLE SESSION: it launches fullscreen games and
  drives UI automation. A loop started from the Run key is already there, with
  the right desktop and the right token, and needs no credentials at all.
- `schtasks /ru SYSTEM` avoids the password but lands the agent in a SYSTEM
  context, which is the wrong place for something that has to open a Glide
  fullscreen window.

The loop costs one `tasklist` every 30 seconds, which is free on these boxes
next to the games they exist to run.

    python3 install-agent-watchdog.py <ip> [--remove] [--check]
"""

import argparse
import asyncio
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from client.retro_protocol import RetroConnection, RetroProtocolError  # noqa: E402

SECRET = "retro-agent-secret"
WD_PATH = r"C:\RETRO_AGENT\agentwd.cmd"
RUN_KEY = r"Software\Microsoft\Windows\CurrentVersion\Run"
RUN_VAL = "RetroAgentWD"

# `start ""` so the loop does not block on the agent, and a ping-based sleep
# because XP's shell has no `timeout` command.
WATCHDOG = "\r\n".join([
    "@echo off",
    "rem retro agent watchdog - restarts the agent if its process disappears.",
    "rem Installed by scripts/fleet/install-agent-watchdog.py. See that file",
    "rem for why this is a Run-key loop rather than a scheduled task.",
    ":loop",
    'tasklist | find /i "retro_agent.exe" >nul',
    'if errorlevel 1 (',
    '  echo %DATE% %TIME% agent not running - starting it >> C:\\RETRO_AGENT\\agentwd.log',
    '  start "" C:\\RETRO_AGENT\\retro_agent.exe',
    ')',
    "ping -n 31 127.0.0.1 >nul",
    "goto loop",
    "",
])


async def cmd(ip, c, timeout=90):
    con = RetroConnection(ip, 9898)
    await con.connect(SECRET, timeout=20.0)
    try:
        st, d = await con.send_command(c, timeout=timeout)
        return st, d.decode("ascii", errors="replace")
    finally:
        await con.close()


async def upload(ip, remote, text):
    con = RetroConnection(ip, 9898)
    await con.connect(SECRET, timeout=20.0)
    try:
        st, d = await con.send_command(
            f"UPLOAD {remote}", binary_payload=text.encode("ascii"), timeout=90)
        if st != 0:
            raise RetroProtocolError(d.decode("ascii", errors="replace"))
    finally:
        await con.close()


async def check(ip):
    st, run = await cmd(ip, f"REGREAD HKLM {RUN_KEY}")
    installed = RUN_VAL in run
    st, ps = await cmd(ip, 'EXEC cmd /c tasklist | find /i "cmd.exe"')
    st, ex = await cmd(ip, f'EXEC cmd /c if exist {WD_PATH} (echo yes) else (echo no)')
    st, lg = await cmd(ip, r'EXEC cmd /c if exist C:\RETRO_AGENT\agentwd.log '
                           r'(type C:\RETRO_AGENT\agentwd.log) else (echo "(no restarts logged)")')
    print(f"  Run value {RUN_VAL}: {'present' if installed else 'ABSENT'}")
    print(f"  {WD_PATH}: {ex.strip()}")
    print(f"  watchdog log:\n    " + "\n    ".join(
        l for l in lg.replace("\r", "").splitlines()[-6:] if l.strip()))
    return installed


async def install(ip):
    await upload(ip, WD_PATH, WATCHDOG)
    st, out = await cmd(ip, f"REGWRITE HKLM {RUN_KEY} {RUN_VAL} REG_SZ {WD_PATH}")
    # Never trust the OK: REGWRITE answers OK for a write that made a subkey.
    st, run = await cmd(ip, f"REGREAD HKLM {RUN_KEY}")
    if RUN_VAL not in run:
        print("FAILED: the Run value did not stick")
        return 2
    # Start it now rather than waiting for the next logon.
    await cmd(ip, f'LAUNCH cmd /c start "" {WD_PATH}')
    print(f"  installed and started: {WD_PATH}")
    print(f"  Run value {RUN_VAL} set (survives reboots via auto-login)")
    st, ex = await cmd(ip, f'EXEC cmd /c if exist {WD_PATH} (echo present) else (echo MISSING)')
    print(f"  script on disk: {ex.strip()}")
    return 0


async def remove(ip):
    await cmd(ip, f"REGDELETE HKLM {RUN_KEY}\\{RUN_VAL}")
    st, run = await cmd(ip, f"REGREAD HKLM {RUN_KEY}")
    print(f"  Run value {RUN_VAL}: "
          f"{'STILL PRESENT' if RUN_VAL in run else 'removed'}")
    print("  note: the running loop keeps going until the next reboot or a "
          "taskkill of its cmd.exe")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("ip")
    ap.add_argument("--remove", action="store_true")
    ap.add_argument("--check", action="store_true")
    a = ap.parse_args()
    if a.check:
        raise SystemExit(asyncio.run(check(a.ip)) is False)
    if a.remove:
        raise SystemExit(asyncio.run(remove(a.ip)))
    raise SystemExit(asyncio.run(install(a.ip)))


if __name__ == "__main__":
    main()
