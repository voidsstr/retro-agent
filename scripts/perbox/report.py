#!/usr/bin/env python3
"""Summarise the measured matrix. Counts what was MEASURED, never what was assumed."""
import json, os, sys, collections

MX  = sys.argv[1]
LIB = '/mnt/retro-share/Files/Games-Library'
IPS = ['192.168.1.123','192.168.1.124','192.168.1.133','192.168.1.143',
       '192.168.1.171','192.168.1.240','192.168.1.246']
TITLES = sorted(x for x in os.listdir(LIB) if not x.startswith('_'))

cell = {}
for ln in open(MX):
    r = json.loads(ln)
    k = (r['ip'], r['title'])
    if k not in cell or r.get('retest'): cell[k] = r

def verdict(r):
    if r is None: return 'untested'
    if r['status'] == 'runs':
        luma = r.get('shot_luma')
        if luma is not None and luma < 3: return 'runs'      # black: not watched
        return 'verified'
    return {'launch_failed':'failed','not_deployed':'n/a',
            'timeout':'untested','error':'untested'}.get(r['status'], 'untested')

# Deployment + gate context, so a correctly-refused title reads as `gated`
# and not as `untested`.  "nobody tested it", "the gate refused it" and "it is
# not on the box" are three different facts with three different follow-ups.
ST  = json.load(open(os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                  'state.json'))) if os.path.exists(
      os.path.join(os.path.dirname(os.path.abspath(__file__)),'state.json')) else {}
DEP = {k: set(v) for k, v in (ST.get('deployed') or {}).items()}
VER = ST.get('verdicts') or {}

def classify(ip, t):
    r = cell.get((ip, t))
    if r is not None: return verdict(r)
    if DEP and t not in DEP.get(ip, set()):
        gv = (VER.get(ip) or {}).get(t)
        if gv and gv[0] == 'no': return 'gated'
        return 'absent'
    return 'untested'

grid = {t: {ip: classify(ip, t) for ip in IPS} for t in TITLES}
tot = collections.Counter()
print('%-28s' % 'TITLE', ''.join('%-10s' % ('.'+i.split('.')[-1]) for i in IPS))
for t in TITLES:
    row = [grid[t][ip] for ip in IPS]
    for v in row: tot[v] += 1
    print('%-28s' % t, ''.join('%-10s' % v[:9] for v in row))
print()
print('CELLS: %d total (%d titles x %d live boxes)' % (len(TITLES)*len(IPS), len(TITLES), len(IPS)))
for k in ('verified','runs','failed','gated','absent','n/a','untested'):
    print('  %-9s %4d' % (k, tot[k]))
measured = tot['verified'] + tot['runs'] + tot['failed']
print('  MEASURED  %4d   (%.0f%% of the cross product)'
      % (measured, 100.0*measured/(len(TITLES)*len(IPS))))
print()
print('FAILED cells (tested and did NOT run):')
for t in TITLES:
    f = [ip for ip in IPS if grid[t][ip] == 'failed']
    if f: print('  %-26s %s' % (t, ' '.join('.'+i.split('.')[-1] for i in f)))
print()
print('GATED cells (the gate refused the title and the box does NOT carry it):')
for t in TITLES:
    g = [ip for ip in IPS if grid[t][ip] == 'gated']
    if g: print('  %-26s %s' % (t, ' '.join('.'+i.split('.')[-1] for i in g)))
print()
print('UNTESTED cells (never exercised - NOT a pass):')
for t in TITLES:
    u = [ip for ip in IPS if grid[t][ip] == 'untested']
    if u: print('  %-26s %s' % (t, ' '.join('.'+i.split('.')[-1] for i in u)))
