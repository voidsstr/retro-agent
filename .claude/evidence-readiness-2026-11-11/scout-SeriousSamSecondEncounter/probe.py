#!/usr/bin/env python3
"""READ-ONLY probe of Serious Sam TSE client state on fleet boxes.

Only DIRLIST / DOWNLOAD / PROCLIST / WINLIST. Nothing is launched, killed or
written on a box. One connection per box, closed gracefully.
"""
import asyncio, json, os, sys, struct

sys.path.insert(0, "/home/voidsstr/development/retro-agent")
from client.retro_protocol import RetroConnection  # noqa: E402

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "ssse_probe")
T = "C:\\Games\\SeriousSamSecondEncounter"
BOXES = [int(x) for x in (sys.argv[1:] or "110 123 124 133 143 171 184 240 186".split())]


def parse_plr(b):
    """PLC4 + CTString name + CTString team + GUID[16] + appearance."""
    try:
        if b[:4] != b"PLC4":
            return {"raw_head": b[:16].hex()}
        i = 4
        n = struct.unpack_from("<i", b, i)[0]; i += 4
        name = b[i:i + n].decode("latin1"); i += n
        n = struct.unpack_from("<i", b, i)[0]; i += 4
        team = b[i:i + n].decode("latin1"); i += n
        guid = b[i:i + 16].hex(); i += 16
        return {"name": name, "team": team, "guid": guid, "len": len(b)}
    except Exception as e:
        return {"error": repr(e), "len": len(b)}


async def dl(c, path):
    try:
        return await c.command_binary(f"DOWNLOAD {path}")
    except Exception as e:
        return e


async def dirlist(c, path):
    try:
        return json.loads(await c.command_text(f"DIRLIST {path}"))
    except Exception as e:
        return {"error": repr(e)}


async def probe(o):
    rep = {"box": o}
    c = RetroConnection(f"192.168.1.{o}", 9898)
    try:
        await c.connect("retro-agent-secret", timeout=15.0)
    except Exception as e:
        return {"box": o, "connect_error": repr(e)}
    try:
        d = os.path.join(OUT, str(o))
        os.makedirs(d, exist_ok=True)
        rep["root"] = await dirlist(c, T)
        rep["players"] = await dirlist(c, T + "\\Players")
        rep["data"] = await dirlist(c, T + "\\Data")
        rep["scripts"] = await dirlist(c, T + "\\Scripts")
        rep["dtools"] = await dirlist(c, "C:\\Program Files\\D-Tools")
        rep["fleetmp"] = await dirlist(c, "C:\\fleetmp")
        rep["games_lanhost"] = await dirlist(c, "C:\\Games\\lanhost.txt")
        if o == 124:
            rep["vcr"] = await dirlist(c, "C:\\vcr")
        for rel, fn in [("Scripts\\PersistentSymbols.ini", "PersistentSymbols.ini"),
                        ("Scripts\\Game_startup.ini", "Game_startup.ini"),
                        ("SeriousSam.log", "SeriousSam.log"),
                        ("Data\\SeriousSam.gms", "SeriousSam.gms"),
                        ("mount-error.txt", "mount-error.txt"),
                        ("Players\\Player0.plr", "Player0.plr")]:
            b = await dl(c, T + "\\" + rel)
            if isinstance(b, (bytes, bytearray)):
                with open(os.path.join(d, fn), "wb") as f:
                    f.write(b)
                rep.setdefault("files", {})[rel] = len(b)
                if fn == "Player0.plr":
                    rep["player0"] = parse_plr(b)
            else:
                rep.setdefault("files", {})[rel] = f"ERR {b!r}"[:160]
        try:
            pl = json.loads(await c.command_text("PROCLIST"))
            pl = pl if isinstance(pl, list) else pl.get("processes", [])
            names = sorted({(p.get("name") or "") for p in pl})
            rep["procs_of_interest"] = [n for n in names if n.lower() in
                                        ("seriousam.exe", "serioussam.exe", "daemon.exe", "dedicatedserver.exe",
                                         "cmd.exe", "setup.exe", "autorun.exe", "dwwin.exe", "drwtsn32.exe")]
            rep["proc_count"] = len(pl)
        except Exception as e:
            rep["proclist_error"] = repr(e)
        try:
            wl = json.loads(await c.command_text("WINLIST"))
            rep["foreground"] = wl.get("foreground") if isinstance(wl, dict) else None
            wins = wl.get("windows", []) if isinstance(wl, dict) else wl
            rep["windows"] = [(w.get("title"), w.get("class")) for w in wins if w.get("title")][:40]
        except Exception as e:
            rep["winlist_error"] = repr(e)
    finally:
        try:
            await c.close()
        except Exception:
            pass
    return rep


async def main():
    os.makedirs(OUT, exist_ok=True)
    res = await asyncio.gather(*[probe(o) for o in BOXES], return_exceptions=True)
    out = {}
    for o, r in zip(BOXES, res):
        out[o] = r if not isinstance(r, Exception) else {"error": repr(r)}
    with open(os.path.join(OUT, "probe.json"), "w") as f:
        json.dump(out, f, indent=1, default=str)
    print(os.path.join(OUT, "probe.json"))


if __name__ == "__main__":
    asyncio.run(main())
