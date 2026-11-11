"""READ-ONLY probe of Serious Sam TFE state on each box (DIRLIST/DOWNLOAD/PROCLIST/REGREAD only)."""
import asyncio, json, os, sys
sys.path.insert(0, '/home/voidsstr/development/retro-agent')
from client.retro_protocol import RetroConnection, RetroProtocolError

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'boxes')
T = r'C:\Games\SeriousSamFirstEncounter'
BOXES = [110, 123, 124, 133, 143, 171, 184, 186, 240]

async def probe(o):
    rep = {'box': o}
    d = os.path.join(OUT, str(o)); os.makedirs(d, exist_ok=True)
    c = RetroConnection(f'192.168.1.{o}', 9898)
    try:
        await c.connect('retro-agent-secret', timeout=20.0)
    except Exception as e:
        rep['connect_error'] = repr(e); return rep
    async def txt(cmd):
        try:
            return await c.command_text(cmd)
        except Exception as e:
            return f'ERR {e!r}'
    async def dl(path, name):
        try:
            b = await c.command_binary(f'DOWNLOAD {path}')
            open(os.path.join(d, name), 'wb').write(b)
            return len(b)
        except Exception as e:
            return f'ERR {e!r}'
    try:
        for sub in ['', r'\Players', r'\Scripts', r'\Data', r'\Temp', r'\Controls']:
            r = await txt(f'DIRLIST {T}{sub}')
            rep['dir' + (sub.replace('\\', '_') or '_root')] = r if r.startswith('ERR') else json.loads(r)
        root = rep.get('dir_root')
        names = [x['name'] for x in root] if isinstance(root, list) else []
        rep['root_names'] = names
        for f in names:
            if f.lower().endswith(('.log', '.txt')) and f.lower() not in ('notes.txt', 'launch.txt'):
                rep['dl_' + f] = await dl(f'{T}\\{f}', f)
        rep['dl_PersistentSymbols'] = await dl(f'{T}\\Scripts\\PersistentSymbols.ini', 'PersistentSymbols.ini')
        rep['dl_Game_startup'] = await dl(f'{T}\\Scripts\\Game_startup.ini', 'Game_startup.ini')
        pl = rep.get('dir_Players')
        for x in (pl if isinstance(pl, list) else []):
            if not x['is_dir']:
                rep['dl_pl_' + x['name']] = await dl(f'{T}\\Players\\{x["name"]}', 'Players_' + x['name'])
        da = rep.get('dir_Data')
        for x in (da if isinstance(da, list) else []):
            if not x['is_dir'] and x['name'].lower().endswith('.gms'):
                rep['dl_data_' + x['name']] = await dl(f'{T}\\Data\\{x["name"]}', 'Data_' + x['name'])
        r = await txt(r'DIRLIST C:\fleetmp')
        rep['fleetmp'] = r if r.startswith('ERR') else [x['name'] for x in json.loads(r)]
        if isinstance(rep['fleetmp'], list) and any(n.lower() == 'ssam-tfe.bat' for n in rep['fleetmp']):
            rep['dl_fleetmp_bat'] = await dl(r'C:\fleetmp\ssam-tfe.bat', 'fleetmp_ssam-tfe.bat')
        for p in [r'C:\Program Files\D-Tools', r'C:\Program Files\DAEMON Tools', r'C:\Program Files\DAEMON Tools Lite', r'C:\Program Files\WinCDEmu']:
            r = await txt(f'DIRLIST {p}')
            rep['mounter ' + p] = 'absent' if r.startswith('ERR') else [x['name'] for x in json.loads(r)][:12]
        r = await txt('PROCLIST')
        try:
            pr = json.loads(r); pr = pr if isinstance(pr, list) else pr.get('processes', [])
            rep['procs'] = sorted({(p.get('name') or '') for p in pr})
        except Exception:
            rep['procs'] = r[:300]
        rep['regread_d347bus'] = (await txt(r'REGREAD HKLM SYSTEM\CurrentControlSet\Services\d347bus'))[:300]
        rep['gameres'] = (await txt('GAMERES'))[:6000]
    finally:
        try:
            await c.close()
        except Exception:
            pass
    return rep

async def main():
    boxes = [int(x) for x in sys.argv[1:]] or BOXES
    res = await asyncio.gather(*[probe(o) for o in boxes], return_exceptions=True)
    for o, r in zip(boxes, res):
        if isinstance(r, Exception):
            r = {'box': o, 'error': repr(r)}
        json.dump(r, open(os.path.join(OUT, f'probe_{o}.json'), 'w'), indent=1)
        print(o, 'ok' if 'connect_error' not in r and 'error' not in r else r.get('connect_error') or r.get('error'))

asyncio.run(main())
