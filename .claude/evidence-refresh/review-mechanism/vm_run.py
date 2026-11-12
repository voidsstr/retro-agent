#!/usr/bin/env python3
"""Exercise modetest.exe 0.2.0 inside a THROWAWAY overlay of the XP build VM
(127.0.0.1:29898 -> the guest agent). The VM has no monitor, so a mode switch
there hurts nothing; nothing here talks to a fleet box.

    vm_run.py upload
    vm_run.py execw <seconds> <modetest args...>
    vm_run.py launch <modetest args...>      (then: vm_run.py log)
    vm_run.py log                            (DOWNLOAD modetest.log)
    vm_run.py cmd <agent command...>
"""
import asyncio, os, sys, time
sys.path.insert(0, '/home/voidsstr/development/retro-agent')
from client.retro_protocol import RetroConnection
HERE = os.path.dirname(os.path.abspath(__file__))
EXE = '/home/voidsstr/development/retro-agent/.claude/worktrees/refresh-mech/tools/modetest/modetest.exe'
RDIR = r'C:\RETRO_AGENT\modetest'
REXE = RDIR + r'\modetest.exe'
RLOG = RDIR + r'\modetest.log'
OUT = os.path.join(HERE, 'vm')
os.makedirs(OUT, exist_ok=True)

async def conn():
    c = RetroConnection('127.0.0.1', 29898)
    await c.connect('retro-agent-secret', timeout=20)
    return c

def save(name, text):
    with open(os.path.join(OUT, name), 'a') as f:
        f.write('# %s\n%s\n' % (time.strftime('%Y-%m-%d %H:%M:%S'), text))

async def main():
    a = sys.argv[1:]
    c = await conn()
    try:
        if a[0] == 'upload':
            print(await c.send_command('MKDIR ' + RDIR))
            data = open(EXE, 'rb').read()
            print(await c.send_command('UPLOAD ' + REXE, binary_payload=data))
            st, d = await c.send_command('DIRLIST ' + RDIR)
            print(st, d.decode(errors='replace'))
        elif a[0] == 'execw':
            secs = int(a[1]); args = ' '.join(a[2:])
            t = time.time()
            st, d = await c.send_command('EXECW %d %s %s' % (secs, REXE, args), timeout=secs + 60)
            txt = d.decode(errors='replace')
            print('status', st, 'in %.1fs' % (time.time() - t))
            print(txt)
            save('execw.txt', 'EXECW %d %s\nstatus %s\n%s' % (secs, args, st, txt))
        elif a[0] == 'launch':
            args = ' '.join(a[1:])
            st, d = await c.send_command('LAUNCH %s %s' % (REXE, args))
            print(st, d.decode(errors='replace'))
            save('launch.txt', 'LAUNCH %s\nstatus %s\n%s' % (args, st, d.decode(errors='replace')))
        elif a[0] == 'log':
            st, d = await c.send_command('DOWNLOAD ' + RLOG, timeout=60)
            txt = d.decode(errors='replace')
            open(os.path.join(OUT, 'modetest.log'), 'w').write(txt)
            print(txt[-int(a[1]) if len(a) > 1 else -6000:])
        elif a[0] == 'cmd':
            st, d = await c.send_command(' '.join(a[1:]), timeout=120)
            print(st, d.decode(errors='replace')[:6000])
    finally:
        await c.close()

asyncio.run(main())
