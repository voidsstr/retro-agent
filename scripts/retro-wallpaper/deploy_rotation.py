#!/usr/bin/env python3
"""
Deploy the rotating dossier wallpaper to a retro XP machine.

  python3 deploy_rotation.py 192.168.1.124 [interval_seconds]

Stages the machine's 10 iteration BMPs (out/<host>.iNN.bmp) into C:\\retro-wall\\
as wall00.bmp..wall09.bmp, uploads rotate_wall.exe, sets the wallpaper style,
removes the legacy bottom-right icon arranger (the agent owns icon layout), installs an
HKCU Run key so the rotator starts at logon, and launches it now. rotate_wall.exe
cycles the wallpapers every <interval> seconds (default 60).
"""
import asyncio, os, sys, glob

sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..")))
from client.retro_protocol import RetroConnection  # noqa: E402

SECRET = os.environ.get("RETRO_AGENT_SECRET", "retro-agent-secret")
HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "out")
WALLDIR = "C:\\retro-wall"
RUN_KEY = "HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Run"


async def upload_file(c, local, remote):
    with open(local, "rb") as f:
        data = f.read()
    await c.send_command("UPLOAD " + remote, binary_payload=data)
    return len(data)


async def deploy(host, interval=60):
    bmps = sorted(glob.glob(os.path.join(OUT, host + ".i*.bmp")))
    if not bmps:
        raise SystemExit("no iteration BMPs for %s (run build_profiles + gen)" % host)
    c = RetroConnection(host, 9898)
    await c.connect(SECRET, timeout=15.0)
    try:
        await c.command_text("MKDIR " + WALLDIR)
        total = 0
        for i, bmp in enumerate(bmps):
            total += await upload_file(c, bmp, "%s\\wall%02d.bmp" % (WALLDIR, i))
        print("%s: staged %d wallpapers (%d MB)" % (host, len(bmps), total // (1024 * 1024)))

        await upload_file(c, os.path.join(HERE, "rotate_wall.exe"),
                          WALLDIR + "\\rotate_wall.exe")
        # wallpaper style: stretch to native (BMPs already match), no tiling
        await c.command_text('EXEC cmd /c reg add "HKCU\\Control Panel\\Desktop" '
                             '/v WallpaperStyle /t REG_SZ /d 2 /f')
        await c.command_text('EXEC cmd /c reg add "HKCU\\Control Panel\\Desktop" '
                             '/v TileWallpaper /t REG_SZ /d 0 /f')
        # Desktop icons: DO NOT stage or run arrange_icons.exe.
        #
        # That tool parks icons in the BOTTOM-RIGHT well. The agent arranges
        # them itself into a TOP-LEFT bay (agent/src/gamesync.c:gs_arrange_icons,
        # gs_icon_bay), so the two disagree about where icons belong and the
        # last one to run wins. Staging it here is what kept putting a fresh
        # copy back on boxes it had already been removed from, and retrowall
        # ran it on every agent start until v1.70.0 -- which UNDID a correct
        # arrangement on every single boot.
        #
        # So this step now REMOVES it instead, renaming it aside (never
        # deleting) so an agent older than v1.70.0 on the same box cannot
        # find it and re-park the icons behind our back.
        for stale in (WALLDIR + "\\arrange_icons.exe",
                      "C:\\WINDOWS\\TEMP\\arrange_icons.exe"):
            await c.command_text(
                'EXEC cmd /c if exist "%s" move /Y "%s" "%s.disabled-bottom-right"'
                % (stale, stale, stale))
        print("%s: legacy bottom-right arranger removed; the agent owns the "
              "top-left icon bay" % host)
        # dark "hacker" system-color theme. Stage retro_theme.reg (the fleet-wide
        # green-on-black scheme) + setsyscolors.exe into C:\retro-wall\ so the
        # agent re-applies the theme on EVERY startup (agent/src/retrowall.c
        # step 6), then apply it live now.
        await upload_file(c, os.path.join(HERE, "retro_theme.reg"),
                          WALLDIR + "\\retro_theme.reg")
        await upload_file(c, os.path.join(HERE, "setsyscolors.exe"),
                          WALLDIR + "\\setsyscolors.exe")
        await c.command_text('EXEC regedit /s %s\\retro_theme.reg' % WALLDIR)
        print("%s: %s" % (host, (await c.command_text(
            "EXEC %s\\setsyscolors.exe" % WALLDIR)).strip()))
        # Starfield screensaver (part of the fleet theme). Stage ssstars.scr into
        # C:\retro-wall\ (works on Win7, which ships none, and needs no system32
        # write), then point the screensaver at it + enable it (10 min).
        scr_local = os.path.join(HERE, "ssstars.scr")
        scr_path = WALLDIR + "\\ssstars.scr"
        if os.path.exists(scr_local):
            await upload_file(c, scr_local, scr_path)
        else:
            scr_path = "%SystemRoot%\\system32\\ssstars.scr"
        await c.command_text('EXEC cmd /c reg add "HKCU\\Control Panel\\Desktop" '
                             '/v "SCRNSAVE.EXE" /t REG_SZ /d "%s" /f' % scr_path)
        await c.command_text('EXEC cmd /c reg add "HKCU\\Control Panel\\Desktop" '
                             '/v ScreenSaveActive /t REG_SZ /d 1 /f')
        await c.command_text('EXEC cmd /c reg add "HKCU\\Control Panel\\Desktop" '
                             '/v ScreenSaveTimeOut /t REG_SZ /d 600 /f')
        print("%s: screensaver -> Starfield (%s)" % (host, scr_path))
        # persist rotator across logon
        cmd = '%s\\rotate_wall.exe %d' % (WALLDIR, interval)
        await c.command_text('EXEC cmd /c reg add "%s" /v RetroWallRotate '
                             '/t REG_SZ /d "%s" /f' % (RUN_KEY, cmd))
        # (re)launch now
        await c.command_text("EXEC taskkill /f /im rotate_wall.exe")
        await c.command_text("LAUNCH %s\\rotate_wall.exe %d" % (WALLDIR, interval))
        print("%s: rotator running (every %ds), starts at logon" % (host, interval))
    finally:
        await c.close()


if __name__ == "__main__":
    host = sys.argv[1]
    interval = int(sys.argv[2]) if len(sys.argv) > 2 else 60
    asyncio.run(deploy(host, interval))
