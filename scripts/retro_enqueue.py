#!/usr/bin/env python3
"""retro_enqueue.py - queue agent commands to run on a retro machine the next
time the chat daemon is connected to it.

The retro chat daemon (nsc-assistant/agent/tools/retro_chat_daemon.py) drains a
per-host task queue whenever it (re)connects to that host and on each idle
cycle, so a task queued while a machine is offline runs as soon as it next comes
online. Storage is plain files under the daemon's runtime dir:

    /tmp/retro-chat/tasks/<ip>/<ts>-<slug>.json   pending  (oldest runs first)
    /tmp/retro-chat/tasks/<ip>/done/<name>        completed (with captured output)
    /tmp/retro-chat/tasks/<ip>/failed/<name>      gave up (machine unreachable)

This helper just writes a pending task file - it needs no network and does not
require the daemon to be running (the daemon picks the file up on its next
drain). It writes the exact same format the daemon's own --enqueue produces.

Examples:
    # one command
    retro_enqueue.py 192.168.1.123 "EXEC C:\\retro-wall\\arrange_icons.exe"

    # several commands, run in order, with a label
    retro_enqueue.py 192.168.1.143 --label "reapply wallpaper" \
        "EXEC cmd /c reg add ..." "LAUNCH C:\\retro-wall\\rotate_wall.exe 60"

    # list what's queued
    retro_enqueue.py --list [IP]
"""
import argparse
import json
import os
import sys
import time
from pathlib import Path

TASKS = Path(os.environ.get('RETRO_CHAT_ROOT', '/tmp/retro-chat')) / 'tasks'


def enqueue(ip: str, cmds: list, label: str = '') -> Path:
    cmds = [c for c in cmds if c and c.strip()]
    if not cmds:
        raise SystemExit('error: no commands given')
    qdir = TASKS / ip
    qdir.mkdir(parents=True, exist_ok=True)
    now = time.time()
    slug = ''.join(ch if ch.isalnum() else '-'
                   for ch in (label or cmds[0]))[:24].strip('-')
    name = f'{int(now * 1000):015d}-{os.getpid()}-{slug or "task"}.json'
    task = {'id': name[:-5], 'label': label, 'cmds': cmds,
            'created': now, 'attempts': 0}
    dest = qdir / name
    tmp = dest.with_suffix('.tmp')
    tmp.write_text(json.dumps(task, indent=2))
    tmp.rename(dest)
    return dest


def list_tasks(ip: str | None):
    ips = ([ip] if ip else sorted(p.name for p in TASKS.glob('*') if p.is_dir()))
    found = False
    for host in ips:
        qdir = TASKS / host
        pend = sorted(qdir.glob('*.json')) if qdir.is_dir() else []
        if not pend:
            continue
        found = True
        print(f'{host}: {len(pend)} pending')
        for f in pend:
            try:
                t = json.loads(f.read_text())
                cmds = t.get('cmds') or [t.get('cmd')]
                print(f'  - {t.get("label") or t.get("id")} '
                      f'({len(cmds)} cmd, attempts={t.get("attempts", 0)})')
            except Exception:
                print(f'  - {f.name} (unreadable)')
    if not found:
        print('no pending tasks')


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('ip', nargs='?', help='target machine IP')
    p.add_argument('cmds', nargs='*', help='agent command(s) to queue, in order')
    p.add_argument('--label', default='', help='human label for the task')
    p.add_argument('--list', dest='do_list', action='store_true',
                   help='list pending tasks (optionally for the given IP) and exit')
    args = p.parse_args()

    if args.do_list:
        list_tasks(args.ip)
        return
    if not args.ip or not args.cmds:
        p.error('need an IP and at least one command (or --list)')
    dest = enqueue(args.ip, args.cmds, args.label)
    print(f'queued {len(args.cmds)} command(s) for {args.ip}')
    print(f'  {dest}')
    print('runs next time the daemon is connected to that host')


if __name__ == '__main__':
    main()
