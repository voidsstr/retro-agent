"""Sweep the per-box verification matrix. Resumable: appends JSONL, skips done."""
import asyncio, json, os, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from fleetlib import Box, BOXES
from measure import measure, launch_entries, LIB

OUT = sys.argv[1] if len(sys.argv) > 1 else '/tmp/matrix.jsonl'
# Titles a sibling agent is actively changing -- measuring them races the edit.
DEFER = {'SeriousSamFirstEncounter','SeriousSamSecondEncounter','JediAcademy',
         'SoldierOfFortune','BF1942','Generals'}

def done_keys(path):
    k = set()
    if os.path.exists(path):
        for ln in open(path):
            try:
                r = json.loads(ln); k.add((r['ip'], r['title'], r['target']))
            except Exception: pass
    return k

async def sweep_box(ip, titles, out_lock, done, settle=30, budget=150):
    gamedirs = {}
    try:
        async with Box(ip, 25.0) as b:
            for t in titles:
                ents = launch_entries(t)
                if not ents: continue
                target = ents[0][0]
                if (ip, t, target) in done: continue
                gd = f'C:\\Games\\{t}'
                try:
                    r = await asyncio.wait_for(
                        measure(b, ip, t, gd, target, settle=settle), timeout=budget)
                except asyncio.TimeoutError:
                    r = {'ip': ip, 'title': t, 'target': target, 'status': 'timeout',
                         'ts': time.strftime('%Y-%m-%dT%H:%M:%S'),
                         'note': f'cell exceeded {budget}s'}
                    try:
                        await b.cmd('EXEC cmd /c taskkill /f /im "%s"' % 'dosbox.exe')
                    except Exception: pass
                except Exception as e:
                    r = {'ip': ip, 'title': t, 'target': target, 'status': 'error',
                         'ts': time.strftime('%Y-%m-%dT%H:%M:%S'),
                         'note': f'{type(e).__name__}: {e}'}
                async with out_lock:
                    with open(OUT, 'a') as f: f.write(json.dumps(r) + '\n')
                print(f"{ip} {t:28s} {r['status']:14s} {r.get('mode','')}", flush=True)
    except Exception as e:
        print(f'{ip} BOX-LEVEL FAIL {type(e).__name__}: {e}', flush=True)

async def main():
    state = json.load(open('/tmp/claude-1000/-home-voidsstr-development-retro-agent--claude-worktrees-arranger/71ba4593-0699-4c2f-9243-0c9be09cb538/scratchpad/state.json'))
    dep  = state['deployed']
    done = done_keys(OUT)
    lock = asyncio.Lock()
    order = sorted(x for x in os.listdir(LIB) if not x.startswith('_'))
    tasks = []
    for ip in ['192.168.1.123','192.168.1.124','192.168.1.133','192.168.1.143',
               '192.168.1.171','192.168.1.240','192.168.1.246']:
        titles = [t for t in order if t in set(dep.get(ip, [])) and t not in DEFER]
        tasks.append(sweep_box(ip, titles, lock, done))
    await asyncio.gather(*tasks)

asyncio.run(main())
