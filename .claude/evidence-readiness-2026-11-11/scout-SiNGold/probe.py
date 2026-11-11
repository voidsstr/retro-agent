# READ-ONLY probe of the SiNGold tree on each box: DIRLIST + DOWNLOAD only.
import asyncio, json, sys, os
sys.path.insert(0, '/home/voidsstr/development/retro-agent')
from client.retro_protocol import RetroConnection

OUT = os.path.dirname(os.path.abspath(__file__))
BOXES = {110: 'C:\\Games', 123: 'C:\\Games', 124: 'C:\\Games', 133: 'C:\\Games', 143: 'C:\\Games',
         171: 'C:\\Games', 184: 'C:\\Games', 186: 'C:\\Games', 240: 'C:\\Games'}

async def one(o, gdir):
    c = RetroConnection(f'192.168.1.{o}', 9898)
    res = {}
    try:
        await c.connect('retro-agent-secret', timeout=15)
        root = gdir + '\\SiNGold'
        for key, cmd in [('root', f'DIRLIST {root}'), ('base', f'DIRLIST {root}\\base'),
                         ('players', f'DIRLIST {root}\\base\\players')]:
            try:
                res[key] = await c.command_text(cmd, timeout=30)
            except Exception as e:
                res[key] = f'ERR {e!r}'
        try:
            b = await c.command_binary(f'DOWNLOAD {root}\\base\\fleetres.cfg', timeout=30)
            res['fleetres'] = b.decode('latin1')
        except Exception as e:
            res['fleetres'] = f'ERR {e!r}'
    except Exception as e:
        res['connect'] = f'ERR {e!r}'
    finally:
        try:
            await c.close()
        except Exception:
            pass
    with open(os.path.join(OUT, f'box_{o}.json'), 'w') as f:
        json.dump(res, f, indent=1)
    return o

async def main():
    r = await asyncio.gather(*[one(o, g) for o, g in BOXES.items()], return_exceptions=True)
    print(r)

asyncio.run(main())
