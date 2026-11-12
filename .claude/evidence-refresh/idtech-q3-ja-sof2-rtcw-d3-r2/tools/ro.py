# Read-only agent queries for the id Tech 3/4 refresh survey (round 2).
# Usage: ro.py <ip> <label> <cmd> [<cmd> ...]
# Only GAMERES (bare), DISPLAYCFG get, DIRLIST, REGREAD, HWPROFILE, DOWNLOAD, PING
# are allowed - anything else is refused before a connection is opened.
import asyncio, json, os, sys, time, hashlib
sys.path.insert(0, '/home/voidsstr/development/retro-agent')
from client.retro_protocol import RetroConnection

EV = '/home/voidsstr/development/retro-agent/.claude/evidence-refresh/idtech-q3-ja-sof2-rtcw-d3-r2/boxes'
SAFE_PREFIX = ('DIRLIST ', 'REGREAD ', 'DOWNLOAD ')
SAFE_EXACT = ('GAMERES', 'DISPLAYCFG get', 'HWPROFILE', 'PING')


def allowed(cmd):
    return cmd in SAFE_EXACT or any(cmd.startswith(p) for p in SAFE_PREFIX)


async def main():
    ip, label, cmds = sys.argv[1], sys.argv[2], sys.argv[3:]
    for cmd in cmds:
        if not allowed(cmd):
            raise SystemExit('refusing non-read-only command: ' + cmd)
    tmo = 20 if ip.endswith('.243') else 15
    out = {'_ip': ip, '_at': time.strftime('%Y-%m-%d %H:%M:%S %Z')}
    c = RetroConnection(ip, 9898)
    await c.connect('retro-agent-secret', timeout=tmo)
    try:
        for cmd in cmds:
            try:
                st, d = await c.send_command(cmd, timeout=90)
                if cmd.startswith('DOWNLOAD '):
                    name = ip.split('.')[-1] + '_' + cmd[9:].replace('\\', '_').replace(':', '').replace(' ', '_')
                    fn = os.path.join(EV, 'files', name)
                    os.makedirs(os.path.dirname(fn), exist_ok=True)
                    with open(fn, 'wb') as f:
                        f.write(d)
                    out[cmd] = {'status': st, 'saved': fn, 'bytes': len(d),
                                'md5': hashlib.md5(d).hexdigest()}
                else:
                    out[cmd] = {'status': st, 'text': d.decode('latin-1')}
            except Exception as e:
                out[cmd] = {'error': repr(e)}
    finally:
        await c.close()
    os.makedirs(EV, exist_ok=True)
    fn = os.path.join(EV, '%s_%s.json' % (ip.split('.')[-1], label))
    with open(fn, 'w') as f:
        json.dump(out, f, indent=1)
    for k, v in out.items():
        if k.startswith('_'):
            continue
        t = v.get('text', v.get('saved', v.get('error', '')))
        extra = (' md5=' + v['md5'] + ' bytes=%d' % v['bytes']) if 'md5' in v else ''
        print('==', ip, k, '[st=%s]' % v.get('status'), '->', (t if len(t) < 6000 else t[:6000] + '...') + extra)


asyncio.run(main())
