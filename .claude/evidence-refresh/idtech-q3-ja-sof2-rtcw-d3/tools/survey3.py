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
async def main():
    for ip in sys.argv[1:]:
        b = ip.split('.')[-1]
        dirs = json.load(open(os.path.join(EV, 'box_%s_dirs.json' % b)))
        want = []
        for d, ents in dirs.items():
            if not isinstance(ents, list): continue
            for name, size, mod in ents:
                nl = name.lower()
                if nl.endswith(('.cfg', '.log')) and size < 200000 and nl not in ('games.log',) and 'botlib' not in nl:
                    want.append(d + '\\' + name)
        outdir = os.path.join(EV, 'files_%s' % b)
        os.makedirs(outdir, exist_ok=True)
        c = RetroConnection(ip, 9898)
        await c.connect('retro-agent-secret', timeout=15)
        got = []
        try:
            for p in want:
                try:
                    st, data = await c.send_command('DOWNLOAD ' + p, timeout=60)
                except Exception as e:
                    print('  ERR', p, e); continue
                if st not in (0, 1):
                    print('  status', st, p, data[:100]); continue
                fn = p.replace(':', '').replace('\\', '_').replace(' ', '_')
                open(os.path.join(outdir, fn), 'wb').write(data)
                got.append((p, len(data)))
        finally:
            await c.close()
        print('=====', ip, len(got), 'files')
        for p, n in got: print('   %7d %s' % (n, p))
asyncio.run(main())
