#!/usr/bin/env python3
"""READ-ONLY opportunistic measurement on .124. The V5 lane's own lan_sweep.py is
launching every desktop shortcut in turn (its log: sweep_armed3.log). When the
sweep starts a GoldSrc/Glide title we send ONLY `DISPLAYCFG get` (EnumDisplay-
Settings ENUM_CURRENT_SETTINGS) at +25 s and +45 s, to read the live refresh
the title runs at now that FX_GLIDE_REFRESH was deleted (04:17:07). Nothing is
launched, written or switched by this script."""
import asyncio, sys, time, re, json, os
sys.path.insert(0, '/home/voidsstr/development/retro-agent')
from client.retro_protocol import RetroConnection
LOG = '/home/voidsstr/development/retro-agent/.claude/worktrees/v56k-bench/scripts/benchmarks/results/v56k_lan_192.168.1.124/sweep_armed3.log'
OUT = '/home/voidsstr/development/retro-agent/.claude/evidence-refresh/hl1-cs16-review/live/124-sweep-displaycfg.log'
WANT = re.compile(r'--- (Team Fortress Classic|Half-Life|Counter-Strike|Deathmatch Classic|Quake III[^:]*):')
DEADLINE = time.time() + 80 * 60

def out(s):
    line = '%s %s' % (time.strftime('%H:%M:%S'), s)
    print(line, flush=True)
    with open(OUT, 'a') as f:
        f.write(line + '\n')

async def dcfg(tag):
    c = RetroConnection('192.168.1.124', 9898)
    try:
        await c.connect('retro-agent-secret', timeout=15)
        st, d = await asyncio.wait_for(c.send_command('DISPLAYCFG get'), timeout=30)
        out('%s DISPLAYCFG get -> %s' % (tag, d.decode('latin-1')))
    except Exception as e:
        out('%s DISPLAYCFG get failed: %r' % (tag, e))
    finally:
        try:
            await c.close()
        except Exception:
            pass

async def main():
    seen = set()
    pos = os.path.getsize(LOG)          # only titles started from now on
    out('watching %s from byte %d' % (LOG, pos))
    while time.time() < DEADLINE:
        with open(LOG, 'rb') as f:
            f.seek(pos)
            chunk = f.read().decode('latin-1', 'replace')
            pos = f.tell()
        for line in chunk.splitlines():
            out('sweep: ' + line[:200]) if ('---' in line or 'PASS' in line or 'FAIL' in line or 'CHECK' in line or 'STOP' in line.upper()) else None
            m = WANT.search(line)
            if m and line not in seen:
                seen.add(line)
                title = m.group(1)
                await asyncio.sleep(25)
                await dcfg('[%s +25s]' % title)
                await asyncio.sleep(20)
                await dcfg('[%s +45s]' % title)
        if 'Team Fortress Classic' in ' '.join(seen) and any('Team Fortress' in s for s in seen):
            # stay a little longer only if nothing else we want is left
            pass
        await asyncio.sleep(3)
    out('deadline reached')

asyncio.run(main())
