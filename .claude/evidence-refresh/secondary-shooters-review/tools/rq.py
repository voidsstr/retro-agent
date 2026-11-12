#!/usr/bin/env python3
"""Read-only query helper: run a list of agent commands against one box and
save each answer under the evidence dir. Only read-only commands are allowed
(GAMERES without APPLY, DISPLAYCFG get, DIRLIST, REGREAD, HWPROFILE, DOWNLOAD,
PING, SYSINFO)."""
import asyncio, sys, os, re, json, time
sys.path.insert(0, '/home/voidsstr/development/retro-agent')
from client.retro_protocol import RetroConnection

EVID = '/home/voidsstr/development/retro-agent/.claude/evidence-refresh/secondary-shooters-review/boxes'
ALLOWED = ('GAMERES', 'DISPLAYCFG GET', 'DIRLIST', 'REGREAD', 'HWPROFILE',
           'DOWNLOAD', 'PING', 'SYSINFO', 'VIDEODIAG')


def safe(cmd):
    u = cmd.upper()
    if u.startswith('GAMERES') and 'APPLY' in u:
        return False
    return any(u.startswith(a) for a in ALLOWED)


async def run(ip, cmds, port=9898, timeout=15.0):
    os.makedirs(EVID, exist_ok=True)
    c = RetroConnection(ip, port)
    await c.connect('retro-agent-secret', timeout=timeout)
    out = []
    try:
        for cmd in cmds:
            if not safe(cmd):
                raise SystemExit('refusing non-read-only command: ' + cmd)
            st, d = await c.send_command(cmd)
            tag = re.sub(r'[^A-Za-z0-9]+', '_', cmd)[:80]
            fn = os.path.join(EVID, '%s__%s.txt' % (ip.split('.')[-1], tag))
            if cmd.upper().startswith('DOWNLOAD'):
                fn = fn[:-4] + '.bin'
                with open(fn, 'wb') as f:
                    f.write(d)
            else:
                with open(fn, 'w') as f:
                    f.write('# %s  %s  status=%s  %s\n' % (ip, cmd, st, time.strftime('%Y-%m-%d %H:%M:%S')))
                    f.write(d.decode('latin-1', errors='replace'))
            out.append((cmd, st, d))
    finally:
        await c.close()
    return out


if __name__ == '__main__':
    ip = sys.argv[1]
    cmds = sys.argv[2:]
    res = asyncio.run(run(ip, cmds))
    for cmd, st, d in res:
        print('=====', ip, cmd, 'status', st, 'len', len(d))
        if not cmd.upper().startswith('DOWNLOAD'):
            print(d.decode('latin-1', errors='replace')[:6000])
