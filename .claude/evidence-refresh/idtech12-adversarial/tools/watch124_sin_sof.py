#!/usr/bin/env python3
"""READ-ONLY opportunistic measurement on .124 for the idtech1/2 refresh review.

The V5 lane's own lan_sweep.py is launching every desktop shortcut in turn
(its log: sweep_armed3.log). When that sweep starts one of THIS review's titles
(SiN, Wages of SiN, SiN software, Soldier of Fortune, Quake, Quake II, Hexen II)
we send ONLY `DISPLAYCFG get` (EnumDisplaySettings ENUM_CURRENT_SETTINGS) at
+25 s, +45 s and +55 s, to read the live mode and refresh the title runs at now
that FX_GLIDE_REFRESH was deleted from 3dfxvs\\Device0\\glide (04:17:07).
Nothing is launched, written or switched by this script. One short connection
per read, closed gracefully."""
import asyncio, sys, time, re, os
sys.path.insert(0, '/home/voidsstr/development/retro-agent')
from client.retro_protocol import RetroConnection

LOG = ('/home/voidsstr/development/retro-agent/.claude/worktrees/v56k-bench/scripts/'
       'benchmarks/results/v56k_lan_192.168.1.124/sweep_armed3.log')
OUT = ('/home/voidsstr/development/retro-agent/.claude/evidence-refresh/'
       'idtech12-adversarial/boxes/124-sweep-displaycfg-idtech12.log')
WANT = re.compile(r'--- (SiN[^:]*|Soldier of Fortune|Quake|Quake - [^:]*|Quake II[^:]*|Hexen II[^:]*): ')
DEADLINE = time.time() + 75 * 60


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
    pos = os.path.getsize(LOG)          # only titles started from now on
    out('watching %s from byte %d' % (LOG, pos))
    while time.time() < DEADLINE:
        size = os.path.getsize(LOG)
        if size > pos:
            with open(LOG, 'r', errors='replace') as f:
                f.seek(pos)
                chunk = f.read()
            pos = size
            name = None
            for line in chunk.splitlines():
                if line.strip():
                    out('sweep: ' + line.strip())
                m = WANT.search(line)
                if m:
                    name = m.group(1)          # the title the sweep has just started
                elif '--- ' in line:
                    name = None                # a later, unrelated title started
            if name:
                for delay, tag in ((25, '+25s'), (20, '+45s'), (10, '+55s')):
                    await asyncio.sleep(delay)
                    await dcfg('[%s %s]' % (name, tag))
        await asyncio.sleep(2)
    out('deadline reached')


if __name__ == '__main__':
    asyncio.run(main())
