import asyncio, json, os, sys
sys.path.insert(0, '/home/voidsstr/development/retro-agent')
from client.retro_protocol import RetroConnection
EV = '/home/voidsstr/development/retro-agent/.claude/evidence-refresh/idtech-q3-ja-sof2-rtcw-d3'
APPDATA = {
 '123': r'C:\Documents and Settings\Administrator\Application Data',
 '145': r'C:\Documents and Settings\voidsstr\Application Data',
 '195': r'C:\Users\admin\AppData\Roaming',
 '240': r'C:\Documents and Settings\User\Application Data',
 '124': r'C:\Documents and Settings\Administrator\Application Data',
}
DIRS = [
 r'C:\Games\Quake3-TeamArena', r'C:\Games\Quake3-TeamArena\baseq3', r'C:\Games\Quake3-TeamArena\missionpack',
 r'C:\Games\JediAcademy', r'C:\Games\JediAcademy\base',
 r'C:\Games\SoldierOfFortune2', r'C:\Games\SoldierOfFortune2\base', r'C:\Games\SoldierOfFortune2\base\MP',
 r'C:\Games\ReturnToCastleWolfenstein', r'C:\Games\ReturnToCastleWolfenstein\Main',
 r'C:\Games\Doom3', r'C:\Games\Doom3\base',
]
async def main():
    for ip in sys.argv[1:]:
        b = ip.split('.')[-1]
        dirs = DIRS + [APPDATA[b] + r'\Quake3', APPDATA[b] + r'\Quake3\baseq3', APPDATA[b] + r'\Quake3\missionpack']
        out = {}
        c = RetroConnection(ip, 9898)
        await c.connect('retro-agent-secret', timeout=15)
        try:
            for dpath in dirs:
                st, d = await c.send_command('DIRLIST ' + dpath, timeout=60)
                t = d.decode('latin-1')
                try:
                    ents = json.loads(t)
                    out[dpath] = [(e['name'], e['size'], e['modified']) for e in ents if not e['name'].lower().endswith(('.pk3', '.pk4'))]
                except Exception:
                    out[dpath] = t[:300]
        finally:
            await c.close()
        json.dump(out, open(os.path.join(EV, 'box_%s_dirs.json' % b), 'w'), indent=1)
        print('=====', ip)
        for k, v in out.items():
            if isinstance(v, list):
                interesting = [x for x in v if x[0].lower().endswith(('.cfg', '.log', '.txt', '.exe', '.dll', '.bat')) or '.' not in x[0]]
                print('  ', k, ':', ', '.join('%s(%s,%s)' % (n, s, m[5:16]) for n, s, m in interesting)[:1500])
            else:
                print('  ', k, ':', v)
asyncio.run(main())
