#!/usr/bin/env python3
"""READ-ONLY probe for the refresh survey (idtech1/2 Quake-era titles).

Only GAMERES (probe, no APPLY), DISPLAYCFG get, DIRLIST, REGREAD, HWPROFILE.
Nothing is launched, written, or mode-switched. One connection per box,
closed gracefully (Win98 RST rule)."""
import asyncio, json, os, sys
sys.path.insert(0, '/home/voidsstr/development/retro-agent')
from client.retro_protocol import RetroConnection

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'boxes')
SECRET = 'retro-agent-secret'

async def probe(ip, cmds, timeout=20.0):
    res = {}
    c = RetroConnection(ip, 9898)
    try:
        greet = await c.connect(SECRET, timeout=timeout)
        res['_greeting'] = greet
        for cmd in cmds:
            try:
                st, d = await c.send_command(cmd)
                res[cmd] = {'status': st, 'text': d.decode('latin1', errors='replace')}
            except Exception as e:
                res[cmd] = {'error': repr(e)}
    except Exception as e:
        res['_error'] = repr(e)
    finally:
        try:
            await c.close()
        except Exception:
            pass
    with open(os.path.join(OUT, ip.replace('.', '_') + '.json'), 'a') as fh:
        fh.write(json.dumps(res, indent=1) + '\n')
    return res

async def main():
    ip = sys.argv[1]
    cmds = sys.argv[2:]
    r = await probe(ip, cmds, timeout=25.0 if ip.endswith('.243') else 15.0)
    for k, v in r.items():
        if isinstance(v, dict):
            print('###', k, 'status', v.get('status'), v.get('error', ''))
            print(v.get('text', '')[:6000])
        else:
            print('###', k, v)

asyncio.run(main())
