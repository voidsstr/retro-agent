import asyncio, json, sys, os
sys.path.insert(0, '/home/voidsstr/development/retro-agent')
from client.retro_protocol import RetroConnection
OUTD = sys.argv[1]
JOBS = {
  124: [r"C:\Games\SoldierOfFortune\user\sof.log", r"C:\Games\SoldierOfFortune\base\lancheck.cfg",
        r"C:\Games\SoldierOfFortune\LANCHECK.BAT", r"C:\Games\SoldierOfFortune\base\fleetres.cfg",
        r"C:\Games\SoldierOfFortune\user\config.cfg"],
  240: [r"C:\Games\SoldierOfFortune\base\fleetres.cfg", r"C:\Games\SoldierOfFortune\user\config.cfg",
        r"C:\Games\SoldierOfFortune\Join Soldier of Fortune - LAN.bat"],
  123: [r"C:\Games\SoldierOfFortune\base\fleetres.cfg"],
  110: [r"C:\Games\SoldierOfFortune\base\fleetres.cfg"],
}
DIRS = {123: [r"C:\Games\SoldierOfFortune\_disc"], 240: [r"C:\Games\SoldierOfFortune\_disc"],
        143: [r"C:\Games\SoldierOfFortune\_disc"], 133: [r"C:\Games\SoldierOfFortune\_disc"],
        171: [r"C:\Games\SoldierOfFortune\_disc"], 184: [r"C:\Games\SoldierOfFortune\_disc"]}
async def one(o):
    c = RetroConnection(f"192.168.1.{o}", 9898)
    res = {}
    try:
        await c.connect('retro-agent-secret', timeout=15)
        for p in JOBS.get(o, []):
            try:
                b = await c.command_binary(f"DOWNLOAD {p}")
                fn = os.path.join(OUTD, f"{o}_" + p.split('\\')[-1].replace(' ', '_'))
                open(fn, 'wb').write(b)
                res[p] = len(b)
            except Exception as e:
                res[p] = f"ERR {e}"
        for p in DIRS.get(o, []):
            try:
                res[p] = json.loads(await c.command_text(f"DIRLIST {p}"))
            except Exception as e:
                res[p] = f"ERR {e}"
    except Exception as e:
        res['connect'] = f"ERR {e}"
    finally:
        try: await c.close()
        except Exception: pass
    return o, res
async def main():
    out = dict(await asyncio.gather(*[one(o) for o in sorted(set(JOBS) | set(DIRS))]))
    json.dump(out, open(os.path.join(OUTD, 'dl.json'), 'w'), indent=1)
asyncio.run(main())
