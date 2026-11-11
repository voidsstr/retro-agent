# READ-ONLY: DAEMON Tools 3.47 emulation config blob per box (d347bus\Cfg) - compare boxes that passed SafeDisc with the untested ones.
import asyncio, json, sys
sys.path.insert(0, '/home/voidsstr/development/retro-agent/.claude/worktrees/fleet-readiness')
from client.retro_protocol import RetroConnection
BOXES = [110, 123, 124, 133, 143, 171, 184, 240]
async def probe(o):
    c = RetroConnection(f'192.168.1.{o}', 9898)
    try:
        await c.connect('retro-agent-secret', timeout=15.0)
    except Exception as e:
        return o, {'connect_error': repr(e)}
    out = {}
    try:
        for key, cmd in [('cfg', 'REGREAD HKLM SYSTEM\\CurrentControlSet\\Services\\d347bus\\Cfg'),
                         ('svc', 'REGREAD HKLM SYSTEM\\CurrentControlSet\\Services\\d347bus'),
                         ('prt', 'REGREAD HKLM SYSTEM\\CurrentControlSet\\Services\\d347prt')]:
            try:
                st, d = await c.send_command(cmd)
                out[key] = d.decode('latin1', 'replace')
            except Exception as e:
                out[key] = repr(e)
    finally:
        await c.close()
    return o, out
async def main():
    res = await asyncio.gather(*[probe(o) for o in BOXES])
    json.dump({str(o): r for o, r in res}, open(sys.argv[1], 'w'), indent=1)
asyncio.run(main())
