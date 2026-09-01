#!/usr/bin/env python3
"""
foolproof-disk.py - finish a PXE-installed XP disk offline, from Linux.

WHY THIS EXISTS
    The live PXE unattend carries `OemPreinstall = No`. cmdlines.txt still runs
    at T-12, so the registry merge lands - but text-mode setup never copies
    `$OEM$\\$1`, and the machine comes up with:

        HKLM\\...\\Run\\RetroAgent = C:\\RETRO_AGENT\\retro_agent.exe   <- not there
        DevicePath               = ...;C:\\D\\C001;...                <- not there

    It boots, auto-logs-on, and then does nothing, while every device XP has no
    in-box driver for raises a Found New Hardware wizard. The signature is
    unmissable once you know it: a correctly imaged box shows several
    "Found ... in C:\\D\\" lines in setupapi.log; a box with this hole shows ZERO.

    Applying the payload offline is also simply faster than fixing the unattend:
    2.4 GB over SMB1 in text-mode setup is many minutes of a machine you cannot
    watch, against a verified copy at USB speed that you can.

WHAT "FOOL PROOF" MEANS HERE
    The disk boots in ANY fleet box and reaches a working, agent-connected,
    game-syncing desktop with nobody at the keyboard:

      payload  the agent, its newimage flag, the C:\\D driver tree DevicePath
               already points at, the wallpapers, PowerStrip.
      storage  every in-box IDE miniport present and boot-start, plus the
               CriticalDeviceDatabase entries that bind a controller before the
               disk driver is up. Without this a disk imaged on one chipset
               STOP 0x7B's on the next.
      harden   no dialog can sit in front of the desktop waiting for a human:
               no hardware wizard, no error-report prompt, no Automatic Updates
               first-run, no hard-error popups, no screensaver, no standby.

    Every file copied is verified by SHA-256 read back off the target, not by
    the exit status of the copy - a dropped write that leaves a short file is a
    failure mode this project has already been bitten by.

USAGE
    python3 scripts/pxe/foolproof-disk.py --root /mnt/xpdisk [--stage all]
    python3 scripts/pxe/foolproof-disk.py --root /mnt/xpdisk --check

    --root      the XP system drive, mounted read-WRITE (this is C:\\)
    --image     the fleet image (default /mnt/retro-share/Files/OS/XPSP3-FLEET)
    --stage     check | payload | storage | harden | all   (default all)
    --check     alias for --stage check; never writes

    Idempotent: safe to re-run. A second run recopies nothing that already
    matches by size and hash.
"""

import argparse
import hashlib
import os
import re
import shutil
import subprocess
import sys
import time

REG_SZ, REG_EXPAND_SZ, REG_BINARY, REG_DWORD = 1, 2, 3, 4

HDC_CLASS = '{4D36E96A-E325-11CE-BFC1-08002BE10318}'
DISK_CLASS = '{4D36E967-E325-11CE-BFC1-08002BE10318}'

# Copied to C:\ (or into WINDOWS / Program Files) exactly as text-mode setup
# would have done from $OEM$. Source is relative to the image root.
PAYLOAD = [
    (os.path.join('$OEM$', '$1', 'RETRO_AGENT'), 'RETRO_AGENT'),
    (os.path.join('$OEM$', '$1', 'retro-wall'), 'retro-wall'),
    (os.path.join('$OEM$', '$1', 'D'), 'D'),
    (os.path.join('$OEM$', '$Progs', 'PowerStrip'),
     os.path.join('Program Files', 'PowerStrip')),
    (os.path.join('$OEM$', '$$'), 'WINDOWS'),
]

# The in-box IDE miniports. atapi and pciide are always installed; the
# vendor ones are only laid down for the controller setup actually saw, so on a
# disk that is about to move to different hardware they have to be expanded off
# the media first. A boot-start service whose .sys is absent is a dead boot, so
# the two go together and neither is done without the other.
MINIPORTS = {
    'atapi': 'ATAPI.SY_',
    'pciide': 'PCIIDE.SY_',
    'intelide': 'INTELIDE.SY_',
    'viaide': 'VIAIDE.SY_',
    'aliide': 'ALIIDE.SY_',
    'cmdide': 'CMDIDE.SY_',
    'toside': 'TOSIDE.SY_',
}
# pciide is a bus driver and needs its extension library beside it.
EXTRA_DRIVER_FILES = {'pciidex.sys': 'PCIIDEX.SY_'}

# The generic CriticalDeviceDatabase entries. pci#cc_0101 (any PCI IDE-class
# controller -> pciide) is stock XP; the channel entries are not, and they are
# what covers a controller enumerated as a legacy channel rather than by class.
GENERIC_CDDB = [
    ('primary_ide_channel', 'atapi', HDC_CLASS),
    ('secondary_ide_channel', 'atapi', HDC_CLASS),
    ('*pnp0600', 'atapi', HDC_CLASS),
    ('*azt0502', 'atapi', HDC_CLASS),
    ('pci#cc_0101', 'pciide', HDC_CLASS),
    ('gendisk', 'disk', DISK_CLASS),
]


def log(msg):
    print(msg, flush=True)


# --------------------------------------------------------------------------
# hivex wrapper
# --------------------------------------------------------------------------
class Hive(object):
    """Thin hivex wrapper. hivex raises RuntimeError('Success') for a missing
    value rather than returning None, which is worth hiding exactly once."""

    def __init__(self, path, write=False):
        import hivex
        self.path = path
        self.write = write
        self.h = hivex.Hivex(path, write=write)
        self.dirty = False

    def node(self, path, create=False):
        n = self.h.root()
        for part in path.split('\\'):
            if not part:
                continue
            try:
                c = self.h.node_get_child(n, part)
            except Exception:
                c = None
            if c is None:
                if not create:
                    return None
                c = self.h.node_add_child(n, part)
                self.dirty = True
            n = c
        return n

    def get(self, path, name):
        n = self.node(path)
        if n is None:
            return None
        try:
            v = self.h.node_get_value(n, name)
        except Exception:
            return None
        if v is None:
            return None
        t, d = self.h.value_value(v)
        if t in (REG_SZ, REG_EXPAND_SZ):
            return d.decode('utf-16-le', 'replace').rstrip('\0')
        if t == REG_DWORD:
            return self.h.value_dword(v)
        return d

    def set_sz(self, path, name, value):
        self._set(path, name, REG_SZ, value.encode('utf-16-le') + b'\0\0')

    def set_dword(self, path, name, value):
        self._set(path, name, REG_DWORD, value.to_bytes(4, 'little'))

    def _set(self, path, name, t, data):
        n = self.node(path, create=True)
        self.h.node_set_value(n, {'key': name, 't': t, 'value': data})
        self.dirty = True

    def children(self, path):
        n = self.node(path)
        return [] if n is None else [self.h.node_name(c) for c in self.h.node_children(n)]

    def commit(self):
        if self.write and self.dirty:
            self.h.commit(None)
        return self.dirty


def control_set(syshive):
    """Resolve HKLM\\SYSTEM\\CurrentControlSet for an offline hive."""
    cur = syshive.get('Select', 'Current')
    return 'ControlSet%03d' % (cur if isinstance(cur, int) and cur else 1)


# --------------------------------------------------------------------------
# stage: check
# --------------------------------------------------------------------------
def stage_check(root, image):
    ok = True
    log('== check ==')
    for f in ('ntldr', 'boot.ini', os.path.join('WINDOWS', 'system32', 'config', 'system')):
        p = os.path.join(root, f)
        exists = os.path.exists(p)
        ok = ok and exists
        log('  %-46s %s' % (f, 'present' if exists else 'MISSING'))
    if not ok:
        log('  --> %s is not an XP system drive' % root)
        return False

    sw = Hive(os.path.join(root, 'WINDOWS', 'system32', 'config', 'software'))
    sy = Hive(os.path.join(root, 'WINDOWS', 'system32', 'config', 'system'))
    cs = control_set(sy)
    log('  control set                                    %s' % cs)
    run = sw.get(r'Microsoft\Windows\CurrentVersion\Run', 'RetroAgent')
    log('  Run\\RetroAgent                                 %s' % (run or '<absent>'))
    dp = sw.get(r'Microsoft\Windows\CurrentVersion', 'DevicePath') or ''
    log('  DevicePath                                     %d chars, %d dirs'
        % (len(dp), len([x for x in dp.split(';') if x])))
    log('  Winlogon AutoAdminLogon                        %s'
        % (sw.get(r'Microsoft\Windows NT\CurrentVersion\Winlogon', 'AutoAdminLogon') or '<absent>'))
    st, oip = sy.get('Setup', 'SetupType'), sy.get('Setup', 'OobeInProgress')
    log('  SetupType=%s OobeInProgress=%s                    %s'
        % (st, oip, 'OOBE WILL RUN AND WAIT FOR A HUMAN'
           if (st or 0) or (oip or 0) else 'boots straight to logon'))

    log('  -- payload on disk --')
    # Counted against the SOURCE file list, never against whatever happens to be
    # in the destination: $OEM$\\$$ lands inside C:\\WINDOWS, so "how many files
    # are in the destination" would report a stock XP as fully provisioned.
    for src_rel, dst in PAYLOAD:
        src = os.path.join(image, src_rel)
        if not os.path.isdir(src):
            log('    %-42s SOURCE MISSING ON IMAGE' % dst)
            continue
        want = have = 0
        for dirpath, _dn, fns in os.walk(src):
            rel = os.path.relpath(dirpath, src)
            for fn in fns:
                want += 1
                dp = os.path.normpath(os.path.join(root, dst, rel, fn))
                if os.path.exists(dp) and \
                   os.path.getsize(dp) == os.path.getsize(os.path.join(dirpath, fn)):
                    have += 1
        log('    %-42s %d/%d files%s'
            % (dst, have, want, '' if have == want else '   <-- INCOMPLETE'))

    log('  -- storage --')
    svcs = sy.children(cs + '\\Services')
    lower = {s.lower(): s for s in svcs}
    for svc in MINIPORTS:
        start = sy.get(cs + '\\Services\\' + lower.get(svc, svc), 'Start') if svc in lower else None
        sysf = os.path.join(root, 'WINDOWS', 'system32', 'drivers', svc + '.sys')
        log('    %-12s Start=%-8s file=%s'
            % (svc, start if start is not None else '-',
               'yes' if os.path.exists(sysf) else 'NO'))
    cddb = set(x.lower() for x in sy.children(cs + '\\Control\\CriticalDeviceDatabase'))
    missing = [n for n, _, _ in GENERIC_CDDB if n.lower() not in cddb]
    log('    CriticalDeviceDatabase: %d entries, %d of the generic set missing%s'
        % (len(cddb), len(missing), (' (%s)' % ', '.join(missing)) if missing else ''))
    return True


# --------------------------------------------------------------------------
# stage: payload
# --------------------------------------------------------------------------
def sha256_of(path, chunk=1 << 20):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for b in iter(lambda: f.read(chunk), b''):
            h.update(b)
    return h.hexdigest()


def copy_verified(src, dst, chunk=1 << 20):
    """Copy and prove it. Returns (action, error). Hashes the source while
    streaming it, then reads the destination back - the copy's exit status is
    not evidence that the bytes arrived."""
    ssize = os.path.getsize(src)
    if os.path.exists(dst) and os.path.getsize(dst) == ssize:
        if sha256_of(dst) == sha256_of(src):
            return 'skip', None
    d = os.path.dirname(dst)
    if d:
        os.makedirs(d, exist_ok=True)
    h = hashlib.sha256()
    with open(src, 'rb') as fi, open(dst, 'wb') as fo:
        for b in iter(lambda: fi.read(chunk), b''):
            h.update(b)
            fo.write(b)
        fo.flush()
        os.fsync(fo.fileno())
    dsize = os.path.getsize(dst)
    if dsize != ssize:
        return 'copy', 'short write: %d of %d bytes' % (dsize, ssize)
    if sha256_of(dst) != h.hexdigest():
        return 'copy', 'hash mismatch after copy'
    return 'copy', None


def stage_payload(root, image):
    log('== payload ==')
    total = copied = skipped = 0
    failures = []
    t0 = time.time()
    for src_rel, dst_rel in PAYLOAD:
        src = os.path.join(image, src_rel)
        dst = os.path.join(root, dst_rel)
        if not os.path.isdir(src):
            failures.append('%s: not on the image' % src_rel)
            log('  %-24s SOURCE MISSING' % dst_rel)
            continue
        n = c = s = 0
        bytes_done = 0
        for dirpath, _dirnames, filenames in os.walk(src):
            rel = os.path.relpath(dirpath, src)
            for fn in filenames:
                sp = os.path.join(dirpath, fn)
                dp = os.path.normpath(os.path.join(dst, rel, fn))
                n += 1
                action, err = copy_verified(sp, dp)
                if err:
                    failures.append('%s: %s' % (os.path.join(dst_rel, rel, fn), err))
                elif action == 'copy':
                    c += 1
                    bytes_done += os.path.getsize(sp)
                else:
                    s += 1
        total += n
        copied += c
        skipped += s
        log('  %-24s %5d files  (%d copied, %d already correct, %.1f MB)'
            % (dst_rel, n, c, s, bytes_done / 1048576.0))
    log('  %d files, %d copied, %d already correct, %d failed, %.0fs'
        % (total, copied, skipped, len(failures), time.time() - t0))
    for f in failures[:20]:
        log('    FAIL %s' % f)
    return not failures


# --------------------------------------------------------------------------
# stage: storage (universal boot)
# --------------------------------------------------------------------------
def parse_mshdc(image, workdir):
    """Read PCI id -> IDE service straight out of the media's own mshdc.inf.
    Hand-copied MergeIDE lists rot the moment the media changes; this cannot."""
    inf = os.path.join(workdir, 'mshdc.inf')
    if not os.path.exists(inf):
        src = None
        for cand in ('MSHDC.IN_', 'mshdc.in_', 'mshdc.inf', 'MSHDC.INF'):
            p = os.path.join(image, 'I386', cand)
            if os.path.exists(p):
                src = p
                break
        if src is None:
            return {}
        os.makedirs(workdir, exist_ok=True)
        if src.lower().endswith('.in_'):
            subprocess.run(['cabextract', '-q', '-d', workdir, src],
                           check=True, capture_output=True)
        else:
            shutil.copyfile(src, inf)
    text = open(inf, encoding='latin-1').read()

    # section name -> list of raw lines
    sections, cur = {}, None
    for line in text.splitlines():
        st = line.strip()
        if st.startswith('[') and st.endswith(']'):
            cur = st[1:-1].strip().lower()
            sections[cur] = []
        elif cur is not None:
            sections[cur].append(line)

    # install section -> service, from "[<sect>.Services] AddService = <svc>"
    svc_of = {}
    for name, lines in sections.items():
        if not name.endswith('.services'):
            continue
        for l in lines:
            m = re.match(r'\s*AddService\s*=\s*([A-Za-z0-9_]+)', l)
            if m:
                svc_of[name[:-len('.services')]] = m.group(1).lower()
                break

    # manufacturer sections carry "%desc% = <install_sect>, PCI\VEN_x&DEV_y"
    mans = []
    for l in sections.get('manufacturer', []):
        l = l.split(';', 1)[0].strip()
        if '=' in l:
            mans.append(l.split('=', 1)[1].strip().strip('"').lower())
    dev = re.compile(r'=\s*([A-Za-z0-9_.]+)\s*,\s*(PCI\\VEN_[0-9A-Fa-f]{4}&DEV_[0-9A-Fa-f]{4})\s*$')
    out = {}
    for man in mans:
        for l in sections.get(man, []):
            l = l.split(';', 1)[0].rstrip()
            m = dev.search(l)
            if not m:
                continue
            svc = svc_of.get(m.group(1).lower())
            if not svc:
                continue
            key = m.group(2).lower().replace('\\', '#')   # pci#ven_8086&dev_7111
            out[key] = svc
    return out


def stage_storage(root, image, workdir):
    log('== storage (universal boot) ==')
    drivers = os.path.join(root, 'WINDOWS', 'system32', 'drivers')
    i386 = os.path.join(image, 'I386')

    # 1. the .sys files, before anything is set boot-start
    have = {}
    wanted = dict(MINIPORTS)
    for fn, packed in EXTRA_DRIVER_FILES.items():
        wanted['@' + fn] = packed
    for svc, packed in sorted(wanted.items()):
        fn = svc[1:] if svc.startswith('@') else svc + '.sys'
        dst = os.path.join(drivers, fn)
        if os.path.exists(dst):
            have[svc] = 'present'
            continue
        src = None
        for cand in (packed, packed.lower(), fn, fn.upper()):
            p = os.path.join(i386, cand)
            if os.path.exists(p):
                src = p
                break
        if src is None:
            have[svc] = 'UNAVAILABLE'
            continue
        if src.lower().endswith('.sy_'):
            os.makedirs(workdir, exist_ok=True)
            subprocess.run(['cabextract', '-q', '-d', workdir, src],
                           check=True, capture_output=True)
            got = None
            for f in os.listdir(workdir):
                if f.lower() == fn.lower():
                    got = os.path.join(workdir, f)
                    break
            if got is None:
                have[svc] = 'EXPAND FAILED'
                continue
            _a, err = copy_verified(got, dst)
        else:
            _a, err = copy_verified(src, dst)
        have[svc] = 'expanded' if not err else ('FAILED: ' + err)
    for k in sorted(have):
        log('  driver %-14s %s' % (k.lstrip('@'), have[k]))

    # 2. registry
    sysp = os.path.join(root, 'WINDOWS', 'system32', 'config', 'system')
    sy = Hive(sysp, write=True)
    cs = control_set(sy)
    real = {s.lower(): s for s in sy.children(cs + '\\Services')}

    boot_started, skipped = [], []
    for svc in sorted(MINIPORTS):
        if not os.path.exists(os.path.join(drivers, svc + '.sys')):
            skipped.append('%s (no .sys - a boot-start service without its file is a dead boot)' % svc)
            continue
        name = real.get(svc, svc)
        path = cs + '\\Services\\' + name
        if sy.node(path) is None:
            skipped.append('%s (no service key)' % svc)
            continue
        sy.set_dword(path, 'Start', 0)
        boot_started.append(svc)
    log('  boot-start: %s' % ', '.join(boot_started))
    for s in skipped:
        log('  skipped   : %s' % s)

    # 3. CriticalDeviceDatabase
    cddb_root = cs + '\\Control\\CriticalDeviceDatabase'
    existing = set(x.lower() for x in sy.children(cddb_root))
    vendor = parse_mshdc(image, workdir)
    entries = list(GENERIC_CDDB) + [
        (k, v, HDC_CLASS) for k, v in sorted(vendor.items())
    ]
    added = kept = dropped = 0
    for name, svc, guid in entries:
        # never point an entry at a driver whose file is not on this disk
        if svc in MINIPORTS and not os.path.exists(os.path.join(drivers, svc + '.sys')):
            dropped += 1
            continue
        if name.lower() in existing:
            kept += 1
            continue
        sy.set_sz(cddb_root + '\\' + name, 'Service', svc)
        sy.set_sz(cddb_root + '\\' + name, 'ClassGUID', guid)
        added += 1
    log('  CriticalDeviceDatabase: +%d added, %d already present, %d dropped '
        '(driver not on this disk); %d from mshdc.inf'
        % (added, kept, dropped, len(vendor)))

    # 4. never sit at a STOP screen waiting for a human; never write a 
    #    RAM-sized dump to a disk that may not have room.
    sy.set_dword(cs + '\\Control\\CrashControl', 'AutoReboot', 1)
    sy.set_dword(cs + '\\Control\\CrashControl', 'LogEvent', 1)
    sy.set_dword(cs + '\\Control\\CrashControl', 'CrashDumpEnabled', 0)
    # ErrorMode 2: suppress the OS hard-error popups ("There is no disk in the
    # drive") that otherwise sit modal in front of everything on a games box
    # that probes empty optical drives.
    sy.set_dword(cs + '\\Control\\Windows', 'ErrorMode', 2)
    log('  CrashControl: auto-reboot on stop, no dump; Windows ErrorMode=2')

    sy.commit()
    return True


# --------------------------------------------------------------------------
# stage: agent (parity with what the fleet auto-updates to)
# --------------------------------------------------------------------------
# The image's staged agent is whatever stage-oem.sh last copied in; the fleet
# runs whatever is published at DEFAULT_UPDATE_PATH. Those drift, and on this
# disk they had: image 218,112 bytes against a published 296,448. The staged
# agent would have self-updated 15s after first boot and restarted itself, so
# nothing was broken - but a first boot that reaches the right version WITHOUT
# a download and a restart is one less thing that has to go right unattended.
PUBLISH_DIR = '/mnt/retro-share/Utility/Retro Automation'
PUBLISHED = ['retro_agent.exe', 'retro_chat.exe']


def stage_agent(root, image, publish_dir=PUBLISH_DIR):
    log('== agent ==')
    dst_dir = os.path.join(root, 'RETRO_AGENT')
    os.makedirs(dst_dir, exist_ok=True)
    ok = True
    sizes = {}
    for name in PUBLISHED:
        src = os.path.join(publish_dir, name)
        if not os.path.exists(src):
            log('  %-18s NOT PUBLISHED at %s' % (name, publish_dir))
            ok = False
            continue
        ver = ''
        vp = src + '.ver'
        if os.path.exists(vp):
            ver = open(vp, encoding='latin-1').read().strip()
        dst = os.path.join(dst_dir, name)
        before = os.path.getsize(dst) if os.path.exists(dst) else 0
        action, err = copy_verified(src, dst)
        if err:
            log('  %-18s FAILED: %s' % (name, err))
            ok = False
            continue
        sizes[name] = os.path.getsize(dst)
        log('  %-18s %s v%s  %d bytes%s'
            % (name, action, ver or '?', sizes[name],
               ('  (was %d)' % before) if before and before != sizes[name] else ''))

    # newimage.flag is read back and logged by the agent's gamesync thread, so
    # it has to describe the disk that actually exists, not the image it came
    # from. Same keys, honest values, plus what was done offline.
    flag = os.path.join(dst_dir, 'newimage.flag')
    lines = [
        'imaged=%s' % time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()),
        'image=%s' % os.path.basename(image.rstrip('/')),
        'agent=%s' % sizes.get('retro_agent.exe', 'unknown'),
        'provisioned=offline foolproof-disk.py',
    ]
    with open(flag, 'wb') as f:
        f.write(('\r\n'.join(lines) + '\r\n').encode('ascii'))
    log('  newimage.flag rewritten (%d bytes): %s'
        % (os.path.getsize(flag), '; '.join(lines)))
    return ok

# --------------------------------------------------------------------------
# stage: harden
# --------------------------------------------------------------------------
FIRSTBOOT_CMD = r"""@echo off
rem Written by scripts/pxe/foolproof-disk.py - runs once, from HKLM RunOnce.
rem Everything here is a Windows-side call with no offline equivalent worth
rem trusting: XP's power policy is a binary blob and powercfg owns it.
set LOG=C:\RETRO_AGENT\foolproof-firstboot.log
echo [%DATE% %TIME%] first boot hardening >> "%LOG%"

rem A box that sleeps drops off the LAN and the agent goes with it.
powercfg /SETACTIVE "Always On" >> "%LOG%" 2>&1
powercfg /CHANGE "Always On" /monitor-timeout-ac 0 /disk-timeout-ac 0 /standby-timeout-ac 0 /hibernate-timeout-ac 0 >> "%LOG%" 2>&1
powercfg /HIBERNATE OFF >> "%LOG%" 2>&1

rem The offline edit covers Default User; this covers the profile already made.
reg add "HKCU\Control Panel\Desktop" /v ScreenSaveActive /t REG_SZ /d 0 /f >> "%LOG%" 2>&1
reg add "HKCU\Control Panel\Desktop" /v ScreenSaveTimeOut /t REG_SZ /d 0 /f >> "%LOG%" 2>&1

echo [%DATE% %TIME%] done >> "%LOG%"
exit /b 0
"""


def stage_harden(root, image):
    log('== harden ==')
    cfg = os.path.join(root, 'WINDOWS', 'system32', 'config')

    # ---- OOBE ----------------------------------------------------------
    # A finished GUI setup leaves the machine queued to run the out-of-box
    # experience on its first boot:
    #
    #   SYSTEM\Setup\SetupType      = 2
    #   SYSTEM\Setup\OobeInProgress = 1
    #   SYSTEM\Setup\CmdLine        = C:\WINDOWS\System32\oobe\msoobe.exe /f /retail
    #
    # winnt.sif asks for that to be skipped (`UnattendSwitch = yes`,
    # `OemSkipWelcome = 1`) - but UnattendSwitch is documented to work only
    # together with `OemPreinstall = Yes`, and this image ships No. So the
    # full-screen "Welcome to Microsoft Windows" wizard runs and waits for a
    # human, in front of the auto-logon that was supposed to make that
    # unnecessary.
    #
    # Clearing it is the right answer for this fleet regardless of which way
    # UnattendSwitch would have gone: OOBE's job is to create an end-user
    # account and offer activation, and the fleet auto-logs-on as the built-in
    # Administrator, which already exists and is enabled. Nothing OOBE makes is
    # wanted here.
    sy = Hive(os.path.join(cfg, 'system'), write=True)
    was = (sy.get('Setup', 'SetupType'), sy.get('Setup', 'OobeInProgress'),
           sy.get('Setup', 'SystemSetupInProgress'))
    sy.set_dword('Setup', 'SetupType', 0)
    sy.set_dword('Setup', 'OobeInProgress', 0)
    sy.set_dword('Setup', 'SystemSetupInProgress', 0)
    sy.commit()
    log('  OOBE: SetupType %s->0, OobeInProgress %s->0, SystemSetupInProgress %s->0'
        % was)

    sw = Hive(os.path.join(cfg, 'software'), write=True)
    # The first page of the Found New Hardware wizard is "may Windows connect
    # to Windows Update?". These two turn the whole wizard into a silent
    # DevicePath search, which is the one this image can actually satisfy.
    sw.set_dword(r'Policies\Microsoft\Windows\DriverSearching',
                 'DontSearchWindowsUpdate', 1)
    sw.set_dword(r'Policies\Microsoft\Windows\DriverSearching',
                 'DontPromptForWindowsUpdate', 1)
    sw.set_dword(r'Policies\Microsoft\Windows\DriverSearching',
                 'SearchOrderConfig', 0)
    # Unsigned is the norm for a driver pack; the rank penalty stays either way,
    # but the modal prompt does not.
    sw.set_dword(r'Policies\Microsoft\Windows NT\Driver Signing',
                 'BehaviorOnFailedVerify', 0)
    sw.set_dword(r'Microsoft\Driver Signing', 'Policy', 0)
    # Error reporting: a crashed game must not leave a modal "send report" box
    # sitting on top of the desktop the agent is driving.
    for base in (r'Microsoft\PCHealth\ErrorReporting',
                 r'Policies\Microsoft\PCHealth\ErrorReporting'):
        sw.set_dword(base, 'DoReport', 0)
        sw.set_dword(base, 'ShowUI', 0)
    sw.set_dword(r'Microsoft\PCHealth\ErrorReporting', 'AllOrNone', 1)
    sw.set_dword(r'Microsoft\PCHealth\ErrorReporting', 'IncludeKernelFaults', 0)
    sw.set_dword(r'Microsoft\PCHealth\ErrorReporting', 'IncludeMicrosoftApps', 0)
    sw.set_dword(r'Microsoft\PCHealth\ErrorReporting', 'IncludeWindowsApps', 0)
    # Automatic Updates: the first-logon balloon and wizard are a dialog nobody
    # is there to answer, and a fleet box has no business rebooting itself.
    sw.set_dword(r'Policies\Microsoft\Windows\WindowsUpdate\AU', 'NoAutoUpdate', 1)
    sw.set_dword(r'Microsoft\Windows\CurrentVersion\WindowsUpdate\Auto Update',
                 'AUOptions', 1)
    sw.set_dword(r'Microsoft\Windows\CurrentVersion\WindowsUpdate\Auto Update',
                 'AUState', 7)
    sw.set_sz(r'Microsoft\Windows\CurrentVersion\RunOnce',
              'FleetFoolproof', r'C:\RETRO_AGENT\foolproof.cmd')
    sw.commit()
    log('  SOFTWARE: hardware wizard silent, error reports off, AU off, RunOnce set')

    # Default User is the template every profile created from now on copies,
    # which on this disk means the Administrator profile that does not exist
    # yet. HKU\.DEFAULT is the logon screen's own profile - both, or the
    # screensaver comes back in one of the two places.
    n = 0
    for path, label in (
        (os.path.join(root, 'Documents and Settings', 'Default User', 'NTUSER.DAT'),
         'Default User'),
        (os.path.join(cfg, 'default'), 'HKU\\.DEFAULT'),
    ):
        if not os.path.exists(path):
            log('  %-14s MISSING - skipped' % label)
            continue
        u = Hive(path, write=True)
        u.set_sz(r'Control Panel\Desktop', 'ScreenSaveActive', '0')
        u.set_sz(r'Control Panel\Desktop', 'ScreenSaveTimeOut', '0')
        u.set_sz(r'Control Panel\Desktop', 'ScreenSaverIsSecure', '0')
        u.set_sz(r'Control Panel\Desktop', 'SCRNSAVE.EXE', '')
        u.set_dword(r'Software\Microsoft\Windows\CurrentVersion\Applets\Tour',
                    'RunCount', 0)
        u.set_dword(r'Software\Microsoft\Windows\CurrentVersion\Explorer'
                    r'\Desktop\CleanupWiz', 'NoRun', 1)
        u.set_dword(r'Software\Microsoft\Windows\CurrentVersion\Policies\Explorer',
                    'NoDriveTypeAutoRun', 0xff)
        u.commit()
        n += 1
        log('  %-14s screensaver off, tour off, cleanup wizard off, autorun off'
            % label)

    # the RunOnce target
    agent_dir = os.path.join(root, 'RETRO_AGENT')
    os.makedirs(agent_dir, exist_ok=True)
    cmd = os.path.join(agent_dir, 'foolproof.cmd')
    with open(cmd, 'wb') as f:
        f.write(FIRSTBOOT_CMD.replace('\n', '\r\n').encode('ascii'))
    log('  wrote %s (%d bytes, CRLF)' % (cmd, os.path.getsize(cmd)))
    return n > 0


# --------------------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--root', required=True, help='the XP system drive, mounted rw')
    ap.add_argument('--image', default='/mnt/retro-share/Files/OS/XPSP3-FLEET')
    ap.add_argument('--stage', default='all',
                    choices=['check', 'payload', 'storage', 'agent', 'harden', 'all'])
    ap.add_argument('--check', action='store_true', help='alias for --stage check')
    ap.add_argument('--workdir', default=None, help='scratch for expanded files')
    a = ap.parse_args()

    stage = 'check' if a.check else a.stage
    root = a.root.rstrip('/')
    workdir = a.workdir or os.path.join(
        os.environ.get('TMPDIR', '/tmp'), 'foolproof-expand')

    if not os.path.isdir(root):
        sys.exit('--root %s is not a directory' % root)
    if not stage_check(root, a.image):
        sys.exit(1)
    if stage == 'check':
        return

    writable = os.access(os.path.join(root, 'boot.ini'), os.W_OK)
    if not writable:
        sys.exit('%s is not writable - remount rw before running a write stage' % root)

    ok = True
    if stage in ('payload', 'all'):
        ok = stage_payload(root, a.image) and ok
    if stage in ('storage', 'all'):
        ok = stage_storage(root, a.image, workdir) and ok
    if stage in ('agent', 'all'):
        ok = stage_agent(root, a.image) and ok
    if stage in ('harden', 'all'):
        ok = stage_harden(root, a.image) and ok
    log('== %s ==' % ('done' if ok else 'DONE WITH FAILURES'))
    sys.exit(0 if ok else 1)


if __name__ == '__main__':
    main()
