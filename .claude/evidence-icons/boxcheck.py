#!/usr/bin/env python3
"""Per-box helper for the 2026-09-28 staged-icon push (ad hoc, not shipped).

    boxcheck.py <ip> status                 HWPROFILE summary + GAMESYNC STATUS
    boxcheck.py <ip> sync [--timeout S]     GAMESYNC RESET + START, poll to the end
    boxcheck.py <ip> syncstart              GAMESYNC RESET + START only
    boxcheck.py <ip> syncwait [--timeout S] poll (default 540 s, fits one tool call);
                                            prints SYNC VERDICT when finished, else
                                            STILL RUNNING - call it again
    boxcheck.py <ip> inspect                shortcut diff + icon files + screenshot
    boxcheck.py <ip> cmd "<AGENT COMMAND>"  one raw text command (keep output small)
    boxcheck.py <ip> download <remote> <local>

Evidence goes to .claude/evidence-icons/<ip>/ (durable, repo tree, untracked).
Only agent-INTERNAL commands are used by sync/inspect (safe on Win9x).
"""
import asyncio
import json
import os
import re
import sys
import time

REPO = '/home/voidsstr/development/retro-agent'
sys.path.insert(0, REPO)
from client.retro_protocol import RetroConnection  # noqa: E402

LIB = '/mnt/retro-share/Files/Games-Library'
EVID = os.path.join(REPO, '.claude', 'evidence-icons')
SECRET = 'retro-agent-secret'
CHANGED = ['Carmageddon1', 'SeriousSamFirstEncounter', 'SeriousSamSecondEncounter',
           'WarcraftII', 'WarcraftOrcsAndHumans', 'Descent1', 'Descent2']
DESKTOPS = [r'C:\Users\Public\Desktop',
            r'C:\Documents and Settings\All Users\Desktop',
            r'C:\WINDOWS\All Users\Desktop',
            r'C:\WINDOWS\Desktop']


async def conn(ip):
    c = RetroConnection(ip, 9898)
    await c.connect(SECRET, timeout=15.0)
    return c


async def text(c, cmd, timeout=120):
    status, data = await c.send_command(cmd, timeout=timeout) \
        if 'timeout' in c.send_command.__code__.co_varnames else await c.send_command(cmd)
    return data.decode('latin1', 'replace')


def jload(s):
    try:
        return json.loads(s)
    except Exception:
        return None


def library_titles():
    out = []
    for t in sorted(os.listdir(LIB)):
        if t.startswith('_'):
            continue
        if os.path.isfile(os.path.join(LIB, t, 'launch.txt')):
            out.append(t)
    return out


def launch_lines(title):
    rows = []
    p = os.path.join(LIB, title, 'launch.txt')
    for line in open(p, encoding='latin1'):
        line = line.rstrip('\r\n')
        if not line.strip() or line.lstrip().startswith('#'):
            continue
        f = line.split('\t')
        if len(f) >= 2:
            rows.append({'target': f[0].strip(), 'name': f[1].strip(),
                         'icon': f[2].strip() if len(f) > 2 else ''})
    return rows


async def cmd_status(ip):
    c = await conn(ip)
    hw = jload(await text(c, 'HWPROFILE')) or {}
    gs = jload(await text(c, 'GAMESYNC STATUS')) or {}
    await c.close()
    out = {'ip': ip, 'hostname': hw.get('hostname'), 'agent': hw.get('agent_version'),
           'os': (hw.get('os') or {}).get('product'), 'os_level': (hw.get('os') or {}).get('level'),
           'host_policy': hw.get('host_policy'), 'capabilities': hw.get('capabilities'),
           'gpu': (hw.get('gpu') or {}).get('name'), 'display': hw.get('display'),
           'gamesync': gs}
    print(json.dumps(out, indent=1))
    return out


async def cmd_syncstart(ip):
    c = await conn(ip)
    before = jload(await text(c, 'GAMESYNC STATUS'))
    if before and before.get('state') in ('copying', 'sizing', 'running', 'scanning',
                                          'starting', 'planning', 'gating'):
        await c.close()
        print('ALREADY RUNNING - not restarting:', json.dumps(before), flush=True)
        return before
    r1 = await text(c, 'GAMESYNC RESET')
    r2 = await text(c, 'GAMESYNC START')
    await c.close()
    print('RESET ->', r1.strip(), '| START ->', r2.strip(), flush=True)
    os.makedirs(os.path.join(EVID, ip), exist_ok=True)
    json.dump({'before': before, 'started': time.time()},
              open(os.path.join(EVID, ip, 'syncstart.json'), 'w'), indent=1)
    return before


async def cmd_sync(ip, timeout_s, start=True):
    before = await cmd_syncstart(ip) if start else None
    t0 = time.time()
    last = None
    st = None
    if start:
        await asyncio.sleep(20)
    while time.time() - t0 < timeout_s:
        try:
            c = await conn(ip)
            st = jload(await text(c, 'GAMESYNC STATUS'))
            await c.close()
        except Exception as e:
            print('poll error:', e, flush=True)
            await asyncio.sleep(30)
            continue
        key = (st or {}).get('state'), (st or {}).get('titles_done')
        if key != last:
            print(time.strftime('%T'), json.dumps(st), flush=True)
            last = key
        started = 0
        try:
            started = json.load(open(os.path.join(EVID, ip, 'syncstart.json')))['started']
        except Exception:
            pass
        just_started = st and st.get('state') == 'idle' and time.time() - started < 180
        if st and not just_started and st.get('state') not in (
                'copying', 'sizing', 'running', 'scanning', 'starting', 'planning', 'gating'):
            break
        await asyncio.sleep(20)
    if st is None or st.get('state') in ('copying', 'sizing', 'running', 'scanning',
                                         'starting', 'planning', 'gating'):
        print('STILL RUNNING after %ds - call syncwait again: %s'
              % (int(time.time() - t0), json.dumps(st)), flush=True)
        return None
    lib_n = len(library_titles())
    verdict = {
        'state': (st or {}).get('state'),
        'failed_files': (st or {}).get('failed_files'),
        'titles_done': (st or {}).get('titles_done'),
        'titles_gated': (st or {}).get('titles_gated'),
        'titles_skipped': (st or {}).get('titles_skipped'),
        'titles_total': (st or {}).get('titles_total'),
        'library_titles': lib_n,
        'files_written': (st or {}).get('files_written'),
        'elapsed_s': int(time.time() - t0),
    }
    ok = (verdict['state'] == 'done' and verdict['failed_files'] == 0
          and verdict['titles_total'] == lib_n
          and (verdict['titles_done'] or 0) + (verdict['titles_gated'] or 0)
          + (verdict['titles_skipped'] or 0) == verdict['titles_total'])
    verdict['PASS'] = bool(ok)
    print('SYNC VERDICT', json.dumps(verdict), flush=True)
    os.makedirs(os.path.join(EVID, ip), exist_ok=True)
    json.dump({'before': before, 'final': st, 'verdict': verdict},
              open(os.path.join(EVID, ip, 'sync.json'), 'w'), indent=1)
    return verdict


def parse_last_run(log):
    """Lines of the most recent GAMESYNC pass (from its last 'done:' back to
    the previous 'done:' or start)."""
    lines = [l for l in log.splitlines() if '[GAMESYNC]' in l]
    ends = [i for i, l in enumerate(lines) if '] done:' in l]
    if not ends:
        return lines, None
    end = ends[-1]
    start = ends[-2] + 1 if len(ends) > 1 else 0
    return lines[start:end + 1], lines[end]


async def cmd_inspect(ip):
    from PIL import Image
    d = os.path.join(EVID, ip)
    os.makedirs(d, exist_ok=True)
    c = await conn(ip)
    hw = jload(await text(c, 'HWPROFILE')) or {}
    desk = {}
    for p in DESKTOPS:
        j = jload(await text(c, 'DIRLIST ' + p))
        if isinstance(j, list):
            desk[p] = sorted(e['name'] for e in j if e['name'].lower().endswith('.lnk'))
    users = jload(await text(c, r'DIRLIST C:\Users')) or []
    for u in users if isinstance(users, list) else []:
        if u.get('is_dir') and u['name'] not in ('Public', 'Default', 'All Users', 'Default User'):
            p = r'C:\Users\%s\Desktop' % u['name']
            j = jload(await text(c, 'DIRLIST ' + p))
            if isinstance(j, list):
                desk[p] = sorted(e['name'] for e in j if e['name'].lower().endswith('.lnk'))
    games = jload(await text(c, r'DIRLIST C:\Games')) or []
    installed = sorted(e['name'] for e in games if isinstance(e, dict) and e.get('is_dir'))
    icons = {}
    for t in CHANGED:
        j = jload(await text(c, r'DIRLIST C:\Games' + '\\' + t))
        if isinstance(j, list):
            icons[t] = {e['name']: e['size'] for e in j
                        if e['name'].lower().endswith('.ico') or e['name'].lower() == 'launch.txt'}
    log = b''
    for lp in (r'C:\RETRO_AGENT\agent.log.1', r'C:\RETRO_AGENT\agent.log'):
        try:
            log += await c.command_binary('DOWNLOAD ' + lp)
        except Exception:
            pass
    open(os.path.join(d, 'agent.log'), 'wb').write(log)
    try:
        await text(c, 'UIKEY SHIFT')
    except Exception:
        pass
    await asyncio.sleep(3)
    bmp = await c.command_binary('SCREENSHOT 0')
    wl = jload(await text(c, 'WINLIST'))
    await c.close()

    bp = os.path.join(d, 'desktop.bmp')
    open(bp, 'wb').write(bmp)
    im = Image.open(bp).convert('RGB')
    im.save(os.path.join(d, 'desktop.png'))
    W, H = im.size
    # Two zoomed crops of the left of the screen, where auto-arrange packs icons.
    cw = min(W, 760)
    left = im.crop((0, 0, cw // 2, H)).resize((cw // 2 * 2, H * 2), Image.NEAREST)
    right = im.crop((cw // 2, 0, cw, H)).resize((cw // 2 * 2, H * 2), Image.NEAREST)
    left.save(os.path.join(d, 'icons_a.png'))
    right.save(os.path.join(d, 'icons_b.png'))
    os.remove(bp)

    run, done_line = parse_last_run(log.decode('latin1', 'replace'))
    suppressed = {}
    made = {}
    for l in run:
        m = re.search(r'\] ([^:]+): SHORTCUT SUPPRESSED "([^"]+)" \(([^)]*)\) - (.*)$', l)
        if m:
            suppressed[m.group(2)] = m.group(4)
        m = re.search(r'\] ([^:]+): desktop shortcut -> (.*?) \(icon: (.*)\)$', l)
        if m:
            made[(m.group(1), m.group(2))] = m.group(3)
    have = set()
    for names in desk.values():
        have |= {n[:-4].lower() for n in names}
    expected, missing = [], []
    inst_lower = {t.lower() for t in installed}
    for t in library_titles():
        if t.lower() not in inst_lower:
            continue
        for row in launch_lines(t):
            expected.append(row['name'])
            if row['name'].lower() not in have:
                missing.append({'title': t, 'name': row['name'],
                                'suppressed_reason': suppressed.get(row['name'])})
    share_icons = {}
    for t in CHANGED:
        td = os.path.join(LIB, t)
        share_icons[t] = {f: os.path.getsize(os.path.join(td, f)) for f in os.listdir(td)
                          if f.lower().endswith('.ico') or f.lower() == 'launch.txt'}
    icon_mismatch = []
    for t in CHANGED:
        if t.lower() not in inst_lower:
            continue
        for f, sz in share_icons[t].items():
            got = (icons.get(t) or {}).get(f)
            if got != sz:
                icon_mismatch.append({'title': t, 'file': f, 'share': sz, 'box': got})
    summary = {
        'ip': ip, 'hostname': hw.get('hostname'), 'os': (hw.get('os') or {}).get('product'),
        'capabilities': hw.get('capabilities'), 'screen': [W, H],
        'desktop_dirs': {k: len(v) for k, v in desk.items()},
        'installed_titles': len(installed), 'library_titles': len(library_titles()),
        'expected_shortcuts': len(expected), 'present_shortcuts': len(expected) - len(missing),
        'missing': missing,
        'missing_unexplained': [m for m in missing if not m['suppressed_reason']],
        'icon_files_mismatch': icon_mismatch,
        'last_gamesync_done_line': done_line,
        'shortcut_icons_changed_titles': {'%s|%s' % k: v for k, v in made.items()
                                          if k[0] in CHANGED},
        'windows': [w.get('title') for w in (wl or {}).get('windows', [])],
        'evidence': {'desktop': os.path.join(d, 'desktop.png'),
                     'icons_a': os.path.join(d, 'icons_a.png'),
                     'icons_b': os.path.join(d, 'icons_b.png'),
                     'agent_log': os.path.join(d, 'agent.log')},
    }
    json.dump(summary, open(os.path.join(d, 'inspect.json'), 'w'), indent=1)
    print(json.dumps(summary, indent=1))
    return summary


async def cmd_raw(ip, command):
    c = await conn(ip)
    out = await text(c, command, timeout=300)
    await c.close()
    print(out)


async def cmd_download(ip, remote, local):
    c = await conn(ip)
    data = await c.command_binary('DOWNLOAD ' + remote)
    await c.close()
    open(local, 'wb').write(data)
    print('%d bytes -> %s' % (len(data), local))


def main():
    ip, verb = sys.argv[1], sys.argv[2]
    if verb == 'status':
        asyncio.run(cmd_status(ip))
    elif verb == 'sync':
        t = 2700
        if '--timeout' in sys.argv:
            t = int(sys.argv[sys.argv.index('--timeout') + 1])
        asyncio.run(cmd_sync(ip, t))
    elif verb == 'syncstart':
        asyncio.run(cmd_syncstart(ip))
    elif verb == 'syncwait':
        t = 540
        if '--timeout' in sys.argv:
            t = int(sys.argv[sys.argv.index('--timeout') + 1])
        asyncio.run(cmd_sync(ip, t, start=False))
    elif verb == 'inspect':
        asyncio.run(cmd_inspect(ip))
    elif verb == 'cmd':
        asyncio.run(cmd_raw(ip, sys.argv[3]))
    elif verb == 'download':
        asyncio.run(cmd_download(ip, sys.argv[3], sys.argv[4]))
    else:
        raise SystemExit(__doc__)


if __name__ == '__main__':
    main()
