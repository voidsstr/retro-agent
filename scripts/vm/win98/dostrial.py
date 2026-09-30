#!/usr/bin/env python3
"""dostrial.py - run ONE command in REAL DOS on the Win98 build VM and photograph it.

    python3 scripts/vm/win98/dostrial.py --dir 'C:\\GAMES\\FLIGH~27' --run 'FALCON3.EXE' \\
        [--run ...] [--ems] [--seconds 150] [--every 6] [--keys-at 60:Escape,Return] --name falcon
    python3 scripts/vm/win98/dostrial.py --pif 'Falcon 3.0.pif' --name falcon     # end to end

--pif runs a staged real-DOS title's own desktop shortcut the way a double-click
does - its Y/N prompt, the copy to C:, the restart - and photographs the game.

The staged real-DOS titles (stage_win9x_dos.py real_dos) run from the one-shot
rundos line in C:\\AUTOEXEC.BAT: the launcher writes C:\\RUNDOS\\NEXT.BAT and
restarts the PC. This does the same with any command - so a game can be tried
in real DOS before its title is (re)generated - then photographs 86Box's own
display (the agent is gone while DOS runs), and finally powers the VM off and
on: the hook deletes the stale RAN.BAT first, so the game never runs twice.

--ems: the game wants EMS, which real DOS has only when CONFIG.SYS loaded
EMM386 (boot menu 2 on .243 and in the VM). CONFIG.SYS's menudefault is set to
EMS for this one boot and put back, byte for byte, once Windows is up again.

The VM must already carry the rundos hook in AUTOEXEC.BAT (any staged real-DOS
title's launcher appends it; this script refuses to run without it).
"""
import argparse
import asyncio
import os
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import sweep  # noqa: E402


def next_bat(directory, lines):
    if not re.match(r'^[A-Za-z]:\\', directory):
        raise SystemExit('--dir must be an absolute DOS path, got %r' % directory)
    for l in lines:
        if re.search(r'[()]', l):
            raise SystemExit('no parentheses in a COMMAND.COM line: %r' % l)
    out = ['@echo off', directory[:2], 'cd %s' % directory] + list(lines) + ['C:', 'cd \\']
    return ('\r\n'.join(out) + '\r\n').encode('ascii')


def ems_config(cfg):
    """CONFIG.SYS with its menudefault naming the EMS block (timeout kept)."""
    txt = cfg.decode('latin-1')
    m = re.search(r'(?im)^menudefault\s*=\s*([^,\r\n]+)(.*)$', txt)
    if not m or not re.search(r'(?im)^\[EMS\]', txt):
        raise SystemExit('CONFIG.SYS has no menudefault or no [EMS] block - cannot select EMS')
    return (txt[:m.start(1)] + 'EMS' + txt[m.end(1):]).encode('latin-1')


LOGO = os.path.join(HERE, 'shutdown-logo.png')      # 80x60 grey thumbnail of the real screen


def is_shutdown_screen(img):
    """Win98's "Windows is shutting down" logo, matched against a thumbnail of
    the real one (taken in this VM). A colour rule was not enough: Falcon 3.0's
    sky-blue title screen passed for it and the trial power-cycled a running
    game. Measured: the logo scores 0-1, game and DOS screens 46-180."""
    from PIL import Image, ImageChops, ImageStat
    ref = Image.open(LOGO).convert('L')
    cur = sweep.emulated(img).convert('L').resize(ref.size, Image.BILINEAR)
    return ImageStat.Stat(ImageChops.difference(cur, ref)).mean[0] < 12


async def shutdown_or_reset(out, limit=60):
    """In 86Box the agent's REBOOT sometimes sits at "Windows is shutting down"
    for good (seen 2026-09-30: 144 s and counting, where the same REBOOT had
    restarted the VM twice that day). NEXT.BAT reached the disk before the
    REBOOT, so a power cycle is safe: the hook still runs it - the Falcon 3.0
    trial proved exactly that. Only a screen STUCK on the logo is reset."""
    t0, stuck_since = time.time(), None
    while time.time() - t0 < limit + 30:
        img = sweep.shot(os.path.join(out, '_shutdown.png'))
        if is_shutdown_screen(img):
            stuck_since = stuck_since or time.time()
            if time.time() - stuck_since > limit:
                print('shutdown hung on the logo for %ds - power cycling' % limit, flush=True)
                subprocess.run(['systemctl', '--user', 'stop', 'w98box'], check=False)
                time.sleep(3)
                subprocess.run(['systemd-run', '--user', '--unit=w98box', '-p', 'CPUQuota=200%',
                                '-p', 'MemoryMax=2G', os.path.join(HERE, 'run-98.sh')],
                               check=False, capture_output=True)
                return
        elif stuck_since or time.time() - t0 > 15:
            return                              # moved on (or never showed it): rebooting
        await asyncio.sleep(5)


def keys_plan(spec):
    plan = []
    for item in spec or []:
        t, keys = item.split(':', 1)
        plan.append((int(t), keys.split(',')))
    return sorted(plan)


async def launch_pif(c, pif, out, limit=240, stuck=60):
    """--pif: START the title's own desktop shortcut - what a double-click does
    - and follow it through the launcher's Y/N prompt (10 s, default Y), the
    copy to C:, and the restart. Returns once the PC is past the shutdown
    screen (power-cycling a shutdown that sticks on the logo, as REBOOT can)."""
    await sweep.cmd(c, 'LAUNCH C:\\WINDOWS\\COMMAND\\START.EXE "C:\\WINDOWS\\Desktop\\%s"' % pif)
    try:
        await c.close()
    except Exception:
        pass
    t0, stuck_since, n = time.time(), None, 0
    while time.time() - t0 < limit:
        n += 1
        img = sweep.shot(os.path.join(out, 'l%03d-%03ds.png' % (n, int(time.time() - t0))))
        if is_shutdown_screen(img):
            stuck_since = stuck_since or time.time()
            if time.time() - stuck_since > stuck:
                print('shutdown hung on the logo for %ds - power cycling' % stuck, flush=True)
                subprocess.run(['systemctl', '--user', 'stop', 'w98box'], check=False)
                time.sleep(3)
                subprocess.run(['systemd-run', '--user', '--unit=w98box', '-p', 'CPUQuota=200%',
                                '-p', 'MemoryMax=2G', os.path.join(HERE, 'run-98.sh')],
                               check=False, capture_output=True)
                return
        elif stuck_since:
            return                              # past the logo: the PC is restarting
        await asyncio.sleep(5)
    print('no restart within %ds - the launcher did not get that far (see the l*.png frames)' % limit)


async def arm_and_reboot(c, a, out):
    """--dir/--run: write C:\\RUNDOS\\NEXT.BAT (and, with --ems, point CONFIG.SYS's
    menudefault at [EMS]) and restart through the agent. Returns the original
    CONFIG.SYS when it was changed, for putting back afterwards."""
    auto = await sweep.cmd(c, 'DOWNLOAD C:\\AUTOEXEC.BAT', binary=True)
    if b'RUNDOS\\NEXT.BAT' not in auto:
        raise SystemExit('C:\\AUTOEXEC.BAT has no rundos hook - launch a staged real-DOS title once first')
    cfg_orig = None
    if a.ems:
        cfg_orig = await sweep.cmd(c, 'DOWNLOAD C:\\CONFIG.SYS', binary=True)
        open(os.path.join(out, 'CONFIG.SYS.orig'), 'wb').write(cfg_orig)
        new = ems_config(cfg_orig)
        await c.send_command('UPLOAD C:\\CONFIG.SYS', binary_payload=new)
        if await sweep.cmd(c, 'DOWNLOAD C:\\CONFIG.SYS', binary=True) != new:
            raise SystemExit('CONFIG.SYS did not upload intact')
    await c.send_command('MKDIR C:\\RUNDOS')
    nb = next_bat(a.dir, a.run)
    await c.send_command('UPLOAD C:\\RUNDOS\\NEXT.BAT', binary_payload=nb)
    if await sweep.cmd(c, 'DOWNLOAD C:\\RUNDOS\\NEXT.BAT', binary=True) != nb:
        raise SystemExit('NEXT.BAT did not upload intact')
    await asyncio.sleep(6)                     # let Win98's lazy writer reach the disk
    await c.send_command('REBOOT')             # SHExitWindowsEx 6 on Win9x (agent 1.85.4+)
    try:
        await c.close()
    except Exception:
        pass
    await shutdown_or_reset(out)
    return cfg_orig


async def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--dir')
    ap.add_argument('--run', action='append')
    ap.add_argument('--pif', help="a desktop shortcut's file name: run the staged launcher end to end")
    ap.add_argument('--ems', action='store_true')
    ap.add_argument('--seconds', type=int, default=150)
    ap.add_argument('--every', type=int, default=6)
    ap.add_argument('--keys-at', action='append', help='SECONDS:KEY[,KEY] - type keys into 86Box then')
    ap.add_argument('--name', default='dostrial')
    ap.add_argument('--out', default=os.path.expanduser('~/.retro-fleet/w98vm/dostrial'))
    a = ap.parse_args()
    out = os.path.join(a.out, a.name)
    os.makedirs(out, exist_ok=True)

    c = await sweep.connect()
    cfg_orig = None
    if a.pif:
        await launch_pif(c, a.pif, out)
    else:
        if not a.dir or not a.run:
            raise SystemExit('--dir and --run, or --pif')
        cfg_orig = await arm_and_reboot(c, a, out)

    t0, n = time.time(), 0
    plan = keys_plan(a.keys_at)
    while time.time() - t0 < a.seconds:
        el = time.time() - t0
        while plan and plan[0][0] <= el:
            _, keys = plan.pop(0)
            subprocess.run([sys.executable, os.path.join(HERE, 'vmkey.py')] + keys, capture_output=True)
            print('%4ds keys %s' % (el, keys), flush=True)
        n += 1
        try:
            sweep.shot(os.path.join(out, 'f%03d-%03ds.png' % (n, int(el))))
        except Exception as e:
            print('shot failed:', e)
        await asyncio.sleep(max(0, a.every - (time.time() - t0 - el)))
    print('%d frame(s) in %s' % (n, out))

    c = await sweep.reset_vm()                 # power cycle: the hook drops RAN.BAT first
    if cfg_orig is not None:
        await c.send_command('UPLOAD C:\\CONFIG.SYS', binary_payload=cfg_orig)
        back = await sweep.cmd(c, 'DOWNLOAD C:\\CONFIG.SYS', binary=True)
        print('CONFIG.SYS restored' if back == cfg_orig else 'CONFIG.SYS NOT RESTORED - fix by hand')
    left = [e['name'] for e in __import__('json').loads(await sweep.cmd(c, 'DIRLIST C:\\RUNDOS'))
            if not e.get('is_dir')]
    print('C:\\RUNDOS now holds:', left)
    await c.close()


if __name__ == '__main__':
    asyncio.run(main())
