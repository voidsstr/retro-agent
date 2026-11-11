import asyncio, json, sys
sys.path.insert(0, '/home/voidsstr/development/retro-agent')
from client.retro_protocol import RetroConnection
BOXES = [110, 123, 124, 133, 143, 171, 184, 186, 240]
R = r"C:\Games\SoldierOfFortune"
OUT = {}
async def one(o):
    c = RetroConnection(f"192.168.1.{o}", 9898)
    res = {}
    try:
        await c.connect('retro-agent-secret', timeout=15)
        for p in [R, R + r"\base", R + r"\user", R + r"\user\scrnshot"]:
            try:
                t = await c.command_text(f"DIRLIST {p}")
                res[p] = json.loads(t)
            except Exception as e:
                res[p] = f"ERR {e}"
        try:
            pl = json.loads(await c.command_text("PROCLIST"))
            lst = pl if isinstance(pl, list) else pl.get("processes", [])
            res["sof_running"] = [p for p in lst if (p.get("name") or "").lower() == "sof.exe"]
        except Exception as e:
            res["sof_running"] = f"ERR {e}"
    except Exception as e:
        res["connect"] = f"ERR {e}"
    finally:
        try:
            await c.close()
        except Exception:
            pass
    OUT[o] = res
async def main():
    await asyncio.gather(*[one(o) for o in BOXES])
    json.dump(OUT, open(sys.argv[1], 'w'), indent=1)
asyncio.run(main())
