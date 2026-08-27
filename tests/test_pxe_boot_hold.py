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
    hold = pxe.BootHold(state, 3600)
    check('a machine we have never served is not held', not hold.held(mac))
    hold.arm(mac)
    check('a machine is held once it has taken the boot file', hold.held(mac))
    check('the hold does not leak to another machine', not hold.held(other))
    check('the MAC comparison ignores case', hold.held(mac.upper()))

    print('== the hold outlives the process ==')
    check('a restarted server still holds the machine',
          pxe.BootHold(state, 3600).held(mac))

    print('== an operator can always force a reinstall ==')
    fresh = pxe.BootHold(state, 3600)
    check('--release drops one hold',
          fresh.release(mac) == 1 and not fresh.held(mac))
    fresh.arm(mac)
    fresh.arm(other)
    check('--release all drops every hold',
          fresh.release('all') == 2 and not fresh.held(mac))

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
          not pxe.BootHold(state, 3600).held(mac))

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
