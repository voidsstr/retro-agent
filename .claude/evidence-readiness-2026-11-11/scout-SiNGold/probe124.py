# READ-ONLY: REGREAD the OpenGL ICD registration and HWPROFILE-free checks on .124
import asyncio, sys
sys.path.insert(0, '/home/voidsstr/development/retro-agent')
from client.retro_protocol import RetroConnection
async def main():
    c = RetroConnection('192.168.1.124', 9898)
    try:
        await c.connect('retro-agent-secret', timeout=15)
        for cmd in ['REGREAD HKLM SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\OpenGLDrivers',
                    'DIRLIST C:\\vcr']:
            try:
                r = await c.command_text(cmd, timeout=30)
            except Exception as e:
                r = f'ERR {e!r}'
            print('>>', cmd, '\n', r[:1500])
        # subkeys if any
        try:
            r = await c.command_text('REGREAD HKLM SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\OpenGLDrivers\\retroicd', timeout=30)
            print('>> retroicd\n', r[:800])
        except Exception as e:
            print('>> retroicd ERR', e)
    finally:
        try: await c.close()
        except Exception: pass
asyncio.run(main())
