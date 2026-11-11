# READ-ONLY probe of System Shock 2 readiness on the XP boxes. Nothing is launched, killed or written.
import asyncio, json, sys
sys.path.insert(0, '/home/voidsstr/development/retro-agent/.claude/worktrees/fleet-readiness')
from client.retro_protocol import RetroConnection

BOXES = [110, 123, 124, 133, 143, 171, 184, 240]
CMDS = [
    ('dir_game', 'DIRLIST C:\\Games\\SystemShock2'),
    ('dir_disc', 'DIRLIST C:\\Games\\SystemShock2\\_disc'),
    ('dir_dt', 'DIRLIST C:\\Program Files\\D-Tools'),
    ('secdrv_reg', 'REGREAD HKLM SYSTEM\\CurrentControlSet\\Services\\secdrv'),
    ('appcompat', 'REGREAD HKCU Software\\Microsoft\\Windows NT\\CurrentVersion\\AppCompatFlags'),
    ('indeo', 'REGREAD HKLM SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Drivers32'),
    ('sc_secdrv', 'EXEC sc query secdrv'),
    ('drives', 'EXEC cmd /c for %d in (D E F G H I J) do @(fsutil fsinfo drivetype %d: 2>nul & vol %d: 2>nul | find "drive")'),
    ('proclist', 'PROCLIST'),
    ('displaycfg', 'DISPLAYCFG get'),
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
    finally:
        await c.close()
    return o, out

async def main():
    res = await asyncio.gather(*[probe(o) for o in BOXES])
    json.dump({str(o): r for o, r in res}, open(sys.argv[1], 'w'), indent=1)

asyncio.run(main())
