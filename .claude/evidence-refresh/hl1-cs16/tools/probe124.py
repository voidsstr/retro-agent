#!/usr/bin/env python3
"""READ-ONLY .124 follow-up: where does the Glide refresh come from?
REGREAD / DIRLIST / DOWNLOAD only; nothing is written or launched."""
import asyncio, json, os, sys, time, hashlib
sys.path.insert(0, '/home/voidsstr/development/retro-agent')
from client.retro_protocol import RetroConnection

OUT = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'live')
CMDS = [
    r'REGREAD HKLM SYSTEM\CurrentControlSet\Services\3dfxvs\Device0\glide',
    r'REGREAD HKCU SYSTEM\CurrentControlSet\Services\3dfxvs\Device0\glide',
    r'REGREAD HKLM SYSTEM\CurrentControlSet\Services\3dfxvs\Device0',
    r'REGREAD HKLM SYSTEM\CurrentControlSet\Control\Session Manager\Environment',
    r'REGREAD HKCU Environment',
    r'REGREAD HKLM SYSTEM\CurrentControlSet\Services\vcrmp\Device0',
    r'REGREAD HKLM SOFTWARE\Microsoft\Windows NT\CurrentVersion\OpenGLDrivers\3dfx',
    r'DIRLIST C:\WINDOWS\system32\glide3x.dll',
    r'DIRLIST C:\WINDOWS\system32\retroicd.dll',
]


async def main():
    res = {'ip': '192.168.1.124', 't': time.strftime('%Y-%m-%d %H:%M:%S %Z'), 'out': {}}
    c = RetroConnection('192.168.1.124', 9898)
    await c.connect('retro-agent-secret', timeout=15.0)
    try:
        for cmd in CMDS:
            st, d = await asyncio.wait_for(c.send_command(cmd), timeout=60)
            res['out'][cmd] = {'status': st, 'text': d.decode('latin-1', 'replace')}
        st, d = await asyncio.wait_for(c.send_command(r'DOWNLOAD C:\WINDOWS\system32\glide3x.dll'), timeout=180)
        fn = os.path.join(OUT, 'files', '124_system32_glide3x.dll')
        with open(fn, 'wb') as f:
            f.write(d)
        res['glide3x_md5'] = hashlib.md5(d).hexdigest()
        res['glide3x_size'] = len(d)
    finally:
        await c.close()
    with open(os.path.join(OUT, 'probe-124-glide.json'), 'w') as f:
        json.dump(res, f, indent=1)
    for k, v in res['out'].items():
        print('##', k, v['status'])
        print(v['text'][:1500])
    print('glide3x', res.get('glide3x_size'), res.get('glide3x_md5'))

asyncio.run(main())
