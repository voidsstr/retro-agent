#!/usr/bin/env python3
"""READ-ONLY: DOWNLOAD a few small files (and .124's ICD log) from XP/7 boxes.
Nothing is written on any box; .243 is not contacted."""
import asyncio, os, sys
sys.path.insert(0, '/home/voidsstr/development/retro-agent')
from client.retro_protocol import RetroConnection

OUT = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'live', 'files')
os.makedirs(OUT, exist_ok=True)
WANT = {
    '192.168.1.124': [r'C:\retrogl.log',
                      r'C:\Games\CounterStrike16\qconsole.log',
                      r'C:\Games\CounterStrike16\V56KBENCH.BAT',
                      r'C:\Games\CounterStrike16\LANCHECK.BAT',
                      r'C:\Games\HalfLife1\TFCTEST.BAT',
                      r'C:\Games\HalfLife1\DMCTEST.BAT',
                      r'C:\Games\CounterStrike16\Play Counter-Strike.bat',
                      r'C:\Games\HalfLife1\Half-Life.bat'],
    '192.168.1.195': [r'C:\Games\CounterStrike16\qconsole.log'],
    '192.168.1.123': [r'C:\Games\CounterStrike16\Play Counter-Strike.bat',
                      r'C:\Games\HalfLife1\Half-Life.bat'],
}


async def fetch(ip, paths):
    c = RetroConnection(ip, 9898)
    await c.connect('retro-agent-secret', timeout=15.0)
    try:
        for p in paths:
            try:
                st, d = await asyncio.wait_for(c.send_command('DOWNLOAD ' + p), timeout=120)
            except Exception as e:
                print(ip, p, 'EXC', e)
                continue
            fn = os.path.join(OUT, '%s_%s' % (ip.split('.')[-1],
                              p.replace(':', '').replace('\\', '_').replace(' ', '_')))
            with open(fn, 'wb') as f:
                f.write(d)
            print(ip, p, 'status', st, len(d), 'B ->', os.path.basename(fn))
    finally:
        await c.close()


async def main():
    await asyncio.gather(*(fetch(ip, ps) for ip, ps in WANT.items()))

asyncio.run(main())
