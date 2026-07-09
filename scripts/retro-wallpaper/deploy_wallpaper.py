#!/usr/bin/env python3
"""
Deploy a generated dossier BMP to a retro machine and set it as the XP desktop
wallpaper.

  python3 deploy_wallpaper.py 192.168.1.145 out/192.168.1.145.bmp

Uploads the BMP to C:\\retro-dossier.bmp, writes the HKCU wallpaper registry
values via a .reg file (regedit /s), then refreshes the desktop with
RUNDLL32 UpdatePerUserSystemParameters so it applies without a logoff.
"""
import asyncio, os, sys

sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..")))
from client.retro_protocol import RetroConnection  # noqa: E402

SECRET = os.environ.get("RETRO_AGENT_SECRET", "retro-agent-secret")
DST = "C:\\retro-dossier.bmp"
REG = (
    b"REGEDIT4\r\n\r\n"
    b"[HKEY_CURRENT_USER\\Control Panel\\Desktop]\r\n"
    b'"Wallpaper"="C:\\\\retro-dossier.bmp"\r\n'
    b'"WallpaperStyle"="2"\r\n'
    b'"TileWallpaper"="0"\r\n'
)


async def deploy(host, bmp_path):
    with open(bmp_path, "rb") as f:
        bmp = f.read()
    c = RetroConnection(host, 9898)
    await c.connect(SECRET, timeout=15.0)
    try:
        st, _ = await c.send_command("UPLOAD " + DST, binary_payload=bmp)
        print("%s: uploaded BMP (%d KB) status=%d" % (host, len(bmp) // 1024, st))
        st, _ = await c.send_command(
            "UPLOAD C:\\WINDOWS\\TEMP\\setwp.reg", binary_payload=REG)
        out = await c.command_text("EXEC regedit /s C:\\WINDOWS\\TEMP\\setwp.reg")
        print("%s: regedit applied" % host)
        await c.command_text(
            "EXEC RUNDLL32.EXE USER32.DLL,UpdatePerUserSystemParameters ,1 ,True")
        print("%s: desktop refreshed -> wallpaper set" % host)
    finally:
        await c.close()


if __name__ == "__main__":
    host, bmp = sys.argv[1], sys.argv[2]
    asyncio.run(deploy(host, bmp))
