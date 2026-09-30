#!/usr/bin/env python3
"""sweep.py - launch every game shortcut on the Win98 build VM's desktop and photograph it.

    python3 scripts/vm/win98/sweep.py [--only SUBSTR ...] [--out DIR] [--settle 12,30,50]

WHY THE VM. On a real Win98 box a full-screen DOS game reads back through GDI
as noise and ALT+RETURN can crash WINOLDAP (CLAUDE.md), so a sweep there can
only say "a window exists". Here the frames come from 86Box's own display on
the host (vmshot.py), so a DOS title in mode 13h or SVGA is photographed
exactly as it renders, and a title that hangs the machine costs one VM reset,
not a walk to .243.

For each .pif/.lnk on C:\\WINDOWS\\Desktop whose target is under C:\\GAMES:
  1. close anything left over; capture the idle desktop as the baseline
  2. START the shortcut (what a double-click does - it honours the PIF)
  3. frames at each settle time; WINLIST + PROCLIST each time
  4. close it: WM_CLOSE + the "Windows cannot shut down this program" Yes
     for DOS boxes, PROCKILL for any NEW Win32 process, then confirm the
     desktop is back. An agent that stops answering -> VM reset, noted.
A frame is judged against the baseline: 'desktop' (nothing appeared - the
title failed to start or exited), 'black', 'error-dialog', or 'renders'.
Results: DIR/results.json + DIR/<n>-<title>-<t>s.png + DIR/sheet-*.png.
"""
import argparse
import asyncio
import io
import json
import os
import re
import subprocess
import sys
import time

from PIL import Image, ImageChops, ImageDraw, ImageStat

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, '..', '..', '..'))
sys.path.insert(0, REPO)
from client.retro_protocol import RetroConnection  # noqa: E402

HOST, PORT, SECRET = '127.0.0.1', 19930, 'retro-agent-secret'
DESK = 'C:\\WINDOWS\\Desktop'
SKIP_PROCS = {'retro_agent.exe', 'retro_chat.exe', 'winkey9x.exe', 'start.exe', 'command.com',
              'kernel32.dll', 'msgsrv32.exe', 'mprexe.exe', 'explorer.exe', 'systray.exe',
              'taskmon.exe', 'rundll32.exe', 'mmtask.tsk', 'ddhelp.exe', 'spool32.exe',
              'scanregw.exe', 'welcome.exe', 'winoa386.mod', 'conagent.exe'}


def shot(path):
    subprocess.run([sys.executable, os.path.join(HERE, 'vmshot.py'), path, '--display', ':22'],
                   check=True, capture_output=True, timeout=30)
    return Image.open(path).convert('RGB')


def emulated(img):
    """The emulated screen only: drop 86Box's menu/toolbar (top) and status bar (bottom)."""
    w, h = img.size
    return img.crop((0, 56, w, h - 26)) if h > 120 else img


def judge(img, base):
    e = emulated(img)
    lum = ImageStat.Stat(e.convert('L'))
    if lum.mean[0] < 3 and lum.extrema[0][1] < 40:
        return 'black'
    if base is not None and base.size == e.size:
        diff = ImageStat.Stat(ImageChops.difference(e, base).convert('L')).mean[0]
        if diff < 2.0:
            return 'desktop'
    return 'renders'


async def connect(timeout=150):
    c = RetroConnection(HOST, PORT)
    await c.connect(SECRET, timeout=timeout)
    return c


async def cmd(c, text, binary=False, timeout=120):
    if binary:
        return await asyncio.wait_for(c.command_binary(text), timeout)
    return await asyncio.wait_for(c.command_text(text), timeout)


async def winlist(c):
    j = json.loads(await cmd(c, 'WINLIST'))
    return j.get('windows', []), j.get('foreground', {})


async def procs(c):
    j = json.loads(await cmd(c, 'PROCLIST'))
    lst = j if isinstance(j, list) else j.get('processes', [])
    return {(p.get('name') or '').lower(): p.get('pid') for p in lst}


def game_windows(ws):
    out = []
    for w in ws:
        t = w.get('title', '')
        if t in ('Program Manager', 'retro_chat') or t.startswith('Retro Remote'):
            continue
        out.append({'title': t, 'class': w.get('class'), 'rect': w.get('rect')})
    return out


async def close_all(c, rounds=10):
    """DOS boxes: WM_CLOSE, then Yes on 'Windows cannot shut down this program'."""
    for _ in range(rounds):
        ws, _fg = await winlist(c)
        live = [w for w in game_windows(ws) if w['class'] in ('tty', '#32770')]
        if not live:
            return []
        dlg = [w for w in live if w['class'] == '#32770']
        if dlg:
            await cmd(c, 'UIKEY RETURN')          # Yes is the default button
            await asyncio.sleep(3)
            continue
        await cmd(c, 'LAUNCH C:\\RETRO_AGENT\\WINKEY9X.EXE "%s" CLOSE' % live[0]['title'][:40])
        await asyncio.sleep(4)
    ws, _fg = await winlist(c)
    return [w['title'] for w in game_windows(ws) if w['class'] in ('tty', '#32770')]


async def reset_vm():
    subprocess.run(['systemctl', '--user', 'stop', 'w98box'], check=False)
    time.sleep(3)
    subprocess.run(['systemd-run', '--user', '--unit=w98box', '-p', 'CPUQuota=200%', '-p', 'MemoryMax=2G',
                    os.path.join(HERE, 'run-98.sh')], check=False, capture_output=True)
    for _ in range(40):
        await asyncio.sleep(15)
        try:
            c = await connect(60)
            await cmd(c, 'PING')
            return c
        except Exception:
            continue
    raise SystemExit('the VM did not come back after a reset')


async def ensure_winkey(c):
    """WINKEY9X.EXE (scripts/fleet/win9x/winkey9x.c) posts WM_CLOSE to a DOS box - the
    agent's own UIKEY types into whatever has focus, which a LAUNCHed DOS box rarely has."""
    have = json.loads(await cmd(c, 'DIRLIST C:\\RETRO_AGENT'))
    if any(e['name'].upper() == 'WINKEY9X.EXE' for e in have):
        return
    exe = os.path.join(os.path.expanduser('~/.retro-fleet/w98vm'), 'WINKEY9X.EXE')
    subprocess.run(['i686-w64-mingw32-gcc', '-O1', '-march=i586', '-mwindows', '-nostdlib', '-fno-builtin',
                    '-e', '_start@0', '-o', exe, os.path.join(REPO, 'scripts/fleet/win9x/winkey9x.c'),
                    '-lkernel32', '-luser32', '-s'], check=True)
    data = open(exe, 'rb').read()
    await c.send_command('UPLOAD C:\\RETRO_AGENT\\WINKEY9X.EXE', binary_payload=data)
    back = await cmd(c, 'DOWNLOAD C:\\RETRO_AGENT\\WINKEY9X.EXE', binary=True)
    if back != data:
        raise SystemExit('WINKEY9X.EXE did not upload intact')


def pif_target(b):
    return b[0x24:0x24 + 63].split(b'\0')[0].decode('latin-1')


def lnk_target(b):
    s = re.findall(rb'[A-Za-z]:\\[\x20-\x7e]{3,200}', b)
    return s[0].decode('latin-1') if s else ''


async def shortcuts(c):
    out = []
    for e in json.loads(await cmd(c, 'DIRLIST ' + DESK)):
        n = e['name']
        if not re.search(r'\.(pif|lnk)$', n, re.I):
            continue
        b = await cmd(c, 'DOWNLOAD %s\\%s' % (DESK, n), binary=True)
        t = pif_target(b) if n.lower().endswith('.pif') else lnk_target(b)
        if t.upper().startswith('C:\\GAMES\\'):
            out.append((n, t))
    return sorted(out)


def sheet(rows, out_dir):
    """Contact sheets: one row per title, its frames left to right, 6 titles a sheet."""
    thumbs = []
    for r in rows:
        ims = [Image.open(f['png']).convert('RGB') for f in r['frames'] if os.path.exists(f['png'])]
        ims = [emulated(i).resize((320, 240)) for i in ims]
        row = Image.new('RGB', (40 + 330 * max(1, len(ims)), 262), 'white')
        for k, im in enumerate(ims):
            row.paste(im, (40 + 330 * k, 20))
        d = ImageDraw.Draw(row)
        d.text((4, 2), '%s  [%s]' % (r['shortcut'], ' '.join(f['verdict'] for f in r['frames'])), fill='black')
        thumbs.append(row)
    names = []
    for s in range(0, len(thumbs), 6):
        part = thumbs[s:s + 6]
        w = max(t.size[0] for t in part)
        sh = Image.new('RGB', (w, 262 * len(part)), 'white')
        for k, t in enumerate(part):
            sh.paste(t, (0, 262 * k))
        p = os.path.join(out_dir, 'sheet-%02d.png' % (s // 6 + 1))
        sh.save(p)
        names.append(p)
    return names


async def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--only', nargs='*', default=[])
    ap.add_argument('--out', default=os.path.expanduser('~/.retro-fleet/w98vm/sweep'))
    ap.add_argument('--settle', default='12,30,50')
    a = ap.parse_args()
    settle = [int(x) for x in a.settle.split(',')]
    os.makedirs(a.out, exist_ok=True)
    res_path = os.path.join(a.out, 'results.json')
    results = json.load(open(res_path)) if os.path.exists(res_path) else {}

    c = await connect()
    await ensure_winkey(c)
    todo = await shortcuts(c)
    if a.only:
        todo = [t for t in todo if any(o.lower() in t[0].lower() for o in a.only)]
    print('%d shortcut(s) to test' % len(todo), flush=True)
    for n, (name, target) in enumerate(todo, 1):
        slug = re.sub(r'[^A-Za-z0-9]+', '_', name.rsplit('.', 1)[0])[:40]
        r = {'shortcut': name, 'target': target, 'frames': [], 'notes': []}
        try:
            left = await close_all(c)
            if left:
                r['notes'].append('left over before launch: %s' % left)
            base = emulated(shot(os.path.join(a.out, '_base.png')))
            before = await procs(c)
            await cmd(c, 'LAUNCH C:\\WINDOWS\\COMMAND\\START.EXE "%s\\%s"' % (DESK, name))
            t0 = time.time()
            for t in settle:
                await asyncio.sleep(max(0, t - (time.time() - t0)))
                png = os.path.join(a.out, '%03d-%s-%02ds.png' % (n, slug, t))
                v = judge(shot(png), base)
                ws, fg = await winlist(c)
                gw = game_windows(ws)
                if any(w['class'] == '#32770' for w in gw):
                    v = 'error-dialog'
                r['frames'].append({'t': t, 'png': png, 'verdict': v, 'windows': gw})
            now = await procs(c)
            new = [p for p in now if p not in before and p not in SKIP_PROCS]
            r['new_processes'] = new
            left = await close_all(c)
            for p in new:
                try:
                    await cmd(c, 'PROCKILL %s' % now[p])
                except Exception as e:
                    r['notes'].append('PROCKILL %s: %s' % (p, e))
            await asyncio.sleep(4)
            left = await close_all(c)
            if left:
                r['notes'].append('could not close: %s' % left)
        except Exception as e:
            r['notes'].append('agent lost (%s: %s) - VM reset' % (type(e).__name__, e))
            try:
                await c.close()
            except Exception:
                pass
            c = await reset_vm()
        r['summary'] = 'renders' if any(f['verdict'] == 'renders' for f in r['frames']) else \
            (r['frames'][-1]['verdict'] if r['frames'] else 'not-run')
        results[name] = r
        json.dump(results, open(res_path, 'w'), indent=1)
        print('%3d/%d %-40s %s %s' % (n, len(todo), name[:40], r['summary'],
                                      '; '.join(r['notes'])[:120]), flush=True)
    await c.close()
    rows = [results[k] for k in sorted(results)]
    for p in sheet(rows, a.out):
        print('sheet', p)


if __name__ == '__main__':
    asyncio.run(main())
