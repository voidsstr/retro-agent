#!/usr/bin/env python3
"""Pre-install popular UT2004 multiplayer content onto retro agents so they
don't have to download 100+ MB of maps/textures every time they join a server.

The bundle is staged on the SMB share at:
    \\\\192.168.1.122\\files\\Game Updates\\UT2004-Multiplayer\\
with one subdir per UT2004 content type (matches the on-disk layout):
    Maps/            *.ut2 — custom and bonus-pack maps
    Textures/        *.utx — texture packages referenced by maps
    StaticMeshes/    *.usx
    Animations/      *.ukx
    Sounds/          *.uax
    Music/           *.ogg
    Speech/          *.trs
    System/          *.u mutator/mod packages, *.ini configs

Initial content (April 2026):
  * Epic Bonus Pack 2 maps (AS/CTF/DM-BP2-*) and their dependency packages —
    these are the maps that show up in public server map rotations and are
    the biggest auto-download offenders.

Expand the bundle by dropping more files into the matching share subdir.
The push script mirrors all of them onto each target agent's C:\\UT2004\\.

Usage:
    cd /path/to/retro-agent
    python3 scripts/game-servers/push-ut2004-mp-paks.py 192.168.1.143 [...]

Prerequisites: UT2004 must be installed on the retro machine. The script
checks for System\\UT2004.exe and skips any agent where it isn't found.
"""

import asyncio
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "..", "..", "client"))
try:
    from retro_protocol import RetroConnection  # type: ignore
except ImportError:
    sys.path.insert(0, "/home/voidsstr/development/nsc-assistant")
    from shared.retro_protocol import RetroConnection  # type: ignore


SMB_UNC = r"\\192.168.1.122\files"
SMB_USER = "admin"
SMB_PASS = "password"
SHARE_ROOT = r"Y:\Game Updates\UT2004-Multiplayer"

UT_CANDIDATE_PATHS = [
    r"C:\UT2004",
    r"C:\Program Files\UT2004",
    r"C:\Games\UT2004",
    r"D:\UT2004",
]

# (share-subdir, target-subdir under the UT2004 install)
SUBDIRS = [
    "Maps",
    "Textures",
    "StaticMeshes",
    "Animations",
    "Sounds",
    "Music",
    "Speech",
    "System",
]


async def _run(cmd: str, c: RetroConnection, timeout: float = 60.0) -> str:
    try:
        return await c.command_text(cmd, timeout=timeout)
    except Exception as e:
        return f"ERR: {e}"


async def find_ut2004(c: RetroConnection) -> str | None:
    for path in UT_CANDIDATE_PATHS:
        r = await _run(rf'EXEC dir "{path}\System\UT2004.exe"', c, 10.0)
        if "File Not Found" not in r and "cannot find" not in r.lower() and "Not Found" not in r:
            return path
    return None


async def push_agent(agent_ip: str, secret: str = "retro-agent-secret") -> None:
    print(f"\n=== {agent_ip} ===")
    c = RetroConnection(agent_ip, 9898)
    await c.connect(secret, timeout=15.0)
    try:
        ut = await find_ut2004(c)
        if not ut:
            print(f"  no UT2004 install found (checked {UT_CANDIDATE_PATHS})")
            return
        print(f"  UT2004 install: {ut}")

        await _run(rf"NETMAP {SMB_UNC} Y: {SMB_USER} {SMB_PASS}", c, 15.0)

        for sub in SUBDIRS:
            src = rf"{SHARE_ROOT}\{sub}"
            dst = rf"{ut}\{sub}"
            # Check if share subdir has any files to copy (skip noise from empties)
            probe = await _run(rf'EXEC dir "{src}\*.*" /b', c, 15.0)
            if not any(ln.strip() for ln in probe.splitlines()
                       if ln.strip() and "File Not Found" not in ln):
                continue
            await _run(rf'EXEC mkdir "{dst}"', c, 10.0)
            r = await _run(
                rf'EXEC xcopy /Y /Q /D "{src}\*.*" "{dst}\\"',
                c, timeout=1800.0,
            )
            msg = next(
                (l.strip() for l in r.splitlines() if "File(s) copied" in l),
                (r.strip().splitlines()[-1][:120] if r.strip() else "?"),
            )
            print(f"  [{sub}] {msg}")
        print(f"  done — {ut} populated")
    finally:
        await c.close()


async def main() -> None:
    if len(sys.argv) < 2:
        print(f"Usage: {sys.argv[0]} <agent-ip> [<agent-ip> ...]", file=sys.stderr)
        sys.exit(1)
    for ip in sys.argv[1:]:
        try:
            await push_agent(ip)
        except Exception as e:
            print(f"  {ip}: {e}")


if __name__ == "__main__":
    asyncio.run(main())
