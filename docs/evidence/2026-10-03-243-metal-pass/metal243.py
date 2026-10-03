#!/usr/bin/env python3
"""metal243.py - on-the-metal pass over .243's 3dfx and DOS shortcuts (2026-10-03).

For each shortcut: launch its own .bat (cwd = the title folder, as the .pif
does), wait for it to load, record which game process is running and any
dialog, then quit it - console 'quit' for engines that have one, else
PROCKILL - close what is left (DOS boxes and their 'terminate?' dialog) and
hand a Glide title's screen back to the 2D card. The agent cannot see the
Voodoo 2's output: the person at the box judges the picture; this records
liveness. Results: ~/.retro-fleet/w98vm/metal243.json
"""
import asyncio, json, os, sys, time
sys.path.insert(0, '/home/voidsstr/development/retro-agent/.claude/worktrees/q1voodoo/scripts/vm/win98')
import sweep
sweep.HOST, sweep.PORT = '192.168.1.243', 9898
OUT = os.path.expanduser('~/.retro-fleet/w98vm/metal243.json')

# (label, dir under E:\GAMES, bat, kind, load seconds)
#   kind: q = id/Quake-family console quit, ue = Unreal console, kill = PROCKILL, dos = DOS box
TESTS = [
    ('Quake - 3dfx Voodoo', 'QUAKE1', 'Q1V640.BAT', 'q', 40),
    ('Quake - 3dfx Voodoo 800x600', 'QUAKE1', 'Q1V800.BAT', 'q', 40),
    ('Quake - Scourge of Armagon - 3dfx Voodoo', 'QUAKE1', 'Q1HIPV.BAT', 'q', 40),
    ('Quake - Dissolution of Eternity - 3dfx Voodoo', 'QUAKE1', 'Q1ROGV.BAT', 'q', 40),
    ('Quake - DOS', 'QUAKE1', 'Q1DOS.BAT', 'dos', 35),
    ('Quake - DOS 360x480', 'QUAKE1', 'Q1DOSM10.BAT', 'dos', 35),
    ('Quake - Scourge of Armagon - DOS', 'QUAKE1', 'Q1HIPD.BAT', 'dos', 35),
    ('Quake - Dissolution of Eternity - DOS', 'QUAKE1', 'Q1ROGD.BAT', 'dos', 35),
    ('Quake II', 'QUAKE2~1', 'PLAYQU~1.BAT', 'q', 45),
    ('Quake II - 3dfx Voodoo', 'QUAKE2~1', 'PLAYQU~2.BAT', 'q', 45),
    ('Quake II - The Reckoning - 3dfx Voodoo', 'QUAKE2~2', 'PLAYQU~2.BAT', 'q', 50),
    ('Quake II - Ground Zero - 3dfx Voodoo', 'QUAKE2~2', 'PLAYQU~1.BAT', 'q', 50),
    ('SiN - 3dfx Voodoo', 'SINGOLD', 'PLAYSI~4.BAT', 'q', 60),
    ('SiN - Wages of SiN - 3dfx Voodoo', 'SINGOLD', 'PLAYWA~2.BAT', 'q', 60),
    ('Half-Life - 3dfx Voodoo', 'HALFLI~1', 'PLAYHA~1.BAT', 'q', 75),
    ('Team Fortress Classic - 3dfx Voodoo', 'HALFLI~1', 'PLAYTE~1.BAT', 'q', 75),
    ('Deathmatch Classic - 3dfx Voodoo', 'HALFLI~1', 'PLAYDE~1.BAT', 'q', 75),
    ('Hexen II - 3dfx Voodoo', 'HEXENII', 'PLAYHE~3.BAT', 'q', 45),
    ('Unreal Gold - 3dfx Voodoo', 'UNREAL~2', 'PLAYUN~2.BAT', 'ue', 75),
    ('Carmageddon 2 - 3dfx Voodoo', 'CARMAG~1', 'PLAYCA~2.BAT', 'kill', 60),
    ('Wing Commander Prophecy - 3dfx Voodoo', 'WCPROP~1', 'WCP.BAT', 'kill', 60),
    ('Wing Commander Secret Ops - 3dfx Voodoo', 'WCPROP~1', 'WCSO.BAT', 'kill', 60),
    ('Die by the Sword - 3dfx Voodoo', 'DIEBYT~1', 'DBTS.BAT', 'kill', 60),
    ('Star Wars Rogue Squadron 3D - 3dfx Voodoo', 'ROGUES~1', 'ROGUESQ.BAT', 'kill', 60),
    ('X-Wing', 'FLIGH~34', 'PLAYX-~1.BAT', 'dos', 35),
    ('TIE Fighter', 'FLIGH~35', 'PLAYTI~1.BAT', 'dos', 35),
    ('Descent - DOS', 'DESCENT1', 'PLAYDE~3.BAT', 'dos', 40),
    ('Descent II - DOS', 'DESCENT2', 'D2DOS.BAT', 'dos', 45),
]
SHELL = {'kernel32.dll', 'msgsrv32.exe', 'mprexe.exe', 'mstask.exe', 'explorer.exe', 'systray.exe',
         'taskmon.exe', 'ddhelp.exe', 'mmtask.tsk', 'retro_agent.exe', 'retro_chat.exe', 'start.exe',
         'winoa386.mod', 'command.com', 'conagent.exe', 'winkey9x.exe', 'glreset9.exe', 'spool32.exe',
         'rundll32.exe', 'deskfix9.exe'}


def say(*a):
    print(time.strftime('%H:%M:%S'), *a, flush=True)


async def procs(c):
    j = json.loads(await sweep.cmd(c, 'PROCLIST'))
    lst = j if isinstance(j, list) else j.get('processes', [])
    return {(p.get('name') or '').split('\\')[-1].lower(): int(p.get('pid')) & 0xFFFFFFFF for p in lst}


async def one(c, label, d, bat, kind, load):
    r = {'title': label, 'bat': 'E:\\GAMES\\%s\\%s' % (d, bat), 'kind': kind}
    before = await procs(c)
    wrap = '@echo off\r\nE:\r\ncd \\GAMES\\%s\r\ncall %s\r\n' % (d, bat)
    await c.send_command('UPLOAD C:\\RETRO_AGENT\\RUNT.BAT', binary_payload=wrap.encode('ascii'))
    await sweep.cmd(c, 'LAUNCH C:\\WINDOWS\\COMMAND\\START.EXE C:\\RETRO_AGENT\\RUNT.BAT')
    say('LAUNCHED', label)
    await asyncio.sleep(load)
    now = await procs(c)
    game = sorted(n for n in now if n not in before and n not in SHELL)
    ws, fg = await sweep.winlist(c)
    dialogs = [w.get('title') for w in ws if w.get('class') == '#32770']
    r.update(game_procs=game, foreground=fg.get('title'), dialogs=dialogs,
             dos_box=any(w.get('class') == 'tty' for w in ws if (w.get('title') or '') not in ('',)))
    say('  running:', game or '(no new Win32 process)', '| fg:', (fg.get('title') or '')[:50], '| dialogs:', dialogs)
    # quit
    quit_how = ''
    fgt = (fg.get('title') or '')
    if kind in ('q', 'ue') and game and fgt and not fgt.startswith(('Retro Remote', 'Program Manager', 'retro_chat')):
        for k in ('TILDE', 'TEXT:quit', 'RETURN'):
            await sweep.cmd(c, 'UIKEY ' + k)
            await asyncio.sleep(1.5)
        for _ in range(8):
            await asyncio.sleep(2)
            if not [n for n in game if n in await procs(c)]:
                quit_how = 'console quit'
                break
    left = [n for n in game if n in await procs(c)]
    if left:
        cur = await procs(c)
        for n in left:
            await sweep.cmd(c, 'PROCKILL %d' % cur[n])
        quit_how = (quit_how + ' + ' if quit_how else '') + 'PROCKILL ' + ','.join(left)
        await asyncio.sleep(4)
    await sweep.agent_pids(c)
    left_windows = await sweep.close_all(c)
    if kind != 'dos':
        r['glreset'] = await sweep.glreset(c)
    r.update(quit=quit_how or ('DOS box closed' if kind == 'dos' else 'exited by itself'),
             windows_left=left_windows,
             still_running=sorted(n for n in await procs(c) if n not in before and n not in SHELL))
    say('  closed:', r['quit'], '| left:', left_windows, r['still_running'], '|', r.get('glreset', ''))
    return r


async def main():
    only = set(sys.argv[1:])
    c = await sweep.connect(timeout=20)
    await sweep.ensure_helpers(c)
    await sweep.agent_pids(c)
    results = []
    for t in TESTS:
        if only and t[0] not in only:
            continue
        try:
            results.append(await one(c, *t))
        except Exception as e:
            say('  ERROR', t[0], repr(e))
            results.append({'title': t[0], 'error': repr(e)})
            try:
                await c.close()
            except Exception:
                pass
            await asyncio.sleep(10)
            c = await sweep.connect(timeout=60)
            await sweep.agent_pids(c)
        json.dump(results, open(OUT, 'w'), indent=1)
        await asyncio.sleep(5)
    await c.close()
    say('DONE', len(results), 'titles ->', OUT)


asyncio.run(main())
