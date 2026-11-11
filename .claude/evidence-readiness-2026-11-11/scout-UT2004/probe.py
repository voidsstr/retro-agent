#!/usr/bin/env python3
"""READ-ONLY probe of the UT2004 install on each box (no launch/kill/write)."""
import asyncio, json, os, sys
sys.path.insert(0, '/home/voidsstr/development/retro-agent')
from client.retro_protocol import RetroConnection

OUT = '/home/voidsstr/development/retro-agent/.claude/evidence-readiness-2026-11-11/scout-UT2004'
BOXES = [int(x) for x in sys.argv[1:]] or [110, 123, 124, 133, 143, 171, 184, 186, 240]
G = 'C:\\Games\\UT2004'
TEXT = [
    ('proclist', 'PROCLIST'),
    ('winlist', 'WINLIST'),
    ('dir_root', 'DIRLIST ' + G),
    ('dir_running', 'DIRLIST ' + G + '\\System\\Running*'),
    ('dir_ut2004', 'DIRLIST ' + G + '\\System\\UT2004*'),
    ('dir_user', 'DIRLIST ' + G + '\\System\\User*'),
    ('dir_cdkey', 'DIRLIST ' + G + '\\System\\cdkey*'),
    ('dir_fleetmp', 'DIRLIST C:\\fleetmp'),
    ('gameres_verify', 'GAMERES VERIFY UT2004'),
]
BIN = [
    ('UT2004.ini', G + '\\System\\UT2004.ini'),
    ('User.ini', G + '\\System\\User.ini'),
    ('UT2004.log', G + '\\System\\UT2004.log'),
    ('Join.bat', G + '\\Join fleet UT2004 server.bat'),
    ('FLEETRES.BAT', G + '\\FLEETRES.BAT'),
    ('fleetmp_ut2004.bat', 'C:\\fleetmp\\ut2004.bat'),
]

async def probe(o):
    d = os.path.join(OUT, str(o))
    os.makedirs(d, exist_ok=True)
    res = {}
    c = RetroConnection(f'192.168.1.{o}', 9898)
    try:
        await c.connect('retro-agent-secret', timeout=15.0)
    except Exception as e:
        return o, {'connect_error': repr(e)}
    try:
        for name, cmd in ([] if os.environ.get('DLONLY') else TEXT):
            try:
                st, data = await c.send_command(cmd)
                txt = data.decode('latin1', 'replace')
                open(os.path.join(d, name + '.txt'), 'w').write(txt)
                res[name] = ('ok' if st == 0 else f'status {st}', len(txt))
            except Exception as e:
                res[name] = ('error', repr(e))
        for name, path in BIN:
            try:
                st, data = await c.send_command('DOWNLOAD ' + path)
                if st != 255:
                    open(os.path.join(d, name), 'wb').write(data)
                    res[name] = ('ok', len(data))
                else:
                    res[name] = (f'status {st}', data.decode('latin1', 'replace')[:120])
            except Exception as e:
                res[name] = ('error', repr(e))
    finally:
        await c.close()
    return o, res

async def main():
    rs = await asyncio.gather(*[probe(o) for o in BOXES])
    summary = dict(rs)
    open(os.path.join(OUT, 'probe_summary.json'), 'w').write(json.dumps(summary, indent=1))
    print(json.dumps(summary, indent=1))

asyncio.run(main())
