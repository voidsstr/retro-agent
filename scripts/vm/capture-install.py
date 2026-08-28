#!/usr/bin/env python3
"""Capture what a Windows installer actually does, so it can be baked into an image.

WHY. Some things the fleet image is supposed to ship "already installed" cannot
be pre-installed by copying a directory: 7-Zip registers shell-extension COM
classes, Daemon Tools installs a storage filter driver. The install has to be
run once on Windows and its effects captured.

WHAT IT CAPTURES. A before/after diff of:
  * the filesystem under the paths that matter (Program Files, Windows,
    Documents and Settings\\All Users) - names and sizes, so a changed file is
    detected as well as a new one
  * HKLM\\SOFTWARE, HKLM\\SYSTEM\\CurrentControlSet\\Services and HKCU\\SOFTWARE,
    exported with reg.exe

It deliberately does NOT try to be a general-purpose installer virtualiser. It
produces evidence for a human to turn into a $OEM$ payload plus a .reg file,
and it says plainly which registry keys appeared so a driver-installing product
can be recognised as unsuitable for the copy-and-merge treatment.

USAGE
  capture-install.py --host H [--port P] snapshot before
  capture-install.py --host H [--port P] run 'C:\\BUILD\\7z920.exe /S'
  capture-install.py --host H [--port P] snapshot after
  capture-install.py --host H [--port P] diff        # writes ./capture/<name>/
"""
import argparse
import asyncio
import os
import re
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                os.pardir, os.pardir))
from client.retro_protocol import RetroConnection          # noqa: E402

SECRET = 'retro-agent-secret'

# Snapshot only where installers actually land. A full C:\ walk on a period box
# takes minutes and buries the signal in temp files and logs.
FS_ROOTS = [
    r'C:\Program Files',
    r'C:\WINDOWS\system32',
    r'C:\WINDOWS\inf',
    r'C:\Documents and Settings\All Users\Start Menu\Programs',
]
REG_KEYS = [
    r'HKLM\SOFTWARE',
    r'HKLM\SYSTEM\CurrentControlSet\Services',
    r'HKCU\SOFTWARE',
]


async def conn(host, port):
    c = RetroConnection(host, port)
    await asyncio.wait_for(c.connect(SECRET), timeout=25.0)
    return c


async def run_cmd(c, cmd, timeout=900.0):
    return await c.command_text(cmd, timeout=timeout)


async def download(c, remote, local):
    status, data = await c.send_command(f'DOWNLOAD {remote}', timeout=900.0)
    if status != 0:
        raise RuntimeError(f'DOWNLOAD {remote} failed: {data[:200]!r}')
    with open(local, 'wb') as fh:
        fh.write(data)
    return len(data)


async def do_snapshot(host, port, tag, outdir):
    os.makedirs(outdir, exist_ok=True)
    c = await conn(host, port)
    try:
        # Filesystem: name + size, so modified files show up too, not just new
        # ones. `dir /s /b` alone would miss a replaced DLL of the same name.
        parts = []
        for root in FS_ROOTS:
            parts.append(f'if exist "{root}" dir /s /-c "{root}"')
        script = ' & '.join(parts)
        remote_fs = f'C:\\snap_{tag}_fs.txt'
        await run_cmd(c, f'EXECW cmd /c ({script}) > {remote_fs} 2>&1')
        n = await download(c, remote_fs, os.path.join(outdir, f'{tag}_fs.txt'))
        print(f'  filesystem snapshot: {n} bytes')

        for i, key in enumerate(REG_KEYS):
            remote = f'C:\\snap_{tag}_reg{i}.reg'
            await run_cmd(c, f'EXECW cmd /c reg export "{key}" {remote} /y')
            try:
                n = await download(c, remote, os.path.join(outdir, f'{tag}_reg{i}.reg'))
                print(f'  {key}: {n} bytes')
            except RuntimeError as exc:
                print(f'  {key}: NOT CAPTURED ({exc})')
    finally:
        await c.close()


def parse_dir_output(path):
    """Extract (fullpath, size) pairs from `dir /s /-c` output."""
    files = {}
    cur = None
    dir_re = re.compile(r'^ Directory of (.+)$')
    # e.g. "27/08/2026  09:42            1110476 7z920.exe"
    file_re = re.compile(r'^\S+\s+\S+\s+([\d,]+)\s+(.+?)\s*$')
    try:
        raw = open(path, encoding='latin1', errors='replace').read()
    except OSError:
        return files
    for line in raw.splitlines():
        m = dir_re.match(line)
        if m:
            cur = m.group(1).strip()
            continue
        if cur is None or '<DIR>' in line:
            continue
        m = file_re.match(line)
        if m:
            size = m.group(1).replace(',', '')
            name = m.group(2).strip()
            if not size.isdigit() or name in ('.', '..'):
                continue
            files[f'{cur}\\{name}'] = int(size)
    return files


def parse_reg(path):
    """Map key -> set of 'value=data' lines, from a reg.exe export."""
    keys = {}
    cur = None
    try:
        raw = open(path, encoding='utf-16', errors='replace').read()
    except (OSError, UnicodeError):
        try:
            raw = open(path, encoding='latin1', errors='replace').read()
        except OSError:
            return keys
    for line in raw.splitlines():
        line = line.rstrip('\r')
        if line.startswith('[') and line.endswith(']'):
            cur = line[1:-1]
            keys.setdefault(cur, set())
        elif cur is not None and '=' in line:
            keys[cur].add(line)
    return keys


def do_diff(outdir):
    before_fs = parse_dir_output(os.path.join(outdir, 'before_fs.txt'))
    after_fs = parse_dir_output(os.path.join(outdir, 'after_fs.txt'))
    new = {p: s for p, s in after_fs.items() if p not in before_fs}
    changed = {p: (before_fs[p], s) for p, s in after_fs.items()
               if p in before_fs and before_fs[p] != s}

    print(f'\nFILES: {len(new)} new, {len(changed)} changed '
          f'({len(before_fs)} -> {len(after_fs)} tracked)')
    with open(os.path.join(outdir, 'new_files.txt'), 'w') as fh:
        for p in sorted(new):
            fh.write(f'{new[p]}\t{p}\n')
    with open(os.path.join(outdir, 'changed_files.txt'), 'w') as fh:
        for p in sorted(changed):
            fh.write(f'{changed[p][0]} -> {changed[p][1]}\t{p}\n')
    for p in sorted(new)[:25]:
        print(f'  + {p}')
    if len(new) > 25:
        print(f'  ... {len(new) - 25} more in new_files.txt')

    total_new_keys = 0
    driver_keys = []
    merged = []
    for i, key in enumerate(REG_KEYS):
        b = parse_reg(os.path.join(outdir, f'before_reg{i}.reg'))
        a = parse_reg(os.path.join(outdir, f'after_reg{i}.reg'))
        added_keys = [k for k in a if k not in b]
        changed_keys = [k for k in a if k in b and a[k] != b[k]]
        total_new_keys += len(added_keys)
        print(f'\nREGISTRY {key}: {len(added_keys)} new keys, '
              f'{len(changed_keys)} changed')
        for k in added_keys:
            if r'CurrentControlSet\Services' in k:
                driver_keys.append(k)
            merged.append((k, sorted(a[k])))
        for k in changed_keys:
            merged.append((k, sorted(a[k] - b[k])))

    # Emit a mergeable .reg of everything that appeared. REGEDIT4 (not 5) so it
    # can be merged by XP's regedit from cmdlines.txt at T-12.
    out_reg = os.path.join(outdir, 'install.reg')
    with open(out_reg, 'w', encoding='latin1', errors='replace', newline='\r\n') as fh:
        fh.write('REGEDIT4\r\n\r\n')
        for k, lines in merged:
            fh.write(f'[{k}]\r\n')
            for l in lines:
                fh.write(l + '\r\n')
            fh.write('\r\n')
    print(f'\nwrote {out_reg} ({total_new_keys} new keys)')

    if driver_keys:
        print('\n*** THIS INSTALLER REGISTERS SERVICES/DRIVERS ***')
        for k in driver_keys[:10]:
            print(f'    {k}')
        print('    A service or filter driver cannot be reproduced by copying'
              '\n    files plus merging a .reg - the driver has to be installed'
              '\n    on the target. Run its installer from cmdlines.txt or'
              '\n    GuiRunOnce instead of pre-baking it.')
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--host', default='127.0.0.1')
    ap.add_argument('--port', type=int, default=19898)
    ap.add_argument('--outdir', default=None)
    ap.add_argument('action', choices=['snapshot', 'run', 'diff'])
    ap.add_argument('arg', nargs='?')
    a = ap.parse_args()

    outdir = a.outdir or os.path.join(os.getcwd(), 'capture')

    if a.action == 'snapshot':
        tag = a.arg or 'before'
        if tag not in ('before', 'after'):
            print('snapshot tag must be "before" or "after"', file=sys.stderr)
            return 2
        asyncio.run(do_snapshot(a.host, a.port, tag, outdir))
        return 0
    if a.action == 'run':
        if not a.arg:
            print('run needs a command', file=sys.stderr)
            return 2

        async def go():
            c = await conn(a.host, a.port)
            try:
                print(await run_cmd(c, f'EXECW {a.arg}', timeout=1800.0))
            finally:
                await c.close()
        asyncio.run(go())
        return 0
    return do_diff(outdir)


if __name__ == '__main__':
    sys.exit(main())
