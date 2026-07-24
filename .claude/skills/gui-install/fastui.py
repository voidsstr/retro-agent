#!/usr/bin/env python3
"""
fastui.py - real-time GUI automation client for the retro agent.

The speed comes from two things:
  1. ONE persistent authenticated connection per box (no reconnect/auth per
     action - that was the dominant latency in the old screenshot-click loop).
  2. Delta frames: SCREENDIFF / CLICKSHOT return only the 64x64 tiles that
     changed, so a click's visual result is a few KB, not a 2 MB BMP.

Agent commands used (agent >= v1.18.0 for CLICKSHOT):
  SCREENDIFF [FULL]         -> dirty-tile delta vs the agent's previous frame
  CLICKSHOT x y [right|dbl] [settle_ms] -> click, settle, return the delta
  WINLIST / UIKEY / UICLICK / LAUNCH / EXEC ... -> as usual

Binary delta format (little-endian):
  header: u16 screen_w, u16 screen_h, u16 tile_size, u16 num_tiles
  per tile: u16 x, u16 y, u16 w, u16 h, then w*h*3 bytes TOP-DOWN BGR

Usage as a library:
    ui = FastUI('192.168.1.133')
    await ui.connect()
    await ui.baseline()                 # SCREENDIFF FULL -> full frame
    img = await ui.clickshot(622, 518)  # click + get updated frame (PIL Image)
    img.save('shot.png')
    await ui.close()

CLI (quick check):
    python3 fastui.py <ip> shot out.png
    python3 fastui.py <ip> click <x> <y> out.png
    python3 fastui.py <ip> bench 20        # 20 clickshots, report ms/frame
"""
import os, sys, struct, asyncio, time

# import the repo client lib regardless of CWD
_REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..', '..'))
for p in (_REPO, os.path.join(_REPO, '..', 'retro-agent')):
    if p not in sys.path:
        sys.path.insert(0, p)
from client.retro_protocol import RetroConnection

try:
    from PIL import Image
except ImportError:
    Image = None

SECRET = 'retro-agent-secret'


class FastUI:
    def __init__(self, ip, port=9898, secret=SECRET):
        self.ip, self.port, self.secret = ip, port, secret
        self.c = None
        self.w = self.h = 0
        self.buf = None          # bytearray of w*h*3, top-down RGB

    async def connect(self, timeout=12.0):
        self.c = RetroConnection(self.ip, self.port)
        await self.c.connect(self.secret, timeout=timeout)
        return self

    async def close(self):
        if self.c:
            try: await self.c.close()
            except Exception: pass
            self.c = None

    # ---- low level ----
    async def _apply(self, blob):
        """Apply a SCREENDIFF/CLICKSHOT delta blob to the local framebuffer."""
        if not blob or len(blob) < 8:
            return
        w, h, tile, n = struct.unpack_from('<HHHH', blob, 0)
        if (self.buf is None) or (w != self.w) or (h != self.h):
            self.w, self.h = w, h
            self.buf = bytearray(w * h * 3)      # RGB, top-down
        pos = 8
        for _ in range(n):
            tx, ty, tw, th = struct.unpack_from('<HHHH', blob, pos); pos += 8
            for row in range(th):
                dst = ((ty + row) * w + tx) * 3
                src = pos + row * tw * 3
                # source row is BGR; swap into the RGB framebuffer
                self.buf[dst:dst + tw * 3] = self._bgr2rgb(blob[src:src + tw * 3])
            pos += tw * th * 3

    @staticmethod
    def _bgr2rgb(line):
        mv = memoryview(line)
        out = bytearray(len(line))
        out[0::3] = mv[2::3]
        out[1::3] = mv[1::3]
        out[2::3] = mv[0::3]
        return out

    def image(self):
        if Image is None or self.buf is None:
            return None
        return Image.frombytes('RGB', (self.w, self.h), bytes(self.buf))

    # ---- high level ----
    async def baseline(self):
        """Full frame (resets the agent's delta cache)."""
        blob = await self.c.command_binary('SCREENDIFF FULL')
        await self._apply(blob)
        return self.image()

    async def refresh(self):
        blob = await self.c.command_binary('SCREENDIFF')
        await self._apply(blob)
        return self.image()

    async def clickshot(self, x, y, button='', settle_ms=60):
        """Click and return the updated frame in one round trip."""
        extra = (button + ' ' if button else '') + str(settle_ms)
        blob = await self.c.command_binary(f'CLICKSHOT {int(x)} {int(y)} {extra}')
        await self._apply(blob)
        return self.image()

    async def text(self, cmd, t=60):
        return await self.c.command_text(cmd, timeout=t) if hasattr(self.c, 'command_text') \
            else (await self.c.send_command(cmd, timeout=t))[1].decode('ascii', 'replace')

    async def winlist(self):
        import json
        try:
            return json.loads(await self.text('WINLIST', 20)).get('windows', [])
        except Exception:
            return []

    async def key(self, spec):
        return await self.text(f'UIKEY {spec}', 20)


# --------------------------- CLI ---------------------------
async def _main():
    if len(sys.argv) < 3:
        print(__doc__); return
    ip, mode = sys.argv[1], sys.argv[2]
    ui = FastUI(ip); await ui.connect()
    if mode == 'shot':
        img = await ui.baseline(); out = sys.argv[3] if len(sys.argv) > 3 else 'shot.png'
        img.save(out); print('saved', out, ui.w, 'x', ui.h)
    elif mode == 'click':
        x, y = int(sys.argv[3]), int(sys.argv[4])
        await ui.baseline()
        img = await ui.clickshot(x, y); out = sys.argv[5] if len(sys.argv) > 5 else 'shot.png'
        img.save(out); print('clicked; saved', out)
    elif mode == 'bench':
        n = int(sys.argv[3]) if len(sys.argv) > 3 else 20
        await ui.baseline()
        t0 = time.time()
        for i in range(n):
            await ui.refresh()
        dt = (time.time() - t0) / n * 1000
        print(f'{n} SCREENDIFF frames: {dt:.1f} ms/frame avg ({ui.w}x{ui.h})')
    await ui.close()

if __name__ == '__main__':
    asyncio.run(_main())
