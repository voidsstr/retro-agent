import asyncio, json, os, sys
sys.path.insert(0, '/home/voidsstr/development/retro-agent')
from client.retro_protocol import RetroConnection
EV = '/home/voidsstr/development/retro-agent/.claude/evidence-refresh/idtech-q3-ja-sof2-rtcw-d3'
async def main():
    ip = sys.argv[1]; keys = sys.argv[2:]
    c = RetroConnection(ip, 9898)
    await c.connect('retro-agent-secret', timeout=15)
    out = {}
    try:
        for k in keys:
            st, d = await c.send_command('REGREAD ' + k, timeout=60)
            out[k] = d.decode('latin-1')
            print('== REGREAD', k, '(status %d)' % st)
            print(out[k][:3000])
    finally:
        await c.close()
    fn = os.path.join(EV, 'reg_%s.json' % ip.split('.')[-1])
    old = json.load(open(fn)) if os.path.exists(fn) else {}
    old.update(out)
    json.dump(old, open(fn, 'w'), indent=1)
asyncio.run(main())
