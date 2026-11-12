#!/usr/bin/env python3
"""GAMESYNC sweep-order check on one box: RESET+START, then poll STATUS plus the
All Users desktop .lnk count and C:\\retro-desktop-backup, fast during sizing."""
import asyncio, json, sys, time
sys.path.insert(0, '/home/voidsstr/development/retro-agent')
from client.retro_protocol import RetroConnection
IP = sys.argv[1]
START = len(sys.argv) < 3 or sys.argv[2] != 'nostart'
import os
PFX = os.environ.get('PFX', '')
DESK = r'C:\Documents and Settings\All Users\Desktop'
BK = r'C:\retro-desktop-backup'
ACTIVE = ('copying', 'sizing', 'running', 'scanning', 'starting', 'planning', 'gating')

async def cmd(c, s):
    st, d = await c.send_command(s)
    return d.decode('latin1', 'replace')

async def sample(c):
    st = json.loads(await cmd(c, 'GAMESYNC STATUS'))
    dl = json.loads(await cmd(c, 'DIRLIST ' + DESK))
    bk = json.loads(await cmd(c, 'DIRLIST ' + BK))
    lnk = sorted(e['name'] for e in dl if e['name'].lower().endswith('.lnk'))
    return st, lnk, bk

async def main():
    c = RetroConnection(IP, 9898)
    await c.connect('retro-agent-secret', timeout=15)
    st, lnk, bk = await sample(c)
    print(json.dumps({'t': time.strftime('%T'), 'phase': 'before', 'state': st.get('state'),
                      'lnk': len(lnk), 'backup_n': len(bk),
                      'backup_newest': max((e['modified'] for e in bk), default=None)}), flush=True)
    json.dump({'lnk': lnk, 'backup': bk, 'status': st}, open(PFX + 'sweep-before.json', 'w'), indent=1)
    if START:
        print('RESET', (await cmd(c, 'GAMESYNC RESET')).strip(), flush=True)
        print('START', (await cmd(c, 'GAMESYNC START')).strip(), flush=True)
    t0 = time.time(); seen_active = False; minlnk = len(lnk); sizing_samples = 0
    while True:
        try:
            st, lnk2, bk2 = await sample(c)
        except Exception as e:
            print('poll error', e, flush=True)
            try: await c.close()
            except Exception: pass
            await asyncio.sleep(10)
            c = RetroConnection(IP, 9898); await c.connect('retro-agent-secret', timeout=15)
            continue
        s = st.get('state')
        if s in ACTIVE: seen_active = True
        if s == 'sizing': sizing_samples += 1
        minlnk = min(minlnk, len(lnk2))
        gone = sorted(set(lnk) - set(lnk2)); new = sorted(set(lnk2) - set(lnk))
        print(json.dumps({'t': time.strftime('%T'), 'el': int(time.time() - t0), 'state': s,
                          'msg': st.get('message'), 'done': st.get('titles_done'),
                          'total': st.get('titles_total'), 'starved_s': st.get('starved_s'),
                          'cpu': st.get('cpu_busy_pct'), 'lnk': len(lnk2), 'gone': gone[:5],
                          'new': new[:5], 'backup_n': len(bk2),
                          'backup_newest': max((e['modified'] for e in bk2), default=None)}), flush=True)
        if s == 'sizing' and len(lnk2) < len(lnk):
            json.dump({'status': st, 'lnk': lnk2, 'backup': bk2},
                      open(PFX + 'sweep-FAIL-during-sizing.json', 'w'), indent=1)
        if seen_active and s not in ACTIVE:
            break
        if not seen_active and time.time() - t0 > 120:
            print('never saw the run start', flush=True); break
        await asyncio.sleep(0.3 if (s in ('sizing', 'starting', 'planning', 'gating', 'idle') or time.time() - t0 < 25) else 10)
    json.dump({'status': st, 'lnk': lnk2, 'backup': bk2, 'min_lnk_seen': minlnk,
               'sizing_samples': sizing_samples}, open(PFX + 'sweep-after.json', 'w'), indent=1)
    print('END min_lnk_seen=%d sizing_samples=%d' % (minlnk, sizing_samples), flush=True)
    await c.close()

asyncio.run(main())
