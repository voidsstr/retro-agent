# Read-only agent queries for the refresh review. Usage: q.py <ip> <label> <cmd> [<cmd> ...]
import asyncio, json, os, sys, time
sys.path.insert(0, '/home/voidsstr/development/retro-agent')
from client.retro_protocol import RetroConnection
EV = '/home/voidsstr/development/retro-agent/.claude/evidence-refresh/review-idtech-q3-ja-sof2-rtcw-d3'
SAFE = ('GAMERES', 'DISPLAYCFG get', 'DIRLIST ', 'REGREAD ', 'HWPROFILE', 'DOWNLOAD ', 'PING', 'SYSINFO', 'PROCLIST')
async def main():
    ip, label, cmds = sys.argv[1], sys.argv[2], sys.argv[3:]
    for cmd in cmds:
        if cmd != 'GAMERES' and not any(cmd == s or cmd.startswith(s) for s in SAFE):
            raise SystemExit('refusing non-read-only command: ' + cmd)
        if cmd.startswith('GAMERES') and cmd != 'GAMERES':
            raise SystemExit('refusing GAMERES variant: ' + cmd)
    out = {'_ip': ip, '_at': time.strftime('%Y-%m-%d %H:%M:%S')}
    c = RetroConnection(ip, 9898)
    await c.connect('retro-agent-secret', timeout=15)
    try:
        for cmd in cmds:
            try:
                st, d = await c.send_command(cmd, timeout=90)
                if cmd.startswith('DOWNLOAD '):
                    fn = os.path.join(EV, 'files', ip.split('.')[-1] + '_' + cmd[9:].replace('\\', '_').replace(':', '').replace(' ', '_'))
                    os.makedirs(os.path.dirname(fn), exist_ok=True)
                    open(fn, 'wb').write(d)
                    out[cmd] = {'status': st, 'saved': fn, 'bytes': len(d)}
                else:
                    out[cmd] = {'status': st, 'text': d.decode('latin-1')}
            except Exception as e:
                out[cmd] = {'error': repr(e)}
    finally:
        await c.close()
    fn = os.path.join(EV, '%s_%s.json' % (ip.split('.')[-1], label))
    json.dump(out, open(fn, 'w'), indent=1)
    for k, v in out.items():
        if k.startswith('_'): continue
        t = v.get('text', v.get('saved', v.get('error', '')))
        print('==', ip, k, '->', (t if len(t) < 4000 else t[:4000] + '...'))
asyncio.run(main())
