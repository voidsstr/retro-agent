#!/usr/bin/env python3
"""READ-ONLY probe for the refresh review: GAMERES / DISPLAYCFG get / DIRLIST /
REGREAD / small DOWNLOADs only. Never launches, never writes on a box.

usage: probe_ro.py <ip> <outfile.json> <cmd> [<cmd> ...]
       a cmd of the form 'DOWNLOAD <path>' stores the bytes next to the json.
"""
import asyncio, json, os, sys
sys.path.insert(0, '/home/voidsstr/development/retro-agent')
from client.retro_protocol import RetroConnection

ALLOWED = ('GAMERES', 'DISPLAYCFG get', 'DIRLIST ', 'REGREAD ', 'DOWNLOAD ', 'HWPROFILE')


async def main(ip, out, cmds):
    for c in cmds:
        if c != 'GAMERES' and c != 'HWPROFILE' and not c.startswith(ALLOWED):
            raise SystemExit('refusing non-read-only command: ' + c)
    res = {}
    c = RetroConnection(ip, 9898)
    try:
        await c.connect('retro-agent-secret', timeout=20)
    except Exception as e:
        res['_error'] = repr(e)
        json.dump(res, open(out, 'w'), indent=1)
        print(ip, 'connect failed', e)
        return
    try:
        for cmd in cmds:
            try:
                st, d = await asyncio.wait_for(c.send_command(cmd), timeout=60)
            except Exception as e:
                res[cmd] = {'error': repr(e)}
                continue
            if cmd.startswith('DOWNLOAD ') and st != 0xFF:
                fn = out + '.' + cmd[9:].replace(':', '').replace('\\', '__').replace(' ', '_')
                open(fn, 'wb').write(d)
                res[cmd] = {'status': st, 'saved': fn, 'bytes': len(d)}
            else:
                res[cmd] = {'status': st, 'text': d.decode('latin1', 'replace')}
    finally:
        await c.close()
    json.dump(res, open(out, 'w'), indent=1)
    print(ip, 'ok', list(res))

asyncio.run(main(sys.argv[1], sys.argv[2], sys.argv[3:]))
