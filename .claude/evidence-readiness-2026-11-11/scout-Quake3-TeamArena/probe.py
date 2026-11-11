import asyncio, json, sys, os
sys.path.insert(0, '/home/voidsstr/development/retro-agent')
from client.retro_protocol import RetroConnection

OUT = os.path.dirname(os.path.abspath(__file__))
BOXES = [110, 123, 124, 133, 143, 171, 184, 240, 186]
CMDS = [
    'DIRLIST C:\\Games\\Quake3-TeamArena',
    'DIRLIST C:\\Games\\Quake3-TeamArena\\missionpack',
    'DIRLIST C:\\WINDOWS\\system32\\3dfx*',
    'REGREAD HKLM Software\\RetroAgent',
    'REGREAD HKLM SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Winlogon',
    'DIRLIST C:\\Documents and Settings',
    'PROCLIST',
]

async def one(o):
    res = {}
    c = RetroConnection(f'192.168.1.{o}', 9898)
    try:
        await c.connect('retro-agent-secret', timeout=20.0)
        for cmd in CMDS:
            if o == 186 and cmd.startswith('DIRLIST C:\\Documents'):
                cmd = 'DIRLIST C:\\Users'
            try:
                st, data = await asyncio.wait_for(c.send_command(cmd), timeout=40)
                res[cmd] = {'status': st, 'text': data.decode('latin1', 'replace')}
            except Exception as e:
                res[cmd] = {'error': repr(e)}
    except Exception as e:
        res['connect_error'] = repr(e)
    finally:
        try:
            await c.close()
        except Exception:
            pass
    with open(os.path.join(OUT, f'probe_{o}.json'), 'w') as f:
        json.dump(res, f, indent=1)
    return o, list(res.keys())

async def main():
    for r in await asyncio.gather(*[one(o) for o in BOXES], return_exceptions=True):
        print(r)

asyncio.run(main())
