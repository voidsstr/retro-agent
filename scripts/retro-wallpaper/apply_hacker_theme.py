#!/usr/bin/env python3
"""
Apply a dark "hacker" green-on-black Windows Classic color scheme to a retro XP
machine, live (no logoff).

  python3 apply_hacker_theme.py 192.168.1.124
  python3 apply_hacker_theme.py 192.168.1.124 --revert

How it works:
  1. Stop the "Themes" service -> XP repaints to Windows Classic immediately
     (Luna can't render custom chrome colors; Classic can). Set it disabled so it
     stays off across reboots.
  2. Write HKCU\\Control Panel\\Colors with a green-on-black scheme via regedit /s.
  3. RUNDLL32 UpdatePerUserSystemParameters -> colors apply live.

--revert restores the Luna theme (Themes service auto + start) and clears the
custom colors back to XP defaults.
"""
import asyncio, os, sys

sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..")))
from client.retro_protocol import RetroConnection  # noqa: E402

SECRET = os.environ.get("RETRO_AGENT_SECRET", "retro-agent-secret")
HERE = os.path.dirname(os.path.abspath(__file__))
HELPER = os.path.join(HERE, "setsyscolors.exe")
HELPER_DST = "C:\\WINDOWS\\TEMP\\setsyscolors.exe"


async def push_and_apply_colors(c):
    """Upload setsyscolors.exe and run it -> pushes HKCU colors into live sys colors."""
    with open(HELPER, "rb") as f:
        exe = f.read()
    await c.send_command("UPLOAD " + HELPER_DST, binary_payload=exe)
    out = await c.command_text("EXEC " + HELPER_DST)
    return out.strip()

# green-on-black hacker scheme (R G B space-separated, XP classic color slots)
COLORS = {
    "ActiveBorder": "0 60 0",
    "ActiveTitle": "0 28 0",
    "AppWorkspace": "0 0 0",
    "Background": "0 0 0",
    "ButtonAlternateFace": "0 0 0",
    "ButtonDkShadow": "0 24 0",
    "ButtonFace": "0 0 0",
    "ButtonHilight": "0 90 0",
    "ButtonLight": "0 52 0",
    "ButtonShadow": "0 40 0",
    "ButtonText": "0 224 0",
    "GradientActiveTitle": "0 52 0",
    "GradientInactiveTitle": "10 14 10",
    "GrayText": "0 100 0",
    "Hilight": "0 112 0",
    "HilightText": "0 255 0",
    "HotTrackingColor": "0 200 0",
    "InactiveBorder": "10 14 10",
    "InactiveTitle": "8 12 8",
    "InactiveTitleText": "0 120 0",
    "InfoText": "0 224 0",
    "InfoWindow": "0 0 0",
    "Menu": "0 0 0",
    "MenuBar": "0 0 0",
    "MenuHilight": "0 112 0",
    "MenuText": "0 224 0",
    "Scrollbar": "0 0 0",
    "TitleText": "0 255 0",
    "Window": "0 0 0",
    "WindowFrame": "0 80 0",
    "WindowText": "0 230 0",
}

# XP default (Windows Standard) colors, for --revert
DEFAULTS = {
    "ActiveBorder": "212 208 200", "ActiveTitle": "10 36 106",
    "AppWorkspace": "128 128 128", "Background": "0 78 152",
    "ButtonAlternateFace": "181 181 181", "ButtonDkShadow": "64 64 64",
    "ButtonFace": "212 208 200", "ButtonHilight": "255 255 255",
    "ButtonLight": "241 239 226", "ButtonShadow": "128 128 128",
    "ButtonText": "0 0 0", "GradientActiveTitle": "166 202 240",
    "GradientInactiveTitle": "192 192 192", "GrayText": "128 128 128",
    "Hilight": "10 36 106", "HilightText": "255 255 255",
    "HotTrackingColor": "0 0 128", "InactiveBorder": "212 208 200",
    "InactiveTitle": "128 128 128", "InactiveTitleText": "212 208 200",
    "InfoText": "0 0 0", "InfoWindow": "255 255 225", "Menu": "212 208 200",
    "MenuBar": "212 208 200", "MenuHilight": "10 36 106",
    "MenuText": "0 0 0", "Scrollbar": "212 208 200", "TitleText": "255 255 255",
    "Window": "255 255 255", "WindowFrame": "0 0 0", "WindowText": "0 0 0",
}


def colors_reg(colors):
    lines = ["REGEDIT4", "", "[HKEY_CURRENT_USER\\Control Panel\\Colors]"]
    for k, v in colors.items():
        lines.append('"%s"="%s"' % (k, v))
    return ("\r\n".join(lines) + "\r\n").encode("ascii")


async def run(host, revert=False):
    c = RetroConnection(host, 9898)
    await c.connect(SECRET, timeout=15.0)
    try:
        if revert:
            reg = colors_reg(DEFAULTS)
            await c.send_command("UPLOAD C:\\WINDOWS\\TEMP\\theme.reg", binary_payload=reg)
            await c.command_text("EXEC regedit /s C:\\WINDOWS\\TEMP\\theme.reg")
            info = await push_and_apply_colors(c)
            await c.command_text("EXEC sc config Themes start= auto")
            await c.command_text("EXEC net start Themes")
            print("%s: reverted to Luna + default colors (%s)" % (host, info))
            return
        # 1. classic mode: stop + disable Themes service (repaints to classic live)
        await c.command_text("EXEC net stop Themes")
        await c.command_text("EXEC sc config Themes start= disabled")
        # 2. write the hacker color scheme
        reg = colors_reg(COLORS)
        await c.send_command("UPLOAD C:\\WINDOWS\\TEMP\\theme.reg", binary_payload=reg)
        await c.command_text("EXEC regedit /s C:\\WINDOWS\\TEMP\\theme.reg")
        # 3. apply live via SetSysColors helper
        info = await push_and_apply_colors(c)
        # 4. stage into C:\retro-wall so the agent's retrowall thread re-applies
        #    the SAME palette on every boot (persistence; else a reboot reverts).
        await c.command_text("EXEC cmd /c if not exist C:\\retro-wall md C:\\retro-wall")
        await c.send_command("UPLOAD C:\\retro-wall\\retro_theme.reg", binary_payload=reg)
        with open(HELPER, "rb") as f:
            await c.send_command("UPLOAD C:\\retro-wall\\setsyscolors.exe", binary_payload=f.read())
        print("%s: dark hacker theme applied + staged to C:\\retro-wall (%s)" % (host, info))
    finally:
        await c.close()


if __name__ == "__main__":
    host = sys.argv[1]
    revert = "--revert" in sys.argv[2:]
    asyncio.run(run(host, revert))
