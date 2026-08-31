"""Shared helpers for the per-box verification matrix sweep."""
import asyncio, json, os, sys, time
sys.path.insert(0, '/home/voidsstr/development/retro-agent/.claude/worktrees/perbox')
from client.retro_protocol import RetroConnection

SECRET = 'retro-agent-secret'
BOXES = ['192.168.1.123','192.168.1.124','192.168.1.133','192.168.1.143',
         '192.168.1.171','192.168.1.240','192.168.1.246','192.168.1.243']
SHOTDIR = '/home/voidsstr/development/retro-agent/.claude/worktrees/perbox/evidence'

class Box:
    def __init__(self, ip, timeout=20.0):
        self.ip, self.timeout, self.c = ip, timeout, None
    async def __aenter__(self):
        last = None
        for attempt in range(4):
            try:
                self.c = RetroConnection(self.ip, 9898)
                await self.c.connect(SECRET, timeout=self.timeout)
                return self
            except Exception as e:
                last = e
                self.c = None
                await asyncio.sleep(1.5 * (attempt + 1))
        raise last
    async def __aexit__(self, *a):
        if self.c:
            try: await self.c.close()
            except Exception: pass
    async def cmd(self, s, timeout=None):
        st, d = await self.c.send_command(s)
        return d.decode('ascii', errors='replace')
    async def binary(self, s):
        return await self.c.command_binary(s)

async def gather_boxes(fn, boxes=None, timeout=20.0):
    boxes = boxes or BOXES
    async def one(ip):
        try:
            async with Box(ip, timeout) as b:
                return ip, await fn(b)
        except Exception as e:
            return ip, {'_error': f'{type(e).__name__}: {e}'}
    return dict(await asyncio.gather(*[one(i) for i in boxes]))

def jload(s):
    try: return json.loads(s)
    except Exception: return None
