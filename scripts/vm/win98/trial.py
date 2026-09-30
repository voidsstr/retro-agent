#!/usr/bin/env python3
"""trial.py - try ONE launcher variant on the Win98 build VM and photograph it.

    python3 scripts/vm/win98/trial.py --dir 'C:\\GAMES\\FLIGH~29' --run 'F19.COM /NJ /GM' \\
        [--run '<second line>' ...] [--settle 8,20,40] [--out DIR] [--name f19] [--keep-open]

The sweep (sweep.py) answers "does each staged shortcut work"; this answers
"would THIS launcher work" before the library is changed. It writes
C:\\RETRO_AGENT\\TRIAL.BAT (cd into --dir, then each --run line, and NO trailing
CLS - a launcher that fails leaves its message on screen instead of closing
the box), STARTs it, takes host-side frames from 86Box's own display at each
settle time with WINLIST, lists any file that appeared in C:\\ (a crash dump
such as EF2000's C:\\EF2000.$$$), then closes everything the way the sweep does.

COMMAND.COM dialect in --run: no %~dp0, no cd /d, no blocks, no parentheses.
"""
import argparse
import asyncio
import json
import os
import re
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import sweep  # noqa: E402  (connect, cmd, shot, judge, close_all, glreset, ...)


def trial_bat(directory, lines):
    """The batch the VM runs. Its own words: no CLS, so an error stays visible."""
    if not re.match(r'^[A-Za-z]:\\', directory):
        raise SystemExit('--dir must be an absolute DOS path, got %r' % directory)
    for l in lines:
        if re.search(r'[()]', l):
            raise SystemExit('no parentheses in a COMMAND.COM line: %r' % l)
    out = ['@echo off', directory[:2], 'cd %s' % directory] + list(lines)
    return ('\r\n'.join(out) + '\r\n').encode('ascii')


async def root_files(c):
    return {e['name']: e['size'] for e in json.loads(await sweep.cmd(c, 'DIRLIST C:\\'))
            if not e.get('is_dir')}


async def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--dir', required=True)
    ap.add_argument('--run', action='append', required=True)
    ap.add_argument('--settle', default='8,20,40')
    ap.add_argument('--out', default=os.path.expanduser('~/.retro-fleet/w98vm/trial'))
    ap.add_argument('--name', default='trial')
    ap.add_argument('--keep-open', action='store_true', help='do not close the game afterwards')
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    settle = [int(x) for x in a.settle.split(',')]

    c = await sweep.connect()
    await sweep.ensure_helpers(c)
    await sweep.agent_pids(c)
    if not sweep.AGENT_PIDS:
        raise SystemExit('could not find the agent process - refusing to close windows blind')
    left = await sweep.close_all(c)
    if left:
        print('left over before launch:', left)
    await sweep.glreset(c)
    bat = trial_bat(a.dir, a.run)
    await c.send_command('UPLOAD C:\\RETRO_AGENT\\TRIAL.BAT', binary_payload=bat)
    if await sweep.cmd(c, 'DOWNLOAD C:\\RETRO_AGENT\\TRIAL.BAT', binary=True) != bat:
        raise SystemExit('TRIAL.BAT did not upload intact')
    before_root = await root_files(c)
    before = await sweep.procs(c)
    base = sweep.emulated(sweep.shot(os.path.join(a.out, '%s-base.png' % a.name)))
    await sweep.cmd(c, 'LAUNCH C:\\WINDOWS\\COMMAND\\START.EXE C:\\RETRO_AGENT\\TRIAL.BAT')
    t0 = time.time()
    report = {'dir': a.dir, 'run': a.run, 'frames': []}
    for t in settle:
        await asyncio.sleep(max(0, t - (time.time() - t0)))
        png = os.path.join(a.out, '%s-%02ds.png' % (a.name, t))
        v = sweep.judge(sweep.shot(png), base)
        ws, fg = await sweep.winlist(c)
        gw = sweep.game_windows(ws)
        report['frames'].append({'t': t, 'png': png, 'verdict': v, 'windows': gw})
        print('%3ds %-8s %s' % (t, v, json.dumps(gw)[:300]), flush=True)
    now = await sweep.procs(c)
    new = [p for p in now if p not in before and re.split(r'[\\/]', p)[-1] not in sweep.SKIP_PROCS]
    after_root = await root_files(c)
    report['new_processes'] = new
    report['new_root_files'] = {n: s for n, s in after_root.items()
                                if n not in before_root or before_root[n] != s}
    for n in report['new_root_files']:
        try:
            body = await sweep.cmd(c, 'DOWNLOAD C:\\' + n, binary=True)
            if len(body) < 4096:
                print('--- C:\\%s ---\n%s' % (n, body.decode('latin-1')))
        except Exception as e:
            print('C:\\%s: %s' % (n, e))
    if not a.keep_open:
        await sweep.close_all(c)
        for p in new:
            try:
                await sweep.cmd(c, 'PROCKILL %s' % now[p])
            except Exception as e:
                print('PROCKILL', p, e)
        await asyncio.sleep(3)
        print('GLRESET:', await sweep.glreset(c))
        left = await sweep.close_all(c)
        if left:
            print('could not close:', left)
    json.dump(report, open(os.path.join(a.out, '%s.json' % a.name), 'w'), indent=1)
    print('new processes:', new, '| new/changed C:\\ files:', report['new_root_files'])
    await c.close()


if __name__ == '__main__':
    asyncio.run(main())
