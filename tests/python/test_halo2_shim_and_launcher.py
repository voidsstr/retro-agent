"""Halo 2's staged tree must not ship a bare dwmapi.dll, and must not launch
through startup.exe.

WHY THIS EXISTS
---------------
Halo 2 was written off on 2026-09-01 as "cannot be licensed on this fleet -
it needs Vista's Software Licensing Service". That was wrong, and both halves
of the mistake are cheap to re-introduce, so both are pinned here.

1. A BARE `dwmapi.dll` IN THE TREE KILLS THE GAME ON WINDOWS 7.

   The disc's XP shim ships a 6,144-byte `dwmapi.dll` exporting exactly one
   function, `DwmEnableComposition`. The disc's own INSTRUCTIONS list it for
   **WinXP and deliberately NOT for Win7**. Vista and later ship a real
   67,072-byte `dwmapi.dll` exporting dozens, and a DLL sitting beside the exe
   SHADOWS `system32` - so on `.246` both `startup.exe` and `halo2.exe` died
   instantly with `0xC0000139 STATUS_ENTRYPOINT_NOT_FOUND`, before `main()`,
   with no dialog and no log. That silent instant exit is what got read as a
   licensing wall for three sessions: nothing was ever alive to reach one.

   So the stub ships as `dwmapi.dll.xpshim` and `Play Halo 2.bat` places it or
   removes it per box. A bare `dwmapi.dll` in the library re-breaks Win7 on the
   next GAMESYNC, silently. This is the same shape as the game-local nGlide
   `glide2x.dll` that hid a real Voodoo from the only two boxes that had one.

2. THE LAUNCHER MUST TARGET `halo2.exe`, NOT `startup.exe`.

   `startup.exe` is the disc AUTORUN/INSTALLER, not the game launcher. Run from
   an installed tree it starts an INSTALL and fails asking for `halo2.exe.dtz`,
   a compressed disc-only payload no installed tree carries - which it reports
   as "Files are missing or damaged in the installation directory". Pointing the
   staged launcher at it was the original defect. `halo2.exe` started directly
   reaches the main menu (verified fullscreen on `.246`).

Both checks read the REAL share, and both SKIP LOUDLY when it is not mounted -
a dev host without the SMB mount must not fail the suite, but a silent skip
would let the library rot unnoticed.
"""
import os

import pytest

LIB = "/mnt/retro-share/Files/Games-Library"
TREE = os.path.join(LIB, "Halo2")
LAUNCHER = os.path.join(TREE, "Play Halo 2.bat")

skip_unmounted = pytest.mark.skipif(
    not os.path.isdir(TREE),
    reason="SKIP: %s not mounted - Halo 2's staged tree was NOT checked" % TREE,
)


def _names():
    """Case-insensitive listing - we are reasoning about a Windows tree."""
    return {n.lower() for n in os.listdir(TREE)}


@skip_unmounted
def test_the_xp_dwmapi_stub_is_not_staged_under_its_shadowing_name():
    names = _names()
    assert "dwmapi.dll" not in names, (
        "Games-Library/Halo2 ships a bare dwmapi.dll. That 6 KB XP stub exports "
        "only DwmEnableComposition and SHADOWS the real 67 KB system dwmapi.dll "
        "on Vista/7, so halo2.exe dies at load with 0xC0000139 "
        "ENTRYPOINT_NOT_FOUND before main() - no dialog, no log, and it reads "
        "exactly like DRM. Ship it as dwmapi.dll.xpshim and let "
        "'Play Halo 2.bat' place it only on a box with no system dwmapi.dll."
    )


@skip_unmounted
def test_the_xp_shim_is_still_present_under_its_safe_name():
    # The other direction: deleting it outright would break XP, which genuinely
    # needs the stub because XP has no dwmapi.dll at all.
    assert "dwmapi.dll.xpshim" in _names(), (
        "dwmapi.dll.xpshim is missing. Windows XP has no dwmapi.dll of its own, "
        "so the stub must still ship - just under a name that cannot shadow."
    )


@skip_unmounted
def test_the_launcher_starts_the_game_not_the_disc_installer():
    body = open(LAUNCHER, encoding="utf-8", errors="replace").read()
    # Ignore REM lines: the launcher explains startup.exe at length on purpose.
    code = "\n".join(
        ln for ln in body.splitlines()
        if not ln.strip().lower().startswith("rem")
    )
    assert "startup.exe" not in code.lower(), (
        "'Play Halo 2.bat' still executes startup.exe. That is the disc "
        "autorun/INSTALLER: from an installed tree it starts an install and "
        "fails asking for halo2.exe.dtz, reported as 'Files are missing or "
        "damaged in the installation directory'. The game binary is halo2.exe."
    )
    assert "halo2.exe" in code.lower(), (
        "'Play Halo 2.bat' never names halo2.exe, which is the game binary."
    )


@skip_unmounted
def test_the_launcher_keeps_the_xp_loader_route():
    # On XP halo2.exe imports Vista-only ADVAPI32!RegGetValueA; Loader.exe
    # redirects it into Wow.dll. Dropping Loader breaks XP silently.
    code = open(LAUNCHER, encoding="utf-8", errors="replace").read().lower()
    assert "loader.exe" in code, (
        "'Play Halo 2.bat' no longer references Loader.exe. On Windows XP "
        "halo2.exe statically imports ADVAPI32!RegGetValueA, which is "
        "Vista-only; Loader.exe is what redirects it into Wow.dll."
    )
