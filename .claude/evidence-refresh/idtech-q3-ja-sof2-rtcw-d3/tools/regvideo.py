import asyncio, json, os, sys, re
sys.path.insert(0, '/home/voidsstr/development/retro-agent')
from client.retro_protocol import RetroConnection
EV = '/home/voidsstr/development/retro-agent/.claude/evidence-refresh/idtech-q3-ja-sof2-rtcw-d3'
PAT = re.compile(r'refresh|freq|override|vrefresh|dalrule|monitor|timing', re.I)
async def rr(c, k):
    st, d = await c.send_command('REGREAD ' + k, timeout=60)
    t = d.decode('latin-1')
    try: return json.loads(t)
    except Exception: return {'_raw': t[:500]}
async def main():
    for ip in sys.argv[1:]:
        b = ip.split('.')[-1]
        c = RetroConnection(ip, 9898)
        await c.connect('retro-agent-secret', timeout=15)
        out = {}
        try:
            root = await rr(c, r'HKLM SYSTEM\CurrentControlSet\Control\Video')
            guids = root.get('subkeys', [])
            for g in guids:
                for inst in ('0000', '0001'):
                    k = r'HKLM SYSTEM\CurrentControlSet\Control\Video\%s\%s' % (g, inst)
                    j = await rr(c, k)
                    vals = j.get('values', [])
                    if not vals: continue
                    desc = [v.get('data') for v in vals if v['name'] in ('Device Description', 'DriverDesc', 'HardwareInformation.AdapterString')]
                    hits = [(v['name'], str(v.get('data'))[:80]) for v in vals if PAT.search(v['name'])]
                    ds = [(v['name'], v.get('data')) for v in vals if v['name'].startswith('DefaultSettings.')]
                    out[k] = {'desc': desc, 'hits': hits, 'defaults': ds, 'nvalues': len(vals)}
        finally:
            await c.close()
        json.dump(out, open(os.path.join(EV, 'reg_video_%s.json' % b), 'w'), indent=1)
        print('=====', ip)
        for k, v in out.items():
            print(' ', k.split('Video\\')[1], v['desc'][:1], 'nvalues', v['nvalues'])
            print('     defaults', v['defaults'])
            for h in v['hits'][:40]: print('     hit', h)
asyncio.run(main())
