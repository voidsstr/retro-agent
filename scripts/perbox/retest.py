#!/usr/bin/env python3
"""Re-measure the cells that FAILED, with a much longer settle.

A 30s settle is a measurement artifact, not a property of the game: a title
that shows a publisher movie, builds a shader cache or scans a CD can take
well over that to put a process on the list.  Recording such a cell as
'failed' would be exactly the mistake this project keeps paying for -- a
measurement fault reported as a defect.  So every failure is retried at 90s
before anyone is told the game is broken.
"""
import asyncio, json, os, sys, time, collections
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from fleetlib import Box
from measure import measure

SRC = sys.argv[1]
OUT = sys.argv[2]

def load_failures(p):
    per = collections.defaultdict(list)
    for ln in open(p):
        r = json.loads(ln)
        if r['status'] in ('launch_failed', 'timeout', 'error'):
            per[r['ip']].append((r['title'], r['target']))
    return per

async def rebox(ip, cells, lock):
    try:
        async with Box(ip, 25.0) as b:
            for title, target in cells:
                gd = f'C:\\Games\\{title}'
                try:
                    r = await asyncio.wait_for(
                        measure(b, ip, title, gd, target, settle=90), timeout=220)
                except Exception as e:
                    r = {'ip': ip, 'title': title, 'target': target,
                         'status': 'error', 'ts': time.strftime('%Y-%m-%dT%H:%M:%S'),
                         'note': f'{type(e).__name__}: {e}'}
                r['retest'] = True
                async with lock:
                    with open(OUT, 'a') as f: f.write(json.dumps(r) + '\n')
                print(f"RETEST {ip} {title:24s} {r['status']:14s} {r.get('mode','')}",
                      flush=True)
    except Exception as e:
        print(f'{ip} BOX-LEVEL FAIL {type(e).__name__}: {e}', flush=True)

async def main():
    per = load_failures(SRC)
    lock = asyncio.Lock()
    print('retesting %d cell(s) across %d box(es)'
          % (sum(len(v) for v in per.values()), len(per)))
    await asyncio.gather(*[rebox(ip, c, lock) for ip, c in per.items()])

asyncio.run(main())
