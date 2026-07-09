#!/usr/bin/env python3
"""
Deploy the rotating dossier wallpaper to a retro XP machine.

  python3 deploy_rotation.py 192.168.1.124 [interval_seconds]

Stages the machine's 10 iteration BMPs (out/<host>.iNN.bmp) into C:\\retro-wall\\
as wall00.bmp..wall09.bmp, uploads rotate_wall.exe, sets the wallpaper style,
parks the desktop icons in the bottom-right well (arrange_icons.exe), installs an
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
        # park icons in the bottom-right well
        await upload_file(c, os.path.join(HERE, "arrange_icons.exe"),
                          "C:\\WINDOWS\\TEMP\\arrange_icons.exe")
        print("%s: %s" % (host, (await c.command_text(
            "EXEC C:\\WINDOWS\\TEMP\\arrange_icons.exe")).strip()))
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
