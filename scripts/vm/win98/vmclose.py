#!/usr/bin/env python3
"""vmclose.py - close every game window on the Win98 build VM the way sweep.py
does (WM_CLOSE + the right answer to each dialog), then hand the screen back
from the Voodoo 2. For after a trial.py --keep-open."""
import asyncio
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import sweep  # noqa: E402


async def main():
    c = await sweep.connect()
    await sweep.ensure_helpers(c)
    await sweep.agent_pids(c)
    if not sweep.AGENT_PIDS:
        raise SystemExit('could not find the agent process - refusing to close windows blind')
    left = await sweep.close_all(c)
    print('GLRESET:', await sweep.glreset(c))
    left = await sweep.close_all(c)
    print('still open:', left)
    await c.close()


if __name__ == '__main__':
    asyncio.run(main())
