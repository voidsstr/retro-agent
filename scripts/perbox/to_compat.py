#!/usr/bin/env python3
"""Load the perbox measured matrix into fleetdb's compat_* tables.

This does NOT define a schema.  It imports `scripts/fleet/compat_db.py` -
fleetdb owns that - and writes `origin='measured'`, `source='perbox'` rows
through its own put_* helpers, so the six distinctions that module exists to
protect are preserved rather than re-implemented here.

Refuses to run until fleetdb has created the tables: creating them from this
side would race the owner and could bake in a stale column set.

MAPPING, and why each one is what it is
---------------------------------------
  runs + a screenshot with real content   -> runs='verified'  (watched it render)
  runs + a BLACK screenshot               -> runs='runs'      NOT verified: an
        exclusive fullscreen surface GDI cannot capture means nobody has
        actually SEEN it render.  The process started and took a display mode;
        that is 'runs', and calling it 'verified' would make the word mean
        "somebody thought so".
  launch_failed                           -> runs='failed'
  not_deployed                            -> runs='n/a' + deploy='absent'
  timeout / error                         -> runs='untested' (the harness gave
        up; that is an absence of measurement, not a measured failure)
"""
import json, os, sys, argparse

FLEETDB = os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(
           os.path.abspath(__file__)))), 'scripts', 'fleet')

# Renderer per title where the staged tree settles it unambiguously.
RENDERER = {
    'Carmageddon1':'dosbox','Descent1':'dosbox','RedneckRampage':'dosbox',
    'SystemShock2':'d3d','ShadowWarrior':'dosbox','WarcraftOrcsAndHumans':'dosbox',
    'MasterOfOrionII':'dosbox','AliensVsPredator':'d3d','WarcraftII':'ddraw',
    'Quake1':'opengl','Quake2Complete':'opengl','Quake3-TeamArena':'opengl',
    'ReturnToCastleWolfenstein':'opengl','SoldierOfFortune2':'opengl',
    'JediAcademy':'opengl','HexenII':'opengl','SiNGold':'opengl','Doom3':'opengl',
    'JediKnightDF2':'d3d','JediKnightMotS':'d3d','StarCraft':'ddraw',
    'RedAlert2':'ddraw','TiberianSun':'ddraw',
}

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('jsonl')
    ap.add_argument('--dry-run', action='store_true')
    a = ap.parse_args()
    sys.path.insert(0, FLEETDB)
    try:
        import compat_db as C
    except ImportError as e:
        print('cannot import fleetdb compat_db: %s' % e, file=sys.stderr); return 2
    con = C.connect() if hasattr(C, 'connect') else None
    if con is None:
        import sqlite3
        con = sqlite3.connect(str(C.DEFAULT_DB)); con.execute('PRAGMA busy_timeout=10000')
    have = {r[0] for r in con.execute(
        "SELECT name FROM sqlite_master WHERE type='table'")}
    if 'compat_render' not in have:
        print('compat_* tables do not exist yet - fleetdb has not migrated.\n'
              'Buffered results stay in %s; re-run this when the tables exist.'
              % a.jsonl, file=sys.stderr)
        return 3
    n = 0
    for ln in open(a.jsonl):
        r = json.loads(ln)
        st, ip, title = r['status'], r['ip'], r['title']
        sc = r.get('target', '')
        detail, runs, deploy = '', None, None
        if st == 'runs':
            black = (r.get('shot_luma') is not None and r['shot_luma'] < 3)
            runs = 'runs' if black or r.get('shot_luma') is None else 'verified'
            if black:
                detail = ('exclusive fullscreen surface - GDI SCREENSHOT is black, '
                          'so the mode change and the live process are the evidence, '
                          'not a picture')
        elif st == 'launch_failed':
            runs, detail = 'failed', r.get('note', '')
        elif st == 'not_deployed':
            runs, deploy, detail = 'n/a', 'absent', r.get('note', '')
        else:
            runs, detail = 'untested', '%s: %s' % (st, r.get('note', ''))
        m = r.get('desktop_mode_after') or {}
        if a.dry_run:
            print(f'{ip} {title:26s} {sc:34s} {runs:9s} '
                  f'{m.get("width")}x{m.get("height")}@{m.get("refresh")} '
                  f'fs={r.get("fullscreen")}')
            n += 1; continue
        fs = ('yes' if r.get('fullscreen') is True
              else 'no' if r.get('fullscreen') is False else 'unknown')
        kw = dict(renderer=RENDERER.get(title, 'unknown'),
                  width=m.get('width'), height=m.get('height'),
                  refresh_hz=m.get('refresh'), fullscreen=fs,
                  detail=detail, source='perbox', measured_at=r.get('ts'))
        # TWO rows on purpose.  The per-shortcut row is the precise fact - a
        # title's halves can need different machines.  The shortcut='' row is
        # the TITLE-level fact the matrix and `summary` read; without it a
        # measurement lands in the database and never reaches the view that
        # answers "is this title verified on this box".
        C.put_render(con, ip, title, 'measured', runs, shortcut=sc, **kw)
        C.put_render(con, ip, title, 'measured', runs, shortcut='', **kw)
        if deploy:
            C.put_deploy(con, ip, title, 'measured', deploy,
                         reason=detail, source='perbox', measured_at=r.get('ts'))
        if r.get('evidence'):
            C.put_evidence(con, ip, title, 'render', 'screenshot',
                           r['evidence'], r.get('ts'))
        n += 1
    if not a.dry_run: con.commit()
    print('%d cell(s) %s' % (n, 'previewed' if a.dry_run else 'written'))
    return 0

sys.exit(main())
