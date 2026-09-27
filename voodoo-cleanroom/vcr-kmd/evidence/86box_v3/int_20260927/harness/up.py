#!/usr/bin/env python3
"""up.py <local> <guest path> - UPLOAD to the 86Box guest only."""
import asyncio, sys
sys.path.insert(0, '/home/voidsstr/development/retro-agent/.claude/worktrees/vk-int')
from client.retro_protocol import RetroConnection
async def main():
    c = RetroConnection('127.0.0.1', 19920)
    await c.connect('retro-agent-secret', timeout=15)
    try:
        st, d = await c.send_command(f'UPLOAD {sys.argv[2]}', binary_payload=open(sys.argv[1], 'rb').read(), timeout=120)
    finally:
        await c.close()
    print(st, d[:200])
    sys.exit(3 if st == 0xFF else 0)
asyncio.run(main())
