#!/usr/bin/env python3
"""Read-only DOWNLOAD of named files from one box into boxes/<last octet>/<sanitized path>."""
import asyncio, sys, os, re
sys.path.insert(0, '/home/voidsstr/development/retro-agent')
from client.retro_protocol import RetroConnection
EVID = '/home/voidsstr/development/retro-agent/.claude/evidence-refresh/secondary-shooters-review/boxes'
async def main(ip, paths):
    tag = ip.split('.')[-1]
    c = RetroConnection(ip, 9898)
    await c.connect('retro-agent-secret', timeout=15)
    try:
        for p in paths:
            st, d = await c.send_command('DOWNLOAD ' + p)
            out = os.path.join(EVID, tag, re.sub(r'[\\/:]+', '__', p))
            os.makedirs(os.path.dirname(out), exist_ok=True)
            if d.startswith(b'Cannot open') or (st not in (0,1)) or (st == 0 and len(d) < 200 and b'rror' in d):
                open(out + '.MISSING', 'w').write('status=%s %s' % (st, d[:200].decode('latin-1','replace')))
                print('%s MISS %s status=%s %r' % (tag, p, st, d[:100]))
            else:
                open(out, 'wb').write(d)
                print('%s OK %8d %s' % (tag, len(d), p))
    finally:
        await c.close()
asyncio.run(main(sys.argv[1], sys.argv[2:]))
