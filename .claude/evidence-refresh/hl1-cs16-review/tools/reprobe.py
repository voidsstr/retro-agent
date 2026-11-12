#!/usr/bin/env python3
"""READ-ONLY re-probe for the adversarial review of the HalfLife1/CounterStrike16
refresh survey. Sends only GAMERES (never APPLY), DISPLAYCFG get, HWPROFILE,
REGREAD, DIRLIST and DOWNLOAD of small text files. Nothing launches, writes or
switches a mode. .243 and .249 are never contacted."""
import asyncio, json, os, sys, time
sys.path.insert(0, '/home/voidsstr/development/retro-agent')
from client.retro_protocol import RetroConnection

OUT = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'live')
os.makedirs(os.path.join(OUT, 'files'), exist_ok=True)

BASE = ['GAMERES', 'DISPLAYCFG get', 'HWPROFILE',
        r'REGREAD HKLM Software\RetroAgent',
        r'REGREAD HKCU Software\Valve\Half-Life\Settings',
        r'DIRLIST C:\Documents and Settings']
EXTRA = {
    '192.168.1.124': [r'REGREAD HKLM SYSTEM\CurrentControlSet\Services\3dfxvs\Device0\glide',
                      r'REGREAD HKCU SYSTEM\CurrentControlSet\Services\3dfxvs\Device0\glide',
                      r'REGREAD HKCU Environment'],
    '192.168.1.195': [r'DIRLIST C:\Users'],
}
TEMPS = {
    '192.168.1.123': [r'C:\Documents and Settings\Administrator\Local Settings\Temp\fleetres.cmd'],
    '192.168.1.145': [r'C:\Documents and Settings\voidsstr\Local Settings\Temp\fleetres.cmd'],
    '192.168.1.240': [r'C:\Documents and Settings\User\Local Settings\Temp\fleetres.cmd'],
    '192.168.1.124': [r'C:\Documents and Settings\Administrator\Local Settings\Temp\fleetres.cmd'],
    '192.168.1.195': [r'C:\Users\admin\AppData\Local\Temp\fleetres.cmd'],
}
BOXES = ['192.168.1.123', '192.168.1.145', '192.168.1.195', '192.168.1.240', '192.168.1.124']
if len(sys.argv) > 1:
    BOXES = [b for b in BOXES if b.split('.')[-1] in sys.argv[1:]]


async def probe(ip):
    res = {'ip': ip, 't': time.strftime('%Y-%m-%d %H:%M:%S %Z'), 'out': {}, 'files': {}}
    c = RetroConnection(ip, 9898)
    try:
        await c.connect('retro-agent-secret', timeout=15.0)
    except Exception as e:
        res['error'] = 'connect: %r' % (e,)
        return res
    try:
        for cmd in BASE + EXTRA.get(ip, []):
            try:
                st, d = await asyncio.wait_for(c.send_command(cmd), timeout=90)
                res['out'][cmd] = {'status': st, 'text': d.decode('latin-1', 'replace')}
            except Exception as e:
                res['out'][cmd] = {'status': None, 'text': 'EXC %r' % (e,)}
        for p in TEMPS.get(ip, []):
            try:
                st, d = await asyncio.wait_for(c.send_command('DOWNLOAD ' + p), timeout=60)
                txt = d.decode('latin-1', 'replace')
                res['files'][p] = {'status': st, 'len': len(d), 'text': txt[:4000]}
                fn = os.path.join(OUT, 'files', '%s_fleetres.cmd' % ip.split('.')[-1])
                if st == 0:
                    open(fn, 'wb').write(d)
            except Exception as e:
                res['files'][p] = {'status': None, 'text': 'EXC %r' % (e,)}
    finally:
        try:
            await c.close()
        except Exception:
            pass
    return res


async def main():
    rs = await asyncio.gather(*(probe(ip) for ip in BOXES))
    for r in rs:
        with open(os.path.join(OUT, 'reprobe-%s.json' % r['ip'].split('.')[-1]), 'w') as f:
            json.dump(r, f, indent=1)
        print(r['ip'], r['t'], r.get('error', 'ok'),
              {k: v['status'] for k, v in r['out'].items()},
              {k: (v['status'], v.get('len')) for k, v in r['files'].items()})

asyncio.run(main())
