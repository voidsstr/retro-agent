# READ-ONLY fact-check probe: DIRLIST / REGREAD / PROCLIST / WINLIST only. Nothing launched, killed or written.
import asyncio, json, sys
sys.path.insert(0, '/home/voidsstr/development/retro-agent/.claude/worktrees/fleet-readiness')
from client.retro_protocol import RetroConnection

PLAN = {
    124: [('vcr', 'DIRLIST C:\\vcr'), ('fleetmp', 'DIRLIST C:\\fleetmp'), ('winlist', 'WINLIST'),
          ('layers', 'REGREAD HKCU Software\\Microsoft\\Windows NT\\CurrentVersion\\AppCompatFlags\\Layers')],
    143: [('layers', 'REGREAD HKCU Software\\Microsoft\\Windows NT\\CurrentVersion\\AppCompatFlags\\Layers'),
          ('layers_lm', 'REGREAD HKLM Software\\Microsoft\\Windows NT\\CurrentVersion\\AppCompatFlags\\Layers')],
    240: [('layers', 'REGREAD HKCU Software\\Microsoft\\Windows NT\\CurrentVersion\\AppCompatFlags\\Layers'),
          ('layers_lm', 'REGREAD HKLM Software\\Microsoft\\Windows NT\\CurrentVersion\\AppCompatFlags\\Layers')],
}

async def probe(o, cmds):
    out = {}
    c = RetroConnection(f'192.168.1.{o}', 9898)
    try:
        await c.connect('retro-agent-secret', timeout=15.0)
    except Exception as e:
        return o, {'connect_error': repr(e)}
    try:
        for key, cmd in cmds:
            try:
                st, data = await c.send_command(cmd)
                out[key] = {'status': st, 'text': data.decode('latin1', 'replace')}
            except Exception as e:
                out[key] = {'error': repr(e)}
    finally:
        await c.close()
    return o, out

async def main():
    res = await asyncio.gather(*[probe(o, cmds) for o, cmds in PLAN.items()])
    json.dump({str(o): r for o, r in res}, open(sys.argv[1], 'w'), indent=1)

asyncio.run(main())
