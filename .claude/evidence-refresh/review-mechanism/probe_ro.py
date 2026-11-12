#!/usr/bin/env python3
"""READ-ONLY registry probes for the refresh-mechanism review (2026-09-29).
REGREAD only - no mode change, no file write on the box, no EXEC."""
import asyncio, json, os, sys, time
sys.path.insert(0, '/home/voidsstr/development/retro-agent')
from client.retro_protocol import RetroConnection
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'boxes')

async def run(ip, cmds):
    c = RetroConnection(ip, 9898)
    await c.connect('retro-agent-secret', timeout=20)
    res = {}
    try:
        for cmd in cmds:
            try:
                st, d = await c.send_command(cmd, timeout=30)
                txt = d.decode('ascii', errors='replace')
            except Exception as e:
                st, txt = -1, 'EXC %r' % e
            res[cmd] = (st, txt)
            fn = '%s__%s.txt' % (ip.split('.')[-1], ''.join(ch if ch.isalnum() else '_' for ch in cmd)[:100])
            with open(os.path.join(OUT, fn), 'w') as f:
                f.write('# %s  %s  status=%s  %s\n%s\n' % (ip, cmd, st, time.strftime('%Y-%m-%d %H:%M:%S'), txt))
    finally:
        await c.close()
    return res

if __name__ == '__main__':
    ip = sys.argv[1]
    cmds = sys.argv[2:]
    r = asyncio.run(run(ip, cmds))
    for k, (st, t) in r.items():
        print('==', k, st)
        print(t[:3000])
