#!/usr/bin/env python3
"""Read-only probe for the refresh-mechanism design (2026-09-29).

Only commands that change nothing on a box: HWPROFILE, DISPLAYCFG get, GAMERES
(no APPLY), REGREAD, DIRLIST. Each result is saved verbatim with a header.
Usage: probe_ro.py <ip> "<CMD>" ["<CMD>" ...]
"""
import asyncio, os, sys, time
sys.path.insert(0, '/home/voidsstr/development/retro-agent')
from client.retro_protocol import RetroConnection

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'boxes')
ALLOWED = ('HWPROFILE', 'DISPLAYCFG get', 'GAMERES', 'REGREAD ', 'DIRLIST ', 'PING')

def safe(cmd):
    if cmd.startswith('GAMERES') and cmd.strip() != 'GAMERES':
        return False                     # GAMERES APPLY writes - never here
    return any(cmd == a or cmd.startswith(a) for a in ALLOWED)

async def run(ip, cmds):
    c = RetroConnection(ip, 9898)
    await c.connect('retro-agent-secret', timeout=20.0)
    try:
        for cmd in cmds:
            if not safe(cmd):
                print('REFUSED (not read-only):', cmd); continue
            st, d = await c.send_command(cmd)
            txt = d.decode('ascii', errors='replace')
            tag = ip.split('.')[-1] + '__' + ''.join(ch if ch.isalnum() else '_' for ch in cmd)[:200]
            with open(os.path.join(OUT, tag + '.txt'), 'w') as f:
                f.write('# %s  %s  status=%s  %s\n%s\n' % (ip, cmd, st, time.strftime('%Y-%m-%d %H:%M:%S'), txt))
            print('==', ip, cmd, 'status', st); print(txt[:3000])
    finally:
        await c.close()

if __name__ == '__main__':
    asyncio.run(run(sys.argv[1], sys.argv[2:]))
