import asyncio, json, sys, os
sys.path.insert(0, '/home/voidsstr/development/retro-agent')
from client.retro_protocol import RetroConnection

OUT = os.path.dirname(os.path.abspath(__file__))
USERS = {110: ['Administrator'], 123: ['Administrator'], 124: ['Administrator'], 133: ['admin'],
         143: ['Administrator', 'Administrator.1GHZ'], 171: ['Administrator'], 184: ['Administrator'], 240: ['User']}

async def one(o):
    res = {}
    c = RetroConnection(f'192.168.1.{o}', 9898)
    try:
        await c.connect('retro-agent-secret', timeout=20.0)
        for u in USERS[o]:
            base = f'C:\\Documents and Settings\\{u}\\Application Data\\Quake3'
            for sub in ('', '\\missionpack', '\\baseq3'):
                cmd = f'DIRLIST {base}{sub}'
                try:
                    st, data = await asyncio.wait_for(c.send_command(cmd), timeout=40)
                    res[cmd] = data.decode('latin1', 'replace')
                except Exception as e:
                    res[cmd] = 'ERR ' + repr(e)
            # download the missionpack q3config.cfg if present
            try:
                lst = json.loads(res[f'DIRLIST {base}\\missionpack'])
                if any(x['name'].lower() == 'q3config.cfg' for x in lst):
                    b = await asyncio.wait_for(c.command_binary(f'DOWNLOAD {base}\\missionpack\\q3config.cfg'), timeout=40)
                    with open(os.path.join(OUT, f'q3config_mp_{o}_{u}.cfg'), 'wb') as f:
                        f.write(b)
                    res['downloaded_' + u] = len(b)
            except Exception as e:
                res['dl_err_' + u] = repr(e)
        # the box's own launcher-written fleetres.cfg for missionpack
        try:
            b = await asyncio.wait_for(c.command_binary('DOWNLOAD C:\\Games\\Quake3-TeamArena\\missionpack\\fleetres.cfg'), timeout=40)
            with open(os.path.join(OUT, f'fleetres_mp_{o}.cfg'), 'wb') as f:
                f.write(b)
        except Exception as e:
            res['fleetres_err'] = repr(e)
    except Exception as e:
        res['connect_error'] = repr(e)
    finally:
        try:
            await c.close()
        except Exception:
            pass
    with open(os.path.join(OUT, f'probe2_{o}.json'), 'w') as f:
        json.dump(res, f, indent=1)
    return o, [k for k in res]

async def main():
    for r in await asyncio.gather(*[one(o) for o in USERS], return_exceptions=True):
        print(r)

asyncio.run(main())
