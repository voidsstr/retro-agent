#!/usr/bin/env python3
"""Adversarial re-probe for the GoldSrc refresh review. READ-ONLY: GAMERES (no
APPLY), DISPLAYCFG get, DIRLIST, REGREAD only. One connection per box, boxes in
parallel, commands sequential. .243 and .249 are deliberately not contacted."""
import asyncio, json, sys, time, os
sys.path.insert(0, '/home/voidsstr/development/retro-agent')
from client.retro_protocol import RetroConnection
OUT = '/home/voidsstr/development/retro-agent/.claude/evidence-refresh/goldsrc-review/live'
os.makedirs(OUT, exist_ok=True)
BASE = ['GAMERES', 'DISPLAYCFG get']
BOXES = {
    '192.168.1.123': BASE + [r'REGREAD HKLM SYSTEM\CurrentControlSet\Enum\DISPLAY'],
    '192.168.1.145': BASE + [r'REGREAD HKLM SYSTEM\CurrentControlSet\Enum\DISPLAY'],
    '192.168.1.195': BASE + [r'REGREAD HKLM SYSTEM\CurrentControlSet\Enum\DISPLAY'],
    '192.168.1.240': BASE + [r'REGREAD HKLM SYSTEM\CurrentControlSet\Enum\DISPLAY'],
    '192.168.1.124': BASE + [r'DIRLIST C:\WINDOWS\system32\glide*', r'DIRLIST C:\WINDOWS\system32\retro*',
                             r'REGREAD HKLM SOFTWARE\Microsoft\Windows NT\CurrentVersion\OpenGLDrivers\3dfx'],
}
async def probe(ip, cmds):
    res = {'ip': ip, 't': time.strftime('%Y-%m-%d %H:%M:%S'), 'out': {}}
    c = RetroConnection(ip, 9898)
    try:
        await c.connect('retro-agent-secret', timeout=15.0)
    except Exception as e:
        res['error'] = 'connect: %r' % (e,); return res
    try:
        for cmd in cmds:
            try:
                st, d = await asyncio.wait_for(c.send_command(cmd), timeout=60)
                res['out'][cmd] = {'status': st, 'text': d.decode('latin-1', 'replace')}
            except Exception as e:
                res['out'][cmd] = {'status': None, 'text': 'EXC %r' % (e,)}
    finally:
        try: await c.close()
        except Exception: pass
    return res
async def main():
    rs = await asyncio.gather(*(probe(ip, cmds) for ip, cmds in BOXES.items()))
    for r in rs:
        with open(os.path.join(OUT, 'reprobe-%s.json' % r['ip'].split('.')[-1]), 'w') as f:
            json.dump(r, f, indent=1)
        print(r['ip'], r['t'], r.get('error', 'ok'), {k: (v['status'], len(v['text'])) for k, v in r['out'].items()})
asyncio.run(main())
