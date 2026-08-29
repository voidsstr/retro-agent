#!/usr/bin/env python3
"""Stage the voodoo-cleanroom MesaFX ICD on .171 GAME-LOCALLY for Quake II.

Touches ONLY:  C:\\RETRO_AGENT\\bench\\q2run.bat
               C:\\RETRO_AGENT\\retrogl\\retrogl.dll   (staging copy)
               C:\\Games\\Quake2Complete\\retrogl.dll   (NEW file, shadows nothing)
               C:\\Games\\Quake2Complete\\baseq2\\config.cfg.preretrogl (backup)
Nothing in system32. No registry. No reboot.

Usage: python3 deploy171.py [--glide]      # --glide also stages our cvg glide3x
       python3 deploy171.py --rollback
"""
import asyncio, hashlib, os, sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
OUT = os.path.abspath(os.path.join(HERE, "..", "out"))
sys.path.insert(0, REPO)
from client.retro_protocol import RetroConnection  # noqa: E402

HOST = os.environ.get("BENCH_HOST", "192.168.1.171")
SECRET = os.environ.get("RETRO_AGENT_SECRET", "retro-agent-secret")

Q2RUN = (
    "@echo off\r\n"
    "rem %1=gl_driver  %2=gl_mode  %3=swapinterval (0=vsync off, 1=on)\r\n"
    "set FX_GLIDE_SWAPINTERVAL=%3\r\n"
    "set FX_GLIDE_NO_SPLASH=1\r\n"
    "set FX_GLIDE_SWAPPENDINGCOUNT=2\r\n"
    "cd /d C:\\Games\\Quake2Complete\r\n"
    "del /f /q baseq2\\qconsole.log >nul 2>&1\r\n"
    "start \"\" quake2.exe +set vid_ref gl +set gl_driver %1 +set gl_mode %2 "
    "+set gl_bitdepth 16 +set vid_fullscreen 1 +set logfile 2 +set s_initsound 0 "
    "+set gl_finish 0 +set gl_ext_swapinterval 1 +set gl_swapinterval %3 "
    "+set cl_maxfps 1000 +set timedemo 1 +map demo1.dm2\r\n"
).encode("ascii")


async def conn():
    for attempt in range(3):
        try:
            c = RetroConnection(HOST, 9898)
            await c.connect(SECRET, timeout=15.0)
            return c
        except Exception:
            if attempt == 2:
                raise
            await asyncio.sleep(3)


async def ex(c, cmd, t=60):
    s, d = await c.send_command(f"EXECW {t} {cmd}")
    return d.decode("ascii", "replace")


async def push(c, local, remote):
    data = open(local, "rb").read()
    await c.send_command(f"UPLOAD {remote}", binary_payload=data)
    back = await c.command_binary(f"DOWNLOAD {remote}")
    a, b = hashlib.md5(data).hexdigest(), hashlib.md5(back).hexdigest()
    print(f"  {remote}: {len(data)}B md5 {a} {'OK' if a == b else 'MISMATCH ' + b}")
    return a == b


async def deploy(with_glide):
    c = await conn()
    try:
        await c.send_command("MKDIR C:\\RETRO_AGENT\\bench")
        await c.send_command("MKDIR C:\\RETRO_AGENT\\retrogl")
        await c.send_command("UPLOAD C:\\RETRO_AGENT\\bench\\q2run.bat", binary_payload=Q2RUN)
        assert await push(c, os.path.join(OUT, "opengl32_retail.dll"),
                          "C:\\RETRO_AGENT\\retrogl\\retrogl.dll")
        await push(c, os.path.join(OUT, "opengl32_retail.dll.ver"),
                   "C:\\RETRO_AGENT\\retrogl\\retrogl.dll.ver")
        print(await ex(c, 'cmd /c if not exist "C:\\Games\\Quake2Complete\\baseq2\\config.cfg.preretrogl" '
                          'copy /Y "C:\\Games\\Quake2Complete\\baseq2\\config.cfg" '
                          '"C:\\Games\\Quake2Complete\\baseq2\\config.cfg.preretrogl"', 30))
        print(await ex(c, "cmd /c copy /Y C:\\RETRO_AGENT\\retrogl\\retrogl.dll "
                          "C:\\Games\\Quake2Complete\\retrogl.dll", 30))
        if with_glide:
            assert await push(c, os.path.join(OUT, "glide3x_cvg.dll"),
                              "C:\\RETRO_AGENT\\retrogl\\glide3x_cvg.dll")
            print(await ex(c, "cmd /c copy /Y C:\\RETRO_AGENT\\retrogl\\glide3x_cvg.dll "
                              "C:\\Games\\Quake2Complete\\glide3x.dll", 30))
        print(await ex(c, "cmd /c dir C:\\Games\\Quake2Complete\\*.dll", 30))
    finally:
        await c.close()


async def rollback():
    c = await conn()
    try:
        for cmd in ("cmd /c taskkill /f /im quake2.exe",
                    "cmd /c del /f /q C:\\Games\\Quake2Complete\\retrogl.dll",
                    "cmd /c del /f /q C:\\Games\\Quake2Complete\\glide3x.dll",
                    "cmd /c del /f /q C:\\retrogl.log",
                    'cmd /c copy /Y "C:\\Games\\Quake2Complete\\baseq2\\config.cfg.preretrogl" '
                    '"C:\\Games\\Quake2Complete\\baseq2\\config.cfg"'):
            print(await ex(c, cmd, 30))
        print(await ex(c, 'cmd /c type C:\\Games\\Quake2Complete\\baseq2\\config.cfg | find "gl_driver"', 30))
        print(await ex(c, "cmd /c dir C:\\Games\\Quake2Complete\\*.dll", 30))
    finally:
        await c.close()


if __name__ == "__main__":
    if "--rollback" in sys.argv:
        asyncio.run(rollback())
    else:
        asyncio.run(deploy("--glide" in sys.argv))
