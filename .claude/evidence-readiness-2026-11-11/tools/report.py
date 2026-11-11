#!/usr/bin/env python3
"""Render rounds/*.result.json into one markdown matrix (title x box)."""
import glob, json, os, sys

EVID = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BOXES = [110, 123, 124, 133, 143, 171, 184, 186, 240, 243]
SYM = {'PASS_MP': 'MP', 'PASS_SP': 'SP', 'FAIL': 'FAIL', 'BLOCKED': 'blk', 'NA': '-'}
order = [x['key'] for x in json.load(open(os.path.join(EVID, 'order.json')))] if os.path.exists(os.path.join(EVID, 'order.json')) else None
res = {}
for p in glob.glob(os.path.join(EVID, 'rounds', '*.result.json')):
    try:
        r = json.load(open(p))
    except Exception as e:
        print(f'unreadable {p}: {e}', file=sys.stderr)
        continue
    res[r.get('key') or os.path.basename(p).split('.')[0]] = r
keys = [k for k in (order or sorted(res)) if k in res] + [k for k in sorted(res) if order and k not in order]
print('| title | ' + ' | '.join(f'.{b}' for b in BOXES) + ' | in one game |')
print('|---|' + '---|' * len(BOXES) + '---|')
for k in keys:
    r = res[k]
    by = {}
    for v in r.get('boxes', []):
        o = int(str(v.get('ip', '0')).split('.')[-1] or 0)
        by[o] = SYM.get(v.get('verdict'), v.get('verdict'))
    print(f"| {k} | " + ' | '.join(by.get(b, '?') for b in BOXES) + f" | {r.get('players_in_one_game', '?')} |")
missing = [k for k in (order or []) if k not in res]
if missing:
    print('\nno result yet: ' + ', '.join(missing))
