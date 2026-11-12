#!/usr/bin/env python3
"""READ-ONLY probe for the adversarial review of the idtech1/2 refresh survey.

Allowed commands (anything else is refused BEFORE a connection is made):
  GAMERES (never GAMERES APPLY), DISPLAYCFG get, HWPROFILE,
  DIRLIST <path>, REGREAD <root> <path>, DOWNLOAD <path> (XP boxes only).
One connection per box, closed gracefully (the Win98 RST rule).

usage: probe_ro.py <ip> <label> <cmd> [<cmd> ...]
Output: ../boxes/<ip>_<label>.json  (+ one file per DOWNLOAD beside it)
"""
import asyncio, json, os, sys, time
sys.path.insert(0, '/home/voidsstr/development/retro-agent')
from client.retro_protocol import RetroConnection

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(os.path.dirname(HERE), 'boxes')
EXACT = {'GAMERES', 'DISPLAYCFG get', 'HWPROFILE'}
PREFIX = ('DIRLIST ', 'REGREAD ', 'DOWNLOAD ')


def allowed(ip, cmd):
    if cmd in EXACT:
        return True
    if cmd.upper().startswith('GAMERES'):
        return False                      # GAMERES APPLY writes - never
    if cmd.startswith('DOWNLOAD ') and ip.endswith('.243'):
        return False                      # keep the Win9x box to agent-internal reads
    return cmd.startswith(PREFIX)


async def main(ip, label, cmds):
    for c in cmds:
        if not allowed(ip, c):
            raise SystemExit('refusing non-read-only command: ' + c)
    os.makedirs(OUT, exist_ok=True)
    base = os.path.join(OUT, ip.replace('.', '_') + '_' + label)
    res = {'_ip': ip, '_started': time.strftime('%Y-%m-%d %H:%M:%S %Z')}
    conn = RetroConnection(ip, 9898)
    try:
        greet = await conn.connect('retro-agent-secret',
                                   timeout=30.0 if ip.endswith('.243') else 15.0)
        res['_greeting'] = greet if isinstance(greet, (str, int, float, type(None))) else repr(greet)
    except Exception as e:
        res['_error'] = repr(e)
        json.dump(res, open(base + '.json', 'w'), indent=1)
        print(ip, 'connect failed', e)
        return
    try:
        for cmd in cmds:
            try:
                st, d = await asyncio.wait_for(conn.send_command(cmd), timeout=90)
            except Exception as e:
                res[cmd] = {'error': repr(e)}
                continue
            if cmd.startswith('DOWNLOAD ') and st != 0xFF:
                fn = base + '.' + cmd[9:].replace(':', '').replace('\\', '__').replace(' ', '_')
                with open(fn, 'wb') as fh:
                    fh.write(d)
                res[cmd] = {'status': st, 'saved': os.path.basename(fn), 'bytes': len(d)}
            else:
                res[cmd] = {'status': st, 'text': d.decode('latin1', 'replace')}
    finally:
        try:
            await conn.close()
        except Exception:
            pass
    res['_finished'] = time.strftime('%Y-%m-%d %H:%M:%S %Z')
    json.dump(res, open(base + '.json', 'w'), indent=1)
    print(ip, 'ok', [k for k in res if not k.startswith('_')])


if __name__ == '__main__':
    asyncio.run(main(sys.argv[1], sys.argv[2], sys.argv[3:]))
