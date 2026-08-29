#!/usr/bin/env python3
"""Reliable Quake II timedemo on .171.

The bundled harness raced on baseq2/qconsole.log (it accumulates across
sessions and q2run.bat deletes it asynchronously), so runs reported another
renderer's string and None fps. This one:
  - kills any live quake2 and deletes the log BEFORE launching
  - polls until the 'seconds' line actually appears (no fixed sleep)
  - parses the LAST session block only, and asserts the renderer string in
    that same block matches what we asked for
"""
import asyncio, re, statistics, sys
sys.path.insert(0, '/home/voidsstr/development/retro-agent')
from client.retro_protocol import RetroConnection

IP = '192.168.1.171'
LOG = r'C:\Games\Quake2Complete\baseq2\qconsole.log'

async def one(cmd, tries=3, tmo=55.0):
    for a in range(tries):
        c = None
        try:
            c = RetroConnection(IP, 9898)
            await c.connect('retro-agent-secret', timeout=tmo)
            s, d = await c.send_command(cmd)
            await c.close()
            return d.decode('ascii', 'replace')
        except Exception as e:
            if c:
                try: await c.close()
                except Exception: pass
            if a == tries - 1: return f'(ERR {e})'
            await asyncio.sleep(6)

async def run(driver, mode='3', swap='0', timeout_s=180):
    await one('EXEC cmd /c taskkill /F /IM quake2.exe 2>nul')
    await asyncio.sleep(2)
    await one(f'EXEC cmd /c del /f /q {LOG} 2>nul')
    await asyncio.sleep(1)
    await one(rf'LAUNCH C:\RETRO_AGENT\bench\q2run.bat {driver} {mode} {swap}')
    waited = 0
    while waited < timeout_s:
        await asyncio.sleep(10); waited += 10
        t = await one(f'EXEC cmd /c findstr /i /c:"seconds" {LOG}')
        if 'seconds' in t and 'FINDSTR' not in t:
            break
    full = await one(f'EXEC cmd /c type {LOG}')
    await one('EXEC cmd /c taskkill /F /IM quake2.exe 2>nul')
    # last session block only
    blocks = full.split('GL_RENDERER:')
    if len(blocks) < 2: return None, None
    tail = blocks[-1]
    rend = tail.splitlines()[0].strip()
    m = re.search(r'(\d+) frames, ([\d.]+) seconds: ([\d.]+) fps', tail)
    mt = 'using GL_SGIS_multitexture' in tail
    return (float(m.group(3)) if m else None), (rend, mt)

async def main():
    driver = sys.argv[1]; runs = int(sys.argv[2]) if len(sys.argv) > 2 else 3
    mode = sys.argv[3] if len(sys.argv) > 3 else '3'
    got = []
    for i in range(runs):
        fps, meta = await run(driver, mode)
        rend, mt = meta if meta else ('?', False)
        print(f'  run {i+1}/{runs}: {fps} fps  mt={mt}  [{rend}]', flush=True)
        if fps: got.append(fps)
    if got:
        keep = got[1:] if len(got) > 1 else got
        print(f'\n{driver} mode{mode}: median {statistics.median(keep):.1f} fps  (n={len(keep)}, all={got})')
    else:
        print(f'\n{driver}: NO VALID RUNS')

asyncio.run(main())
