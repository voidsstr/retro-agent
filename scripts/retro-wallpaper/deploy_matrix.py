#!/usr/bin/env python3
"""deploy_matrix.py — apply the Matrix/hacker desktop to a fleet box:
  * a per-box green digital-rain wallpaper (bottom-left kept clear for icons)
  * the desktop icons parked bottom-left, organized by theme, My Computer first
  * the green-on-black "hacker" system-color theme (Windows Classic)

All three persist across reboots via C:\\retro-wall\\ (the agent's retrowall
thread re-applies them at startup) — we overwrite the staged arrange_icons.exe
with the theme-sorted lower-left build and the wallpapers with the matrix ones.

  python3 deploy_matrix.py 192.168.1.123
  python3 deploy_matrix.py all
"""
import asyncio
import io
import os
import sys

sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..")))
from client.retro_protocol import RetroConnection  # noqa: E402
from PIL import Image  # noqa: E402
import gen_matrix_wall  # noqa: E402

SECRET = os.environ.get("RETRO_AGENT_SECRET", "retro-agent-secret")
HERE = os.path.dirname(os.path.abspath(__file__))
WALLDIR = "C:\\retro-wall"
RUN_KEY = "HKLM\\Software\\Microsoft\\Windows\\CurrentVersion\\Run"

FLEET = ["192.168.1.123", "192.168.1.124", "192.168.1.133", "192.168.1.143",
         "192.168.1.145", "192.168.1.240", "192.168.1.246"]

# hacker green-on-black classic scheme (same palette as the matrix rain)
COLORS = {
    "ActiveBorder": "0 60 0", "ActiveTitle": "0 28 0", "AppWorkspace": "0 0 0",
    "Background": "0 0 0", "ButtonDkShadow": "0 24 0", "ButtonFace": "0 0 0",
    "ButtonHilight": "0 90 0", "ButtonLight": "0 52 0", "ButtonShadow": "0 40 0",
    "ButtonText": "0 224 0", "GradientActiveTitle": "0 52 0",
    "GradientInactiveTitle": "10 14 10", "GrayText": "0 100 0",
    "Hilight": "0 112 0", "HilightText": "0 255 0", "HotTrackingColor": "0 200 0",
    "InactiveBorder": "10 14 10", "InactiveTitle": "8 12 8",
    "InactiveTitleText": "0 120 0", "InfoText": "0 224 0", "InfoWindow": "0 0 0",
    "Menu": "0 0 0", "MenuBar": "0 0 0", "MenuHilight": "0 112 0",
    "MenuText": "0 224 0", "Scrollbar": "0 20 0", "TitleText": "0 255 0",
    "Window": "0 0 0", "WindowFrame": "0 80 0", "WindowText": "0 230 0",
}


async def cmd(c, x, t=90.0):
    st, d = await c.send_command(x, timeout=t)
    return d.decode("ascii", "replace")


async def upload(c, local, remote):
    with open(local, "rb") as f:
        await c.send_command("UPLOAD " + remote, binary_payload=f.read())


def colors_reg():
    lines = ["REGEDIT4", "", "[HKEY_CURRENT_USER\\Control Panel\\Colors]"]
    for k, v in COLORS.items():
        lines.append('"%s"="%s"' % (k, v))
    return ("\r\n".join(lines) + "\r\n").encode("ascii")


async def get_res(c):
    """Exact screen resolution from a quarter-size screenshot (fast), scaled up."""
    data = await c.command_binary("SCREENSHOT 2", timeout=60.0)
    im = Image.open(io.BytesIO(data))
    return im.width * 2, im.height * 2   # SCREENSHOT 2 = quarter res -> x2 per axis... verified per-box below


async def get_res_exact(c):
    data = await c.command_binary("SCREENSHOT 0", timeout=90.0)
    im = Image.open(io.BytesIO(data))
    return im.width, im.height


async def get_specs(c):
    import json
    host, cpu, gpu, ram, osv = "RETRO", "", "", "", ""
    try:
        si = json.loads(await cmd(c, "SYSINFO"))
        host = si.get("hostname", host)
        cpu = (si.get("cpu", {}) or {}).get("name") or si.get("cpu_name") or ""
        mem = si.get("memory", {}) or {}
        ram = "%s MB" % mem.get("total_mb", "?")
        osv = (si.get("os", {}) or {}).get("product") or si.get("os_version") or "Windows"
    except Exception:
        pass
    try:
        vd = json.loads(await cmd(c, "VIDEODIAG"))
        ad = vd.get("adapters") or vd.get("adapter")
        if isinstance(ad, list) and ad:
            gpu = ad[0].get("name") or ad[0].get("description") or ""
        elif isinstance(ad, str):
            gpu = ad
    except Exception:
        pass
    return host, cpu.strip(), gpu.strip(), ram, str(osv)


def load_profile(host):
    """Curated per-box specs from profiles/<host>.i00.json (accurate CPU/GPU
    names + native resolution). Falls back to live query if absent."""
    import json, glob
    files = sorted(glob.glob(os.path.join(HERE, "profiles", host + ".i0*.json")))
    if not files:
        return None
    p = json.load(open(files[0]))
    hn = p.get("hostname", host)
    w, h = p.get("width"), p.get("height")
    specs = []
    for k, v in p.get("specs", []):
        if k in ("CPU", "GPU", "RAM", "OS"):
            specs.append("%-4s %s" % (k + ":", v[:34]))
    specs.append("NET: " + host)
    return hn, w, h, specs


async def deploy(host):
    c = RetroConnection(host, 9898)
    await c.connect(SECRET, timeout=15.0)
    try:
        prof = load_profile(host)
        if prof:
            hn, w, h, specs = prof
            if not w or not h:
                w, h = await get_res_exact(c)
        else:
            w, h = await get_res_exact(c)
            hn, cpu, gpu, ram, osv = await get_specs(c)
            specs = []
            if cpu: specs.append("CPU: " + cpu[:34])
            if gpu: specs.append("GPU: " + gpu[:34])
            if ram: specs.append("RAM: " + ram)
            if osv: specs.append("OS:  " + osv[:30])
            specs.append("NET: " + host)
        print("%s: %dx%d  %s" % (host, w, h, hn))

        # --- generate matrix wallpaper(s) at native res (3 variants to rotate) ---
        await cmd(c, "MKDIR " + WALLDIR)
        for i in range(3):
            local = "/tmp/matrix_%s_%d.bmp" % (host.replace(".", "_"), i)
            gen_matrix_wall.generate(w, h, hn, specs, local, variant=i)
            await upload(c, local, "%s\\wall%02d.bmp" % (WALLDIR, i))
            os.remove(local)
        # remove any leftover dossier wallpapers so rotation stays matrix
        await cmd(c, r'EXEC cmd /c for /L %%i in (3,1,15) do del "%s\wall0%%i.bmp" 2>nul' % WALLDIR)
        await cmd(c, r'EXEC cmd /c del "%s\wall1*.bmp" 2>nul' % WALLDIR)
        await cmd(c, 'EXEC cmd /c reg add "HKCU\\Control Panel\\Desktop" /v WallpaperStyle /t REG_SZ /d 2 /f')
        await cmd(c, 'EXEC cmd /c reg add "HKCU\\Control Panel\\Desktop" /v TileWallpaper /t REG_SZ /d 0 /f')

        # --- hacker theme: classic mode + green-on-black colors, live + persisted ---
        await cmd(c, "EXEC net stop Themes")           # may be denied on Win7/UAC — best-effort
        await cmd(c, "EXEC sc config Themes start= disabled")
        reg = colors_reg()
        await c.send_command("UPLOAD %s\\retro_theme.reg" % WALLDIR, binary_payload=reg)
        await cmd(c, "EXEC regedit /s %s\\retro_theme.reg" % WALLDIR)
        await upload(c, os.path.join(HERE, "setsyscolors.exe"), "%s\\setsyscolors.exe" % WALLDIR)
        print("  colors:", (await cmd(c, "EXEC %s\\setsyscolors.exe" % WALLDIR)).strip()[:60])

        # --- lower-left theme-sorted icons: overwrite the staged arrange exe so the
        #     agent's retrowall thread parks them bottom-left on every boot too ---
        await upload(c, os.path.join(HERE, "arrange_icons_ll.exe"), "%s\\arrange_icons.exe" % WALLDIR)
        await upload(c, os.path.join(HERE, "arrange_icons_ll.exe"), "C:\\WINDOWS\\TEMP\\arrange_ll.exe")

        # --- set the wallpaper live + (re)launch the rotator (persists at logon) ---
        await upload(c, os.path.join(HERE, "rotate_wall.exe"), "%s\\rotate_wall.exe" % WALLDIR)
        await cmd(c, 'EXEC cmd /c reg add "%s" /v RetroWallRotate /t REG_SZ /d "%s\\rotate_wall.exe 90" /f'
                  % (RUN_KEY, WALLDIR))
        await cmd(c, "EXEC taskkill /f /im rotate_wall.exe")
        await cmd(c, "LAUNCH %s\\rotate_wall.exe 90" % WALLDIR)
        await asyncio.sleep(3)
        # arrange AFTER the wallpaper is up (auto-arrange off, snap off avoided)
        print("  icons:", (await cmd(c, "EXEC C:\\WINDOWS\\TEMP\\arrange_ll.exe")).strip()[:80])
        print("%s: matrix theme + lower-left icons + hacker colors deployed" % host)
    finally:
        await c.close()


async def main():
    arg = sys.argv[1]
    hosts = FLEET if arg == "all" else [arg]
    for h in hosts:
        try:
            await deploy(h)
        except Exception as e:
            print("%s: FAILED %r" % (h, e))


if __name__ == "__main__":
    asyncio.run(main())
