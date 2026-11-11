# READ-ONLY: DOWNLOAD qconsole.log + DIRLIST players\blade (+ DOWNLOAD its config.cfg if any)
import asyncio, json, sys, os
sys.path.insert(0, '/home/voidsstr/development/retro-agent')
from client.retro_protocol import RetroConnection
OUT = os.path.dirname(os.path.abspath(__file__))
BOXES = [123, 133, 143, 240, 124, 171, 184]

async def one(o):
    c = RetroConnection(f'192.168.1.{o}', 9898)
    root = 'C:\\Games\\SiNGold\\base'
    try:
        await c.connect('retro-agent-secret', timeout=15)
        try:
            b = await c.command_binary(f'DOWNLOAD {root}\\qconsole.log', timeout=40)
            open(os.path.join(OUT, f'qconsole_{o}.log'), 'wb').write(b)
        except Exception as e:
            open(os.path.join(OUT, f'qconsole_{o}.log'), 'w').write(f'ERR {e!r}')
        try:
            t = await c.command_text(f'DIRLIST {root}\\players\\blade', timeout=30)
        except Exception as e:
            t = f'ERR {e!r}'
        open(os.path.join(OUT, f'blade_{o}.json'), 'w').write(t)
        if 'config.cfg' in t.lower():
            try:
                b = await c.command_binary(f'DOWNLOAD {root}\\players\\blade\\config.cfg', timeout=30)
                open(os.path.join(OUT, f'bladecfg_{o}.cfg'), 'wb').write(b)
            except Exception as e:
                open(os.path.join(OUT, f'bladecfg_{o}.cfg'), 'w').write(f'ERR {e!r}')
    finally:
        try: await c.close()
        except Exception: pass
    return o

async def main():
    print(await asyncio.gather(*[one(o) for o in BOXES], return_exceptions=True))
asyncio.run(main())
