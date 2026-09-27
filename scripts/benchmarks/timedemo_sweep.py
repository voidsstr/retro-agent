#!/usr/bin/env python3
"""Resolution sweep of built-in id-engine benchmarks on one fleet box.

    python3 scripts/benchmarks/timedemo_sweep.py 192.168.1.143 --titles q3,rtcw \
        --res 640x480,800x600,1024x768,1280x960,1600x1200

- q3   : Quake III 1.32 `timedemo 1; demo four` (full demo playback, CPU + GPU).
- rtcw : RTCW multiplayer `timedemo 1; demo tdbench` - a scripted fly-through of
         mp_beach recorded on the box (the GOG build has no timerefresh and RTCW
         ships no demo; none is on the share).

Each run is launched through LAUNCH + a batch (GUI programs must not go
through EXEC), the result is read from the game's own console log, and the
game is killed afterwards. Results land in scripts/benchmarks/results/.

Written 2026-09-26 for .143 (GeForce 6800, 1 GHz Athlon). Modes are left at the
driver's default refresh (60 Hz on XP) - the point is the renderer's speed, and
r_swapInterval 0 keeps vsync out of it (checked: a result pinned at exactly the
refresh rate means vsync is forced somewhere and the number is meaningless).
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

TITLES = {
    'q3': {
        'dir': r'C:\Quake III Arena\Quake3', 'exe': 'quake3.exe', 'mod': 'baseq3',
        'cfg': ('set timedemo 1\nseta r_swapInterval 0\nseta com_maxfps 0\n'
                'seta r_mode -1\nseta r_customwidth {w}\nseta r_customheight {h}\n'
                'seta r_colorbits 32\nseta r_depthbits 24\nseta r_texturebits 32\n'
                'seta r_fullscreen 1\nseta r_picmip 0\nvid_restart\ndemo four\n'),
        'args': '+set r_glDriver opengl32 +set fs_game baseq3 +set logfile 2 +exec tdsweep.cfg',
        'result': re.compile(r'(\d+) frames, ([\d.]+) seconds: ([\d.]+) fps'),
        'timeout': 240,
    },
    'rtcw': {
        # The GOG build removed timerefresh and RTCW ships no demo, so this plays
        # back tdbench.dm_60: a scripted spectator fly-through of mp_beach
        # recorded on .143 on 2026-09-26 (see record_rtcw_demo in the notes).
        # Comparable across resolutions and boxes, not with published scores.
        'dir': r'C:\Games\ReturnToCastleWolfenstein', 'exe': 'WolfMP.exe', 'mod': 'main',
        'log': r'Main\rtcwconsole.log',
        'cfg': ('set timedemo 1\nseta r_swapInterval 0\nseta com_maxfps 0\nseta r_mode -1\n'
                'seta r_customwidth {w}\nseta r_customheight {h}\nseta r_colorbits 32\n'
                'seta r_depthbits 24\nseta r_fullscreen 1\nseta r_picmip 0\nvid_restart\ndemo tdbench\n'),
        'args': '+set r_glDriver opengl32 +set logfile 2 +exec tdsweep.cfg',
        'result': re.compile(r'(\d+) frames, ([\d.]+) seconds: ([\d.]+) fps'),
        'timeout': 240,
    },
}


async def one(ip, key, w, h, secret):
    t = TITLES[key]
    c = RetroConnection(ip, 9898)
    await c.connect(secret, timeout=15)
    try:
        d = t['dir']
        log = f"{d}\\{t['log']}" if 'log' in t else f"{d}\\{t['mod']}\\qconsole.log"
        await c.send_command(f"UPLOAD {d}\\{t['mod']}\\tdsweep.cfg",
                             binary_payload=t['cfg'].format(w=w, h=h).replace('\n', '\r\n').encode())
        bat = (f'@echo off\r\ncd /d "{d}"\r\ndel "{log}" 2>nul\r\n'
               f"{t['exe']} {t['args']}\r\n").encode()
        await c.send_command(r'UPLOAD C:\WINDOWS\TEMP\tdsweep.bat', binary_payload=bat)
        await c.command_text(r'LAUNCH C:\WINDOWS\TEMP\tdsweep.bat', timeout=30)
        text, res = '', None
        for _ in range(t['timeout'] // 5):
            await asyncio.sleep(5)
            try:
                text = (await c.command_binary(f'DOWNLOAD {log}', timeout=30)).decode('latin1')
            except Exception:
                continue
            m = t['result'].search(text)
            if m:
                res = m
                break
        await c.command_text(f"EXEC taskkill /f /im {t['exe']}", timeout=30)
        await asyncio.sleep(6)          # pace the next mode switch (1998 CRT)
        mode = re.findall(r'\.\.\.setting mode (-?\d+):\s*(\d+) (\d+)', text)
        glr = re.search(r'GL_RENDERER: (.*)', text)
        return {'title': key, 'res': f'{w}x{h}',
                'fps': float(res.group(res.lastindex)) if res else None,
                'raw': res.group(0) if res else None,
                'mode_set': mode[-1][1:] if mode else None,
                'renderer': glr.group(1).strip() if glr else None,
                'log_tail': None if res else text[-800:]}
    finally:
        await c.close()


async def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('ip')
    ap.add_argument('--titles', default='q3,rtcw')
    ap.add_argument('--res', default='640x480,800x600,1024x768,1280x960,1600x1200')
    ap.add_argument('--secret', default='retro-agent-secret')
    a = ap.parse_args()
    stamp = datetime.datetime.now().strftime('%Y%m%d-%H%M%S')
    out = os.path.join(REPO, 'scripts', 'benchmarks', 'results', f'{a.ip}_timedemo_{stamp}')
    os.makedirs(out, exist_ok=True)
    rows = []
    for key in a.titles.split(','):
        for r in a.res.split(','):
            w, h = r.split('x')
            row = await one(a.ip, key, int(w), int(h), a.secret)
            rows.append(row)
            print(f"{key:5s} {row['res']:>10s}  {row['fps'] if row['fps'] is not None else 'FAILED':>7}  "
                  f"(mode {row['mode_set']}, {row['renderer']})", flush=True)
            json.dump(rows, open(os.path.join(out, 'results.json'), 'w'), indent=1)
    print('results:', out)


if __name__ == '__main__':
    asyncio.run(main())
