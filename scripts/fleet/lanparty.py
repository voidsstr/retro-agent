#!/usr/bin/env python3
"""Put the whole fleet into one multiplayer game on the host's servers, and prove it.

    python3 scripts/fleet/lanparty.py run   <game> [--boxes 110,123,...] [--wait 90]
    python3 scripts/fleet/lanparty.py check <game> [--boxes ...]      # server roster + screenshots
    python3 scripts/fleet/lanparty.py kill  [--boxes ...]             # end every party game

<game> is one of: ssam-tfe ssam-tse q3 q2 cs ut99 ut2004.

Each box gets a small launcher in C:\\fleetmp\\ that runs the staged title's own
FLEETRES.BAT (so the resolution is the box's own) and joins the fleet server on
192.168.1.132 under the name Fleet<octet> where the engine takes a name on the
command line. Evidence is the SERVER's own player list (bots separated) plus a
screenshot of every box - a process that is running is not a player in a game.
"""
import argparse, asyncio, io, json, os, re, socket, sys, time

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, REPO)
from client.retro_protocol import RetroConnection  # noqa: E402

HOST = "192.168.1.132"
SECRET = "retro-agent-secret"
DEFAULT_BOXES = [110, 123, 124, 133, 143, 145, 171, 184, 197, 240]
GAME_EXES = ["seriousSam.exe", "quake3.exe", "ioquake3.x86.exe", "quake2.exe", "hl.exe",
             "unrealtournament.exe", "ut2004.exe"]

G = "C:\\Games\\"
GAMES = {
    "ssam-tfe": dict(title="Serious Sam TFE", query=("gamespy", 25601), exe="SeriousSam.exe",
                     bat='@echo off\r\ncall "' + G + 'SeriousSamFirstEncounter\\Join Serious Sam TFE - LAN.bat" ' + HOST + ':25600\r\n'),
    "ssam-tse": dict(title="Serious Sam TSE", query=("gamespy", 25611), exe="SeriousSam.exe",
                     bat='@echo off\r\ncall "' + G + 'SeriousSamSecondEncounter\\Join Serious Sam TSE - LAN.bat" ' + HOST + ':25610\r\n'),
    "q3": dict(title="Quake III Arena", query=("q3", 27961), exe=("quake3.exe", "ioquake3.x86.exe"),
               bat='@echo off\r\ncd /d "' + G + 'Quake3-TeamArena"\r\ncall FLEETRES.BAT\r\n'
                   'call FLEETGL.BAT "' + G + 'Quake3-TeamArena\\baseq3\\fleetres.cfg"\r\n'
                   'cd /d "' + G + 'Quake3-TeamArena"\r\n'
                   'start "" %FR_ENGINE% +set r_mode -1 +set r_customwidth %FR_W% +set r_customheight %FR_H% '
                   '+set r_customaspect 1 +set r_customPixelAspect 1 +set r_fullscreen 1 +set cg_fov %FR_FOV% '
                   '+set name {name} +connect ' + HOST + ':27961\r\n'),
    "q2": dict(title="Quake II", query=("q2", 27910), exe="quake2.exe",
               bat='@echo off\r\ncd /d "' + G + 'Quake2Complete"\r\ncall FLEETRES.BAT\r\ncd /d "' + G + 'Quake2Complete"\r\n'
                   'start "" quake2.exe +set gl_mode %FR_Q2WIDE% +set vid_fullscreen 1 +set name {name} +connect ' + HOST + ':27910\r\n'),
    "cs": dict(title="Counter-Strike 1.6", query=("a2s", 27018), exe="hl.exe",
               bat='@echo off\r\ncd /d "' + G + 'CounterStrike16"\r\ncall FLEETRES.BAT\r\ncd /d "' + G + 'CounterStrike16"\r\n'
                   'set MESA_FORCE_SSE=1\r\n'
                   'start "" hl.exe -game cstrike -full -gl -w %FR_W% -h %FR_H% +name {name} +connect ' + HOST + ':27015\r\n'),
    "ut99": dict(title="Unreal Tournament", query=("gamespy", 7798), exe="UnrealTournament.exe",
                 bat='@echo off\r\ncd /d "' + G + 'UnrealTournament436"\r\ncall FLEETRES.BAT\r\n'
                     'cd /d "' + G + 'UnrealTournament436\\System"\r\nif exist Running.ini del /q Running.ini\r\n'
                     'start "" UnrealTournament.exe ' + HOST + ':7797?Name={name}\r\n'),
    "ut2004": dict(title="UT2004", query=("gamespy", 7787), exe="UT2004.exe",
                   bat='@echo off\r\ncall "' + G + 'UT2004\\Join fleet UT2004 server.bat"\r\n'),
}


# ---------------------------------------------------------------- server side
def _udp(port, payload, timeout=3.0, many=False):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(timeout)
    s.sendto(payload, (HOST, port))
    out = []
    try:
        while True:
            d, _ = s.recvfrom(65535)
            out.append(d)
            if not many:
                break
            s.settimeout(0.6)
    except socket.timeout:
        pass
    return out


def roster(game):
    """[(name, ping, is_bot)] from the server's own reply."""
    kind, port = GAMES[game]["query"]
    if kind in ("q3", "q2"):
        req = b"\xff\xff\xff\xffgetstatus" if kind == "q3" else b"\xff\xff\xff\xffstatus"
        r = _udp(port, req)
        if not r:
            return None
        lines = r[0].decode("latin1").split("\n")[2:]
        out = []
        for ln in lines:
            m = re.match(r'^(-?\d+) (-?\d+) "(.*)"', ln.strip())
            if m:
                ping = int(m.group(2))
                name = re.sub(r"\^.", "", m.group(3))
                # ping 0 marks a bot - unless it is one of ours, on a LAN 0 ms is real
                out.append((name, ping, ping == 0 and not name.startswith("Fleet")))
        return out
    if kind == "a2s":
        r = _udp(port, b"\xff\xff\xff\xffU\xff\xff\xff\xff")
        if not r or r[0][4:5] != b"A":
            return None
        r = _udp(port, b"\xff\xff\xff\xffU" + r[0][5:9])
        d = r[0]
        n, i, out = d[5], 6, []
        for _ in range(n):
            i += 1
            j = d.index(b"\0", i)
            name = d[i:j].decode("latin1")
            i = j + 1 + 4 + 4
            out.append((name, None, None))
        return out
    # GameSpy
    r = _udp(port, b"\\status\\", many=True)
    if not r:
        return None
    txt = "".join(x.decode("latin1") for x in r)
    kv = txt.split("\\")[1:]
    d = dict(zip(kv[0::2], kv[1::2]))
    out = []
    for k, v in d.items():
        m = re.match(r"player_(\d+)$", k)
        if m:
            idx = m.group(1)
            ping = d.get("ping_" + idx)
            bot = d.get("ngsecret_" + idx) == "bot" or (ping is not None and ping == "0" and game.startswith("ut"))
            out.append((v, int(ping) if ping and ping.lstrip("-").isdigit() else None, bot))
    out.append(("__info__", d.get("numplayers"), d.get("maxplayers")))
    return out


# ---------------------------------------------------------------- box side
async def connect(o):
    c = RetroConnection(f"192.168.1.{o}", 9898)
    await c.connect(SECRET, timeout=15)
    return c


async def kill_games(c):
    try:
        procs = json.loads(await c.command_text("PROCLIST"))
    except Exception:
        return []
    lst = procs if isinstance(procs, list) else procs.get("processes", [])
    killed = []
    for p in lst:
        n = (p.get("name") or "").lower()
        if n in [x.lower() for x in GAME_EXES]:
            try:
                await c.command_text(f"PROCKILL {p['pid']}")
                killed.append(n)
            except Exception:
                pass
    return killed


async def run_box(o, game):
    c = await connect(o)
    try:
        killed = await kill_games(c)
        if killed:
            await asyncio.sleep(4)
        await c.command_text("MKDIR C:\\fleetmp")
        bat = GAMES[game]["bat"].replace("{name}", f"Fleet{o}").encode("ascii")
        path = f"C:\\fleetmp\\{game}.bat"
        await c.send_command(f"UPLOAD {path}", binary_payload=bat)
        r = await c.command_text(f'LAUNCH "{path}"')
        return o, "launched", killed, r[:80]
    finally:
        await c.close()


async def shot_box(o, game, outdir):
    c = await connect(o)
    try:
        procs = json.loads(await c.command_text("PROCLIST"))
        lst = procs if isinstance(procs, list) else procs.get("processes", [])
        exes = GAMES[game]["exe"]
        exes = [exes] if isinstance(exes, str) else list(exes)
        running = any((p.get("name") or "").lower() in [e.lower() for e in exes] for p in lst)
        wl = json.loads(await c.command_text("WINLIST"))
        fg = (wl.get("foreground") or {}).get("title") if isinstance(wl, dict) else None
        bmp = await c.command_binary("SCREENSHOT 1")
    finally:
        await c.close()
    from PIL import Image
    img = Image.open(io.BytesIO(bmp)).convert("RGB")
    p = os.path.join(outdir, f"{game}_{o}.jpg")
    img.save(p, quality=70)
    ex = img.getextrema()
    black = all(hi < 8 for _, hi in ex)
    return o, running, fg, p, black


async def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("action", choices=["run", "check", "kill", "roster"])
    ap.add_argument("game", nargs="?")
    ap.add_argument("--boxes", default=",".join(map(str, DEFAULT_BOXES)))
    ap.add_argument("--wait", type=int, default=0)
    ap.add_argument("--out", default=os.path.join(REPO, ".claude", "evidence-lanparty-2026-10-13"))
    a = ap.parse_args()
    boxes = [int(x) for x in a.boxes.split(",") if x]
    os.makedirs(a.out, exist_ok=True)
    if a.action == "kill":
        async def k(o):
            c = await connect(o)
            try:
                return o, await kill_games(c)
            finally:
                await c.close()
        for r in await asyncio.gather(*[k(o) for o in boxes], return_exceptions=True):
            print(r)
        return
    if a.action == "roster":
        print(json.dumps(roster(a.game), indent=1))
        return
    if a.action == "run":
        for r in await asyncio.gather(*[run_box(o, a.game) for o in boxes], return_exceptions=True):
            print(r)
        if a.wait:
            time.sleep(a.wait)
        else:
            return
    ro = roster(a.game)
    humans = [x for x in (ro or []) if x[0] != "__info__" and not x[2]]
    info = [x for x in (ro or []) if x[0] == "__info__"]
    print(f"\n{GAMES[a.game]['title']}: server roster ({len(humans)} non-bot):")
    for x in ro or []:
        print("   ", x)
    shots = await asyncio.gather(*[shot_box(o, a.game, a.out) for o in boxes], return_exceptions=True)
    rep = {"game": a.game, "time": time.strftime("%Y-%m-%dT%H:%M:%S"), "roster": ro, "boxes": {}}
    for s in shots:
        if isinstance(s, Exception):
            print("   shot error:", s)
            continue
        o, running, fg, p, black = s
        named = any(n == f"Fleet{o}" for n, *_ in humans)
        rep["boxes"][o] = dict(running=running, foreground=fg, shot=os.path.basename(p), black=black, named_on_server=named)
        print(f"  .{o}: running={running} on_server={'yes' if named else '?'} black={black} fg={fg!r}")
    with open(os.path.join(a.out, f"{a.game}.json"), "w") as f:
        json.dump(rep, f, indent=1)


if __name__ == "__main__":
    asyncio.run(main())
