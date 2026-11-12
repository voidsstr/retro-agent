#!/usr/bin/env python3
"""READ-ONLY: DOWNLOAD small text files from a box into boxes/<ip>/ (no writes on the box)."""
import asyncio, os, sys
sys.path.insert(0, '/home/voidsstr/development/retro-agent')
from client.retro_protocol import RetroConnection
async def main(ip, paths):
    out = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'boxes', ip.replace('.', '_'))
    os.makedirs(out, exist_ok=True)
    c = RetroConnection(ip, 9898)
    await c.connect('retro-agent-secret', timeout=20)
    try:
        for p in paths:
            try:
                st, d = await c.send_command('DOWNLOAD ' + p)
            except Exception as e:
                print('!!', p, repr(e)); continue
            if st == 0xFF:
                print('!!', p, d[:200].decode('latin1', 'replace')); continue
            fn = os.path.join(out, p.replace(':', '').replace('\\', '__'))
            open(fn, 'wb').write(d)
            print('==', p, len(d), 'bytes ->', fn)
    finally:
        await c.close()
asyncio.run(main(sys.argv[1], sys.argv[2:]))
