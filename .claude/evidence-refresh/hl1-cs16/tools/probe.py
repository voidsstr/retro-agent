#!/usr/bin/env python3
"""READ-ONLY probe for the HalfLife1 / CounterStrike16 refresh survey.

Only GAMERES (never APPLY), DISPLAYCFG get, DIRLIST, REGREAD and HWPROFILE are
sent - nothing that launches, writes, or changes a mode. One connection per
box, boxes in parallel, each box's commands sequential. .243 (Win98,
single-threaded) gets agent-internal reads only, spaced out. .249 (Windows 11,
unmanaged) is never contacted.
"""
import asyncio, json, sys, time, os
sys.path.insert(0, '/home/voidsstr/development/retro-agent')
from client.retro_protocol import RetroConnection

OUT = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'live')
os.makedirs(OUT, exist_ok=True)

NT = [
    'GAMERES',
    'DISPLAYCFG get',
    'HWPROFILE',
    r'DIRLIST C:\Games',
    r'DIRLIST C:\Games\HalfLife1',
    r'DIRLIST C:\Games\CounterStrike16',
    r'REGREAD HKCU Software\Valve\Half-Life\Settings',
    r'REGREAD HKLM Software\RetroAgent',
]
BOXES = {
    '192.168.1.123': NT,
    '192.168.1.145': NT,
    '192.168.1.195': NT,
    '192.168.1.240': NT,
    '192.168.1.124': NT + [r'DIRLIST D:\Games'],
    '192.168.1.243': ['GAMERES', r'DIRLIST C:\Games',
                      r'REGREAD HKCU Software\Valve\Half-Life\Settings'],
}
if len(sys.argv) > 1:
    BOXES = {k: v for k, v in BOXES.items() if k.split('.')[-1] in sys.argv[1:]}


async def probe(ip, cmds):
    res = {'ip': ip, 't': time.strftime('%Y-%m-%d %H:%M:%S %Z'), 'out': {}}
    slow = ip.endswith('.243')
    c = RetroConnection(ip, 9898)
    try:
        await c.connect('retro-agent-secret', timeout=25.0 if slow else 15.0)
    except Exception as e:
        res['error'] = 'connect: %r' % (e,)
        return res
    try:
        for cmd in cmds:
            try:
                st, d = await asyncio.wait_for(c.send_command(cmd), timeout=90)
                res['out'][cmd] = {'status': st, 'text': d.decode('latin-1', 'replace')}
            except Exception as e:
                res['out'][cmd] = {'status': None, 'text': 'EXC %r' % (e,)}
            if slow:
                await asyncio.sleep(1.5)
    finally:
        try:
            await c.close()
        except Exception:
            pass
    return res


async def main():
    rs = await asyncio.gather(*(probe(ip, cmds) for ip, cmds in BOXES.items()))
    for r in rs:
        fn = os.path.join(OUT, 'probe-%s.json' % r['ip'].split('.')[-1])
        with open(fn, 'w') as f:
            json.dump(r, f, indent=1)
        print(r['ip'], r['t'], r.get('error', 'ok'),
              {k: (v['status'], len(v['text'])) for k, v in r['out'].items()})

asyncio.run(main())
