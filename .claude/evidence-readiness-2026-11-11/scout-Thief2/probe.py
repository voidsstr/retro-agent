#!/usr/bin/env python3
"""READ-ONLY Thief2 scout probe (2026-11-11): DIRLIST/DOWNLOAD/PROCLIST/AUDIOINFO only."""
import asyncio, json, sys, os, hashlib
sys.path.insert(0, "/home/voidsstr/development/retro-agent")
from client.retro_protocol import RetroConnection

OUT = os.path.dirname(os.path.abspath(__file__))
BOXES = [110, 123, 124, 133, 143, 171, 184, 186, 240]
T = "C:\\Games\\Thief2"

async def cmd_text(c, s):
    try:
        st, data = await c.send_command(s)
        return st, data.decode("latin1", "replace")
    except Exception as e:
        return None, f"EXC {e!r}"

async def probe(o):
    r = {"box": o}
    c = RetroConnection(f"192.168.1.{o}", 9898)
    try:
        await c.connect("retro-agent-secret", timeout=15)
    except Exception as e:
        r["connect_error"] = repr(e)
        return r
    try:
        for key, s in [("dir_root", f"DIRLIST {T}"),
                       ("dir_movies", f"DIRLIST {T}\\MOVIES"),
                       ("dir_saves", f"DIRLIST {T}\\SAVES"),
                       ("proclist", "PROCLIST"),
                       ("audioinfo", "AUDIOINFO")]:
            st, t = await cmd_text(c, s)
            r[key] = t
        for key, path in [("cam_cfg", f"{T}\\cam.cfg"),
                          ("play_bat", f"{T}\\Play Thief 2.bat"),
                          ("user_cfg", f"{T}\\USER.CFG")]:
            try:
                b = await c.command_binary(f"DOWNLOAD {path}")
                r[key] = b.decode("latin1", "replace")
                r[key + "_md5"] = hashlib.md5(b).hexdigest()
            except Exception as e:
                r[key] = f"EXC {e!r}"
    finally:
        try:
            await c.close()
        except Exception:
            pass
    return r

async def main():
    res = await asyncio.gather(*[probe(o) for o in BOXES])
    for r in res:
        with open(os.path.join(OUT, f"probe_{r['box']}.json"), "w") as f:
            json.dump(r, f, indent=1)
        print(r["box"], "ok" if "connect_error" not in r else r["connect_error"])

asyncio.run(main())
