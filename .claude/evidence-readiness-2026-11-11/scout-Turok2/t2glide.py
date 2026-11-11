import asyncio, json, sys
sys.path.insert(0, '/home/voidsstr/development/retro-agent')
from client.retro_protocol import RetroConnection
BOXES = sys.argv[1].split(',')
async def one(ip):
    out = {'ip': ip}
    c = RetroConnection(ip, 9898)
    try:
        await c.connect('retro-agent-secret', timeout=20)
    except Exception as e:
        out['error'] = repr(e); return out
    try:
        for p in (r'C:\WINDOWS\system32\glide*', r'C:\Games\Turok2\glide*'):
            st, d = await c.send_command('DIRLIST ' + p)
            try:
                out[p] = [(x['name'], x['size']) for x in json.loads(d.decode('latin1'))]
            except Exception:
                out[p] = d.decode('latin1', 'replace')[:200]
        st, d = await c.send_command('WINLIST')
        out['winlist_fg'] = d.decode('latin1', 'replace')[:300]
    finally:
        try: await c.close()
        except Exception: pass
    return out
async def main():
    for r in await asyncio.gather(*[one(ip) for ip in BOXES]):
        print(json.dumps(r)[:900])
asyncio.run(main())
