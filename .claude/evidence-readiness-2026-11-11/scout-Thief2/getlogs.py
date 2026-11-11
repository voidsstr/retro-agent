#!/usr/bin/env python3
"""READ-ONLY: DOWNLOAD each box's NewDark Thief2.log (2026-11-11 scout)."""
import asyncio, os, sys
sys.path.insert(0, "/home/voidsstr/development/retro-agent")
from client.retro_protocol import RetroConnection
OUT = os.path.dirname(os.path.abspath(__file__))
async def get(o):
    c = RetroConnection(f"192.168.1.{o}", 9898)
    await c.connect("retro-agent-secret", timeout=15)
    try:
        b = await c.command_binary("DOWNLOAD C:\\Games\\Thief2\\Thief2.log")
        open(os.path.join(OUT, f"Thief2_{o}.log"), "wb").write(b)
        return o, len(b)
    except Exception as e:
        return o, repr(e)
    finally:
        await c.close()
async def main():
    for r in await asyncio.gather(*[get(o) for o in [123, 124, 133, 143, 171, 184, 240]], return_exceptions=True):
        print(r)
asyncio.run(main())
