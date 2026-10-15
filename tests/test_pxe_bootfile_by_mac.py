#!/usr/bin/env python3
"""One machine gets Windows 7, every other machine keeps XP - and the hold
still arms for both.

`bootfile_by_mac` (2026-10-15) hands a listed MAC the Windows 7 iPXE chain
(win7/undionly.kpxe) while the default stays startrom.n12. Three things this
pins, each of which fails silently:

  * the per-MAC choice is made, case-insensitively, and an unlisted MAC still
    gets the default. The XP default is UNATTENDED and repartitions, so a Win7
    box that falls through to it is wiped (two did, 2026-10-14/15, before the
    Win7 chain existed);
  * the boot hold arms on EVERY servable boot file. It used to compare against
    cfg['bootfile'] only, so a machine served anything else was re-offered on
    every boot - the reinstall loop that destroyed a finished install on
    2026-09-02;
  * the HTTP payload server (boot.wim is 145 MB; over this TFTP two of three
    transfers logged DONE were rejected by iPXE) serves files and nothing else.

Plus source checks on the Win7 build's own traps (make-win7-source.sh).
"""
import importlib.util
import os
import socket
import tempfile
import threading
import time
import urllib.error
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
PXE = os.path.join(HERE, os.pardir, 'scripts', 'pxe')


def load(name):
    spec = importlib.util.spec_from_file_location(name, os.path.join(PXE, name + '.py'))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


pxe = load('pxe_server')
pxe.LOGFILE = None
FAILS = []


def check(name, cond):
    print(('  PASS  ' if cond else '  FAIL  ') + name)
    if not cond:
        FAILS.append(name)


class Req:
    def __init__(self, mac, arch=0):
        self.mac, self.arch = mac, arch


CFG = {'bootfile': 'startrom.n12', 'bootfile_by_arch': {},
       'bootfile_by_mac': {'D4:BE:D9:B9:BF:93': 'win7/undionly.kpxe'}}


def test_selection():
    p = pxe.ProxyDHCP(CFG, 4011)
    check('listed MAC (other case) gets the Win7 chain',
          p.bootfile(Req('d4:be:d9:b9:bf:93')) == 'win7/undionly.kpxe')
    check('unlisted MAC keeps the XP default',
          p.bootfile(Req('00:11:22:33:44:55')) == 'startrom.n12')
    check('no MAC keeps the XP default', p.bootfile(Req('')) == 'startrom.n12')
    check('per-MAC beats per-arch',
          pxe.ProxyDHCP(dict(CFG, bootfile_by_arch={'0': 'x.0'}), 4011)
          .bootfile(Req('d4:be:d9:b9:bf:93')) == 'win7/undionly.kpxe')
    check('DEFAULT_CONFIG declares bootfile_by_mac and http_port',
          pxe.DEFAULT_CONFIG.get('bootfile_by_mac') == {}
          and pxe.DEFAULT_CONFIG.get('http_port') == 8069)


def test_hold_arms_on_every_bootfile():
    s = pxe.servable_bootfiles(CFG)
    for name in ('startrom.n12', '/startrom.n12', 'win7/undionly.kpxe',
                 '\\win7\\undionly.kpxe', '/WIN7/UNDIONLY.KPXE'):
        check(f'{name!r} arms the hold', pxe.norm_bootpath(name) in s)
    for name in ('undionly.kpxe', 'win7/boot.wim', 'win7/boot.ipxe', 'ntldr'):
        check(f'{name!r} does not arm it', pxe.norm_bootpath(name) not in s)
    src = open(os.path.join(PXE, 'pxe_server.py')).read()
    check('the TFTP DONE path arms via servable_bootfiles',
          'norm_bootpath(filename) in servable_bootfiles(self.cfg)' in src)
    check("the old cfg['bootfile']-only comparison is gone",
          "== str(\n                    self.cfg.get('bootfile', '')).lower()" not in src)


def test_http_server():
    root = tempfile.mkdtemp()
    os.makedirs(os.path.join(root, 'win7'))
    payload = os.urandom(70000)
    with open(os.path.join(root, 'win7', 'boot.wim'), 'wb') as f:
        f.write(payload)
    s = socket.socket()
    s.bind(('127.0.0.1', 0))
    port = s.getsockname()[1]
    s.close()
    t = pxe.HTTPServer({'tftp_root': root, 'http_port': port})
    t.start()
    base = f'http://127.0.0.1:{port}'
    for _ in range(50):
        try:
            urllib.request.urlopen(base + '/win7/boot.wim', timeout=2).read()
            break
        except Exception:                               # noqa: BLE001
            time.sleep(0.1)
    got = urllib.request.urlopen(base + '/win7/boot.wim', timeout=5).read()
    check('HTTP serves a file byte-identical', got == payload)

    def status(path):
        try:
            return urllib.request.urlopen(base + path, timeout=5).status
        except urllib.error.HTTPError as e:
            return e.code
    check('no directory listing', status('/win7/') == 404)
    check('no listing of the root', status('/') == 404)
    check('no escape from the root', status('/../../etc/passwd') == 404)


def test_win7_build_traps():
    sh = open(os.path.join(PXE, 'make-win7-source.sh')).read()
    cmd = open(os.path.join(PXE, 'win7-install.cmd')).read()
    check('build sets cmd AutoRun (winpeshl drops LaunchApps args)',
          'AutoRun' in sh and 'string:X:\\\\fleet\\\\install.cmd' in sh)
    check('winpeshl.ini launches bare cmd.exe, no arguments',
          "X:\\\\Windows\\\\System32\\\\cmd.exe\\r\\n' > peadd/winpeshl.ini" in sh)
    check('no grep -m1 in a pipe under pipefail (SIGPIPE, exit 141)',
          '| grep -m1' not in sh and 'set -euo pipefail' in sh)
    check('RTL8168 drivers for both arches, sha256-pinned (inbox netrtx32 stops at REV_03)',
          'rt86win7' in sh and 'rt64win7' in sh and 'sha256sum -c' in sh
          and 'X86_SHA=' in sh and 'X64_SHA=' in sh)
    check('boot.ipxe picks x64 by CPU long mode, x86 otherwise',
          "cpuid --ext 29 && set a x64 || set a x86" in sh)
    check('install.cmd takes the NAS copy matching its own architecture',
          '"%PROCESSOR_ARCHITECTURE%"=="AMD64" set SRC=Z:\\Files\\OS\\Win7PRO-x64-PXE' in cmd)
    check('wpeinit and the network bring-up never block the launcher',
          'start "wpeinit" /min wpeinit' in cmd
          and 'start "network" /min wpeutil InitializeNetwork' in cmd)
    check('payload goes over HTTP', 'set s http://' in sh)
    check('install.cmd guards against AutoRun re-entry',
          'if defined FLEETRAN goto :eof' in cmd and 'set FLEETRAN=1' in cmd)
    check('install.cmd re-initialises the network after drvload',
          cmd.index('drvload') < cmd.index('wpeutil InitializeNetwork'))
    check('install.cmd has no < or > in a rem line (cmd parser)',
          not any(l.lower().startswith('rem') and ('<' in l or '>' in l)
                  for l in cmd.splitlines()))
    check('the repo copy is LF (the build converts to CRLF)', '\r' not in cmd)
    # .186 (REV_02, 2026-10-15): WinPE's inbox netrtx64 had already bound, and
    # drvload of the same-named Rt64win7.sys failed over it - "errored out".
    check('drvload is skipped when an RTL8167 driver is already bound',
          'find /i "RTL8167"' in cmd and 'goto inboxnic' in cmd
          and cmd.index('find /i "RTL8167"') < cmd.index('drvload "'))
    # .145 (2026-10-15): Setup finished and Windows 7 booted with no network -
    # a driver drvload'ed in WinPE never reaches the installed image.
    import xml.dom.minidom
    ua_path = os.path.join(PXE, 'win7-unattend.xml')
    ua = xml.dom.minidom.parse(ua_path)
    passes = [e.getAttribute('pass') for e in ua.getElementsByTagName('settings')]
    archs = sorted(c.getAttribute('processorArchitecture')
                   for c in ua.getElementsByTagName('component'))
    paths = [p.firstChild.data for p in ua.getElementsByTagName('Path')]
    check('unattend: offlineServicing ONLY, so every Setup page stays interactive',
          passes == ['offlineServicing'])
    check('unattend: PnpCustomizationsNonWinPE for amd64 and x86',
          archs == ['amd64', 'x86'] and all(
              c.getAttribute('name') == 'Microsoft-Windows-PnpCustomizationsNonWinPE'
              for c in ua.getElementsByTagName('component')))
    check('unattend: injects X:\\fleet\\drivers', paths == ['X:\\fleet\\drivers'] * 2)
    check('unattend: no product key or password in a served file',
          not any(t in open(ua_path).read() for t in ('ProductKey', 'Password', '<Key>')))
    check('build stages unattend.xml and install.cmd passes it to setup.exe',
          'peadd/fleet/unattend.xml' in sh
          and 'set UA=/unattend:X:\\fleet\\unattend.xml' in cmd
          and 'setup.exe %UA%' in cmd)
    check('no sc.exe (WinPE 3.0 does not ship it)',
          not any(l.strip().lower().startswith('sc ') or ' sc query' in l.lower()
                  for l in cmd.splitlines() if not l.lower().startswith('rem')))


def test_pxe_bootfile_by_mac():
    test_selection()
    test_hold_arms_on_every_bootfile()
    test_http_server()
    test_win7_build_traps()
    assert not FAILS, FAILS


if __name__ == '__main__':
    test_selection()
    test_hold_arms_on_every_bootfile()
    test_http_server()
    test_win7_build_traps()
    print(f'\n{"FAILED: " + ", ".join(FAILS) if FAILS else "all passed"}')
    raise SystemExit(1 if FAILS else 0)
