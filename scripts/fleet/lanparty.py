#!/usr/bin/env python3
"""Put the whole fleet into one multiplayer game, and prove each box is in it.

    python3 scripts/fleet/lanparty.py list                             # every title it knows
    python3 scripts/fleet/lanparty.py run   <game> [--boxes 110,123,...] [--wait 90]
    python3 scripts/fleet/lanparty.py check <game> [--boxes ...] [--sniff 8]
    python3 scripts/fleet/lanparty.py kill  [--boxes ...] [--game <game>]
    python3 scripts/fleet/lanparty.py roster <game>
    python3 scripts/fleet/lanparty.py sniff <game> [--seconds 8]      # packets per box -> server port

The seven built-in titles (ssam-tfe ssam-tse q3 q2 cs ut99 ut2004) are joined
by the 2026-10-13 party. Everything else comes from TITLES_FILE
(scripts/fleet/lanparty_titles.json): one entry per title with its launcher(s),
the server it joins (or the box that hosts it), its process names and how to
read the server's roster.

Each box gets a small launcher in C:\\fleetmp\\ that runs the staged title's own
FLEETRES.BAT (so the resolution is the box's own) and joins the server on
192.168.1.132 - or a fleet box that hosts the game - under the name
Fleet<octet> where the engine takes a name.

EVIDENCE, in order of strength. A process that is running is not a player.
  1. the SERVER's own player list, bots separated (where the engine lists names);
  2. PACKETS: every box's UDP traffic to the server's game port on this host,
     counted for a few seconds with tcpdump (works for every engine, including
     the ones whose reply lists no names - Tribes 2, Descent 3, Carmageddon 2 -
     and the GoldSrc proxy that shows every client as 127.0.0.1);
  3. a frame per box (XP GDI photographs most fullscreen games; Windows 7 gives
     black; the Voodoo 5 6000 on vcr-kmd needs vcrctl fbshot; the Win98 box's
     Voodoo 2 output is a pass-through GDI never sees).
"""
import argparse, asyncio, collections, io, json, os, re, socket, subprocess, sys, time

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, REPO)
from client.retro_protocol import RetroConnection  # noqa: E402

HOST = "192.168.1.132"
HOST_IFACE = os.environ.get("LANPARTY_IFACE", "enp129s0")
SECRET = "retro-agent-secret"
# the boxes switched on for the 2026-11-11 readiness rounds; a title's own "boxes" wins
DEFAULT_BOXES = [110, 123, 124, 133, 143, 171, 184, 186, 240, 243]
WIN9X_BOXES = {243}        # COMMAND.COM, single-threaded agent, PROCLIST gives full paths
LIB = "/mnt/retro-share/Files/Games-Library"
RCON_PW = "retroadmin"     # fleet convention on the isolated LAN (CLAUDE.md "deliberately NOT secret")
GRACE_S = 12               # WM_CLOSE first; force only what is still running after this
TITLES_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "lanparty_titles.json")
SUDO_FILE = os.path.expanduser("~/.fleet-sudo")
VCRCTL = "C:\\vcr\\vcrctl.exe"
VCR_BOXES = {124}          # vcr-kmd: a GDI frame of a fullscreen game is the old desktop

G = "C:\\Games\\"
GAMES = {
    "ssam-tfe": dict(title="Serious Sam TFE", query=("gamespy", 25601), port=25600, exe="SeriousSam.exe",
                     bat='@echo off\r\ncall "' + G + 'SeriousSamFirstEncounter\\Join Serious Sam TFE - LAN.bat" ' + HOST + ':25600\r\n'),
    "ssam-tse": dict(title="Serious Sam TSE", query=("gamespy", 25611), port=25610, exe="SeriousSam.exe",
                     bat='@echo off\r\ncall "' + G + 'SeriousSamSecondEncounter\\Join Serious Sam TSE - LAN.bat" ' + HOST + ':25610\r\n'),
    "q3": dict(title="Quake III Arena", query=("q3", 27961), port=27961, exe=("quake3.exe", "ioquake3.x86.exe"),
               bat='@echo off\r\ncd /d "' + G + 'Quake3-TeamArena"\r\ncall FLEETRES.BAT\r\n'
                   'call FLEETGL.BAT "' + G + 'Quake3-TeamArena\\baseq3\\fleetres.cfg"\r\n'
                   'cd /d "' + G + 'Quake3-TeamArena"\r\n'
                   'start "" %FR_ENGINE% +set r_mode -1 +set r_customwidth %FR_W% +set r_customheight %FR_H% '
                   '+set r_customaspect 1 +set r_customPixelAspect 1 +set r_fullscreen 1 +set cg_fov %FR_FOV% '
                   '+set name {name} +connect ' + HOST + ':27961\r\n'),
    "q2": dict(title="Quake II", query=("q2", 27910), port=27910, exe="quake2.exe",
               bat='@echo off\r\ncd /d "' + G + 'Quake2Complete"\r\ncall FLEETRES.BAT\r\ncd /d "' + G + 'Quake2Complete"\r\n'
                   'start "" quake2.exe +set gl_mode %FR_Q2WIDE% +set vid_fullscreen 1 +set name {name} +connect ' + HOST + ':27910\r\n'),
    "cs": dict(title="Counter-Strike 1.6", query=("a2s", 27018), port=27015, exe="hl.exe",
               bat='@echo off\r\ncd /d "' + G + 'CounterStrike16"\r\ncall FLEETRES.BAT\r\ncd /d "' + G + 'CounterStrike16"\r\n'
                   'set MESA_FORCE_SSE=1\r\n'
                   'start "" hl.exe -game cstrike -full -gl -w %FR_W% -h %FR_H% +name {name} +connect ' + HOST + ':27015\r\n'),
    "ut99": dict(title="Unreal Tournament", query=("gamespy", 7798), port=7797, exe="UnrealTournament.exe",
                 bat='@echo off\r\ncd /d "' + G + 'UnrealTournament436"\r\ncall FLEETRES.BAT\r\n'
                     'cd /d "' + G + 'UnrealTournament436\\System"\r\nif exist Running.ini del /q Running.ini\r\n'
                     'start "" UnrealTournament.exe ' + HOST + ':7797?Name={name}\r\n'),
    "ut2004": dict(title="UT2004", query=("gamespy", 7787), port=7777, exe="UT2004.exe",
                   bat='@echo off\r\ncall "' + G + 'UT2004\\Join fleet UT2004 server.bat"\r\n'),
}


def load_titles(path=TITLES_FILE):
    """Merge the JSON title file over the built-ins. A malformed file is an
    error, never a silent fallback to the seven built-ins - a party run that
    quietly lost half its titles would report the missing ones as untested
    rather than as broken tooling."""
    games = {k: dict(v) for k, v in GAMES.items()}
    if not os.path.exists(path):
        return games
    with open(path) as f:
        data = json.load(f)
    for key, t in data.items():
        if key.startswith("_"):
            continue
        games[key] = t
    return games


def exes_of(t):
    e = t.get("exe") or []
    e = [e] if isinstance(e, str) else list(e)
    host = t.get("host") or {}
    h = host.get("exe") or []
    h = [h] if isinstance(h, str) else list(h)
    return e, h


def all_game_exes(games):
    out = set()
    for t in games.values():
        e, h = exes_of(t)
        out.update(x.lower() for x in e + h)
    return sorted(out)


def bat_for(t, octet):
    """The launcher text for one box: a per-box variant if the title has one,
    else its '*' entry, else the single `bat`. None = the title has nothing
    for this box (do not launch there)."""
    o = str(octet)
    if "bats" in t:
        star = None
        for b in t["bats"]:
            boxes = [str(x).split(".")[-1] for x in b.get("boxes", [])]
            if o in boxes:
                return b["bat"]
            if "*" in boxes:
                star = b["bat"]
        return star
    if int(o) in WIN9X_BOXES:
        return None    # a single-bat title is cmd.exe dialect: never hand it to COMMAND.COM
    return t.get("bat")


def bat_path(key, octet):
    """Where a box's launcher goes. Win9x gets an 8.3 name in C:\\FLEETMP: the
    route proven on .243 is an 8.3 batch started by START.EXE, and a quoted long
    name through command.com /c was never exercised there."""
    if int(octet) in WIN9X_BOXES:
        short = re.sub(r"[^A-Za-z0-9]", "", key).upper()[:8] or "FLEET"
        return "C:\\FLEETMP\\" + short + ".BAT"
    return f"C:\\fleetmp\\{key}.bat"


def launch_cmd(octet, path):
    """START.EXE is an existing .exe first token, so agent 1.90.1+ starts it
    directly in its own console and it runs the batch in its own DOS box."""
    if int(octet) in WIN9X_BOXES:
        return f"LAUNCH C:\\WINDOWS\\COMMAND\\START.EXE {path}"
    return f'LAUNCH "{path}"'


def base(name):
    """PROCLIST basename, lower case: Win9x reports full paths."""
    return (name or "").replace("/", "\\").split("\\")[-1].lower()


def render_bat(text, octet, host_ip):
    """{name} -> Fleet<octet>, {host} -> the server or hosting box, CRLF line ends."""
    s = text.replace("{name}", f"Fleet{octet}").replace("{host}", host_ip)
    s = s.replace("\r\n", "\n").replace("\n", "\r\n")
    if not s.endswith("\r\n"):
        s += "\r\n"
    return s.encode("ascii")


def host_ip_of(t):
    h = t.get("host")
    if h and h.get("box"):
        return f"192.168.1.{str(h['box']).split('.')[-1]}"
    return HOST


# ---------------------------------------------------------------- server side
def _udp(port, payload, timeout=3.0, many=False, host=HOST):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(timeout)
    s.sendto(payload, (host, port))
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
    finally:
        s.close()
    return out


def rcon(kind, port, cmd, host=HOST, password=RCON_PW):
    """One rcon command, its reply text (None = no answer). 'q' is the Quake
    family (Q1 DarkPlaces, Q2, Q3, RTCW: one packet, no challenge); 'goldsrc'
    (HLDS, WON Half-Life) asks for a challenge first."""
    if kind == "goldsrc":
        r = _udp(port, b"\xff\xff\xff\xffchallenge rcon\n", host=host)
        if not r:
            return None
        parts = r[0][4:].decode("latin1").split()
        if len(parts) < 3:
            return None
        pkt = f'rcon {parts[2]} "{password}" {cmd}\n'.encode("latin1")
    else:
        pkt = f"rcon {password} {cmd}\n".encode("latin1")
    r = _udp(port, b"\xff\xff\xff\xff" + pkt, many=True, host=host)
    if not r:
        return None
    txt = []
    for d in r:
        d = d[4:] if d.startswith(b"\xff\xff\xff\xff") else d
        d = d[1:] if d[:1] in (b"l", b"n") else d          # GoldSrc 'l' / Quake 'print\n'
        t = d.decode("latin1")
        txt.append(t[6:] if t.startswith("print\n") else t)
    return "".join(txt)


GOLDSRC_RE = re.compile(r'^#\s*\d+\s+"(.*)"\s+\d+\s+(\S+)\s+-?\d+\s+\S+\s+(\d+)\s+\d+(?:\s+(\S+))?')


def parse_goldsrc_status(text):
    """[(name, ping, is_bot, ip)] from HLDS/WON 'status'. Bots carry uniqueid BOT
    and no address; the listen-server host's own line ends 'loopback'."""
    out = []
    for ln in (text or "").splitlines():
        m = GOLDSRC_RE.match(ln.strip())
        if not m:
            continue
        name, uid, ping, adr = m.group(1), m.group(2), int(m.group(3)), m.group(4) or ""
        ip = adr.split(":")[0] if re.match(r"\d+\.\d+\.\d+\.\d+", adr) else None
        out.append((name, ping, uid.upper() == "BOT", ip))
    return out


def parse_t2_info(d):
    """Tribes 2 GameInfo reply (0x14, sent for request 0x12 flags 0x02): the
    plain content block after the header. [(name, None, is_bot, None)] plus an
    __info__ row with (players, max). TorqueScript colour bytes mark a bot
    (10 0E) - strip them for the name. Best effort: None when it does not parse."""
    try:
        if not d or d[0] != 0x14:
            return None
        i = 6
        for _ in range(3):                      # mod, game type, map
            n = d[i]
            i += 1 + n
        status, players, mx, bots = d[i], d[i + 1], d[i + 2], d[i + 3]
        i += 6                                  # + u16 cpu
        n = d[i]
        i += 1 + n                              # info string
        ln = d[i] | (d[i + 1] << 8)
        body = d[i + 2:i + 2 + ln].decode("latin1").split("\n")
        nteams = int(body[0])
        k = 1 + nteams
        nclients = int(body[k]) if len(body) > k else 0
        out = []
        for row in body[k + 1:k + 1 + nclients]:
            raw = row.split("\t")[0]
            bot = "\x10\x0e" in raw
            name = re.sub(r"[\x01-\x07\x0b\x0c\x0e-\x11]", "", raw).replace("\x10", "")
            out.append((name, None, bot, None))
        out.append(("__info__", players - bots, mx))
        return out
    except Exception:
        return None


def roster(game, games=None):
    """[(name, ping, is_bot, ip)] from the server's own reply - ip only where
    the engine reports it (GoldSrc rcon status) - plus an ("__info__", n, max)
    row where the reply carries counts. None when the server lists nothing
    readable (use `sniff` for those)."""
    games = games or load_titles()
    t = games[game]
    q = t.get("query")
    if not q:
        return None
    kind, port = q[0], int(q[1])
    qhost = host_ip_of(t) if t.get("host") or t.get("query_host_box") else HOST
    if t.get("query_host_box"):
        qhost = f"192.168.1.{str(t['query_host_box']).split('.')[-1]}"
    ignore = set(t.get("roster_ignore") or [])
    if kind in ("q3", "q2"):
        req = b"\xff\xff\xff\xffgetstatus" if kind == "q3" else b"\xff\xff\xff\xffstatus"
        r = _udp(port, req, host=qhost)
        if not r:
            return None
        lines = r[0].decode("latin1").split("\n")[2:]
        out = []
        for ln in lines:
            m = re.match(r'^(-?\d+) (-?\d+)(?: -?\d+)? "(.*)"', ln.strip())
            if m:
                ping = int(m.group(2))
                name = re.sub(r"\^.", "", m.group(3))
                if name in ignore:
                    continue
                # ping 0 marks a bot - unless it is one of ours, on a LAN 0 ms is real
                out.append((name, ping, ping == 0 and not name.startswith("Fleet"), None))
        return out
    if kind == "goldsrc":
        txt = rcon("goldsrc", port, "status", host=qhost, password=t.get("rcon_password", RCON_PW))
        if txt is None:
            return None
        return [x for x in parse_goldsrc_status(txt) if x[0] not in ignore]
    if kind == "t2":
        r = _udp(port, b"\x12\x02" + os.urandom(4), host=qhost)
        return parse_t2_info(r[0]) if r else None
    if kind == "a2s":
        r = _udp(port, b"\xff\xff\xff\xffU\xff\xff\xff\xff", host=qhost)
        if not r or r[0][4:5] != b"A":
            return None
        r = _udp(port, b"\xff\xff\xff\xffU" + r[0][5:9], host=qhost)
        if not r:
            return None
        d = r[0]
        n, i, out = d[5], 6, []
        for _ in range(n):
            i += 1
            j = d.index(b"\0", i)
            name = d[i:j].decode("latin1")
            i = j + 1 + 4 + 4
            out.append((name, None, None, None))
        return out
    if kind == "nq":
        sys.path.insert(0, os.path.join(REPO, "scripts", "game-servers"))
        import gameservers  # noqa: E402
        r = gameservers.probe_nq(port, host=qhost)
        if not r:
            return None
        return [("__info__", r.get("players"), r.get("max_players"))]
    if kind == "ut2003":
        sys.path.insert(0, os.path.join(REPO, "scripts", "game-servers"))
        import gameservers  # noqa: E402
        raw, _ = gameservers._ue2_ask(port, 2, 2.0, qhost)
        names = []
        for d in raw:
            names.extend(gameservers.parse_ue2_players(d))
        info = gameservers.probe_ut2003(port, host=qhost) or {}
        return [(n, None, False, None) for n in names] + [("__info__", info.get("players"), info.get("max_players"))]
    # GameSpy \status\ (UT99, UT2004, Serious Sam, Unreal Gold, Deus Ex, Shogo, Halo, Descent 3, Turok 2)
    r = _udp(port, b"\\status\\", many=True, host=qhost)
    if not r:
        return None
    txt = "".join(x.decode("latin1").rstrip("\0") for x in r)
    kv = txt.split("\\")[1:]
    d = dict(zip(kv[0::2], kv[1::2]))
    out = []
    for k, v in d.items():
        m = re.match(r"player_(\d+)$", k)
        if m and v not in ignore:
            idx = m.group(1)
            ping = d.get("ping_" + idx)
            bot = d.get("ngsecret_" + idx) == "bot" or (ping is not None and ping == "0" and game.startswith("ut"))
            out.append((v, int(ping) if ping and ping.lstrip("-").isdigit() else None, bot, None))
    out.append(("__info__", d.get("numplayers"), d.get("maxplayers")))
    return out


def run_prelaunch(t):
    """Host-side steps before any box starts: rcon a level change (a server
    parked at intermission spawns every joiner frozen), or a pause."""
    out = []
    for step in t.get("prelaunch") or []:
        if "sleep" in step:
            time.sleep(float(step["sleep"]))
            out.append(f"slept {step['sleep']}s")
        elif "rcon" in step:
            kind = step.get("kind", "q")
            r = rcon("goldsrc" if kind == "goldsrc" else "q", int(step["port"]), step["rcon"],
                     host=step.get("host", HOST), password=step.get("password", RCON_PW))
            out.append(f"rcon {step['rcon']!r} -> {(r or 'NO REPLY').strip()[:120]!r}")
    return out


def gen_file(kind, octet):
    """Per-box file bytes a title stages next to its launcher."""
    if kind == "d3pilot":
        # the staged sdf.plt with its callsign replaced (recipe Descent3: 'sdf\0' at 4..8)
        with open(os.path.join(LIB, "Descent3", "sdf.plt"), "rb") as f:
            lib = f.read()
        if lib[4:8] != b"sdf\x00":
            raise ValueError("Descent3/sdf.plt no longer has 'sdf' at offset 4 - refusing to build a pilot")
        return lib[:4] + f"Fleet{octet}".encode("ascii") + b"\x00" + lib[8:]
    raise ValueError(f"unknown generated file kind {kind!r}")


def files_for(t, octet):
    out = []
    for f in t.get("files") or []:
        boxes = [str(x).split(".")[-1] for x in f.get("boxes", ["*"])]
        if "*" in boxes or str(octet) in boxes:
            out.append((f["path"].replace("{name}", f"Fleet{octet}"), gen_file(f["gen"], octet)))
    return out


PKT_RE = re.compile(r"IP (192\.168\.1\.\d+)\.\d+ > (192\.168\.1\.\d+)\.(\d+):")


def count_packets(lines, dst_ip, port):
    """{source ip: packets} for tcpdump -n -q lines aimed at dst_ip:port."""
    c = collections.Counter()
    for ln in lines:
        m = PKT_RE.search(ln)
        if m and m.group(2) == dst_ip and int(m.group(3)) == int(port):
            c[m.group(1)] += 1
    return dict(c)


def sniff(port, seconds=8, dst=HOST, iface=HOST_IFACE):
    """Packets each LAN box sends to dst:port over `seconds`, from tcpdump on
    this host (root through sudo -S with the password file on STDIN - never on
    the command line). Returns None when tcpdump could not run, which is not
    the same as {} (ran, and nobody was talking)."""
    if not os.path.exists(SUDO_FILE):
        return None
    flt = f"dst host {dst} and dst port {port} and src net 192.168.1.0/24"
    cmd = ["sudo", "-S", "-p", "", "timeout", str(int(seconds)), "tcpdump", "-i", iface, "-n", "-q", "-l", flt]
    try:
        with open(SUDO_FILE) as pw:
            p = subprocess.run(cmd, stdin=pw, capture_output=True, text=True, timeout=seconds + 15)
    except Exception:
        return None
    if "permission" in p.stderr.lower() or ("listening on" not in p.stderr and not p.stdout):
        return None
    return count_packets(p.stdout.splitlines(), dst, port)


# ---------------------------------------------------------------- box side
async def connect(o, timeout=15):
    c = RetroConnection(f"192.168.1.{o}", 9898)
    await c.connect(SECRET, timeout=20 if int(o) in WIN9X_BOXES else timeout)
    return c


async def proclist(c):
    procs = json.loads(await c.command_text("PROCLIST"))
    return procs if isinstance(procs, list) else procs.get("processes", [])


async def kill_games(c, octet, names, quit9x=None, grace=GRACE_S):
    """Close every running process named in `names` and return what was closed.

    NT: taskkill WITHOUT /f first (WM_CLOSE), force only what still runs after
    `grace` s - a TerminateProcess'd Glide/GL process on .124's vcr-kmd leaves
    a dead board mapping for the next title (c0000005 in grGlideInit).
    Win9x: the title's console-quit keys if the game has the focus, then
    PROCKILL, then GLRESET9 to take the monitor back from a Voodoo 2 left in
    3D pass-through. Names are compared on the basename (9x PROCLIST = paths)."""
    names = {x.lower() for x in names}

    def live(lst):
        return [p for p in lst if base(p.get("name")) in names]
    try:
        run = live(await proclist(c))
    except Exception:
        return []
    if not run:
        return []
    o = int(octet)
    if o in WIN9X_BOXES:
        if quit9x:
            fg = None
            try:
                wl = json.loads(await c.command_text("WINLIST"))
                fg = (wl.get("foreground") or {}).get("pid") if isinstance(wl, dict) else None
            except Exception:
                pass
            if fg in {p.get("pid") for p in run}:
                for k in quit9x:
                    try:
                        await c.command_text(f"UIKEY {k}")
                    except Exception:
                        pass
                    await asyncio.sleep(0.7)
                for _ in range(max(1, grace // 2)):
                    await asyncio.sleep(2)
                    run = live(await proclist(c))
                    if not run:
                        return ["console quit"]
        killed = []
        for p in run:
            try:
                await c.command_text(f"PROCKILL {p['pid']}")
                killed.append(base(p.get("name")) + " (killed)")
            except Exception:
                pass
        try:
            await c.command_text("LAUNCH C:\\RETRO_AGENT\\GLRESET9.EXE")
        except Exception:
            pass
        return killed
    closed = sorted({base(p.get("name")) for p in run})
    for n in closed:
        try:
            await c.command_text(f'EXEC cmd /c taskkill /im "{n}"')
        except Exception:
            pass
    for _ in range(max(1, grace // 2)):
        await asyncio.sleep(2)
        run = live(await proclist(c))
        if not run:
            return [n + " (closed)" for n in closed]
    forced = []
    for p in run:
        try:
            await c.command_text(f"PROCKILL {p['pid']}")
            forced.append(base(p.get("name")))
        except Exception:
            pass
    return [n + (" (forced)" if n in forced else " (closed)") for n in closed]


async def launch_bat(o, key, text, host_ip, kill_first, files=(), quit9x=None):
    c = await connect(o)
    try:
        killed = await kill_games(c, o, kill_first, quit9x=quit9x) if kill_first else []
        if killed:
            # .124 drives a 1998 CRT: never two mode switches closer than 10 s
            await asyncio.sleep(12 if int(o) == 124 else 4)
        path = bat_path(key, o)
        await c.command_text("MKDIR " + path.rsplit("\\", 1)[0])
        for fpath, data in files:
            await c.send_command(f"UPLOAD {fpath}", binary_payload=data)
        await c.send_command(f"UPLOAD {path}", binary_payload=render_bat(text, o, host_ip))
        r = await c.command_text(launch_cmd(o, path))
        return o, "launched", killed, r[:80]
    finally:
        await c.close()


async def shot_box(o, game, t, outdir):
    exes, hexes = exes_of(t)
    want = {e.lower() for e in exes + hexes}
    c = await connect(o)
    vcr = None
    try:
        lst = await proclist(c)
        running = sorted({base(p.get("name")) for p in lst if base(p.get("name")) in want})
        fg = None
        try:
            wl = json.loads(await c.command_text("WINLIST"))
            fg = (wl.get("foreground") or {}).get("title") if isinstance(wl, dict) else None
        except Exception:
            pass
        bmp = None
        if o in VCR_BOXES and not t.get("d3d124"):
            # the scanned-out frame; GDI would photograph the old desktop memory.
            # NEVER under Direct3D: fbshot's escapes park in the kernel behind
            # win32k's display lock while a D3D game holds it (vcr-kmd README).
            try:
                await c.command_text(f"EXECW 30 {VCRCTL} fbshot C:\\fleetmp\\fb.bmp")
                bmp = await c.command_binary("DOWNLOAD C:\\fleetmp\\fb.bmp")
                vcr = "fbshot"
            except Exception as e:
                vcr = f"fbshot failed: {e}"
        if bmp is None:
            bmp = await c.command_binary("SCREENSHOT 1")
    finally:
        await c.close()
    from PIL import Image
    img = Image.open(io.BytesIO(bmp)).convert("RGB")
    if img.width > 1280:
        img = img.resize((img.width // 2, img.height // 2))
    p = os.path.join(outdir, f"{game}_{o}.jpg")
    img.save(p, quality=70)
    ex = img.getextrema()
    black = all(hi < 8 for _, hi in ex)
    return o, running, fg, p, black, vcr


def applicable(t, boxes):
    """The boxes this title is launched on: --boxes filtered to those the title
    has a launcher for (a box with no launcher entry is not applicable)."""
    return [o for o in boxes if bat_for(t, o) is not None]


def host_box_of(t):
    ip = host_ip_of(t)
    return int(ip.split(".")[-1]) if ip != HOST else None


def on_server(o, roster_rows):
    """A box is on the server when the server names it Fleet<octet> or reports its address."""
    for row in roster_rows or []:
        if row[0] == "__info__" or (len(row) > 2 and row[2]):
            continue
        if row[0] == f"Fleet{o}" or (len(row) > 3 and row[3] == f"192.168.1.{o}"):
            return True
    return False


async def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("action", choices=["run", "check", "kill", "roster", "sniff", "list", "rcon"])
    ap.add_argument("game", nargs="?")
    ap.add_argument("--boxes", default=None)
    ap.add_argument("--wait", type=int, default=0)
    ap.add_argument("--host-wait", type=int, default=None)
    ap.add_argument("--sniff", type=int, default=8, help="seconds of packet capture in check (0 = off)")
    ap.add_argument("--seconds", type=int, default=8)
    ap.add_argument("--no-host", action="store_true", help="peer titles: do not (re)start the hosting box")
    ap.add_argument("--no-prelaunch", action="store_true", help="skip the title's host-side prelaunch steps")
    ap.add_argument("--cmd", default=None, help="rcon: the command to send to the title's server")
    ap.add_argument("--out", default=os.path.join(REPO, ".claude", "evidence-lanparty-2026-10-13"))
    a = ap.parse_args()
    games = load_titles()
    if a.action == "list":
        for k, t in sorted(games.items()):
            print(f"{k:26} {t.get('title', '')}  mode={t.get('mode', 'host_server')} port={t.get('port')} "
                  f"query={t.get('query')} host={host_ip_of(t)} boxes={t.get('boxes') or '-'}")
        return
    if a.action != "kill" and a.game not in games:
        sys.exit(f"unknown game {a.game!r}; one of: {' '.join(sorted(games))}")
    t = games.get(a.game) if a.game else None
    if a.boxes:
        boxes = [int(x.split(".")[-1]) for x in a.boxes.split(",") if x]
    elif t and t.get("boxes"):
        boxes = [int(str(x).split(".")[-1]) for x in t["boxes"]]
    else:
        boxes = DEFAULT_BOXES
    os.makedirs(a.out, exist_ok=True)

    if a.action == "kill":
        names = all_game_exes(games) if not a.game else [x.lower() for x in sum(exes_of(games[a.game]), [])]
        quit9x = (t or {}).get("quit9x")

        async def k(o):
            c = await connect(o)
            try:
                return o, await kill_games(c, o, names, quit9x=quit9x)
            finally:
                await c.close()
        for r in await asyncio.gather(*[k(o) for o in boxes], return_exceptions=True):
            print(r)
        return
    if a.action == "roster":
        print(json.dumps(roster(a.game, games), indent=1))
        return
    if a.action == "rcon":
        q = t.get("query") or []
        kind = "goldsrc" if q and q[0] == "goldsrc" else "q"
        port = int(t.get("rcon_port") or (q[1] if q else t.get("port")))
        print(rcon(kind, port, a.cmd or "status", host=host_ip_of(t) if t.get("host") else HOST,
                   password=t.get("rcon_password", RCON_PW)))
        return
    if a.action == "sniff":
        port = t.get("port")
        if not port:
            sys.exit(f"{a.game} has no server port on this host to sniff")
        print(json.dumps(sniff(port, a.seconds), indent=1))
        return

    host_ip = host_ip_of(t)
    host_box = host_box_of(t)
    if a.action == "run":
        kill_first = all_game_exes(games)
        if not a.no_prelaunch:
            for line in run_prelaunch(t):
                print("prelaunch:", line)
        if host_box and not a.no_host:
            h = t["host"]
            print(f"host .{host_box}:", await launch_bat(host_box, a.game + "-host", h["bat"], host_ip, kill_first,
                                                         quit9x=t.get("quit9x")))
            time.sleep(a.host_wait if a.host_wait is not None else int(h.get("wait", 30)))
        targets = applicable(t, boxes)
        skipped = [o for o in boxes if o not in targets and o != host_box]
        if skipped:
            print("no launcher for:", skipped)
        # the host box keeps its server running: never kill it while starting its own client
        res = await asyncio.gather(*[launch_bat(o, a.game, bat_for(t, o), host_ip,
                                                [] if o == host_box else kill_first,
                                                files=files_for(t, o), quit9x=t.get("quit9x"))
                                     for o in targets], return_exceptions=True)
        for r in res:
            print(r)
        if a.wait:
            time.sleep(a.wait)
        else:
            return
    targets = applicable(t, boxes)
    if host_box and host_box in boxes and host_box not in targets:
        targets = [host_box] + targets
    ro = roster(a.game, games)
    humans = [x for x in (ro or []) if x[0] != "__info__" and not x[2]]
    print(f"\n{t.get('title', a.game)}: server roster ({len(humans)} non-bot):")
    for x in ro or []:
        print("   ", x)
    pk = None
    if a.sniff and t.get("port") and host_ip == HOST:
        pk = sniff(t["port"], a.sniff)
        print(f"  packets to {HOST}:{t['port']} in {a.sniff}s: {pk}")
    shots = await asyncio.gather(*[shot_box(o, a.game, t, a.out) for o in targets], return_exceptions=True)
    rep = {"game": a.game, "time": time.strftime("%Y-%m-%dT%H:%M:%S"), "roster": ro,
           "packets": pk, "port": t.get("port"), "host": host_ip, "boxes": {}}
    for o, s in zip(targets, shots):
        if isinstance(s, Exception):
            print(f"  .{o}: shot error: {s!r}")
            rep["boxes"][o] = dict(error=repr(s))
            continue
        o, running, fg, p, black, vcr = s
        named = on_server(o, ro)
        pkts = (pk or {}).get(f"192.168.1.{o}", 0) if pk is not None else None
        rep["boxes"][o] = dict(running=running, foreground=fg, shot=os.path.basename(p), black=black,
                               on_server=named, packets=pkts, frame=vcr or "gdi",
                               host=(o == host_box))
        print(f"  .{o}{' (host)' if o == host_box else ''}: running={running or 'NO'} "
              f"on_server={'yes' if named else '?'} packets={pkts} black={black} fg={fg!r}{' ' + vcr if vcr else ''}")
    with open(os.path.join(a.out, f"{a.game}.json"), "w") as f:
        json.dump(rep, f, indent=1)


if __name__ == "__main__":
    asyncio.run(main())
