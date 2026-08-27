#!/usr/bin/env python3
"""Build the PCI-ID -> NIC driver map that the BINL responder answers from.

WHY THIS EXISTS. When setupldr is booted over the network it does NOT choose a
network driver from the image. It asks the server, over BINL on UDP 4011, with
a NetCard Query carrying the adapter's PCI vendor/device IDs, and the server
answers with the .sys filename and service name to load. See binl.py.

So the server needs a map from PCI ID to (sys file, service name), and the only
authoritative source for it is the INF files shipped beside the drivers. This
walks them the way Windows would:

    [Manufacturer]        -> manufacturer sections
    manufacturer section  -> "%desc% = <install-section>, <hwid>, <hwid>, ..."
    <install-section>[.NTx86][.5.1] + ".Services"
                          -> "AddService = <service>, <flags>, <svc-install>"
    [<svc-install>]       -> "ServiceBinary = %12%\\<driver>.sys"

and emits JSON. Generating this at build time rather than parsing 26 INFs per
request keeps the server simple, makes the mapping inspectable, and means a
driver that cannot be resolved is discovered now instead of during a boot.

The .sys file must exist flat in I386: setupldr loads it as a bare filename
from the boot source, with no [SourceDisksFiles] lookup, so an entry naming a
file that is not there would fail at the worst possible moment. Entries whose
binary is missing are dropped and reported.
"""
import argparse
import json
import os
import re
import sys

HWID = re.compile(r'PCI\\VEN_([0-9A-Fa-f]{4})&DEV_([0-9A-Fa-f]{4})'
                  r'(?:&SUBSYS_([0-9A-Fa-f]{8}))?(?:&REV_([0-9A-Fa-f]{2}))?')


def read_inf(path):
    """Return {section_lower: [logical lines]} with continuations joined."""
    try:
        with open(path, 'rb') as fh:
            raw = fh.read()
    except OSError:
        return {}
    # INFs are ANSI or occasionally UTF-16. latin-1 never throws and preserves
    # byte values, which is all we need for ASCII keywords and hex IDs.
    if raw[:2] in (b'\xff\xfe', b'\xfe\xff'):
        text = raw.decode('utf-16', errors='replace')
    else:
        text = raw.decode('latin-1')

    sections = {}
    cur = None
    pending = ''
    for line in text.splitlines():
        line = line.rstrip('\r')
        # Strip comments, but not a ';' inside quotes.
        out, q = [], False
        for ch in line:
            if ch == '"':
                q = not q
            if ch == ';' and not q:
                break
            out.append(ch)
        line = ''.join(out).strip()
        if not line and not pending:
            continue
        if pending:
            line = pending + line
            pending = ''
        if line.endswith('\\'):
            pending = line[:-1]
            continue
        if line.startswith('[') and line.endswith(']'):
            cur = line[1:-1].strip().lower()
            sections.setdefault(cur, [])
            continue
        if cur is not None:
            sections[cur].append(line)
    return sections


def find_section(sections, base):
    """Resolve an install section, trying the decorations Windows tries."""
    base = base.strip().lower()
    for suffix in ('.ntx86', '.nt', ''):
        if base + suffix in sections:
            return base + suffix
    # The Manufacturer line may already carry a full decoration.
    return base if base in sections else None


def service_for(sections, install_sec):
    """(service_name, sys_file) for an install section, or (None, None)."""
    for suffix in ('.services', '.ntx86.services', '.nt.services'):
        svc_sec = install_sec + suffix
        if svc_sec in sections:
            break
        # install_sec may already end in .ntx86; try replacing rather than appending
        if install_sec.endswith('.ntx86') and (install_sec + '.services') in sections:
            svc_sec = install_sec + '.services'
            break
    else:
        return None, None
    if svc_sec not in sections:
        return None, None

    for line in sections[svc_sec]:
        if not line.lower().startswith('addservice'):
            continue
        rhs = line.split('=', 1)[1] if '=' in line else ''
        parts = [p.strip() for p in rhs.split(',')]
        if len(parts) < 3:
            continue
        name = parts[0].strip('"')
        inst = parts[2].strip('"').lower()
        for l2 in sections.get(inst, []):
            if l2.lower().replace(' ', '').startswith('servicebinary'):
                val = l2.split('=', 1)[1].strip().strip('"')
                sys_file = val.replace('/', '\\').split('\\')[-1]
                return name, sys_file
        # A service with no ServiceBinary in its own section is usually one
        # whose binary is named by the service - report it rather than guess.
        return name, None
    return None, None


def parse_one(path):
    """[(ven, dev, subsys, sys_file, service, inf_name)] for one INF."""
    sections = read_inf(path)
    if 'manufacturer' not in sections:
        return []
    out = []
    mfg_secs = []
    for line in sections['manufacturer']:
        if '=' not in line:
            continue
        rhs = [p.strip() for p in line.split('=', 1)[1].split(',')]
        base = rhs[0].strip('"')
        # "Intel, NTx86, NTx86.5.1" means sections [Intel.NTx86] etc also apply.
        cands = [base] + [f'{base}.{d}' for d in rhs[1:]]
        for c in cands:
            if c.lower() in sections:
                mfg_secs.append(c.lower())

    for mfg in dict.fromkeys(mfg_secs):
        for line in sections[mfg]:
            if '=' not in line:
                continue
            rhs = line.split('=', 1)[1]
            parts = [p.strip() for p in rhs.split(',')]
            if len(parts) < 2:
                continue
            install = parts[0].strip('"')
            sec = find_section(sections, install)
            if not sec:
                continue
            service, sys_file = service_for(sections, sec)
            if not service or not sys_file:
                continue
            for hw in parts[1:]:
                m = HWID.search(hw)
                if not m:
                    continue
                ven, dev, subsys, _rev = m.groups()
                out.append((ven.upper(), dev.upper(),
                            (subsys or '').upper(), sys_file, service,
                            os.path.basename(path)))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--i386', required=True, help='the image I386 directory')
    ap.add_argument('--out', required=True, help='where to write nicdb.json')
    a = ap.parse_args()

    if not os.path.isdir(a.i386):
        print(f'no such directory: {a.i386}', file=sys.stderr)
        return 1

    present = {n.lower() for n in os.listdir(a.i386)}
    infs = sorted(n for n in os.listdir(a.i386) if n.lower().endswith('.inf'))

    exact = {}      # "VEN&DEV&SUBSYS" -> entry
    generic = {}    # "VEN&DEV"        -> entry
    missing_bin = []
    for name in infs:
        for ven, dev, subsys, sys_file, service, inf in parse_one(
                os.path.join(a.i386, name)):
            if sys_file.lower() not in present:
                missing_bin.append((f'{ven}:{dev}', sys_file, inf))
                continue
            entry = {'sys': sys_file, 'service': service, 'inf': inf}
            if subsys:
                exact.setdefault(f'{ven}&{dev}&{subsys}', entry)
            else:
                generic.setdefault(f'{ven}&{dev}', entry)
    # A subsys-qualified match implies the generic pair works too; without this
    # a card whose subsystem is not listed would find nothing even though its
    # vendor/device is clearly supported.
    for key, entry in exact.items():
        ven, dev, _ = key.split('&')
        generic.setdefault(f'{ven}&{dev}', entry)

    db = {'exact': exact, 'generic': generic}
    with open(a.out, 'w') as fh:
        json.dump(db, fh, indent=1, sort_keys=True)

    print(f'INFs parsed        : {len(infs)}')
    print(f'exact  (VEN&DEV&SS): {len(exact)}')
    print(f'generic (VEN&DEV)  : {len(generic)}')
    print(f'written            : {a.out}')
    if missing_bin:
        print(f'\nDROPPED - driver binary not present flat in I386 ({len(missing_bin)}):')
        for hw, sysf, inf in missing_bin[:10]:
            print(f'  {hw}  needs {sysf}  ({inf})')
        if len(missing_bin) > 10:
            print(f'  ... {len(missing_bin) - 10} more')
    for probe, label in ((('8086', '100E'), 'QEMU e1000'),
                         (('8086', '1229'), 'Intel PRO/100'),
                         (('8086', '1209'), 'Intel 8255x'),
                         (('10EC', '8139'), 'Realtek RTL8139')):
        k = f'{probe[0]}&{probe[1]}'
        e = generic.get(k)
        print(f'  probe {k} {label:18s} -> ' +
              (f"{e['sys']} / service {e['service']}" if e else 'NOT FOUND'))
    return 0


if __name__ == '__main__':
    sys.exit(main())
