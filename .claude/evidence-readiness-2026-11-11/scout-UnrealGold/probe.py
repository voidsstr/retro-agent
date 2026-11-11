#!/usr/bin/env python3
"""READ-ONLY scout probe for Unreal Gold (2026-11-11).
Per box: DIRLIST of the title root and System, PROCLIST, REGREAD of
HKLM\\Software\\RetroAgent, DOWNLOAD of System\\User.ini / Unreal.ini /
(Voodoo.ini on .243) / Unreal.log when small. No EXEC, no writes on any box."""
import asyncio, json, os, sys
sys.path.insert(0, '/home/voidsstr/development/retro-agent')
from client.retro_protocol import RetroConnection

OUT = os.path.dirname(os.path.abspath(__file__))
BOXES = {110: 'C:\\Games', 123: 'C:\\Games', 124: 'C:\\Games', 133: 'C:\\Games',
         143: 'C:\\Games', 171: 'C:\\Games', 184: 'C:\\Games', 186: 'C:\\Games',
         240: 'C:\\Games', 243: 'E:\\GAMES'}

def entries(js):
    try:
        d = json.loads(js)
    except Exception:
        return []
    if isinstance(d, dict):
        for k in ('entries', 'files', 'items'):
            if isinstance(d.get(k), list):
                return d[k]
    return d if isinstance(d, list) else []

async def probe(o):
    root = BOXES[o] + '\\UnrealGold'
    rep = {'ip': f'192.168.1.{o}', 'root': root}
    c = RetroConnection(f'192.168.1.{o}', 9898)
    await c.connect('retro-agent-secret', timeout=20 if o == 243 else 15)
    try:
        for tag, path in (('dir_root', root), ('dir_system', root + '\\System')):
            try:
                rep[tag + '_raw'] = await c.command_text('DIRLIST ' + path, timeout=60)
            except Exception as e:
                rep[tag + '_raw'] = 'ERR ' + str(e)
        try:
            rep['proclist'] = await c.command_text('PROCLIST', timeout=60)
        except Exception as e:
            rep['proclist'] = 'ERR ' + str(e)
        try:
            rep['reg_retroagent'] = await c.command_text('REGREAD HKLM Software\\RetroAgent', timeout=30)
        except Exception as e:
            rep['reg_retroagent'] = 'ERR ' + str(e)
        if o == 124:
            try:
                rep['dir_vcr'] = await c.command_text('DIRLIST C:\\vcr', timeout=30)
            except Exception as e:
                rep['dir_vcr'] = 'ERR ' + str(e)
        sysents = {str(e.get('name', '')).lower(): e for e in entries(rep.get('dir_system_raw', ''))}
        wanted = ['user.ini', 'unreal.ini', 'unreal.log']
        if o == 243:
            wanted += ['voodoo.ini', 'voodoo.tpl']
        for n in wanted:
            e = sysents.get(n)
            if not e:
                rep['dl_' + n] = 'absent'
                continue
            size = e.get('size') or 0
            if n.endswith('.log') and size > 400000:
                rep['dl_' + n] = f'skipped (size {size})'
                continue
            try:
                data = await c.command_binary('DOWNLOAD ' + root + '\\System\\' + e.get('name'), timeout=120)
                with open(os.path.join(OUT, f'{o}_{n}'), 'wb') as f:
                    f.write(data)
                rep['dl_' + n] = f'saved {len(data)} B'
            except Exception as ex:
                rep['dl_' + n] = 'ERR ' + str(ex)
    finally:
        await c.close()
    with open(os.path.join(OUT, f'probe_{o}.json'), 'w') as f:
        json.dump(rep, f, indent=1)
    return o, {k: (v if k.startswith('dl_') else len(v) if isinstance(v, str) else v) for k, v in rep.items()}

async def main():
    sel = [int(x) for x in sys.argv[1:]] or list(BOXES)
    res = await asyncio.gather(*[probe(o) for o in sel], return_exceptions=True)
    for r in res:
        print(r)

asyncio.run(main())
