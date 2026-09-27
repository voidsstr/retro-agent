#!/usr/bin/env python3
"""dl.py <guest path> <local path> - DOWNLOAD from the 86Box guest only."""
import asyncio, sys
sys.path.insert(0, '/home/voidsstr/development/retro-agent/.claude/worktrees/vk-int')
from client.retro_protocol import RetroConnection
async def main():
    c = RetroConnection('127.0.0.1', 19920)
    await c.connect('retro-agent-secret', timeout=15)
    try:
        st, d = await c.send_command(f'DOWNLOAD {sys.argv[1]}', timeout=120)
    finally:
        await c.close()
    if st == 0xFF:
        print('ERR', d[:200]); sys.exit(3)
    open(sys.argv[2], 'wb').write(d)
asyncio.run(main())
