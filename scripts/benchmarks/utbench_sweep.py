#!/usr/bin/env python3
"""UTbench (the classic UT 4.36 benchmark demo) across resolutions and renderers.

    python3 scripts/benchmarks/utbench_sweep.py 192.168.1.143 \
        --res 640x480,1024x768 --devices OpenGLDrv.OpenGLRenderDevice,D3DDrv.D3DRenderDevice

UE1 takes no console commands on its command line (`UnrealTournament.exe
UTbench.dem?timedemo=1` is parsed as a SERVER address and fails with "Pending
connect ... failed"), so this starts UT to its menu and types the two commands
into the console through the agent: `timedemo 1 1` (on, and save to file) then
`demoplay utbench`. UT appends "<avg> Unreal 436 / <min> Min / <max> Max" to
System\\fps.txt when the demo ends. Resolution and renderer are written into
UnrealTournament.ini with the staged FLEETRES.EXE -ini writer first.

Needs UTbench.dem in <UT>\\System (copied from the share's
"Games/Benchmarks & Tech Demos"). Written 2026-09-27 for .143.
"""
import argparse
import asyncio
import datetime
import json
import os
import re
import sys

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, REPO)
from client.retro_protocol import RetroConnection  # noqa: E402

DEM = '/mnt/retro-share/Files/Games/Benchmarks & Tech Demos/unreal-tournament-utbench/UTbench.dem'


async def one(ip, ut, w, h, dev, secret):
    ini = f'{ut}\\System\\UnrealTournament.ini'
    fr = f'{ut}\\FLEETRES.EXE'
    bat = (f'@echo off\r\ncd /d {ut}\\System\r\n'
           f'"{fr}" -ini "{ini}" WinDrv.WindowsClient FullscreenViewportX {w}\r\n'
           f'"{fr}" -ini "{ini}" WinDrv.WindowsClient FullscreenViewportY {h}\r\n'
           f'"{fr}" -ini "{ini}" WinDrv.WindowsClient StartupFullscreen True\r\n'
           f'"{fr}" -ini "{ini}" Engine.Engine GameRenderDevice {dev}\r\n'
           'if exist Running.ini del /q Running.ini\r\ndel UnrealTournament.log fps.txt 2>nul\r\n'
           'UnrealTournament.exe -nosound\r\n').encode()
    c = RetroConnection(ip, 9898)
    await c.connect(secret, timeout=15)
    try:
        await c.send_command(r'UPLOAD C:\WINDOWS\TEMP\utsweep.bat', binary_payload=bat)
        await c.command_text(r'LAUNCH C:\WINDOWS\TEMP\utsweep.bat', timeout=30)
        await asyncio.sleep(35)
        for k in ('UIKEY TILDE', 'UIKEY TEXT:timedemo 1 1', 'UIKEY ENTER',
                  'UIKEY TEXT:demoplay utbench', 'UIKEY ENTER'):
            await c.command_text(k, timeout=30)
            await asyncio.sleep(2)
        fps, log = None, ''
        for _ in range(60):
            await asyncio.sleep(6)
            try:
                t = (await c.command_binary(f'DOWNLOAD {ut}\\System\\fps.txt', timeout=30)).decode('latin1')
            except Exception:
                continue
            m = re.search(r'([\d.]+) Unreal \S+\s+([\d.]+) Min\s+([\d.]+) Max', t)
            if m:
                fps = tuple(float(x) for x in m.groups())
                break
        try:
            log = (await c.command_binary(f'DOWNLOAD {ut}\\System\\UnrealTournament.log', timeout=30)).decode('latin1')
        except Exception:
            pass
        await c.command_text('EXEC taskkill /f /im UnrealTournament.exe', timeout=30)
        await asyncio.sleep(6)          # pace the next mode switch (1998 CRT)
        rend = re.search(r'GL_RENDERER\): (.*)|Direct3D adapter.*?:\s*(.*)', log)
        return {'res': f'{w}x{h}', 'device': dev,
                'avg': fps[0] if fps else None, 'min': fps[1] if fps else None,
                'max': fps[2] if fps else None,
                'bound': re.findall(r'Bound to (\w+Drv)\.dll', log)[-1:] or None,
                'log_tail': None if fps else log[-600:]}
    finally:
        await c.close()


async def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('ip')
    ap.add_argument('--ut', default=r'C:\Games\UnrealTournament436')
    ap.add_argument('--res', default='640x480,800x600,1024x768,1280x960,1600x1200')
    ap.add_argument('--devices', default='OpenGLDrv.OpenGLRenderDevice,D3DDrv.D3DRenderDevice')
    ap.add_argument('--secret', default='retro-agent-secret')
    a = ap.parse_args()
    c = RetroConnection(a.ip, 9898)
    await c.connect(a.secret, timeout=15)
    await c.send_command(f'UPLOAD {a.ut}\\System\\UTbench.dem', binary_payload=open(DEM, 'rb').read())
    await c.close()
    stamp = datetime.datetime.now().strftime('%Y%m%d-%H%M%S')
    out = os.path.join(REPO, 'scripts', 'benchmarks', 'results', f'{a.ip}_utbench_{stamp}')
    os.makedirs(out, exist_ok=True)
    rows = []
    for dev in a.devices.split(','):
        for r in a.res.split(','):
            w, h = r.split('x')
            row = await one(a.ip, a.ut, int(w), int(h), dev, a.secret)
            rows.append(row)
            print(f"{dev.split('.')[0]:10s} {row['res']:>10s}  avg {row['avg']}  min {row['min']}  "
                  f"max {row['max']}  bound {row['bound']}", flush=True)
            json.dump(rows, open(os.path.join(out, 'results.json'), 'w'), indent=1)
    print('results:', out)


if __name__ == '__main__':
    asyncio.run(main())
