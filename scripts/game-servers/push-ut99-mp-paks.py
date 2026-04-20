#!/usr/bin/env python3
"""Pre-install popular UT99 multiplayer content onto retro agents — maps,
mod .u packages, textures, music — so they don't download megabytes per
map on every server join.

Share layout at \\\\192.168.1.122\\files\\Game Updates\\UT99-Multiplayer\\
(matches UT99's on-disk directory structure):

    Maps/           *.unr custom maps
    System/         *.u mod packages (MonsterHunt.u, Jailbreak.u, ChaosUT.u, ...)
                    *.int mod localization
                    *.ini config snippets
    Textures/       *.utx
    Sounds/         *.uax
    Music/          *.umx
    MonsterHunt/    (legacy subdir style — older UT99 mods used
                     per-mod subdirs. The push script inspects and copies
                     any subdir that matches a known install target.)

Dropping a new mod into the right subdir is the whole workflow — next run
of this script pushes it to the fleet.

April 2026 snapshot from 333networks UT99 master:
    MH   (MonsterHunt)  4 servers    https://oldunreal.com/download#mh
    CTF  (stock)         3 servers   — shipped with UT99
    BT   (BunnyTrack)    1 server    https://oldunreal.com/download#bt
    DM   (stock)         1 server

Usage:
    cd /path/to/retro-agent
    python3 scripts/game-servers/push-ut99-mp-paks.py 192.168.1.143 [...]

Prerequisites: UT99 installed (C:\\UT\\System\\UnrealTournament.exe exists).
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
SHARE_ROOT = r"Y:\Game Updates\UT99-Multiplayer"

UT_CANDIDATE_PATHS = [
    r"C:\UT",
    r"C:\UnrealTournament",
    r"C:\Games\UT",
    r"C:\Program Files\UnrealTournament",
    r"D:\UT",
]

# Direct content subdirs (match UT99 install layout)
SUBDIRS = ["Maps", "System", "Textures", "Sounds", "Music"]


async def _run(cmd: str, c: RetroConnection, timeout: float = 60.0) -> str:
    try:
        return await c.command_text(cmd, timeout=timeout)
    except Exception as e:
        return f"ERR: {e}"


async def find_ut(c: RetroConnection) -> str | None:
    for path in UT_CANDIDATE_PATHS:
        r = await _run(rf'EXEC dir "{path}\System\UnrealTournament.exe"', c, 10.0)
        if "File Not Found" not in r and "cannot find" not in r.lower() and "Not Found" not in r:
            return path
    return None


async def push_agent(agent_ip: str, secret: str = "retro-agent-secret") -> None:
    print(f"\n=== {agent_ip} ===")
    c = RetroConnection(agent_ip, 9898)
    await c.connect(secret, timeout=15.0)
    try:
        ut = await find_ut(c)
        if not ut:
            print(f"  no UT99 install found (checked {UT_CANDIDATE_PATHS})")
            return
        print(f"  UT99 install: {ut}")

        await _run(rf"NETMAP {SMB_UNC} Y: {SMB_USER} {SMB_PASS}", c, 15.0)

        for sub in SUBDIRS:
            src = rf"{SHARE_ROOT}\{sub}"
            dst = rf"{ut}\{sub}"
            probe = await _run(rf'EXEC dir "{src}\*.*" /b', c, 15.0)
            if not any(ln.strip() for ln in probe.splitlines()
                       if ln.strip() and "File Not Found" not in ln):
                continue
            await _run(rf'EXEC mkdir "{dst}"', c, 10.0)
            r = await _run(
                rf'EXEC xcopy /Y /Q /D "{src}\*.*" "{dst}\\"',
                c, timeout=1200.0,
            )
            msg = next(
                (l.strip() for l in r.splitlines() if "File(s) copied" in l),
                (r.strip().splitlines()[-1][:120] if r.strip() else "?"),
            )
            print(f"  [{sub}] {msg}")

        # Mod-specific subdirs (some 1999-era mods used their own subtree)
        for mod_sub in ("MonsterHunt", "ChaosUT", "Jailbreak"):
            src = rf"{SHARE_ROOT}\{mod_sub}"
            probe = await _run(rf'EXEC dir "{src}\*.*" /b', c, 15.0)
            if not any(ln.strip() for ln in probe.splitlines()
                       if ln.strip() and "File Not Found" not in ln):
                continue
            # UT99 mods extract to the UT root (they have their own
            # System/Maps/Textures/Sounds inside the archive)
            r = await _run(
                rf'EXEC xcopy /Y /Q /E /D "{src}\*" "{ut}\\"',
                c, timeout=1200.0,
            )
            msg = next(
                (l.strip() for l in r.splitlines() if "File(s) copied" in l),
                "?",
            )
            print(f"  [{mod_sub}] {msg}")

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
