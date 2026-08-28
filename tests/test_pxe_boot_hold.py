#!/usr/bin/env python3
"""The PXE server must serve a machine once, then stand aside.

A proxyDHCP server cannot distinguish a bare machine from one that has just
finished installing - both send an identical DISCOVER. So while network boot
sits ahead of the disk in the BIOS order, a machine PXE boots again the instant
text-mode setup reboots it, and because winnt.sif carries AutoPartition and
Repartition, setup REPARTITIONS AND REFORMATS the disk it was halfway through
installing. The install can never complete, and nothing looks like an error:
the log shows a clean, identical boot sequence every few minutes. That is how
the Gateway 550 (00:d0:b7:40:96:a9) spent 2026-08-27 looping every 5.5 minutes.

These assertions pin the two decisions that make the guard correct:
  * it arms on a completed boot-file DOWNLOAD, never on an offer - offers are
    retried several times within one boot (the Intel Boot Agent here sends
    three), so arming on the offer would block the boot it exists to permit;
  * it survives a restart, because the reinstall loop outlives the process.
"""
import importlib.util
import json
import os
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
SERVER = os.path.join(HERE, os.pardir, 'scripts', 'pxe', 'pxe_server.py')

spec = importlib.util.spec_from_file_location('pxe_server', SERVER)
pxe = importlib.util.module_from_spec(spec)
spec.loader.exec_module(pxe)
pxe.LOGFILE = None                      # keep the suite's output clean

FAILS = []


def check(name, cond):
    print(('  PASS  ' if cond else '  FAIL  ') + name)
    if not cond:
        FAILS.append(name)


def main():
    tmp = tempfile.mkdtemp()
    state = os.path.join(tmp, 'state.json')
    mac = '00:d0:b7:40:96:a9'
    other = '00:13:20:aa:bb:cc'

    print('== a machine is served once, then held ==')
    # grace=0: these assertions are about the hold itself. The grace window
    # gets its own section below.
    hold = pxe.BootHold(state, 3600, 0)
    check('a machine we have never served is not held', not hold.held(mac))
    hold.arm(mac)
    check('a machine is held once it has taken the boot file', hold.held(mac))
    check('the hold does not leak to another machine', not hold.held(other))
    check('the MAC comparison ignores case', hold.held(mac.upper()))

    print('== the hold outlives the process ==')
    check('a restarted server still holds the machine',
          pxe.BootHold(state, 3600, 0).held(mac))

    print('== an operator can always force a reinstall ==')
    fresh = pxe.BootHold(state, 3600, 0)
    check('--release drops one hold',
          fresh.release(mac) == 1 and not fresh.held(mac))
    fresh.arm(mac)
    fresh.arm(other)
    check('--release all drops every hold',
          fresh.release('all') == 2 and not fresh.held(mac))

    print('== an external --release reaches the RUNNING server ==')
    # --release runs as a separate process and can only rewrite the state file.
    # If the running server keeps its stale in-memory copy, the release does
    # nothing and then gets overwritten on the next arm - indistinguishable
    # from the hold being broken, and it cost a test cycle to spot.
    running = pxe.BootHold(state, 3600, 0)
    running.arm(mac)
    external = pxe.BootHold(state, 3600, 0)
    external.release(mac)
    check('the running server sees a release made by another process',
          not running.held(mac))
    # ...and must not resurrect it when it next writes.
    running.arm(other)
    check('arming after an external release does not revive the old hold',
          not running.held(mac) and running.held(other))
    running.release('all')

    print('== a quick retry is NOT locked out ==')
    # The hold arms when the boot file is DOWNLOADED, so a machine that
    # downloaded and then failed used to be refused for six hours - and the
    # operator retrying got "no boot image provided" from a server that was
    # refusing deliberately and saying so only in its own log. Text-mode setup
    # takes far longer than the grace window, so a machine back this fast
    # cannot have finished, and must be allowed to try again.
    graced = pxe.BootHold(os.path.join(tmp, 'g.json'), 3600, 900)
    graced.arm(mac)
    check('a machine returning immediately is re-offered', not graced.held(mac))

    # ...but once past the window it is a finished install booting the wrong
    # device, and must be left alone or it reinstalls over itself.
    old_state = os.path.join(tmp, 'old.json')
    with open(old_state, 'w', encoding='ascii') as fh:
        json.dump({mac: time.time() - 1800}, fh)      # served 30 minutes ago
    aged = pxe.BootHold(old_state, 3600, 900)
    check('a machine returning after the grace window is still held',
          aged.held(mac))

    check('grace 0 restores the original always-hold behaviour',
          pxe.BootHold(os.path.join(tmp, 'g0.json'), 3600, 0) is not None)
    g0 = pxe.BootHold(os.path.join(tmp, 'g0.json'), 3600, 0)
    g0.arm(other)
    check('with grace 0 an immediate return is held', g0.held(other))

    print('== arming without a boot, and the blanket release ==')
    # A machine that is about to be REBOOTED needs its hold armed before it
    # goes down, because these boxes boot from the network first: an unheld
    # machine takes the install offer and repartitions itself. arm() therefore
    # has to work on a MAC that has never fetched a boot file.
    pre = pxe.BootHold(os.path.join(tmp, 'pre.json'), 3600, 0)
    check('a never-seen MAC can be armed ahead of a reboot',
          not pre.held(mac))
    pre.arm(mac)
    check('...and is held once armed', pre.held(mac))

    # '--release all' unprotects every machine at once. That cost an hour of
    # provisioning on 2026-08-28: it was used to let ONE box reinstall, and a
    # different box rebooted minutes later took the offer and wiped itself.
    # release() itself still supports 'all' - the CLI is what demands --yes -
    # so pin that the blanket release really does clear everything, which is
    # exactly why the confirmation exists.
    many = pxe.BootHold(os.path.join(tmp, 'many.json'), 3600, 0)
    many.arm(mac)
    many.arm(other)
    check('two machines held', many.held(mac) and many.held(other))
    check('release all clears both', many.release('all') == 2)
    check('...leaving neither held', not many.held(mac) and not many.held(other))

    single = pxe.BootHold(os.path.join(tmp, 'one.json'), 3600, 0)
    single.arm(mac)
    single.arm(other)
    check('releasing ONE mac reports one', single.release(mac) == 1)
    check('...frees that machine', not single.held(mac))
    check('...and leaves the other protected', single.held(other))

    print('== the guard fails open, never closed ==')
    disabled = pxe.BootHold(os.path.join(tmp, 'd.json'), 0)
    disabled.arm(mac)
    check('boot_hold_seconds 0 restores always-offer', not disabled.held(mac))

    expiring = pxe.BootHold(os.path.join(tmp, 'e.json'), 1)
    expiring.arm(mac)
    time.sleep(1.1)
    check('a hold expires on its own, so a reinstall needs no intervention',
          not expiring.held(mac))

    with open(state, 'w', encoding='ascii') as fh:
        fh.write('{ this is not json')
    check('a corrupt state file does not stop machines booting',
          not pxe.BootHold(state, 3600, 0).held(mac))

    print('== the trigger is the boot file, not any TFTP transfer ==')
    check('mac_for_ip returns None for an address not in the ARP cache',
          pxe.mac_for_ip('203.0.113.7') is None)

    print()
    if FAILS:
        print(f'{len(FAILS)} FAILED: ' + ', '.join(FAILS))
        return 1
    print('pxe boot-hold: all checks passed')
    return 0


def test_pxe_boot_hold():
    """pytest entry point.

    Without this the file is named test_*.py, is collected, contains no test
    function, and reports as zero tests - which reads exactly like passing.
    """
    assert main() == 0, 'boot-hold assertions failed: ' + ', '.join(FAILS)


if __name__ == '__main__':
    sys.exit(main())
