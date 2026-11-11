import asyncio, json, sys, os
sys.path.insert(0, '/home/voidsstr/development/retro-agent')
from client.retro_protocol import RetroConnection
OUT = sys.argv[1]
BOXES = sys.argv[2].split(',')
FILES = [r'C:\Games\Turok2\Data\config.ned', r'C:\Games\Turok2\turok2.ini', r'C:\Games\Turok2\integrated.cfg', r'C:\Games\Turok2\gspylite.cfg']
async def one(ip):
    res = {'ip': ip}
    c = RetroConnection(ip, 9898)
    try:
        await c.connect('retro-agent-secret', timeout=20)
    except Exception as e:
        res['error'] = 'connect: %r' % e
        return res
    try:
        st, d = await c.send_command(r'DIRLIST C:\Games\Turok2')
        res['dirlist'] = d.decode('latin1', 'replace')
        try:
            names = [x['name'] for x in json.loads(res['dirlist'])]
        except Exception:
            names = []
        extra = [n for n in names if n.lower().endswith(('.log', '.txt')) and n.lower() not in ('readmeenglish.txt', 'launch.txt', 'readme-fleet.txt')]
        for f in FILES + [r'C:\Games\Turok2' + '\\' + n for n in extra]:
            try:
                st, d = await c.send_command('DOWNLOAD ' + f)
                fn = os.path.join(OUT, ip.split('.')[-1] + '__' + f.split('\\')[-1])
                if st == 0 or len(d) > 0:
                    open(fn, 'wb').write(d)
                    res.setdefault('files', {})[f] = (st, len(d))
            except Exception as e:
                res.setdefault('files', {})[f] = 'err %r' % e
    finally:
        try:
            await c.close()
        except Exception:
            pass
    return res
async def main():
    rs = await asyncio.gather(*[one(ip) for ip in BOXES])
    json.dump(rs, open(os.path.join(OUT, 'summary.json'), 'w'), indent=1)
    for r in rs:
        print(r['ip'], r.get('error', ''), json.dumps(r.get('files', {}))[:600])
asyncio.run(main())
