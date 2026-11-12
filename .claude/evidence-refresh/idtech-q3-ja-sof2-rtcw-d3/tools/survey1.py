import asyncio, json, os, sys
sys.path.insert(0, '/home/voidsstr/development/retro-agent')
from client.retro_protocol import RetroConnection
EV = '/home/voidsstr/development/retro-agent/.claude/evidence-refresh/idtech-q3-ja-sof2-rtcw-d3'
TITLES = ['Quake3-TeamArena', 'JediAcademy', 'SoldierOfFortune2', 'ReturnToCastleWolfenstein', 'Doom3']

async def box(ip, cmds):
    out = {}
    c = RetroConnection(ip, 9898)
    try:
        await c.connect('retro-agent-secret', timeout=15)
    except Exception as e:
        return {'_error': 'connect: %r' % e}
    try:
        for cmd in cmds:
            try:
                st, d = await c.send_command(cmd, timeout=60)
                out[cmd] = {'status': st, 'text': d.decode('latin-1')}
            except Exception as e:
                out[cmd] = {'error': repr(e)}
    finally:
        await c.close()
    return out

async def main():
    ips = sys.argv[1:]
    for ip in ips:
        cmds = ['HWPROFILE', 'GAMERES', 'DISPLAYCFG get', 'DIRLIST C:\\Games',
                'DIRLIST C:\\Documents and Settings', 'DIRLIST C:\\Users']
        r = await box(ip, cmds)
        fn = os.path.join(EV, 'box_%s_base.json' % ip.split('.')[-1])
        json.dump(r, open(fn, 'w'), indent=1)
        print('==', ip, 'saved', fn)
        for k, v in r.items():
            t = v.get('text', v.get('error', '')) if isinstance(v, dict) else str(v)
            print('  ', k, '->', t[:300].replace('\n', ' '))
asyncio.run(main())
