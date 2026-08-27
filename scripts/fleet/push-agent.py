#!/usr/bin/env python3
"""Push an agent build to one box and restart into it, without risking the box.

WHY THIS EXISTS AS A SCRIPT. The obvious hand-rolled version is

    move retro_agent.exe retro_agent_old.exe
    move retro_agent_new.exe retro_agent.exe
    start retro_agent.exe

and it is a trap. If the second move fails - the upload did not land, someone
re-ran the batch, the disk filled - the box is left with NO retro_agent.exe at
all, reachable only physically. I did exactly that to a test machine on
2026-08-27 by re-running the batch without uploading a binary first.

The agent's own autoupdate.c gets this right and this mirrors it:
  * COPY over the target, never move it away first, so a failure leaves the
    working binary exactly where it was
  * retry a bounded number of times, because a running exe cannot be
    overwritten and the old process needs a moment to exit
  * ALWAYS end by starting an agent, even on the give-up path - an agent on the
    old version beats no agent at all
  * verify afterwards by asking the box what version it is now running

Win9x-safe: no `&&` chaining, no `2>&1`, no SET /A.
"""
import argparse
import asyncio
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                os.pardir, os.pardir))
from client.retro_protocol import RetroConnection            # noqa: E402

SECRET = 'retro-agent-secret'
INSTALL_DIR = r'C:\RETRO_AGENT'
TRIES = 10


def restart_bat(temp_exe):
    lines = ['@echo off', 'echo push-agent: waiting for the old agent to exit...']
    for _ in range(TRIES):
        lines.append('ping -n 3 127.0.0.1 > nul')
        lines.append(f'copy /Y {temp_exe} {INSTALL_DIR}\\retro_agent.exe')
        lines.append('if not errorlevel 1 goto swapped')
    lines += [
        'echo push-agent: could not replace the binary; restarting what is there',
        f'start {INSTALL_DIR}\\retro_agent.exe',
        'goto done',
        ':swapped',
        f'del {temp_exe}',
        'echo push-agent: starting the new agent...',
        f'start {INSTALL_DIR}\\retro_agent.exe',
        ':done',
        f'del {INSTALL_DIR}\\push_agent.bat',
    ]
    return ('\r\n'.join(lines) + '\r\n').encode('ascii')


async def go(host, port, exe):
    with open(exe, 'rb') as fh:
        binary = fh.read()

    c = RetroConnection(host, port)
    await asyncio.wait_for(c.connect(SECRET), timeout=20)
    before = await c.command_text('SYSINFO', timeout=30)
    print(f'  before: {before[:120]}')

    temp = INSTALL_DIR + r'\retro_agent_new.exe'
    st, r = await c.send_command(f'UPLOAD {temp}', binary_payload=binary, timeout=600)
    if st != 0:
        print(f'  UPLOAD FAILED: {r[:120]!r} - box untouched', file=sys.stderr)
        return 1
    print(f'  uploaded {len(binary)} bytes to {temp}')

    bat = INSTALL_DIR + r'\push_agent.bat'
    st, r = await c.send_command(f'UPLOAD {bat}', binary_payload=restart_bat(temp),
                                 timeout=60)
    if st != 0:
        print(f'  batch upload failed: {r[:120]!r} - box untouched', file=sys.stderr)
        return 1

    # Fire and forget: the batch kills us, so a reply is not expected.
    try:
        await c.send_command(f'EXEC cmd /c start /min {bat}', timeout=15)
    except Exception:
        pass
    try:
        await c.close()
    except Exception:
        pass

    print('  restarting...')
    for attempt in range(20):
        await asyncio.sleep(10)
        try:
            c2 = RetroConnection(host, port)
            await asyncio.wait_for(c2.connect(SECRET), timeout=10)
            after = await c2.command_text('SYSINFO', timeout=20)
            await c2.close()
            print(f'  after:  {after[:120]}')
            return 0
        except Exception:
            continue
    print('  BOX DID NOT COME BACK - check it physically', file=sys.stderr)
    return 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('host')
    ap.add_argument('--port', type=int, default=9898)
    ap.add_argument('--exe', default=os.path.join(
        os.path.dirname(os.path.abspath(__file__)), os.pardir, os.pardir,
        'agent', 'retro_agent.exe'))
    a = ap.parse_args()
    if not os.path.isfile(a.exe):
        print(f'no such binary: {a.exe}', file=sys.stderr)
        return 2
    return asyncio.run(go(a.host, a.port, a.exe))


if __name__ == '__main__':
    sys.exit(main())
