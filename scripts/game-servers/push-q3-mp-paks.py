#!/usr/bin/env python3
"""Pre-install popular Quake 3 Arena multiplayer download-packs onto one or
more retro agents, so the retro machines don't have to auto-download 100+ MB
every time they join an active public server.

The pak bundle is staged on the SMB share at:
    \\\\192.168.1.122\\files\\Game Updates\\Q3-Multiplayer\\
with one subdir per mod (baseq3/, cpma/, osp/, defrag/, ufreeze/, threewave/,
arena/, edawn/, excessiveplus/). Sources — sampled April 2026 from the 137
Q3 servers registered on dpmaster.deathmask.net:

  mod           servers-running   source
  ---           ---------------   ------
  baseq3 (stock)   53            —
  excessiveplus    12            games.square-r00t.net
  osp               9            dl.warserver.net
  unfreeze          8            dl.warserver.net (ufreeze/)
  q3plus            7            (not mirrored — supplied by servers directly)
  cpma              1+           cdn.playmorepromode.com (v1.53-nomaps + mappack-full)
  defrag            2            dl.warserver.net
  threewave         1            games.square-r00t.net
  arena (RA3)       1            games.square-r00t.net

For each target agent this script:
  1. Looks for an existing Quake 3 install (pak0.pk3 in baseq3/) in common paths
  2. Maps the SMB share as drive Y:
  3. xcopies each mod's subdir from the share into the matching Q3 subdir
     (e.g. share/osp/*.pk3 -> C:\\Quake III Arena\\osp\\)
  4. Extracts CPMA zips (cpma-1.53-nomaps.zip into cpma/, cpma-mappack-full.zip
     into baseq3/) via a small JScript Shell.Application shim — XP's
     PowerShell is 2.0 so no `Expand-Archive`

Usage:
    cd /path/to/retro-agent
    python3 scripts/game-servers/push-q3-mp-paks.py 192.168.1.143 [...]

Prerequisites:
  * Q3 must already be installed (pak0.pk3 present). The script skips any
    agent where it can't locate a Q3 install.
  * Official 1.32 point release (pak1.pk3..pak8.pk3) should also be applied
    — staged separately on the share under Game Updates/Quake3e/.

This approach mirrors what the Q3 engine would fetch over sv_dlURL on first
connect to a pure server. The packs that servers typically serve are the
same packs we pre-populate here, just installed up front so players connect
and drop into a game immediately.
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
SHARE_ROOT = r"Y:\Game Updates\Q3-Multiplayer"

# Where on the retro PC does Q3 live? Checked in order; first hit wins.
Q3_CANDIDATE_PATHS = [
    r"C:\Quake III Arena",
    r"C:\Program Files\Quake III Arena",
    r"C:\Games\Quake III Arena",
    r"D:\Quake III Arena",
    r"C:\Q3",
]

# (share-subdir, target-subdir-under-Q3, kind: "pk3" or "zip")
MOD_LAYOUT = [
    ("baseq3",         "baseq3",         "pk3"),
    ("osp",            "osp",            "pk3"),
    ("defrag",         "defrag",         "pk3"),
    ("ufreeze",        "ufreeze",        "pk3"),
    ("threewave",      "threewave",      "pk3"),
    ("arena",          "arena",          "pk3"),
    ("edawn",          "edawn",          "pk3"),
    ("excessiveplus",  "xp",             "pk3"),
]

# CPMA ships as zips
CPMA_ZIPS = [
    # (share-filename,                 extract-target-subdir-under-Q3)
    ("cpma-1.53-nomaps.zip",           "cpma"),
    ("cpma-mappack-full.zip",          "baseq3"),
]

UNZIP_JS = (
    b"var sh=new ActiveXObject('Shell.Application');"
    b"var src=sh.NameSpace(WScript.Arguments(0));"
    b"var dst=sh.NameSpace(WScript.Arguments(1));"
    b"if(!src || !dst){WScript.Echo('src/dst missing'); WScript.Quit(1);}"
    b"dst.CopyHere(src.Items(), 4|16);\r\n"
)


async def _run(cmd: str, c: RetroConnection, timeout: float = 60.0) -> str:
    try:
        return await c.command_text(cmd, timeout=timeout)
    except Exception as e:
        return f"ERR: {e}"


async def find_q3(c: RetroConnection) -> str | None:
    for path in Q3_CANDIDATE_PATHS:
        r = await _run(rf'EXEC dir "{path}\baseq3\pak0.pk3"', c, 10.0)
        if "File Not Found" not in r and "cannot find" not in r.lower() and "Not Found" not in r:
            return path
    return None


async def push_agent(agent_ip: str, secret: str = "retro-agent-secret") -> None:
    print(f"\n=== {agent_ip} ===")
    c = RetroConnection(agent_ip, 9898)
    await c.connect(secret, timeout=15.0)
    try:
        q3 = await find_q3(c)
        if not q3:
            print(f"  no Q3 install found (checked {Q3_CANDIDATE_PATHS})")
            return
        print(f"  Q3 install: {q3}")

        # Map share
        await _run(rf"NETMAP {SMB_UNC} Y: {SMB_USER} {SMB_PASS}", c, 15.0)

        # Stage the unzip shim once per connection
        await c.send_command(r"UPLOAD C:\WINDOWS\TEMP\q3_unzip.js",
                             binary_payload=UNZIP_JS)

        # Copy pk3 files per mod
        for share_sub, q3_sub, kind in MOD_LAYOUT:
            src = rf'{SHARE_ROOT}\{share_sub}'
            dst = rf'{q3}\{q3_sub}'
            await _run(rf'EXEC mkdir "{dst}"', c, 10.0)
            # xcopy with overwrite, quiet, keep subdirs
            r = await _run(
                rf'EXEC xcopy /Y /Q "{src}\*.pk3" "{dst}\\"',
                c, timeout=900.0,
            )
            # Parse "N File(s) copied" line
            msg = next(
                (l.strip() for l in r.splitlines() if "File(s) copied" in l),
                r.strip().splitlines()[-1][:120] if r.strip() else "?",
            )
            print(f"  [{share_sub} -> {q3_sub}] {msg}")

        # Extract CPMA zips
        for zip_name, target_sub in CPMA_ZIPS:
            src_zip = rf'{SHARE_ROOT}\cpma\{zip_name}'
            tmp_zip = rf'C:\WINDOWS\TEMP\{zip_name}'
            dst = rf'{q3}\{target_sub}'
            print(f"  staging + extracting {zip_name} -> {target_sub}\\")
            await _run(rf'EXEC copy /Y "{src_zip}" "{tmp_zip}"', c, timeout=180.0)
            await _run(rf'EXEC mkdir "{dst}"', c, 10.0)
            r = await _run(
                rf'EXEC cscript //Nologo //E:JScript '
                rf'"C:\WINDOWS\TEMP\q3_unzip.js" "{tmp_zip}" "{dst}"',
                c, timeout=600.0,
            )
            if r.strip():
                print(f"    {r.strip().splitlines()[-1][:120]}")
            await _run(rf'EXEC del "{tmp_zip}"', c, 10.0)

        print(f"  done — {q3} populated with Q3 MP paks")
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
