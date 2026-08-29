#!/usr/bin/env python3
"""Make the fleet's fullscreen games run at a chosen refresh rate with vsync.

Why this exists
---------------
On XP + ForceWare a game that changes video mode without asking for a
frequency lands at **60Hz**, no matter what the desktop was set to and no
matter what CDS_UPDATEREGISTRY stored for that exact mode.  Measured on .124
(GeForce2 GTS, ForceWare 71.89, Sony CPD-G200) on 2026-08-25: desktop at
1024x768x32@100, launch Quake II fullscreen at 1024x768, DISPLAYCFG reports
``refresh: 60``.  ForceWare 71.89's control panel has **no** refresh-rate
override page, so there is nothing in the driver to switch on.

Only Quake III has an in-engine refresh setting (``r_displayRefresh``).
Quake II, Unreal Tournament 99 and GoldSrc (Half-Life / Counter-Strike 1.6)
have none, so this script:

* deploys ``refreshkeep.exe`` (agent/tools/refreshkeep.c), which watches the
  current mode beside the game and re-applies it *with* DM_DISPLAYFREQUENCY
  whenever it drifts off target, and exits when the game does;
* writes a launcher .bat per game that starts refreshkeep hidden, then the
  game;
* writes the engine-level vsync/refresh settings each engine *does* support;
* optionally repoints the existing desktop shortcuts at the launchers.

Vsync itself comes from two places: the engine cvar where one exists
(``r_swapInterval``, ``gl_swapinterval``, ``gl_vsync``) and the NVIDIA control
panel's OpenGL "Vertical sync: On by default", which covers the engines that
have no cvar (UT99's stock OpenGLDrv, WON Half-Life).

Usage:
    python3 deploy_game_refresh.py 192.168.1.124 [--hz 100] [--dry-run]
                                   [--no-shortcuts] [--only quake3,quake2]
"""
from __future__ import annotations

import argparse
import asyncio
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__)))))

from client.retro_protocol import RetroConnection  # noqa: E402

SECRET = "retro-agent-secret"
REMOTE_DIR = r"C:\RETRO_AGENT"
LAUNCH_DIR = REMOTE_DIR + r"\launch"
HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))

# --------------------------------------------------------------------------
# Per-engine settings blocks.  Each is a list of "cvar value" lines; the
# deploy merges them into the game's autoexec, dropping any stale copy of a
# cvar we manage so a re-run is idempotent rather than additive.
# --------------------------------------------------------------------------
BLOCK_START = "// --- retro-agent 100Hz + vsync (managed) ---"
BLOCK_END = "// --- end retro-agent block ---"


def q3_block(hz: int) -> list[str]:
    return [
        # r_displayRefresh is the ONLY in-engine refresh control among these
        # five games; Q3 passes it to ChangeDisplaySettings as
        # DM_DISPLAYFREQUENCY, which is exactly what the others fail to do.
        f'seta r_displayRefresh "{hz}"',
        'seta r_swapInterval "1"',
        # com_maxfps was 85 here, i.e. *below* the refresh: with vsync on that
        # is a guaranteed stutter (frames get held for a whole extra refresh).
        f'seta com_maxfps "{hz}"',
        'seta r_fullscreen "1"',
    ]


def q2_block(hz: int) -> list[str]:
    return [
        'set gl_swapinterval "1"',
        'set gl_ext_swapinterval "1"',
        'set vid_fullscreen "1"',
    ]


def goldsrc_block(hz: int) -> list[str]:
    return [
        'gl_vsync "1"',
        f'fps_max "{hz}"',
    ]


# --------------------------------------------------------------------------
# Game table.  `dirs` are probed in order; the first that contains `exe` wins,
# so the same table works on boxes with different install layouts.
# --------------------------------------------------------------------------
GAMES = [
    dict(
        key="quake3", label="Quake III Arena", exe="quake3.exe",
        dirs=[r"C:\Quake III Arena\Quake3",
              r"C:\Program Files\Quake III Arena",
              r"D:\Games\Quake III Arena"],
        cfgs=[r"baseq3\autoexec.cfg"],
        # Q3 writes/reads its config from fs_homepath when one is set.
        extra_cfgs=[r"C:\q3home\baseq3\autoexec.cfg"],
        block=q3_block, comment="//",
        args="+set r_displayRefresh {hz} +set r_swapInterval 1",
        shortcut="Quake III Arena.lnk",
    ),
    dict(
        key="quake2", label="Quake II", exe="quake2.exe",
        dirs=[r"C:\Games\Quake2", r"D:\Games\Quake2"],
        cfgs=[r"baseq2\autoexec.cfg"],
        extra_cfgs=[],
        block=q2_block, comment="//",
        args="+set vid_ref gl +set gl_driver opengl32 +set vid_fullscreen 1",
        shortcut="Quake II.lnk",
    ),
    dict(
        key="ut99", label="Unreal Tournament", exe="UnrealTournament.exe",
        dirs=[r"C:\Games\Unreal Tournament (Installed)\System",
              r"C:\Games\UT99\System",
              r"D:\Games\Unreal Tournament\System"],
        cfgs=[], extra_cfgs=[], block=None, comment=";",
        args="", shortcut="Unreal Tournament.lnk",
    ),
    dict(
        key="halflife", label="Half-Life", exe="hl.exe",
        dirs=[r"C:\Sierra\Half-Life"],
        cfgs=[r"valve\autoexec.cfg"], extra_cfgs=[],
        block=goldsrc_block, comment="//",
        args="-game valve -freq {hz} -refresh {hz}",
        shortcut="Half-Life.lnk",
    ),
    dict(
        key="cs16", label="Counter-Strike 1.6", exe="hl.exe",
        dirs=[r"D:\Program Files\Bcs16 Romania\Counter-Strike 1.6",
              r"C:\Program Files\Counter-strike",
              r"C:\Program Files\Bcs16 Romania\Counter-Strike 1.6"],
        cfgs=[r"cstrike\autoexec.cfg"], extra_cfgs=[],
        block=goldsrc_block, comment="//",
        args="-game cstrike -freq {hz} -refresh {hz}",
        shortcut="Counter-Strike 1.6.lnk",
    ),
]


class Box:
    """Thin wrapper so every step reads as one line."""

    def __init__(self, conn: RetroConnection, dry: bool):
        self.c = conn
        self.dry = dry

    async def text(self, cmd: str) -> str:
        status, data = await self.c.send_command(cmd)
        return data.decode("ascii", errors="replace")

    async def exists(self, path: str) -> bool:
        out = await self.text(f'EXEC cmd /c if exist "{path}" echo YES')
        return "YES" in out

    async def read(self, path: str) -> str:
        try:
            status, data = await self.c.send_command(f"DOWNLOAD {path}")
            if status != 0:
                return ""
            return data.decode("ascii", errors="replace")
        except Exception:
            return ""

    async def write(self, path: str, text: str) -> None:
        print(f"    write {path} ({len(text)} bytes)")
        if self.dry:
            return
        payload = text.replace("\n", "\r\n").encode("ascii", errors="replace")
        await self.c.send_command(f"UPLOAD {path}", binary_payload=payload)

    async def mkdir(self, path: str) -> None:
        if not self.dry:
            await self.text(f"MKDIR {path}")


def merge_block(existing: str, lines: list[str], comment: str) -> str:
    """Replace our managed block, leaving everything else untouched.

    Re-running must not stack duplicate cvars, and it must not throw away the
    hand-tuned lines already in these configs (.124's Q3 autoexec carries the
    mouse/netcode settings from an earlier session).
    """
    kept, skipping = [], False
    for line in existing.replace("\r\n", "\n").split("\n"):
        if line.strip() == BLOCK_START:
            skipping = True
            continue
        if line.strip() == BLOCK_END:
            skipping = False
            continue
        if skipping:
            continue
        # Drop stale copies of the cvars we own wherever else they appear -
        # a leftover `seta r_swapInterval "0"` further down the file would
        # otherwise win and silently undo vsync.
        managed = {ln.split()[1] for ln in lines if len(ln.split()) > 1}
        first = line.split()
        name = first[1] if len(first) > 1 and first[0] in ("set", "seta") \
            else (first[0] if first else "")
        if name in managed:
            continue
        kept.append(line)
    body = "\n".join(kept).rstrip()
    block = "\n".join([BLOCK_START] + lines + [BLOCK_END])
    return (body + "\n\n" + block + "\n") if body else block + "\n"


def launcher_bat(game: dict, gdir: str, hz: int) -> str:
    args = game["args"].format(hz=hz)
    proc = game["exe"]
    return "\n".join([
        "@echo off",
        f"rem {game['label']} at {hz}Hz with vsync - generated by",
        "rem scripts/game-refresh/deploy_game_refresh.py",
        "rem refreshkeep re-applies the video mode WITH a frequency after the",
        "rem game sets it, because the engine asks for none and XP then picks",
        "rem 60Hz. It exits by itself when the game does.",
        f'start "" wscript //nologo "{REMOTE_DIR}\\hidden.vbs" '
        f'"{REMOTE_DIR}\\refreshkeep.exe {hz} {proc} 7200 '
        f'>> {REMOTE_DIR}\\refreshkeep.log 2>&1"',
        f'cd /d "{gdir}"',
        f'start "" "{game["exe"]}" {args}'.rstrip(),
    ]) + "\n"


SHORTCUT_VBS = r"""
Set sh = CreateObject("WScript.Shell")
Set fso = CreateObject("Scripting.FileSystemObject")
p = WScript.Arguments(0)
t = WScript.Arguments(1)
If Not fso.FileExists(p) Then WScript.Echo "MISSING " & p : WScript.Quit 0
Set lnk = sh.CreateShortcut(p)
If lnk.TargetPath = t Then WScript.Echo "ALREADY " & p : WScript.Quit 0
If lnk.IconLocation = "" Or InStr(lnk.IconLocation, ",") = 0 Then
  lnk.IconLocation = lnk.TargetPath & ",0"
End If
lnk.TargetPath = t
lnk.Arguments = ""
lnk.WindowStyle = 7
lnk.Save
WScript.Echo "REPOINTED " & p & " -> " & t
"""


async def run(host: str, hz: int, dry: bool, shortcuts: bool,
              only: set[str]) -> int:
    conn = RetroConnection(host, 9898)
    await conn.connect(SECRET, timeout=20.0)
    box = Box(conn, dry)
    try:
        print(f"== {host}: hold games at {hz}Hz with vsync ==")
        await box.mkdir(LAUNCH_DIR)

        exe = os.path.join(REPO, "agent", "tools", "refreshkeep.exe")
        if not os.path.exists(exe):
            print(f"!! {exe} not built - run "
                  f"i686-w64-mingw32-gcc -O2 -o refreshkeep.exe "
                  f"refreshkeep.c -lgdi32")
            return 2
        print(f"  stage refreshkeep.exe ({os.path.getsize(exe)} bytes)")
        if not dry:
            with open(exe, "rb") as fh:
                await conn.send_command(
                    f"UPLOAD {REMOTE_DIR}\\refreshkeep.exe",
                    binary_payload=fh.read())
        with open(os.path.join(HERE, "files", "hidden.vbs")) as fh:
            await box.write(REMOTE_DIR + r"\hidden.vbs", fh.read())
        if not dry:
            await box.write(REMOTE_DIR + r"\shortcut_repoint.vbs",
                            SHORTCUT_VBS.lstrip())

        found = []
        for game in GAMES:
            if only and game["key"] not in only:
                continue
            gdir = None
            for cand in game["dirs"]:
                if await box.exists(cand + "\\" + game["exe"]):
                    gdir = cand
                    break
            if gdir is None:
                print(f"  -- {game['label']}: not installed, skipped")
                continue
            print(f"  {game['label']}: {gdir}")
            found.append((game, gdir))

            if game["block"]:
                lines = game["block"](hz)
                targets = [gdir + "\\" + c for c in game["cfgs"]]
                targets += game["extra_cfgs"]
                for path in targets:
                    parent = path.rsplit("\\", 1)[0]
                    if not await box.exists(parent):
                        continue
                    current = await box.read(path)
                    await box.write(path, merge_block(current, lines,
                                                      game["comment"]))

            bat = f"{LAUNCH_DIR}\\{game['key']}_{hz}hz.bat"
            await box.write(bat, launcher_bat(game, gdir, hz))

            if shortcuts and game.get("shortcut") and not dry:
                for base in (r"D:\Documents and Settings\All Users\Desktop",
                             r"C:\Documents and Settings\All Users\Desktop",
                             r"D:\Documents and Settings\voidsstr\Desktop",
                             r"C:\Documents and Settings\voidsstr\Desktop"):
                    lnk = f"{base}\\{game['shortcut']}"
                    if not await box.exists(lnk):
                        continue
                    out = await box.text(
                        f'EXEC cscript //nologo '
                        f'{REMOTE_DIR}\\shortcut_repoint.vbs "{lnk}" "{bat}"')
                    print("    " + out.strip())

        print(f"== {len(found)} game(s) configured ==")
        return 0
    finally:
        await conn.close()


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("host")
    ap.add_argument("--hz", type=int, default=100)
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--no-shortcuts", action="store_true")
    ap.add_argument("--only", default="",
                    help="comma-separated game keys: "
                         + ",".join(g["key"] for g in GAMES))
    a = ap.parse_args()
    only = {s.strip() for s in a.only.split(",") if s.strip()}
    return asyncio.run(run(a.host, a.hz, a.dry_run, not a.no_shortcuts, only))


if __name__ == "__main__":
    raise SystemExit(main())
