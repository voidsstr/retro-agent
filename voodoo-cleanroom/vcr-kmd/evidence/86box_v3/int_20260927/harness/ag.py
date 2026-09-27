#!/usr/bin/env python3
"""ag.py CMD... - one agent command against the 86Box guest ONLY (127.0.0.1:19920)."""
import asyncio, sys
sys.path.insert(0, '/home/voidsstr/development/retro-agent/.claude/worktrees/vk-int')
from client.retro_protocol import RetroConnection
HOST, PORT = '127.0.0.1', 19920
async def main():
    cmd = ' '.join(sys.argv[1:])
    t = 120
    if cmd.startswith('EXECW '):
        t = int(cmd.split()[1]) + 30
    c = RetroConnection(HOST, PORT)
    await c.connect('retro-agent-secret', timeout=15)
    try:
        st, d = await c.send_command(cmd, timeout=t)
    finally:
        await c.close()
    sys.stdout.write(d.decode('latin1', 'replace'))
    if not d.endswith(b'\n'):
        sys.stdout.write('\n')
    sys.exit(3 if st == 0xFF else 0)
asyncio.run(main())
