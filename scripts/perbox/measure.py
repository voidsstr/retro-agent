"""Measure one (box, title) verification cell on real hardware.

Records: does it launch, actual resolution/refresh, fullscreen or not,
renderer, and screenshot evidence.  An untested cell is NEVER recorded as a
pass -- every status here comes from an observation.
"""
import asyncio, json, os, re, sys, time, struct
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from fleetlib import Box, jload

LIB = '/mnt/retro-share/Files/Games-Library'
EVID = os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(
        os.path.abspath(__file__)))), 'evidence')
# Processes that are NOT the game.  This list is load-bearing: `daemon.exe`
# (the Daemon Tools mounter) appearing while a disc-mount title is still
# mounting made two cells on .240 score `runs` with the game never started --
# this project's signature failure, a helper's success read as the real
# thing.  A cell whose ONLY new processes are these is a launch failure.
IGNORE = {'cmd.exe','conhost.exe','fleetres.exe','taskkill.exe','net.exe',
          'regedit.exe','dwwin.exe','dumprep.exe','wmiprvse.exe','csrss.exe',
          'daemon.exe','searchfilterhost.exe','find.exe','findstr.exe',
          'rundll32.exe','wuauclt.exe','imapi.exe','msiexec.exe','alg.exe',
          'ctfmon.exe','explorer.exe','wscntfy.exe','logonui.exe'}

def _is_game(name):
    """A transient ~xxxx.tmp helper is not the game either."""
    return name not in IGNORE and not re.match(r'^~[0-9a-f]+\.tmp$', name)

def launch_entries(title):
    """(target, display_name) per launch.txt data line, first 1023 bytes."""
    p = os.path.join(LIB, title, 'launch.txt')
    if not os.path.exists(p): return []
    out = []
    with open(p,'rb') as f: raw = f.read(1023)
    for ln in raw.decode('latin-1').splitlines():
        if not ln.strip() or ln.lstrip().startswith('#'): continue
        f_ = ln.split('\t')
        if len(f_) >= 2: out.append((f_[0].strip(), f_[1].strip()))
    return out

def bmp_stats(data):
    """(mean_luma, width, height) of a 24-bit BMP, sampled."""
    try:
        off = struct.unpack('<I', data[10:14])[0]
        w   = struct.unpack('<i', data[18:22])[0]
        h   = abs(struct.unpack('<i', data[22:26])[0])
        bpp = struct.unpack('<H', data[28:30])[0]
        if bpp != 24: return (None, w, h)
        row = ((w*3 + 3)//4)*4
        tot = n = 0
        for y in range(0, h, max(1, h//60)):
            base = off + y*row
            for x in range(0, w, max(1, w//60)):
                i = base + x*3
                if i+3 <= len(data):
                    tot += data[i]+data[i+1]+data[i+2]; n += 3
        return (tot/n if n else None, w, h)
    except Exception:
        return (None, None, None)

async def procset(b):
    j = jload(await b.cmd('PROCLIST')) or []
    return {(p['pid'], p['name'].lower()) for p in j if isinstance(p, dict)}

async def mode(b):
    return jload(await b.cmd('DISPLAYCFG get')) or {}

async def measure(b, ip, title, gamedir, target, settle=35, shot=True):
    """Launch one shortcut, observe, screenshot, clean up."""
    r = {'ip': ip, 'title': title, 'target': target, 'ts': time.strftime('%Y-%m-%dT%H:%M:%S'),
         'status': 'untested', 'evidence': None}
    base_procs = await procset(b)
    base_mode  = await mode(b)
    r['desktop_mode_before'] = base_mode
    path = gamedir + '\\' + target
    chk = await b.cmd(f'EXEC cmd /c if exist "{path}" (echo YES) else (echo NO)')
    if 'YES' not in chk:
        r['status'] = 'not_deployed'; r['note'] = f'{path} absent'; return r
    await b.cmd(f'EXEC cmd /c start "" /D "{gamedir}" "{target}"')
    await asyncio.sleep(settle)
    now = await procset(b)
    new = [n for (p, n) in (now - base_procs) if _is_game(n)]
    r['new_processes'] = sorted(set(new))
    r['desktop_mode_after'] = m = await mode(b)
    if not new:
        r['status'] = 'launch_failed'
        r['note'] = 'no GAME process after %ds (helpers seen: %s)' % (
            settle, ','.join(sorted({n for (p, n) in (now - base_procs)})) or 'none')
    else:
        r['status'] = 'runs'
        r['mode'] = f"{m.get('width')}x{m.get('height')}x{m.get('bpp')}@{m.get('refresh')}"
        r['mode_changed'] = (m != base_mode)
    if shot and new:
        try:
            data = await b.binary('SCREENSHOT 0')
            luma, w, h = bmp_stats(data)
            r['shot_luma'] = None if luma is None else round(luma, 1)
            r['shot_dims'] = f'{w}x{h}'
            os.makedirs(EVID, exist_ok=True)
            fn = f"{ip.split('.')[-1]}_{title}_{target.replace(' ','_').replace('.bat','')}.bmp"
            fp = os.path.join(EVID, fn)
            with open(fp, 'wb') as f: f.write(data)
            r['evidence'] = fp
            r['fullscreen'] = (f'{w}x{h}' == f"{m.get('width')}x{m.get('height')}")
            if luma is not None and luma < 3:
                r['capture'] = 'black - exclusive fullscreen surface, GDI cannot capture'
        except Exception as e:
            r['shot_error'] = f'{type(e).__name__}: {e}'
    for n in set(new):
        await b.cmd(f'EXEC cmd /c taskkill /f /im "{n}"')
    await asyncio.sleep(3)
    fin = await mode(b)
    if fin != base_mode:
        await b.cmd(f"DISPLAYCFG set {base_mode.get('width')} {base_mode.get('height')} "
                    f"{base_mode.get('bpp')} {base_mode.get('refresh')}")
        r['restored_mode'] = True
    return r
