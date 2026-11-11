"""READ-ONLY: _disc listing + lanhost.txt + WINLIST foreground per box."""
import asyncio, json, os, sys
sys.path.insert(0, '/home/voidsstr/development/retro-agent')
from client.retro_protocol import RetroConnection
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'boxes')
T = r'C:\Games\SeriousSamFirstEncounter'
async def probe(o):
    rep = {'box': o}
    c = RetroConnection(f'192.168.1.{o}', 9898)
    await c.connect('retro-agent-secret', timeout=20.0)
    try:
        async def txt(cmd):
            try: return await c.command_text(cmd)
            except Exception as e: return f'ERR {e!r}'
        r = await txt(f'DIRLIST {T}\\_disc'); rep['_disc'] = r if r.startswith('ERR') else json.loads(r)
        r = await txt(r'DIRLIST C:\Games'); rep['games_lanhost'] = r if r.startswith('ERR') else [x for x in json.loads(r) if x['name'].lower()=='lanhost.txt']
        try:
            b = await c.command_binary(r'DOWNLOAD C:\Games\lanhost.txt'); rep['lanhost'] = b.decode('latin1')
        except Exception as e:
            rep['lanhost'] = f'ERR {e!r}'
        r = await txt('WINLIST'); 
        try:
            w = json.loads(r); rep['foreground'] = w.get('foreground') if isinstance(w, dict) else None
        except Exception: rep['foreground'] = r[:200]
        rep['hwprofile_os'] = (await txt('HWPROFILE'))[:1500]
    finally:
        await c.close()
    return rep
async def main():
    boxes=[110,123,124,133,143,171,184,240]
    res = await asyncio.gather(*[probe(o) for o in boxes], return_exceptions=True)
    for o, r in zip(boxes, res):
        if isinstance(r, Exception): r={'box':o,'error':repr(r)}
        json.dump(r, open(os.path.join(OUT, f'probe2_{o}.json'), 'w'), indent=1)
        d=r.get('_disc'); 
        print(o, [ (x['name'], x['size']) for x in d] if isinstance(d,list) else d, '| lanhost:', repr(r.get('lanhost'))[:60], '| fg:', (r.get('foreground') or {}).get('title') if isinstance(r.get('foreground'),dict) else r.get('foreground'))
asyncio.run(main())
