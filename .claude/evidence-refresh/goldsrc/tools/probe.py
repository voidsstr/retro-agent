#!/usr/bin/env python3
"""Read-only probe of the GoldSrc refresh survey. Only GAMERES (no APPLY),
DISPLAYCFG get, DIRLIST and REGREAD are sent - nothing that changes a box.
One connection per box, boxes in parallel, each box's commands sequential."""
import asyncio, json, sys, time, os
sys.path.insert(0, '/home/voidsstr/development/retro-agent')
from client.retro_protocol import RetroConnection

OUT = '/home/voidsstr/development/retro-agent/.claude/evidence-refresh/goldsrc/live'
XP_CMDS = [
    'GAMERES',
    'DISPLAYCFG get',
    r'DIRLIST C:\Games',
    r'DIRLIST C:\Games\HalfLife1',
    r'DIRLIST C:\Games\CounterStrike16',
    r'REGREAD HKCU Software\Valve\Half-Life\Settings',
    r'REGREAD HKLM Software\RetroAgent',
]
BOXES = {
    '192.168.1.123': XP_CMDS,
    '192.168.1.145': XP_CMDS,
    '192.168.1.195': XP_CMDS,
    '192.168.1.240': XP_CMDS,
    '192.168.1.124': XP_CMDS + [r'DIRLIST D:\Games'],
    # Win98, single-threaded: agent-internal reads only, and few of them
    '192.168.1.243': ['GAMERES', r'DIRLIST C:\Games',
                      r'REGREAD HKCU Software\Valve\Half-Life\Settings'],
}

async def probe(ip, cmds):
    res = {'ip': ip, 't': time.strftime('%Y-%m-%d %H:%M:%S'), 'out': {}}
    to = 25.0 if ip.endswith('.243') else 15.0
    c = RetroConnection(ip, 9898)
    try:
        await c.connect('retro-agent-secret', timeout=to)
    except Exception as e:
        res['error'] = 'connect: %r' % (e,)
        return res
    try:
        for cmd in cmds:
            try:
                st, d = await asyncio.wait_for(c.send_command(cmd), timeout=60)
                res['out'][cmd] = {'status': st, 'text': d.decode('latin-1', 'replace')}
            except Exception as e:
                res['out'][cmd] = {'status': None, 'text': 'EXC %r' % (e,)}
            if ip.endswith('.243'):
                await asyncio.sleep(1.0)
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
        print(r['ip'], r.get('error', 'ok'), {k: (v['status'], len(v['text'])) for k, v in r['out'].items()})

asyncio.run(main())
