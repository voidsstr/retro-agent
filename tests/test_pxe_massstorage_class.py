#!/usr/bin/env python3
"""A slipstreamed storage driver must claim a controller only in the MODE its INF names.

Found 2026-09-25 PXE-imaging a Dell Dimension 4600 (865G + ICH5). The ICH5 SATA
controller (PCI\\VEN_8086&DEV_24D1) runs in IDE mode on that board, where retail
XP drives it through PCI\\CC_0101 = "pciide". inject-massstorage.py had written

    PCI\\VEN_8086&DEV_24D1 = "iaStor2"

from dpsI2.inf, which names the chip ONLY as ...&DEV_24D1&CC_0106 (AHCI). The
bare VEN&DEV is one of every PCI device's compatible ids and outranks the class
fallback, so text mode ran an IDE-mode disk on Intel's RAID driver. It rebooted
before finishing - twice - and each time left the disk booting a restartable
setup loader that stopped at "txtsetup.sif is corrupt or missing, status 21".
That message is the loader on the DISK, not a network or NAS fault.

The injector stripped &CC_ from every id; 59 injected entries had the shape
(ICH6 2651 -> iaStor3, nForce, Marvell). Two invariants:

  * parse_driver() keeps an INF's &CC_ qualifier (and still drops SUBSYS/REV);
  * the shipped TXTSETUP.SIF maps no bare VEN&DEV whose INF names it only in a
    class mode (checked on the Intel ids known to have been wrong).

Run: python3 tests/test_pxe_massstorage_class.py   (run_all.sh runs it)
"""
import importlib.util
import os
import re
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
INJ = os.path.join(HERE, '..', 'scripts', 'pxe', 'inject-massstorage.py')
SIF = '/mnt/retro-share/Files/OS/XPSP3-FLEET/I386/TXTSETUP.SIF'
fails = []


def check(label, cond):
    print(f'  {"PASS" if cond else "FAIL"}  {label}')
    if not cond:
        fails.append(label)


def load_injector():
    spec = importlib.util.spec_from_file_location('inj', INJ)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


# dpsI2.inf's shape (M021): an AHCI/RAID-only model line.
INF_AHCI_ONLY = (
    '[Version]\r\nSignature="$Windows NT$"\r\nClass=SCSIAdapter\r\n'
    '[Manufacturer]\r\n%INTEL%=INTEL\r\n'
    '[INTEL]\r\n%D%=iaStor2_Inst, PCI\\VEN_8086&DEV_24D1&CC_0106\r\n'
    '[iaStor2_Inst.Services]\r\nAddService=iaStor2,0x2,iaStor2_Service\r\n'
    '[iaStor2_Service]\r\nServiceBinary=%12%\\iaStor2.sys\r\n'
)
# A plain controller INF: bare and subsystem-qualified ids.
INF_PLAIN = (
    '[Manufacturer]\r\n%S%=SII\r\n'
    '[SII]\r\n%D%=S_Inst, PCI\\VEN_1095&DEV_3112\r\n'
    '%D%=S_Inst, PCI\\VEN_1095&DEV_3112&SUBSYS_61121095\r\n'
    '[S_Inst.Services]\r\nAddService=Si3112,0x2,S_Svc\r\n'
    '[S_Svc]\r\nServiceBinary=%12%\\Si3112.sys\r\n'
)


def hwdb(text):
    m = re.search(r'^\[HardwareIdsDatabase\][^\r\n]*\r\n(.*?)^\[', text, re.M | re.S)
    out = {}
    for line in m.group(1).split('\r\n'):
        if '=' in line and not line.lstrip().startswith(';'):
            k, v = line.split('=', 1)
            out[k.strip().upper()] = v.strip().strip('"')
    return out


def main():
    inj = load_injector()

    print('== the generator keeps the class qualifier ==')
    with tempfile.TemporaryDirectory() as td:
        p = os.path.join(td, 'dpsI2.inf')
        open(p, 'w', newline='').write(INF_AHCI_ONLY)
        got = inj.parse_driver(p)
        check('dpsI2-shaped INF parses', got is not None)
        ids = got[2] if got else []
        check('it yields PCI\\VEN_8086&DEV_24D1&CC_0106', 'PCI\\VEN_8086&DEV_24D1&CC_0106' in ids)
        check('and NOT the bare PCI\\VEN_8086&DEV_24D1 that took IDE-mode ICH5',
              'PCI\\VEN_8086&DEV_24D1' not in ids)
        p = os.path.join(td, 'si3112.inf')
        open(p, 'w', newline='').write(INF_PLAIN)
        got = inj.parse_driver(p)
        ids = got[2] if got else []
        check('a plain INF still yields the bare VEN&DEV', ids == ['PCI\\VEN_1095&DEV_3112'])

    print('== the shipped TXTSETUP.SIF ==')
    if not os.path.isfile(SIF):
        print(f'  SKIP  {SIF} not present (share not mounted) - NOT a pass')
    else:
        db = hwdb(open(SIF, 'rb').read().decode('latin1'))
        for bare, svc in (('PCI\\VEN_8086&DEV_24D1', 'iaStor2'),
                          ('PCI\\VEN_8086&DEV_2651', 'iaStor3'),
                          ('PCI\\VEN_8086&DEV_27C1', 'iaStor')):
            check(f'{bare} is not bound bare to {svc} (IDE mode must reach pciide)',
                  db.get(bare) != svc)
        check('ICH5 AHCI mode still reaches iaStor2',
              db.get('PCI\\VEN_8086&DEV_24D1&CC_0106') == 'iaStor2')
        check('retail PCI\\CC_0101 = pciide is intact', db.get('PCI\\CC_0101') == 'pciide')

    print(f'\npxe mass-storage class: {"all checks passed" if not fails else str(len(fails)) + " FAILED"}')
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
