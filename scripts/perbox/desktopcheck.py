#!/usr/bin/env python3
"""Detect FALSE PASSES by comparing each cell's frame against the box's desktop.

A process in PROCLIST is not evidence -- and this sweep proved a sharper
version of that rule: a game that dies with an illegal instruction KEEPS ITS
NAME IN THE PROCESS LIST, held by Windows Error Reporting, so the cell scores
`runs` while nothing was ever drawn.  UnrealTournament 469e on .124 and .133
does exactly this (exit 0xC000001D, confirmed by errorlevel).

So the honest test is not "did a process appear" but "did the SCREEN change".
This captures a clean desktop per box and flags any cell whose frame is
substantially identical to it.
"""
import asyncio, json, os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from fleetlib import Box, BOXES
from PIL import Image, ImageChops

EV = os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(
     os.path.abspath(__file__)))), 'evidence')
BASE = os.path.join(EV, '_desktop')

KILL = ('UnrealTournament.exe','dosbox.exe','dwwin.exe','dumprep.exe',
        'daemon.exe','hl.exe','quake2.exe','maxpayne.exe','rf.exe')

async def grab(b, ip):
    for im in KILL:
        await b.cmd('EXEC cmd /c taskkill /f /im "%s"' % im)
    await asyncio.sleep(4)
    data = await b.binary('SCREENSHOT 0')
    os.makedirs(BASE, exist_ok=True)
    p = os.path.join(BASE, '%s.bmp' % ip.split('.')[-1])
    with open(p, 'wb') as f: f.write(data)
    return p

def sig(path, size=(64, 64)):
    im = Image.open(path).convert('L').resize(size, Image.BILINEAR)
    return im

def diff(a, b):
    d = ImageChops.difference(a, b)
    px = list(d.getdata())
    return sum(px) / float(len(px))

async def main():
    async def one(ip):
        try:
            async with Box(ip, 25.0) as b:
                return ip, await grab(b, ip)
        except Exception as e:
            return ip, None
    got = dict(await asyncio.gather(*[one(i) for i in BOXES if i != '192.168.1.243']))
    base = {ip: sig(p) for ip, p in got.items() if p}
    print('desktop baselines captured:', sorted(k.split('.')[-1] for k in base))
    out = []
    for ln in open(sys.argv[1]):
        r = json.loads(ln)
        if r['status'] != 'runs' or not r.get('evidence'): continue
        ip = r['ip']
        if ip not in base or not os.path.exists(r['evidence']): continue
        try:
            d = diff(sig(r['evidence']), base[ip])
        except Exception:
            continue
        out.append((d, ip, r['title'], r.get('shot_luma')))
    out.sort()
    print('\nCells most similar to the bare desktop (low = likely FALSE PASS):')
    for d, ip, t, l in out[:25]:
        print('  diff=%6.2f  %s %-26s luma=%s' % (d, ip, t, l))
    print('\n(diff is mean abs greyscale difference, 64x64. Under ~4 the frame is')
    print(' effectively the desktop and the title did NOT draw.)')

asyncio.run(main())
