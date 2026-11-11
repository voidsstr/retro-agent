# READ-ONLY on the Win98 box: DIRLIST only (agent-internal, spawns nothing). Graceful close.
import asyncio, json, sys, os
sys.path.insert(0, '/home/voidsstr/development/retro-agent')
from client.retro_protocol import RetroConnection
OUT = os.path.dirname(os.path.abspath(__file__))
async def main():
    c = RetroConnection('192.168.1.243', 9898)
    res = {}
    try:
        await c.connect('retro-agent-secret', timeout=20)
        for k, cmd in [('root', 'DIRLIST E:\\GAMES\\SiNGold'), ('base', 'DIRLIST E:\\GAMES\\SiNGold\\base'),
                       ('players', 'DIRLIST E:\\GAMES\\SiNGold\\base\\players')]:
            try:
                res[k] = await c.command_text(cmd, timeout=45)
            except Exception as e:
                res[k] = f'ERR {e!r}'
    except Exception as e:
        res['connect'] = f'ERR {e!r}'
    finally:
        try: await c.close()
        except Exception: pass
    json.dump(res, open(os.path.join(OUT, 'box_243.json'), 'w'), indent=1)
    print({k: (v[:120] if isinstance(v, str) else v) for k, v in res.items()})
asyncio.run(main())
