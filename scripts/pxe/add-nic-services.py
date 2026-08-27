#!/usr/bin/env python3
"""Pre-create NIC service keys in setupreg.hiv, the boot hive text-mode setup uses.

WHY. A PXE-booted setupldr asks the server which NIC driver to load (see binl.py)
and the server's reply may also carry a registry blob describing that driver's
service. We answer the query but send no blob, and the machine then bugchecks

    STOP 0x000000BB (0x3, 0xC00000C0, 0, 0)   NETWORK_BOOT_INITIALIZATION_FAILED

with 0xC00000C0 = STATUS_DEVICE_DOES_NOT_EXIST, immediately after the whole
network stack has loaded. Traced in the disassembly: setupldr stores the blob
pointer and length and never reads them again, so they are handed to the kernel,
which is what turns them into
\\Registry\\Machine\\System\\CurrentControlSet\\Services\\<service>. With no blob
there is no key, so the driver is in memory but there is no device.

This creates those keys directly in the boot hive instead, which sidesteps
needing to reconstruct an undocumented blob format. The hive IS
HKLM\\SYSTEM during text-mode setup, so a service declared here is a service the
kernel can find.

The value set matches what the hive's own boot drivers carry, plus what an NDIS
miniport needs to be startable: Type/Start/ErrorControl/Group/ImagePath. It is
deliberately minimal - anything the driver needs beyond this comes from its INF
during GUI-mode setup, by which point the network is no longer load-bearing.
"""
import argparse
import json
import shutil
import sys

try:
    import hivex
except ImportError:
    print('python3-hivex is required: apt-get install python3-hivex', file=sys.stderr)
    raise SystemExit(1)

REG_SZ, REG_EXPAND_SZ, REG_DWORD = 1, 2, 4


def wsz(s):
    return (s + '\0').encode('utf-16-le')


def dword(n):
    return n.to_bytes(4, 'little')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--hive', required=True, help='setupreg.hiv to modify in place')
    ap.add_argument('--nicdb', required=True, help='nicdb.json from build-nicdb.py')
    ap.add_argument('--backup', action='store_true', help='keep a .orig copy')
    ap.add_argument('--dry-run', action='store_true')
    a = ap.parse_args()

    with open(a.nicdb, encoding='ascii') as fh:
        db = json.load(fh)
    # One key per distinct service, not per PCI id - 267 ids collapse to a few
    # dozen drivers, and writing the same key repeatedly just bloats the hive.
    services = {}
    for entry in list(db.get('generic', {}).values()) + list(db.get('exact', {}).values()):
        services.setdefault(entry['service'], entry['sys'])
    print(f'{len(services)} distinct NIC service(s) to declare')

    if a.dry_run:
        for svc, sysf in sorted(services.items())[:12]:
            print(f'  {svc:<18} -> system32\\drivers\\{sysf}')
        return 0

    if a.backup:
        shutil.copy2(a.hive, a.hive + '.orig')
        print(f'  backup: {a.hive}.orig')

    h = hivex.Hivex(a.hive, write=True)
    node = h.root()
    for part in ('ControlSet001', 'Services'):
        child = h.node_get_child(node, part)
        if child is None:
            print(f'FATAL: {part} not found in hive', file=sys.stderr)
            return 1
        node = child

    added = updated = 0
    for svc, sysf in sorted(services.items()):
        existing = h.node_get_child(node, svc)
        if existing is None:
            n = h.node_add_child(node, svc)
            added += 1
        else:
            n = existing
            updated += 1
        h.node_set_values(n, [
            {'key': 'Type',         't': REG_DWORD,     'value': dword(1)},   # kernel driver
            {'key': 'Start',        't': REG_DWORD,     'value': dword(0)},   # boot start
            {'key': 'ErrorControl', 't': REG_DWORD,     'value': dword(1)},   # normal
            {'key': 'Group',        't': REG_SZ,        'value': wsz('NDIS')},
            {'key': 'Tag',          't': REG_DWORD,     'value': dword(1)},
            {'key': 'ImagePath',    't': REG_EXPAND_SZ,
             'value': wsz(f'system32\\drivers\\{sysf}')},
        ])
    h.commit(None)
    print(f'  {added} service key(s) added, {updated} updated')

    # Read it back rather than trusting the write - a corrupt boot hive is an
    # unbootable image, and the failure would appear as an unrelated bugcheck.
    v = hivex.Hivex(a.hive)
    n = v.root()
    for part in ('ControlSet001', 'Services'):
        n = v.node_get_child(n, part)
    names = {v.node_name(c) for c in v.node_children(n)}
    missing = [s for s in services if s not in names]
    print(f'  verify: {len(names)} services in hive, {len(missing)} of ours missing')
    if missing:
        print(f'  FAILED: {missing[:5]}', file=sys.stderr)
        return 1
    print('  ok')
    return 0


if __name__ == '__main__':
    sys.exit(main())
