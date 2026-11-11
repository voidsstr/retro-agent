# READ-ONLY: REGREAD the 3dfx ICD subkey and look for vcrctl.exe on .124
import asyncio, sys, json
sys.path.insert(0, '/home/voidsstr/development/retro-agent')
from client.retro_protocol import RetroConnection
async def main():
    c = RetroConnection('192.168.1.124', 9898)
    try:
        await c.connect('retro-agent-secret', timeout=15)
        r = await c.command_text('REGREAD HKLM SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\OpenGLDrivers\\3dfx', timeout=30)
        print('>> 3dfx ICD\n', r[:1500])
        r = await c.command_text('DIRLIST C:\\vcr', timeout=30)
        names = [e['name'] for e in json.loads(r)]
        print('>> vcr has vcrctl.exe:', [n for n in names if n.lower().startswith('vcrctl')])
    finally:
        try: await c.close()
        except Exception: pass
asyncio.run(main())
