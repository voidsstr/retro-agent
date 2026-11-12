#!/usr/bin/env python3
"""Emulate provisioning/fleetres/fleetres.c's target selection (LCD branch,
-cap handling, the classic 4:3 ladder, q2_mode_for) against the mode lists that
GAMERES reported on the four 1080p LCD boxes on 2026-09-28 23:55-23:57
(.claude/evidence-1080p/*/00-gameres-before.json, .240 from 01b-gameres.json).

Read-only: it only reads the captured JSON. It answers "what -width/-height or
gl_mode does each staged launcher hand the engine on each LCD box today".
Logic transcribed from fleetres.c lines 746-821 and 318-333 (bpp>=16 modes,
deduplicated by w x h; GAMERES' list is the same set).
"""
import json, os
HERE = os.path.dirname(os.path.abspath(__file__))
EV = os.path.dirname(HERE)
SRC = {'.123': 'q3-123/00-gameres-before.json',
       '.145': '145-q3/00-gameres-before.json',
       '.195': 'q3-195/00-gameres-before.json',
       '.240': '240-q3/01b-gameres.json'}
Q2TAB = [(320,240),(400,300),(512,384),(640,480),(800,600),(960,720),(1024,768),(1152,864),(1280,960),(1600,1200)]
LADDER = [(640,480),(800,600),(1024,768),(1152,864),(1280,960),(1600,1200)]

def load(p):
    raw = open(os.path.join(EV, p), 'rb').read().decode('latin1')
    d = json.loads(raw[raw.find('{'):])
    modes = [tuple(int(x) for x in m.split('@')[0].split('x')) for m in d['modes']]
    return d, modes

def fleetres(modes, native=(1920,1080), cap=None):
    have = lambda w,h: (w,h) in modes
    offered = lambda w,h: True if len(modes) < 4 else have(w,h)
    tw, th = native                      # LCD + native offered -> native
    if cap and (tw > cap[0] or th > cap[1]):
        want = tw/th; bw = bh = 0
        for (w,h) in modes:
            r = w/h
            if want-0.02 < r < want+0.02 and w <= cap[0] and h <= cap[1] and w > bw:
                bw, bh = w, h
        tw, th = (bw, bh) if bw else cap
    t43 = (640,480)
    for (w,h) in LADDER:
        if w > tw or h > th: continue
        if not have(w,h) and len(modes) > 2: continue
        t43 = (w,h)
    best = best_fit = 3
    for i,(w,h) in enumerate(Q2TAB):
        if w > t43[0] or h > t43[1]: continue
        best_fit = i
        if offered(w,h): best = i
    q2 = best if best > 3 else (best if offered(*Q2TAB[best_fit]) else best_fit)
    return (tw,th), t43, q2

CASES = [('Quake1  Play Quake.bat      -cap 1280 960 -> GLQUAKE -width FR_W43 -height FR_H43', (1280,960), 't43'),
         ('HexenII Play/Host/Join .bat -cap 1024 768 -> glh2 -width FR_W -height FR_H',       (1024,768), 'tgt'),
         ('Quake2Complete / SiNGold / SoF (no cap)  -> gl_mode FR_Q2MODE',                     None,       'q2'),
         ('Doom3 (no cap)                           -> r_customWidth/Height FR_W/FR_H',        None,       'tgt')]
for label, cap, what in CASES:
    print(label)
    for box, p in SRC.items():
        d, modes = load(p)
        tgt, t43, q2 = fleetres(modes, cap=cap)
        val = {'t43': '%dx%d' % t43, 'tgt': '%dx%d' % tgt,
               'q2': 'gl_mode %d = %dx%d' % (q2, *Q2TAB[q2])}[what]
        print('   %-5s -> %-22s (FR_W/H %dx%d, FR_W43/H43 %dx%d, FR_Q2MODE %d; GAMERES q2mode %s)'
              % (box, val, tgt[0], tgt[1], t43[0], t43[1], q2, d['target']['q2mode']))
