import asyncio, json, os, sys
sys.path.insert(0, '/home/voidsstr/development/retro-agent')
from client.retro_protocol import RetroConnection
EV = '/home/voidsstr/development/retro-agent/.claude/evidence-refresh/idtech-q3-ja-sof2-rtcw-d3'
async def main():
    c = RetroConnection('192.168.1.243', 9898)
    await c.connect('retro-agent-secret', timeout=20)
    try:
        st, d = await c.send_command('DIRLIST C:\\Games', timeout=90)
    finally:
        await c.close(graceful=True, drain_timeout=5)
    t = d.decode('latin-1')
    open(os.path.join(EV, 'box_243_dirlist_games.json'), 'w').write(t)
    try:
        names = [e['name'] for e in json.loads(t)]
    except Exception:
        names = t[:500]
    print(st, names)
asyncio.run(main())
