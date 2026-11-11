# READ-ONLY: which drive letters are CD-ROMs and what volume is in each; .123's leftover mount-error.txt.
import asyncio, json, sys
sys.path.insert(0, '/home/voidsstr/development/retro-agent/.claude/worktrees/fleet-readiness')
from client.retro_protocol import RetroConnection

BOXES = [110, 123, 124, 133, 143, 171, 184, 240]
CMDS = [
    ('types', 'EXEC cmd /c fsutil fsinfo drivetype D: & fsutil fsinfo drivetype E: & fsutil fsinfo drivetype F: & fsutil fsinfo drivetype G: & fsutil fsinfo drivetype H:'),
    ('vols', 'EXEC cmd /c vol D: & vol E: & vol F: & vol G: & vol H:'),
    ('ssecure', 'REGREAD HKCU Control Panel\\Desktop'),
]

async def probe(o):
    out = {}
    c = RetroConnection(f'192.168.1.{o}', 9898)
    try:
        await c.connect('retro-agent-secret', timeout=15.0)
    except Exception as e:
        return o, {'connect_error': repr(e)}
    try:
        for key, cmd in CMDS:
            try:
                st, data = await c.send_command(cmd)
                out[key] = {'status': st, 'text': data.decode('latin1', 'replace')}
            except Exception as e:
                out[key] = {'error': repr(e)}
        if o == 123:
            try:
                b = await c.command_binary('DOWNLOAD C:\\Games\\SystemShock2\\mount-error.txt')
                out['mount_error'] = b.decode('latin1', 'replace')
            except Exception as e:
                out['mount_error'] = repr(e)
            try:
                st, data = await c.send_command('DIRLIST C:\\Games\\SystemShock2')
                j = json.loads(data.decode('latin1', 'replace'))
                ents = j if isinstance(j, list) else j.get('entries', j.get('files', []))
                out['mount_error_meta'] = [e for e in ents if (e.get('name') or '').lower() == 'mount-error.txt']
            except Exception as e:
                out['mount_error_meta'] = repr(e)
    finally:
        await c.close()
    return o, out

async def main():
    res = await asyncio.gather(*[probe(o) for o in BOXES])
    json.dump({str(o): r for o, r in res}, open(sys.argv[1], 'w'), indent=1)

asyncio.run(main())
