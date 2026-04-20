#!/usr/bin/env python3
"""Pre-install popular Quake 2 multiplayer content onto retro agents.

Q2's mod layout is one subdir per mod off the game root; each subdir looks
like baseq2/ (gamex86.dll + pak*.pak + maps/*.bsp). Common mods active on
public servers in 2026:
    ctf/         Threewave CTF (shipped with Q2 v3.20; ensure pak0.pak updated)
    rogue/       Ground Zero (expansion)
    xatrix/      Reckoning (expansion)
    aq2/         Action Quake 2 (if pulled in)
    rocketarena/ Rocket Arena 2 (if pulled in)

Share layout at \\\\192.168.1.122\\files\\Game Updates\\Q2-Multiplayer\\
mirrors the on-disk structure: one subdir per mod, contents copied
verbatim into the matching subdir under the Q2 install.

Usage:
    cd /path/to/retro-agent
    python3 scripts/game-servers/push-q2-mp-paks.py 192.168.1.143 [...]

Prerequisites: Q2 installed (quake2.exe + baseq2/pak0.pak present).
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
SHARE_ROOT = r"Y:\Game Updates\Q2-Multiplayer"

Q2_CANDIDATE_PATHS = [
    r"C:\QUAKE2",
    r"C:\Quake2",
    r"C:\Games\Quake2",
    r"C:\Program Files\Quake II",
    r"D:\QUAKE2",
]

# Mods — each maps share/<mod>/<stuff> -> Q2/<mod>/<stuff>
MODS = ["baseq2", "ctf", "rogue", "xatrix", "aq2", "rocketarena"]


async def _run(cmd: str, c: RetroConnection, timeout: float = 60.0) -> str:
    try:
        return await c.command_text(cmd, timeout=timeout)
    except Exception as e:
        return f"ERR: {e}"


async def find_q2(c: RetroConnection) -> str | None:
    for path in Q2_CANDIDATE_PATHS:
        r = await _run(rf'EXEC dir "{path}\baseq2\pak0.pak"', c, 10.0)
        if "File Not Found" not in r and "cannot find" not in r.lower() and "Not Found" not in r:
            return path
    return None


async def push_agent(agent_ip: str, secret: str = "retro-agent-secret") -> None:
    print(f"\n=== {agent_ip} ===")
    c = RetroConnection(agent_ip, 9898)
    await c.connect(secret, timeout=15.0)
    try:
        q2 = await find_q2(c)
        if not q2:
            print(f"  no Q2 install found (checked {Q2_CANDIDATE_PATHS})")
            return
        print(f"  Q2 install: {q2}")

        await _run(rf"NETMAP {SMB_UNC} Y: {SMB_USER} {SMB_PASS}", c, 15.0)

        for mod in MODS:
            src = rf"{SHARE_ROOT}\{mod}"
            dst = rf"{q2}\{mod}"
            # Is there anything in the share subdir?
            probe = await _run(rf'EXEC dir "{src}\*.*" /b /s', c, 15.0)
            if not any(ln.strip() for ln in probe.splitlines()
                       if ln.strip() and "File Not Found" not in ln):
                continue
            await _run(rf'EXEC mkdir "{dst}"', c, 10.0)
            r = await _run(
                rf'EXEC xcopy /Y /Q /E /D "{src}\*" "{dst}\\"',
                c, timeout=1800.0,
            )
            msg = next(
                (l.strip() for l in r.splitlines() if "File(s) copied" in l),
                (r.strip().splitlines()[-1][:120] if r.strip() else "?"),
            )
            print(f"  [{mod}] {msg}")
        print(f"  done — {q2} populated")
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
