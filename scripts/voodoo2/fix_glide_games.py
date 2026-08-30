#!/usr/bin/env python3
"""Make Glide games actually use the Voodoo 2 (hardware-verified on .171, 2026-08-30).

Two independent faults kept UnrealGold off the Voodoo 2 on 192.168.1.171, and
either one alone is enough to hide the card:

1. **An nGlide wrapper shadowed the real Glide.**  ``glide2x.dll`` (1,310,720
   bytes) sat in the game directory.  Game-local DLLs win over ``system32``, so
   Unreal loaded the wrapper -- which reports ``Glide 2.60`` and translates to
   Direct3D on the Intel 865G -- instead of the real 3dfx ``glide2x.dll``
   (226,304 bytes, ``Glide 2.56.00.0459``) driving the actual card.  On this box
   the wrapper's ``grSstOpen`` fails outright (``Color buffers 3 failed`` /
   ``Resolution 7 failed`` / ``grSstOpen failed (2, 3)``), so the game got
   neither the Voodoo 2 nor a working wrapper.

2. **Unreal fell out of Glide on its first focus change.**  Unreal's own splash
   dialog takes the foreground a few seconds after the viewport has already gone
   fullscreen on Glide.  The viewport gets ``WM_KILLFOCUS`` -> ``EndFullscreen``,
   and Unreal then switches to ``WindowedRenderDevice``.  A Voodoo 2 is a 3D-only
   passthrough card that CANNOT render windowed, so the stock
   ``WindowedRenderDevice=SoftDrv.SoftwareRenderDevice`` dropped the game onto
   the *software rasterizer* for the rest of the session -- 100% CPU, no card.
   Nothing external steals the focus; a foreground-window trace over a full run
   showed only Unreal's own splash -> viewport handoff.  Pointing
   ``WindowedRenderDevice`` at GlideDrv makes Unreal re-open Glide instead
   (verified: ``grSstOpen`` twice, zero SoftDrv binds, zero D3D binds).

The mode also has to be one a Voodoo 2 can actually scan out: it is **16-bit
only** and a single 4 MB-FBI card tops out at 640x480 once Unreal asks for three
colour buffers (800x600x16 triple-buffered needs 3.84 MB of a 4 MB frame buffer
and is refused -- observed as ``Resolution 8 failed``).  The stock ini asked for
1024x768x32, which no Voodoo 2 can do.

Usage::

    python3 scripts/voodoo2/fix_glide_games.py --host 192.168.1.171 --check
    python3 scripts/voodoo2/fix_glide_games.py --host 192.168.1.171 --apply
"""
from __future__ import annotations

import argparse
import asyncio
import os
import re
import sys

# --- facts about the two DLLs, so we never guess which one is loaded ---------
REAL_GLIDE2X_SIZE = 226_304      # 3dfx Glide 2.56.00.0459, Voodoo 2 XP kit
NGLIDE_GLIDE2X_SIZE = 1_310_720  # nGlide wrapper (Glide -> Direct3D)

# A Voodoo 2 is 16bpp-only; a single 4MB-FBI card cannot do 800x600 with the
# three colour buffers Unreal requests, and cannot do 1024x768 without SLI.
VOODOO2_FULLSCREEN_X = "640"
VOODOO2_FULLSCREEN_Y = "480"
VOODOO2_COLOR_BITS = "16"

GLIDE_DEVICE = "GlideDrv.GlideRenderDevice"

# All three must point at Glide.  WindowedRenderDevice is the one that actually
# matters: it is what EndFullscreen switches to when the splash steals focus.
UNREAL_INI_FIXES = {
    "GameRenderDevice": GLIDE_DEVICE,
    "RenderDevice": GLIDE_DEVICE,
    "WindowedRenderDevice": GLIDE_DEVICE,
    "FullscreenViewportX": VOODOO2_FULLSCREEN_X,
    "FullscreenViewportY": VOODOO2_FULLSCREEN_Y,
    "FullscreenColorBits": VOODOO2_COLOR_BITS,
}


def fix_unreal_ini(text: str) -> tuple[str, list[str]]:
    """Point Unreal.ini at Glide in a Voodoo 2-legal mode.

    Pure: takes the ini text, returns ``(new_text, [descriptions of changes])``.
    Line endings and every unrelated line are preserved.

    Each key is matched **anchored to the start of a line**.  ``RenderDevice``
    is a substring of both ``GameRenderDevice`` and ``WindowedRenderDevice``, so
    an unanchored rule also fires inside those lines.  Today that is merely
    invisible rather than harmful -- all three happen to be set to the same
    value -- but it means the rule would rewrite any other ``*RenderDevice=``
    key too, and it breaks the moment the three values diverge.  The anchor
    keeps each rule to the whole key it names.
    """
    changes: list[str] = []
    for key, value in UNREAL_INI_FIXES.items():
        pattern = re.compile(rf"^{re.escape(key)}=(.*?)(\r?)$", re.MULTILINE)

        def _sub(m: re.Match) -> str:
            old = m.group(1)
            if old != value:
                changes.append(f"{key}: {old} -> {value}")
            return f"{key}={value}{m.group(2)}"

        text = pattern.sub(_sub, text)
    return text, changes


def classify_glide2x(size: int) -> str:
    """Name the glide2x.dll sitting in a game directory, by size."""
    if size == NGLIDE_GLIDE2X_SIZE:
        return "nglide-wrapper"
    if size == REAL_GLIDE2X_SIZE:
        return "real-3dfx"
    return "unknown"


# --- remote application ------------------------------------------------------
UNREAL_SYSTEM = r"C:\Games\UnrealGold\System"


async def _connect(host: str):
    sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..")))
    from client.retro_protocol import RetroConnection

    conn = RetroConnection(host, 9898)
    await conn.connect("retro-agent-secret", timeout=15.0)
    return conn


async def _cmd(conn, text: str, payload: bytes | None = None) -> str:
    status, data = await conn.send_command(text, binary_payload=payload)
    return data.decode("ascii", errors="replace")


async def run(host: str, apply: bool) -> int:
    conn = await _connect(host)
    try:
        listing = await _cmd(conn, rf"EXEC cmd /c dir /b {UNREAL_SYSTEM}\glide2x*")
        names = listing.split()
        shadowing = "glide2x.dll" in names
        print(f"game-local glide2x files: {' '.join(names) or '(none)'}")
        print(f"  real Glide shadowed by a game-local DLL: {shadowing}")

        status, raw = await conn.send_command(rf"DOWNLOAD {UNREAL_SYSTEM}\Unreal.ini")
        fixed, changes = fix_unreal_ini(raw.decode("latin-1"))
        print("  ini changes needed:" if changes else "  ini already correct")
        for c in changes:
            print(f"    {c}")

        if not apply:
            return 0
        if shadowing:
            await _cmd(
                conn,
                rf"EXEC cmd /c move /Y {UNREAL_SYSTEM}\glide2x.dll "
                rf"{UNREAL_SYSTEM}\glide2x.dll.nglide",
            )
            print("  retired the game-local wrapper -> glide2x.dll.nglide")
        if changes:
            await conn.send_command(
                rf"UPLOAD {UNREAL_SYSTEM}\Unreal.ini", binary_payload=fixed.encode("latin-1")
            )
            print("  wrote Unreal.ini")
        return 0
    finally:
        await conn.close()


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--host", required=True)
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--check", action="store_true", help="report only")
    g.add_argument("--apply", action="store_true", help="retire the wrapper and fix the ini")
    a = ap.parse_args()
    return asyncio.run(run(a.host, a.apply))


if __name__ == "__main__":
    raise SystemExit(main())
