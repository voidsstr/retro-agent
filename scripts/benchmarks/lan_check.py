#!/usr/bin/env python3
"""
lan_check.py - is each staged title STABLE on this box's driver stack, run the
way its desktop shortcut runs it?

v56k_bench.py answers a different question (how fast is a named driver lane
at a named resolution) and deliberately builds its own command lines - its
Quake III route is retail quake3.exe on AmigaMerlin's ICD, which is not what
the "Quake III Arena" shortcut on .124 starts (ioquake3 -> opengl32 -> the
system ICD). Before a LAN party what matters is the shortcut's path, so every
check here:

  * asks the box's own FLEETRES.BAT for the values the launcher will use
    (FR_W/FR_H/FR_HZ/FR_Q2MODE/FR_Q3MODE/...), and writes the same per-run
    files the launcher writes (fleetres.cfg);
  * starts the same exe with the launcher's own arguments, adding only an
    engine script: a timedemo (the render loop completes, and a number), an
    engine screenshot (the engine composes it from the frame it drew - an
    agent GDI capture of a Glide surface is not evidence), and a multiplayer
    soak on the fleet server (bots, so there is something to draw);
  * ends the game through its own `quit`, never TerminateProcess: a killed
    Glide process never unmaps its board mapping (v56k_bench.graceful_kill);
  * reports the post-condition: exit on time, the log's renderer line and
    fps, errors in the log, a new Dr. Watson entry, an error window, board
    health (glideprobe --noopen) and agent liveness afterwards.

Evidence (logs, shots as PNG, a JSON per run) goes under --outdir in the repo
tree, never the scratchpad.

    python3 scripts/benchmarks/lan_check.py --host 192.168.1.124 --titles q3 \
        --server 192.168.1.196 --soak 300 \
        --outdir scripts/benchmarks/results/v56k_lan_192.168.1.124
"""
import argparse
import asyncio
import importlib.util
import io
import json
import re
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent


def _load_bench():
    spec = importlib.util.spec_from_file_location("v56k_bench", HERE / "v56k_bench.py")
    mod = importlib.util.module_from_spec(spec)
    sys.modules["v56k_bench"] = mod
    spec.loader.exec_module(mod)
    return mod


bench = _load_bench()


def _load_gameservers():
    spec = importlib.util.spec_from_file_location(
        "gameservers", HERE.parent / "game-servers" / "gameservers.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def server_humans(t, server):
    """The fleet server's own count of HUMAN players (ping 0 = a bot), or None.
    Evidence a client got into the game that does not depend on the client's
    log saying so - Jedi Academy's never does."""
    kind = getattr(t, "server_probe", None)
    if not kind or not server:
        return None
    try:
        r = getattr(_load_gameservers(), f"probe_{kind}")(t.port, timeout=2.0, host=server)
    except Exception:
        return None
    return None if not r else max(0, r.get("players", 0) - r.get("bots", 0))
Box = bench.Box
DRWTSN = r"C:\Documents and Settings\All Users\Application Data\Microsoft\Dr Watson\drwtsn32.log"


def log(m):
    print(time.strftime("[%H:%M:%S] ") + m, flush=True)


# --------------------------------------------------------------------------- #
# box helpers
# --------------------------------------------------------------------------- #

async def fleetres_env(box, root, extra=""):
    """The values the title's own launcher computes: run its FLEETRES.BAT on
    the box and read FR_* back. Through a .bat of its own: a one-line
    `cmd /c cd ... && call FLEETRES.BAT` under EXEC never finds the file."""
    bat = r"C:\RETRO_AGENT\lanfr.bat"
    await box.upload(bat, "\r\n".join(["@echo off", f'cd /d "{root}"',
                                         f'call "{root}\\FLEETRES.BAT" {extra} >nul 2>&1', "set FR_", ""]))
    out = await box.exec_(bat, timeout=90)
    env = {}
    for line in out.splitlines():
        m = re.match(r"(FR_\w+)=(.*)", line.strip())
        if m:
            env[m.group(1)] = m.group(2).strip()
    return env


async def appdata(box):
    out = await box.exec_("cmd /c echo %APPDATA%", timeout=30)
    return out.strip().splitlines()[-1].strip()


async def running(box, image):
    out = await box.exec_(f'cmd /c tasklist /fi "imagename eq {image}" /nh', timeout=30)
    return image.lower() in out.lower()


async def drwatson_settle(box, limit=60):
    """Wait for Dr. Watson to finish with a crashed process. While drwtsn32 /
    dwwin hold it, a crashed game can be missing from tasklist and then come
    back: on 2026-09-28 Deathmatch Classic's hl.exe crashed, read as "not
    running", and the next title was launched over it - WON Half-Life is
    single-instance, so that run watched the crashed process for 5 minutes."""
    t0 = time.time()
    while time.time() - t0 < limit:
        out = (await box.exec_('cmd /c tasklist /nh', timeout=30)).lower()
        if not re.search(r"(?m)^(drwtsn32|dwwin|dumprep)\.exe\s", out):
            return round(time.time() - t0)
        await asyncio.sleep(3)
    return None


async def games_running(box):
    """Every game image this harness (or lan_sweep's titles) is known to start
    that is running now. A run must not start on top of one: on 2026-09-28 a
    GLQuake left sitting at its own "Confirm Exit" box had SoF launched over it."""
    out = (await box.exec_('cmd /c tasklist /nh', timeout=30)).lower()
    names = {t.exe.lower() for t in TITLES.values() if getattr(t, "exe", None)}
    for t in TITLES.values():
        names.update(i.lower() for i in getattr(t, "images", ()) or ())
    names.update(("sof.exe", "hl.exe", "quake2.exe", "glquake.exe", "wolfmp.exe", "unreal.exe",
                  "unrealtournament.exe", "ioquake3.x86.exe", "quake3.exe", "dosbox.exe"))
    return sorted(n for n in names if n and re.search(r"(?m)^" + re.escape(n) + r"\s", out))


async def dir_stamps(box, path, ext):
    """{name: (size, modified)} of the *.ext files in `path`, from the agent's
    DIRLIST. A NAME is not enough to tell a new screenshot: RtCW numbers its
    shots from 0000 again each session and overwrites, so the 2026-09-28 map
    cycle wrote nine fresh shots and a name diff found none."""
    try:
        st, out = await box.cmd(f"DIRLIST {path}", timeout=60)
        rows = json.loads(out)
    except Exception:
        return {}
    return {r["name"]: (r.get("size"), r.get("modified")) for r in rows
            if isinstance(r, dict) and not r.get("is_dir") and r.get("name", "").lower().endswith("." + ext)}


async def dir_names(box, path, pattern="*"):
    out = await box.exec_(f'cmd /c dir /b /o:d "{path}\\{pattern}" 2>nul', timeout=30)
    return [l.strip() for l in out.splitlines() if l.strip() and "File Not Found" not in l]


async def file_size(box, path):
    out = await box.exec_(f'cmd /c for %I in ("{path}") do @echo %~zI', timeout=30)
    try:
        return int(out.strip().splitlines()[-1])
    except (ValueError, IndexError):
        return 0


async def error_windows(box):
    """Visible windows whose title says a program failed."""
    try:
        st, out = await box.cmd("WINLIST", timeout=30)
        wins = json.loads(out)
    except Exception:
        return []
    bad = []
    for w in wins if isinstance(wins, list) else wins.get("windows", []):
        t = (w.get("title") or "") if isinstance(w, dict) else str(w)
        if re.search(r"error|has encountered|not responding|fatal|exception|dr\.? watson", t, re.I):
            bad.append(t)
    return bad


def shot_stats(data):
    """(width, height, mean luma, stddev) of an image, or None."""
    try:
        from PIL import Image, ImageStat
        im = Image.open(io.BytesIO(data)).convert("L")
        st = ImageStat.Stat(im)
        return im.size[0], im.size[1], round(st.mean[0], 1), round(st.stddev[0], 1)
    except Exception:
        return None


def save_png(data, path):
    from PIL import Image
    Image.open(io.BytesIO(data)).convert("RGB").save(path, optimize=True)


# --------------------------------------------------------------------------- #
# titles
# --------------------------------------------------------------------------- #

class IdTech3:
    """ioquake3 / retail Quake III / RtCW MP: an exec'd script with `wait N`
    (frames) and screenshotJPEG, qconsole.log via `logfile 2`."""

    tid = "q3"
    name = "Quake III Arena (shortcut: ioquake3 -> system ICD)"
    root = r"C:\Games\Quake3-TeamArena"
    mod = "baseq3"
    exe = "ioquake3.x86.exe"
    port = 27961
    demo = "four"
    homepath = "appdata"          # ioquake3 on XP: %APPDATA%\Quake3
    shot_cmd = "screenshotJPEG"   # SoF2 MP: "Unknown command" - its `screenshot` writes the jpg
    # the fleet server's rotation twice over, and the two biggest maps again
    map_cycle = ["q3dm7", "q3dm17", "q3tourney2", "q3dm6", "q3ctf1",
                 "q3dm7", "q3dm17", "q3tourney2", "q3dm6", "q3ctf1", "q3dm13", "q3dm7"]

    def launcher_args(self, env):
        # Play Quake III Arena.bat, verbatim
        return (f"+set r_mode -1 +set r_customwidth {env['FR_W']} +set r_customheight {env['FR_H']} "
                f"+set r_customaspect 1 +set r_customPixelAspect 1 +set r_fullscreen 1 "
                f"+set cg_fov {env.get('FR_FOV', '90')}")

    def fleetres_cfg(self, env):
        # what the launcher writes (FLEETGL.BAT adds nothing on a box with no 3dfxvgl.dll)
        return "\r\n".join([
            "// written by the launcher at every start - do not edit",
            'seta r_mode "-1"',
            f'seta r_customwidth "{env["FR_W"]}"',
            f'seta r_customheight "{env["FR_H"]}"',
            'seta r_customaspect "1"',
            'seta r_customPixelAspect "1"',
            'seta r_fullscreen "1"',
            f'seta r_displayRefresh "{env.get("FR_HZ", "0")}"',
            f'seta cg_fov "{env.get("FR_FOV", "90")}"',
            "",
        ])

    def script(self, phase, soak_frames, shots, server=None):
        if phase == "timedemo":
            return ['set timedemo 1', 'set nextdemo "quit"', f'demo {self.demo}']
        if phase == "shot":
            return ['set timedemo 0', f'demo {self.demo}', 'wait 400', 'screenshotJPEG',
                    'wait 400', 'screenshotJPEG', 'wait 30', 'quit']
        if phase == "maps":
            # texture churn: a local game walks the maps a LAN evening does,
            # and each load frees and re-uploads every texture through the
            # ICD and Glide. A 20-minute soak on one server map loads ONE map
            # (the q3 soak of 2026-09-28 did), so it proves nothing about this.
            lines = ['set timedemo 0']
            for m in self.map_cycle:
                lines += [f'map {m}', 'wait 900', self.shot_cmd]
            self.maps_expected = len(self.map_cycle)
            return lines + ['wait 60', 'quit']
        # soak: connect FIRST - anything after `+exec` on the command line
        # waits behind this script, which ends in `quit`
        lines = ['set timedemo 0', f'connect {server}:{self.port}', 'wait 600']
        for _ in range(shots):
            lines += [f'wait {max(1, soak_frames // shots)}', self.shot_cmd]
        return lines + ['disconnect', 'wait 60', 'quit']

    async def paths(self, box):
        base = self.root if self.homepath == "root" else (await appdata(box)) + r"\Quake3"
        return {"log": rf"{base}\{self.mod}\qconsole.log",
                "shots": rf"{base}\{self.mod}\screenshots",
                "cfg": rf"{self.root}\{self.mod}\lancheck.cfg"}

    def command(self, env, phase, server):
        return f'{self.exe} {self.launcher_args(env)} +set logfile 2 +exec lancheck.cfg'

    def parse(self, raw):
        r = {}
        m = None
        for m in re.finditer(r"(\d+) frames,?\s*([\d.]+) seconds?:?\s*([\d.]+) fps", raw):
            pass
        if m:
            r["fps"] = float(m.group(3))
            r["frames"] = int(m.group(1))
        g = re.findall(r"GL_RENDERER:\s*(.+)", raw)
        if g:
            r["gl_renderer"] = g[-1].strip()
        mo = re.findall(r"MODE:\s*(-?\d+),\s*(\d+)\s*x\s*(\d+)\s*(\w*)", raw)
        if mo:
            r["mode"] = "x".join(mo[-1][1:3]) + (" " + mo[-1][3] if mo[-1][3] else "")
        cb = re.findall(r"PIXELFORMAT:\s*color\((\d+)-bits\)", raw)
        if cb:
            r["colorbits"] = int(cb[-1])
        errs = [l.strip() for l in raw.splitlines()
                if re.search(r"^\s*(\*+\s*)?(ERROR|FATAL)|Sys_Error|Hunk_Alloc failed|"
                             r"could not set|failed hard|R_Init failed|GLW_StartOpenGL|"
                             r"exception", l, re.I)]
        if errs:
            r["errors"] = errs[:12]
        r["connected"] = bool(re.search(r"CL_InitCGame|entered the game", raw))
        r["maps_loaded"] = len(re.findall(r"\.\.\.loaded \d+ faces", raw))
        # the soak script ends `disconnect`, which RtCW (and Q2) report as
        # "ERROR: Disconnected from server": ours when it follows the last
        # screenshot the script took, a real drop anywhere else
        if r.get("errors"):
            last_shot = raw.rfind("Wrote screenshots")
            drop = raw.rfind("Disconnected from server")
            if last_shot >= 0 and drop > last_shot and raw.count("Disconnected from server") == 1:
                r["errors"] = [e for e in r["errors"] if "Disconnected from server" not in e]
                if not r["errors"]:
                    del r["errors"]
        return r


class RTCW(IdTech3):
    tid = "rtcw"
    name = "Return to Castle Wolfenstein MP (shortcut: RTCW Multiplayer / Join LAN)"
    root = r"C:\Games\ReturnToCastleWolfenstein"
    mod = "Main"
    exe = "WolfMP.exe"
    port = 27963
    demo = None
    homepath = "root"
    map_cycle = ["mp_beach", "mp_village", "mp_assault", "mp_base", "mp_depot", "mp_sub",
                 "mp_beach", "mp_village", "mp_assault", "mp_base"]

    def launcher_args(self, env):
        return f"+set r_mode {env['FR_Q3MODE']} +set r_fullscreen 1"

    def fleetres_cfg(self, env):
        return "\r\n".join([
            "// written by the launcher at every start - do not edit",
            f'seta r_mode "{env["FR_Q3MODE"]}"',
            'seta r_fullscreen "1"',
            f'seta r_displayRefresh "{env.get("FR_HZ", "0")}"',
            "",
        ])

    def script(self, phase, soak_frames, shots, server=None):
        if phase in ("timedemo", "shot"):
            # the MP binary has no demo of ours; the soak carries the test
            return []
        return super().script(phase, soak_frames, shots, server)

    async def paths(self, box):
        # RtCW names its console log rtcwconsole.log, not qconsole.log
        p = await super().paths(box)
        p["log"] = rf"{self.root}\{self.mod}\rtcwconsole.log"
        return p


class SoF2MP(IdTech3):
    """Soldier of Fortune II multiplayer as its shortcut runs it
    (Play Soldier of Fortune II - Multiplayer.bat): sof2mp.exe -> opengl32 ->
    the system ICD, r_mode from FLEETRES (the fork has no r_mode -1), the fleet
    server's port 20100. The MP binary needs no disc (SoF2.exe single player
    does - it is gated on disc_mount)."""
    tid = "sof2"
    name = "Soldier of Fortune II MP (shortcut: sof2mp.exe -> system ICD)"
    root = r"C:\Games\SoldierOfFortune2"
    mod = r"base\mp"
    exe = "sof2mp.exe"
    port = 20100
    demo = None
    homepath = "root"
    # the fleet server's rotation, twice
    map_cycle = ["mp_shop", "mp_kam3", "mp_hos1", "mp_shop", "mp_kam3", "mp_hos1"]
    shot_cmd = "screenshot"       # measured on .124: screenshotJPEG is unknown to sof2mp

    def launcher_args(self, env):
        return f"+set r_mode {env['FR_Q3MODE']} +set r_fullscreen 1 +set cg_fov {env.get('FR_FOV', '90')}"

    def fleetres_cfg(self, env):
        return None         # the launcher writes base\fleetres.cfg; see prepare()

    async def prepare(self, box, env):
        await box.upload(rf"{self.root}\base\fleetres.cfg", "\r\n".join([
            "// written by the launcher at every start - do not edit",
            f'seta r_mode "{env["FR_Q3MODE"]}"', 'seta r_fullscreen "1"',
            f'seta r_displayRefresh "{env.get("FR_HZ", "0")}"', f'seta cg_fov "{env.get("FR_FOV", "90")}"', ""]))

    def script(self, phase, soak_frames, shots, server=None):
        if phase in ("timedemo", "shot"):
            return []
        if phase == "soak":
            # NOT a wait chain: frames drawn while connecting count against `wait`
            # at an unknown rate and the gamestate load (CL_InitCGame, 16-26 s on
            # .124) draws none, so two runs spent every wait before the player was
            # in - their shots fired while connecting and the script's disconnect
            # came straight after "entered the game". The host presses the keys.
            # F9 is `quit` alone: with "disconnect; quit" sof2mp ran the
            # disconnect, opened its server browser and dropped the quit
            # (2026-09-28, 0.1.78 run) - a menu takes what is left of a bind
            return [f'bind F10 "{self.shot_cmd}"', 'bind F9 "quit"',
                    'set timedemo 0', f'connect {server}:{self.port}']
        return super().script(phase, soak_frames, shots, server)

    def host_keys(self, phase, soak, shots):
        if phase != "soak":
            return []
        keys = [(60 + (i + 1) * max(10, (soak - 60) // (shots + 1)), "F10") for i in range(shots)]
        return keys + [(soak, "F9")]


class JediAcademyMP(SoF2MP):
    """Jedi Academy multiplayer as its shortcut runs it (Play Jedi Academy -
    Multiplayer.bat): the disc image mounted first (jamp.exe scans for the
    JEDIACAD disc before anything else), then jamp.exe -> opengl32 -> the system
    ICD at r_mode -1 with the launcher's custom size, the fleet server on 29070."""
    tid = "jka"
    name = "Jedi Academy MP (shortcut: mount + jamp.exe -> system ICD)"
    root = r"C:\Games\JediAcademy"
    mod = "base"
    exe = "jamp.exe"
    port = 29070
    map_cycle = ["mp/ffa1", "mp/ffa3", "mp/ffa5", "mp/ffa1", "mp/ffa3", "mp/ffa5"]
    shot_cmd = "screenshot"
    image = r"C:\Games\JediAcademy\_disc\JediAcademy_CD1.iso"
    mounter = r"C:\Program Files\WinCDEmu\batchmnt.exe"
    server_probe = "q3"     # the client log never says "entered the game"

    def parse(self, raw):
        r = super().parse(raw)
        # JA prints no "...loaded N faces": a map is in once its cgame loads
        r["maps_loaded"] = len(re.findall(r"^Server: \S+[\s\S]*?Loading dll file cgame", raw, re.M))
        return r

    def host_keys(self, phase, soak, shots):
        # JA opens its Player Configuration menu on joining (measured on .124:
        # the first map-cycle shot shows it), and a menu takes the keys - F10
        # and F9 did nothing for a whole soak. Escape closes it first.
        keys = super().host_keys(phase, soak, shots)
        return [(55, "ESC")] + keys if keys else keys

    def launcher_args(self, env):
        return (f"+set r_mode -1 +set r_customwidth {env['FR_W']} +set r_customheight {env['FR_H']} "
                f"+set r_customaspect 1 +set r_customPixelAspect 1 +set r_fullscreen 1 "
                f"+set cg_fov {env.get('FR_FOV', '90')}")

    async def prepare(self, box, env):
        await box.upload(rf"{self.root}\base\fleetres.cfg", "\r\n".join([
            "// written by the launcher at every start - do not edit",
            f'seta r_customwidth "{env["FR_W"]}"', f'seta r_customheight "{env["FR_H"]}"',
            f'seta r_displayRefresh "{env.get("FR_HZ", "0")}"', f'seta cg_fov "{env.get("FR_FOV", "90")}"', ""]))
        # the disc, as the launcher mounts it; the label decides it is mounted
        vol = await box.exec_('cmd /c for %d in (D E F G H I J K L M N O P) do @vol %d: 2>nul | find "JEDIACAD"', timeout=60)
        if "JEDIACAD" not in vol:
            # cmd /c strips the OUTER pair of quotes when the line holds more
            # than two, so two quoted paths need one more pair around the lot:
            # without it cmd ran `C:\Program`, the mounter never started, and
            # jamp.exe sat at its "insert the disc" prompt (2026-09-28)
            out = await box.exec_(f'cmd /c ""{self.mounter}" "{self.image}" /wait"', timeout=60)
            log(f"    mount: {out.strip()[:120]}")
            for _ in range(15):
                await asyncio.sleep(2)
                vol = await box.exec_('cmd /c for %d in (D E F G H I J K L M N O P) do @vol %d: 2>nul | find "JEDIACAD"', timeout=60)
                if "JEDIACAD" in vol:
                    break
        if "JEDIACAD" not in vol:
            return f"disc image not mounted - no JEDIACAD volume after {self.mounter}"


def wait_chain(frames):
    """`wait` that takes no argument (id Tech 2, GoldSrc, NetQuake): N frames
    as alias expansions - a flat chain of thousands overflows the buffer."""
    out = ['alias lw10 "wait;wait;wait;wait;wait;wait;wait;wait;wait;wait"',
           'alias lw100 "lw10;lw10;lw10;lw10;lw10;lw10;lw10;lw10;lw10;lw10"']
    return out, ["lw100"] * max(1, frames // 100)


class IdTech2(IdTech3):
    """Quake II 3.20: gl_driver opengl32 (the staged autoexec) -> the system
    ICD. `wait` is one frame; `screenshot` writes baseq2\\scrnshot\\*.tga."""
    tid = "q2"
    name = "Quake II (shortcut: quake2.exe gl_driver opengl32 -> system ICD)"
    root = r"C:\Games\Quake2Complete"
    mod = "baseq2"
    exe = "quake2.exe"
    port = 27910
    homepath = "root"
    shot_ext = "tga"
    shot_dir = "scrnshot"

    def launcher_args(self, env):
        return f"+set gl_mode {env['FR_Q2MODE']}"

    def fleetres_cfg(self, env):
        return "\r\n".join(["// written by the launcher at every start - do not edit",
                             f'set gl_mode "{env["FR_Q2MODE"]}"', 'set vid_fullscreen "1"', ""])

    async def paths(self, box):
        return {"log": rf"{self.root}\{self.mod}\qconsole.log",
                "shots": rf"{self.root}\{self.mod}\{self.shot_dir}",
                "cfg": rf"{self.root}\{self.mod}\lancheck.cfg"}

    def script(self, phase, soak_frames, shots, server=None):
        head, w = wait_chain(300)
        if phase == "timedemo":
            return ['set timedemo "1"', 'demomap demo1.dm2', 'set nextserver "killserver; quit"']
        if phase == "shot":
            # the demo's end must quit, not load base2 (CM_InlineModel: bad
            # number - the engine loading a map over a demo, not the driver)
            # a real map, not the demo: at normal speed demo1.dm2 hands over
            # to base2 at once and the ENGINE errors (CM_InlineModel)
            return head + ['set timedemo "0"', 'map base1'] + w * 3 + ['screenshot'] + w + \
                ['screenshot', 'quit']
        # NO wait chain here: a joining client is driven by commands the
        # server appends to the same command buffer (precache, begin), and a
        # long `wait` chain in front of them kept the client on the loading
        # console for the whole soak. The host presses the keys instead.
        return self.soak_binds() + [f'connect {server}:{self.port}']

    def soak_binds(self):
        return ['bind F10 "screenshot"', 'bind F9 "disconnect; quit"']

    def host_keys(self, phase, soak, shots):
        if phase != "soak":
            return []
        keys = [(30 + (i + 1) * max(10, (soak - 30) // (shots + 1)), "F10") for i in range(shots)]
        return keys + [(soak, "F9")]

    def parse(self, raw):
        r = super().parse(raw)
        mo = re.findall(r"setting mode (\d+):\s*(\d+)\s+(\d+)", raw)
        if mo:
            r["mode"] = f"{mo[-1][1]}x{mo[-1][2]} (mode {mo[-1][0]})"
        # the client_connect answer, and no drop before our own disconnect
        r["connected"] = "client_connect" in raw or "entered the game" in raw
        if r.get("errors"):
            # `disconnect` in id Tech 2 IS Com_Error(ERR_DROP, "Disconnected
            # from server") - the soak's own exit, not a fault
            r["errors"] = [e for e in r["errors"] if "Disconnected from server" not in e] or None
            if not r["errors"]:
                del r["errors"]
        return r


class SoF(IdTech2):
    tid = "sof"
    name = "Soldier of Fortune (shortcut: SoF.exe ref_gl -> gl_driver)"
    root = r"C:\Games\SoldierOfFortune"
    mod = "base"
    exe = "SoF.exe"
    port = 28910
    shot_dir = "scrnshot"
    shot_ext = "tga"

    async def paths(self, box):
        # SoF writes its log and screenshots under user\, not base\ - the
        # 2026-09-28 night runs wrote user\sof.log and user\scrnshot\sofNN.tga
        # while the harness looked in base\ and reported "no screenshot"
        return {"log": rf"{self.root}\user\sof.log",
                "shots": rf"{self.root}\user\scrnshot",
                "cfg": rf"{self.root}\{self.mod}\lancheck.cfg"}

    def script(self, phase, soak_frames, shots, server=None):
        # no demo of ours and no SoF 1 server on the host. SINGLE PLAYER: any
        # multiplayer map ('deathmatch 1') hits the WON check "Please insert the
        # SOF CD" on every fleet box, disc mounted or not (requires.json); the
        # 2026-09-28 soak asked for dm/nycdm1 and photographed a black screen.
        # The menu 'shot' phase only ever caught the intro cinematic (black), so
        # the soak's host-key screenshots in nyc1 are the render evidence.
        if phase in ("timedemo", "shot"):
            return []
        return self.soak_binds() + ['deathmatch 0', 'map nyc1']


class GoldSrc(IdTech2):
    """Counter-Strike 1.6: hl.exe -game cstrike -full -gl -w -h (MESA_FORCE_SSE=1),
    EngineGLDriver Default -> opengl32 -> the system ICD. -condebug writes
    qconsole.log in the ROOT. BCShield blocks timerefresh; `snapshot` writes
    cstrike\\<map>NNNN.bmp."""
    tid = "cs16"
    name = "Counter-Strike 1.6 (shortcut: hl.exe -gl -> system ICD)"
    root = r"C:\Games\CounterStrike16"
    mod = "cstrike"
    exe = "hl.exe"
    port = 27015
    shot_ext = "bmp"

    def launcher_args(self, env):
        return f"-game cstrike -full -gl -w {env['FR_W']} -h {env['FR_H']} -condebug"

    def fleetres_cfg(self, env):
        return None

    async def paths(self, box):
        return {"log": rf"{self.root}\qconsole.log", "shots": rf"{self.root}\{self.mod}",
                "cfg": rf"{self.root}\{self.mod}\lancheck.cfg"}

    def command(self, env, phase, server):
        return f'{self.exe} {self.launcher_args(env)} +exec lancheck.cfg'

    def env_lines(self):
        return ["set MESA_FORCE_SSE=1"]

    def script(self, phase, soak_frames, shots, server=None):
        if phase == "timedemo":
            # GoldSrc stays at the console after a timedemo, and this build
            # (BCShield) ignores injected keys - so a frame-counted quit
            # (the demo is ~1,050 frames; waits count them too)
            head, w = wait_chain(100)
            return head + ['timedemo cs16_bench'] + w * 25 + ['quit']
        if phase == "shot":
            # demo playback: no MOTD / team menu holding the keyboard (a
            # listen server's VGUI swallowed every bound key)
            head, w = wait_chain(300)
            return head + ['playdemo cs16_bench'] + w + ['snapshot'] + w + w + ['snapshot', 'quit']
        # no host keys: this build ignores injected input (and a RETURN that
        # misses the game lands on the desktop), so the snapshots are scripted.
        # But ANY script still pending sits in front of the server's commands
        # in GoldSrc's one command buffer - the map-change "reconnect" is
        # appended behind it - so the 2026-09-28 soak, which kept its quit
        # behind a 30-minute wait chain, froze at "Loading..." at the first
        # map change (and overflowed the buffer 17,474 times). So: two
        # snapshots in the first two minutes, then the script is DONE and the
        # rest of the soak - map changes included - runs with an empty buffer.
        # The harness closes the game at the deadline (close_after_soak).
        head, w = wait_chain(3000)
        return head + [f'connect {server}:{self.port}'] + w + w + ['snapshot'] + w + ['snapshot']

    close_after_soak = True

    def host_keys(self, phase, soak, shots):
        return []

    def soak_binds(self):
        return ['bind F10 "snapshot"', 'bind F9 "disconnect; quit"']

    # a real result: GoldSrc prints "-1 frames 1.000 seconds -1.000 fps" while a demo loads
    done_re = {"timedemo": r"(?<![-\d])[1-9]\d* frames\s+[\d.]+ seconds\s+\d[\d.]* fps"}
    quit_key = "F9"

    def parse(self, raw):
        r = super().parse(raw)
        # this build prints "Connection accepted by <server>"; the server's
        # own log is where "entered the game" appears
        r["connected"] = bool(re.search(r"Connection accepted by|entered the game", raw, re.I))
        # one serverinfo per map: "BUILD 10211 SERVER (0 CRC)"
        r["maps_loaded"] = len(re.findall(r"^BUILD \d+ SERVER", raw, re.M))
        n = len(re.findall(r"Cbuf_\w+: overflow", raw))
        if n:
            r["harness_fault"] = f"command buffer overflowed {n}x - a pending script blocks the server's commands"
        return r


class WonHLMod(GoldSrc):
    r"""A WON Half-Life mod as its shortcut runs it (HalfLife1\<Mod>.bat):
    hl.exe -nosierra -full -gl -w/-h from FLEETRES' 4:3 pair -game <mod>. The
    fleet has no dedicated server for these, so it is a LOCAL listen server on
    a stock map: `+map` on the command line and a cfg that only binds keys -
    in GoldSrc anything still pending in the one command buffer sits in front
    of the listen server's own `connect local` and holds it at "Loading..."
    (the CS 1.6 finding). The host presses F10 (snapshot) and F9 (quit)."""
    root = r"C:\Games\HalfLife1"
    port = 0
    start_map = ""
    close_after_soak = False

    def launcher_args(self, env):
        return (f"-nosierra -full -gl -w {env.get('FR_W43', env['FR_W'])} "
                f"-h {env.get('FR_H43', env['FR_H'])} -toconsole -game {self.mod} -condebug")

    def env_lines(self):
        return []

    def command(self, env, phase, server):
        return f'{self.exe} {self.launcher_args(env)} +exec lancheck.cfg +map {self.start_map}'

    def script(self, phase, soak_frames, shots, server=None):
        if phase != "soak":
            return []
        return ['bind F10 "snapshot"', 'bind F9 "quit"']

    def host_keys(self, phase, soak, shots):
        if phase != "soak":
            return []
        keys = [(45 + (i + 1) * max(10, (soak - 45) // (shots + 1)), "F10") for i in range(shots)]
        return keys + [(soak, "F9")]

    def parse(self, raw):
        r = super().parse(raw)
        # a listen server: the map is in once its serverinfo arrives
        r["connected"] = r.get("maps_loaded", 0) > 0 or r.get("connected", False)
        return r


class DMC(WonHLMod):
    tid = "dmc"
    name = "Deathmatch Classic (shortcut: WON hl.exe -gl -> system ICD, listen server dmc_dm2)"
    mod = "dmc"
    start_map = "dmc_dm2"


class TFC(WonHLMod):
    tid = "tfc"
    name = "Team Fortress Classic (shortcut: WON hl.exe -gl -> system ICD, listen server 2fort)"
    mod = "tfc"
    start_map = "2fort"


class UT99(IdTech3):
    """Unreal Tournament 436 as its shortcut runs it: System\\UnrealTournament.exe
    with the staged ini (the launcher patches only the viewport and fullscreen),
    so the ini's GameRenderDevice decides the renderer. UE1 takes synthetic keys
    in exclusive fullscreen: F11=Shot (System\\ShotNNNN.bmp), F10=Exit - the clean
    exit that flushes the log and leaves no Running.ini. The timedemo is
    v56k_bench's UTbench route (ut99:<api>), not repeated here."""
    tid = "ut99"
    name = "Unreal Tournament 436 (shortcut: ini GameRenderDevice)"
    root = r"C:\Games\UnrealTournament436"
    mod = "System"
    exe = "UnrealTournament.exe"
    port = 7797
    shot_ext = "bmp"

    def fleetres_cfg(self, env):
        return None

    async def paths(self, box):
        return {"log": rf"{self.root}\System\lancheck.log", "shots": rf"{self.root}\System",
                "cfg": None}

    async def prepare(self, box, env):
        ini = rf"{self.root}\System\UnrealTournament.ini"
        for k, v in (("FullscreenViewportX", env["FR_W"]), ("FullscreenViewportY", env["FR_H"]),
                     ("StartupFullscreen", "True")):
            await box.exec_(f'"{self.root}\\FLEETRES.EXE" -ini "{ini}" WinDrv.WindowsClient {k} {v}',
                            timeout=60)
        uini = rf"{self.root}\System\User.ini"
        txt = (await box.download(uini) or b"").decode("latin-1")
        new = re.sub(r"(?m)^F10=.*$", "F10=Exit", txt, count=1)
        new = re.sub(r"(?m)^F11=.*$", "F11=Shot", new, count=1)
        if txt and new != txt:
            await box.upload(uini, new.encode("latin-1"))
        await box.exec_(f'cmd /c del /q "{self.root}\\System\\Running.ini" 2>nul')

    def script(self, phase, soak_frames, shots, server=None):
        return ["(host keys)"] if phase in ("shot", "soak") else []

    def command(self, env, phase, server):
        url = f"{server}:{self.port} " if phase == "soak" else ""
        return f'cd System && {self.exe} {url}-log=lancheck.log'

    def host_keys(self, phase, soak, shots):
        if phase == "shot":
            return [(40, "F11"), (55, "F11"), (65, "F10")]
        keys = [(40 + (i + 1) * max(10, (soak - 40) // (shots + 1)), "F11") for i in range(shots)]
        return keys + [(soak, "F10")]

    def parse(self, raw):
        r = {}
        m = re.findall(r"(?i)Log: (?:Using|Initializing) (\w+Drv[^\r\n]*)", raw)
        if m:
            r["gl_renderer"] = m[-1].strip()
        g = re.findall(r"GL_RENDERER\s*:?\s*(.+)", raw)
        if g:
            r["gl_renderer"] = g[-1].strip()
        mo = re.findall(r"(?i)(?:Setting|SetRes)[^\r\n]*?(\d{3,4})x(\d{3,4})", raw)
        if mo:
            r["mode"] = "x".join(mo[-1])
        errs = [l.strip() for l in raw.splitlines()
                if re.search(r"Critical:|General protection fault|Exit: .*failed|appError", l)]
        if errs:
            r["errors"] = errs[:12]
        r["connected"] = bool(re.search(r"(?i)LoadMap: DM-|Joined|Welcome", raw)) and \
            bool(re.search(r"(?i)Browse: \d+\.\d+", raw))
        return r


class GLQuake(IdTech2):
    """Quake 1, the "Quake" shortcut: root GLQUAKE.EXE -> opengl32 -> the
    system ICD, at the launcher's 4:3 mode (FLEETRES -cap 1280 960) in 32 bpp.
    No -condebug: the ICD's extension string overruns Con_DebugLog's static
    1 KB buffer (v56k plan), so the evidence is engine screenshots - the demo
    running, and the console after the timedemo, which shows the fps line."""
    tid = "q1"
    name = "Quake (shortcut: GLQUAKE.EXE -> system ICD)"
    root = r"C:\Games\Quake1"
    mod = "id1"
    exe = "GLQUAKE.EXE"
    port = 26000
    fleetres_extra = "-cap 1280 960"

    def launcher_args(self, env):
        return f"-width {env['FR_W43']} -height {env['FR_H43']} -bpp 32"

    def fleetres_cfg(self, env):
        return None

    async def paths(self, box):
        return {"log": rf"{self.root}\{self.mod}\qconsole.log",
                "shots": rf"{self.root}\{self.mod}", "cfg": rf"{self.root}\{self.mod}\lancheck.cfg"}

    def command(self, env, phase, server):
        return f'{self.exe} {self.launcher_args(env)} +exec lancheck.cfg'

    def script(self, phase, soak_frames, shots, server=None):
        head, w = wait_chain(500)
        if phase == "timedemo":
            # the result is printed on the console; photograph it
            return head + ['timedemo demo1'] + w * 5 + ['screenshot'] + w + ['quit']
        if phase == "shot":
            return head + ['playdemo demo2'] + w + ['screenshot'] + w + ['screenshot', 'quit']
        return self.soak_binds() + [f'connect {server}:{self.port}']

    def host_keys(self, phase, soak, shots):
        # a bound `quit` during play is Host_Quit_f -> M_Menu_Quit_f, the
        # "really quit? y/n" menu; the 2026-09-28 soak sat there, then its
        # WM_CLOSE raised WinQuake's own "Confirm Exit" box on top
        keys = list(super().host_keys(phase, soak, shots))
        return keys + [(soak + 4, "?Y")] if keys else keys

    def parse(self, raw):
        return {"connected": True} if not raw else super().parse(raw)


class GLQuakeVoodoo(GLQuake):
    """Quake 1, the "Quake - 3dfx Voodoo" shortcut: VOODOO\\GLQUAKE.EXE
    -width 640 -height 480 -bpp 16 with the staged 3dfx MiniGL (VOODOO\\
    OPENGL32.DLL) -> glide2x (the Glide2-to-Glide3 translator) -> our glide3x."""
    tid = "q1voodoo"
    name = "Quake (shortcut: VOODOO\\GLQUAKE.EXE -> 3dfx MiniGL -> Glide)"
    fleetres_extra = ""

    def launcher_args(self, env):
        return "-width 640 -height 480 -bpp 16"

    def command(self, env, phase, server):
        return f'VOODOO\\{self.exe} {self.launcher_args(env)} +exec lancheck.cfg'


class Launcher:
    """A title driven only through its own desktop launcher: the shortcut's
    .bat is started UNCHANGED, the agent's GDI SCREENSHOT is the picture (a
    DirectDraw / Direct3D title's primary on our driver is the GDI surface),
    and the game must still be running, with no error window, when the time is
    up; it is then closed with WM_CLOSE only (and reported if it would not go).
    Phase "smoke" only."""
    tid = "launcher"
    name = "a launcher"
    root = None
    bat = None
    images = ()            # the process image(s) the launcher starts
    keys = ()              # (seconds, UIKEY) to get past intros, e.g. ESC
    shots_at = (45, 90)

    async def run(self, box, args, outdir):
        rec = {"title": self.tid, "phase": "smoke", "t0": time.strftime("%Y-%m-%d %H:%M:%S"),
               "command": self.bat}
        for img in self.images:
            if await running(box, img):
                rec["error"] = f"{img} already running - not started"
                return rec
        await bench.quiesce(box)
        dr_before = await file_size(box, DRWTSN)
        log(f"--- {self.tid} smoke: {self.bat}")
        started = time.time()
        # a one-line `cmd /c cd /d X && Y` does not change directory under the
        # agent (FLEETRES, SoF and this route all hit it): a wrapper .bat that
        # cd's first and CALLs the launcher, as the shortcut's working dir does
        wrap = r"C:\RETRO_AGENT\lanlaunch.bat"
        await box.upload(wrap, "\r\n".join(["@echo off", f'cd /d "{self.root}"', f'call "{self.bat}"', ""]))
        await box.text(f"LAUNCH {wrap}")
        events = sorted([(t, "key", k) for t, k in self.keys] + [(t, "shot", None) for t in self.shots_at])
        rec["shots"], rec["keys_sent"], rec["alive"] = [], [], []
        for at, kind, k in events:
            wait = started + at - time.time()
            if wait > 0:
                await asyncio.sleep(wait)
            if kind == "key":
                st, out = await box.cmd(f"UIKEY {k}", timeout=30)
                rec["keys_sent"].append(f"{k}@{at}s:{out.strip()[:16]}")
                continue
            alive = [img for img in self.images if await running(box, img)]
            rec["alive"].append({"at": at, "running": alive})
            try:
                data = await box.cmd_binary("SCREENSHOT 0") if hasattr(box, "cmd_binary") else None
            except Exception:
                data = None
            if data is None:
                from client.retro_protocol import RetroConnection
                c = RetroConnection(box.ip, 9898)
                await c.connect(bench.SECRET, timeout=20)
                try:
                    data = await c.command_binary("SCREENSHOT 0", timeout=90)
                finally:
                    await c.close()
            png = outdir / f"{self.tid}_smoke_{at}s.png"
            save_png(data, png)
            rec["shots"].append({"file": png.name, "stats": shot_stats(data), "at": at})
        rec["error_windows"] = await error_windows(box)
        rec["running_at_end"] = [img for img in self.images if await running(box, img)]
        for img in rec["running_at_end"]:
            await box.exec_(f'cmd /c taskkill /im "{img}" 2>nul', timeout=30)
        await asyncio.sleep(15)
        rec["alive_after_close"] = [img for img in self.images if await running(box, img)]
        dr_after = await file_size(box, DRWTSN)
        if dr_after != dr_before:
            rec["drwatson_grew"] = dr_after - dr_before
        rec["agent_alive"] = await bench.agent_alive(box)
        rec["seconds"] = round(time.time() - started, 1)
        return rec


class RedAlert2(Launcher):
    tid = "ra2"
    name = "Command & Conquer: Red Alert 2 (shortcut: Launch Red Alert 2.bat, DirectDraw)"
    root = r"C:\Games\RedAlert2"
    bat = r"C:\Games\RedAlert2\Launch Red Alert 2.bat"
    images = ("Ra2.exe", "game.exe")
    keys = ((20, "ESCAPE"), (28, "ESCAPE"))
    shots_at = (40, 70)


class Turok2(Launcher):
    tid = "turok2"
    name = "Turok 2 (shortcut: Play Turok 2.bat, Direct3D)"
    root = r"C:\Games\Turok2"
    bat = r"C:\Games\Turok2\Play Turok 2.bat"
    images = ("Turok2English.exe",)
    keys = ((25, "ESCAPE"), (35, "ESCAPE"))
    shots_at = (45, 75)


TITLES = {"q3": IdTech3, "rtcw": RTCW, "sof2": SoF2MP, "jka": JediAcademyMP, "dmc": DMC, "tfc": TFC, "q2": IdTech2, "sof": SoF, "cs16": GoldSrc, "ut99": UT99,
          "q1": GLQuake, "q1voodoo": GLQuakeVoodoo, "ra2": RedAlert2, "turok2": Turok2}


# --------------------------------------------------------------------------- #
# one run
# --------------------------------------------------------------------------- #

async def run_phase(box, t, env, phase, args, outdir):
    rec = {"title": t.tid, "phase": phase, "env": env, "t0": time.strftime("%Y-%m-%d %H:%M:%S")}
    lines = t.script(phase, args.soak_frames, args.shots, args.server)
    if not lines:
        rec["skipped"] = "no script for this phase"
        return rec
    p = await t.paths(box)
    img = t.exe
    if await running(box, img):
        rec["error"] = f"{img} already running - not started"
        return rec
    await drwatson_settle(box)
    others = await games_running(box)
    if others:
        rec["error"] = f"another game still running ({', '.join(others)}) - not started"
        return rec
    await bench.quiesce(box)
    fcfg = t.fleetres_cfg(env)
    if fcfg is not None:
        await box.upload(rf"{t.root}\{t.mod}\fleetres.cfg", fcfg)
    if hasattr(t, "prepare"):
        err = await t.prepare(box, env)
        if err:
            rec["error"] = err
            return rec
    if p.get("cfg"):
        await box.upload(p["cfg"], "\r\n".join(lines) + "\r\n")
    await box.exec_(f'cmd /c del /f /q "{p["log"]}" 2>nul')
    ext = getattr(t, "shot_ext", "jpg")
    shots_before = await dir_stamps(box, p["shots"], ext)
    dr_before = await file_size(box, DRWTSN)
    bat = rf"{t.root}\LANCHECK.BAT"
    await box.upload(bat, "\r\n".join(["@echo off"] + (t.env_lines() if hasattr(t, "env_lines") else []) +
                                      [f'cd /d "{t.root}"', t.command(env, phase, args.server), ""]))
    rec["command"] = t.command(env, phase, args.server)
    log(f"--- {t.tid} {phase}: {rec['command']}")
    started = time.time()
    await box.text(f"LAUNCH {bat}")
    planned_close = phase == "soak" and getattr(t, "close_after_soak", False)
    budget = args.soak if planned_close else args.soak + 180 if phase == "soak" else args.max_run
    seen, hung = False, False
    keys = list(t.host_keys(phase, args.soak, args.shots)) if hasattr(t, "host_keys") else []
    rec["keys_sent"] = []
    done_re = getattr(t, "done_re", {}).get(phase)
    polled = 0.0
    while True:
        await asyncio.sleep(5)
        if phase == "soak" and time.time() - polled >= 30:
            polled = time.time()
            h = await asyncio.to_thread(server_humans, t, args.server)
            if h is not None:
                rec.setdefault("server_humans", []).append(f"{int(polled - started)}s:{h}")
                rec["server_humans_max"] = max(rec.get("server_humans_max", 0), h)
        if done_re and time.time() - started > 15:
            part = (await box.download(p["log"]) or b"").decode("latin-1", errors="replace")
            if re.search(done_re, part):
                st, out = await box.cmd(f"UIKEY {t.quit_key}", timeout=30)
                rec["keys_sent"].append(f"{t.quit_key}@done:{out.strip()[:20]}")
                done_re = None
        el0 = time.time() - started
        while keys and el0 >= keys[0][0]:
            at, k = keys.pop(0)
            if k.startswith("?"):
                # only into a live game: a key that misses it lands on the desktop
                k = k[1:]
                if not await running(box, img):
                    rec["keys_sent"].append(f"{k}@{int(el0)}s:skipped, game gone")
                    continue
            try:
                st, out = await box.cmd(f"UIKEY {k}", timeout=30)
                rec["keys_sent"].append(f"{k}@{int(el0)}s:{out.strip()[:20]}")
            except Exception as e:
                rec["keys_sent"].append(f"{k}@{int(el0)}s:ERR {e}")
        alive = await running(box, img)
        seen = seen or alive
        el = time.time() - started
        if not alive and (seen or el > 30):
            break
        if el > budget:
            hung = not planned_close
            rec["closed_by_harness"] = planned_close
            break
    rec["seconds"] = round(time.time() - started, 1)
    raw = (await box.download(p["log"]) or b"").decode("latin-1", errors="replace")
    (outdir / f"{t.tid}_{phase}.log").write_text(raw)
    rec.update(t.parse(raw))
    if rec.get("server_humans_max"):
        rec["connected"] = True     # the server saw a human player on it
    if phase == "maps":
        rec["maps_expected"] = getattr(t, "maps_expected", 0)
    rec["log_bytes"] = len(raw)
    if hung or rec.get("closed_by_harness"):
        if hung:
            rec["hung"] = True
        rec["error_windows"] = await error_windows(box)
        log(f"    still running after {budget}s - WM_CLOSE only (no TerminateProcess)"
            + (" - the planned end of this soak" if rec.get("closed_by_harness") else ""))
        await box.exec_(f'cmd /c taskkill /im "{img}" 2>nul', timeout=30)
        await asyncio.sleep(15)
        rec["alive_after_close"] = await running(box, img)
    rec["error_windows"] = rec.get("error_windows") or await error_windows(box)
    dr_after = await file_size(box, DRWTSN)
    if dr_after != dr_before:
        rec["drwatson_grew"] = dr_after - dr_before
        # a crash: let Dr. Watson finish, then look again - the process can
        # outlive the crash (stuck in its own error path) and must not be
        # left for the next title to be launched over
        rec["drwatson_settled_s"] = await drwatson_settle(box)
        if await running(box, img):
            rec["crash_left_process"] = True
            log(f"    {img} survived its crash - TerminateProcess (it is past rendering), then pace-mark")
            await box.exec_(f'cmd /c taskkill /f /im "{img}" 2>nul', timeout=30)
            await box.exec_(r'C:\vcr\vcrctl.exe pace-mark', timeout=30)
            await asyncio.sleep(5)
    after = await dir_stamps(box, p["shots"], ext)
    new = sorted(n for n, st in after.items() if shots_before.get(n) != st)
    rec["shots"] = []
    for s in new:
        data = await box.download(rf"{p['shots']}\{s}")
        if not data:
            continue
        png = outdir / f"{t.tid}_{phase}_{Path(s).stem}.png"
        try:
            save_png(data, png)
        except Exception as e:
            rec.setdefault("shot_errors", []).append(f"{s}: {e}")
            continue
        rec["shots"].append({"file": png.name, "stats": shot_stats(data)})
    rec["agent_alive"] = await bench.agent_alive(box)
    return rec


def verdict(rec):
    bad = []
    if rec.get("skipped"):
        return "skipped"
    if rec.get("error"):
        bad.append(rec["error"])
    if rec.get("hung"):
        bad.append("did not exit")
    if rec.get("drwatson_grew"):
        bad.append("Dr. Watson entry")
    if rec.get("error_windows"):
        bad.append("error window")
    if rec.get("errors"):
        bad.append("log errors")
    if rec.get("harness_fault"):
        bad.append("HARNESS: " + rec["harness_fault"])
    if rec.get("closed_by_harness") and rec.get("alive_after_close"):
        bad.append("would not close at the end of the soak")
    # the fleet CS server changes map every 20 minutes (mp_timelimit 20):
    # a longer soak that saw one map proves nothing about the change
    if (rec.get("closed_by_harness") and rec.get("seconds", 0) > 1500
            and rec.get("maps_loaded", 0) < 2):
        bad.append(f"no map change seen in {rec.get('seconds')}s (maps_loaded {rec.get('maps_loaded', 0)})")
    if rec["phase"] == "timedemo" and not rec.get("fps") and not rec.get("shots"):
        bad.append("no timedemo result")
    if rec["phase"] == "maps" and rec.get("maps_loaded", 0) < rec.get("maps_expected", 1):
        bad.append(f"loaded {rec.get('maps_loaded', 0)} of {rec.get('maps_expected')} maps")
    if rec["phase"] in ("shot", "soak", "maps"):
        if not rec.get("shots"):
            bad.append("no screenshot")
        for s in rec.get("shots", []):
            st = s.get("stats")
            if not st or st[3] < 4:
                bad.append(f"blank shot {s['file']}")
    if rec["phase"] == "soak" and not rec.get("connected"):
        bad.append("never entered the game")
    if not rec.get("agent_alive", True):
        bad.append("AGENT DEAD")
    return "PASS" if not bad else "FAIL: " + "; ".join(bad)


async def amain(a):
    box = Box(a.host)
    outroot = Path(a.outdir) / time.strftime("%Y%m%d_%H%M%S")
    outroot.mkdir(parents=True, exist_ok=True)
    summary = []
    for tid in a.titles.split(","):
        t = TITLES[tid]()
        od = outroot / tid
        od.mkdir(exist_ok=True)
        if isinstance(t, Launcher):
            rec = await t.run(box, a, od)
            bad = [x for x in ("error", "drwatson_grew") if rec.get(x)]
            if rec.get("error_windows"):
                bad.append("error window")
            if not rec.get("running_at_end"):
                bad.append("not running at the end")
            if rec.get("alive_after_close"):
                bad.append("would not close")
            if not rec.get("agent_alive", True):
                bad.append("AGENT DEAD")
            rec["verdict"] = "PASS" if not bad else "FAIL: " + "; ".join(bad)
            (od / "smoke.json").write_text(json.dumps(rec, indent=1))
            log(f"    smoke: {rec['verdict']}  alive={rec.get('alive')}  shots={[x['stats'] for x in rec.get('shots', [])]}")
            summary.append({"title": tid, "phase": "smoke", "verdict": rec["verdict"]})
            continue
        env = await fleetres_env(box, t.root, getattr(t, "fleetres_extra", ""))
        log(f"=== {t.name}: FLEETRES {env}")
        if not env.get("FR_W"):
            summary.append({"title": tid, "verdict": "FAIL: FLEETRES.BAT gave no FR_W"})
            continue
        for phase in a.phases.split(","):
            rec = await run_phase(box, t, env, phase, a, od)
            rec["verdict"] = verdict(rec)
            (od / f"{phase}.json").write_text(json.dumps(rec, indent=1))
            log(f"    {phase}: {rec['verdict']}  fps={rec.get('fps')}  renderer={rec.get('gl_renderer')}  "
                f"mode={rec.get('mode')}  {rec.get('seconds')}s  shots={[s['stats'] for s in rec.get('shots', [])]}")
            summary.append({"title": tid, "phase": phase, "verdict": rec["verdict"], "fps": rec.get("fps"),
                            "renderer": rec.get("gl_renderer"), "mode": rec.get("mode")})
            if not rec.get("agent_alive", True):
                log("agent dead - stopping")
                break
            health = await bench.board_alive(box)
            rec["board_after"] = health
            if health is False:
                log("board no longer brings Glide up - stopping")
                summary.append({"title": tid, "phase": phase, "verdict": "BOARD WEDGED after this run"})
                (outroot / "summary.json").write_text(json.dumps(summary, indent=1))
                return 2
    (outroot / "summary.json").write_text(json.dumps(summary, indent=1))
    log(f"summary -> {outroot / 'summary.json'}")
    for s in summary:
        log(f"  {s}")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", required=True)
    ap.add_argument("--titles", default="q3")
    ap.add_argument("--phases", default="timedemo,shot,soak")
    ap.add_argument("--server", default="192.168.1.196", help="the fleet game-server host")
    ap.add_argument("--soak", type=int, default=300, help="seconds connected (soak phase)")
    ap.add_argument("--soak-fps", type=int, default=90, help="expected fps, to turn --soak into frames")
    ap.add_argument("--shots", type=int, default=4, help="screenshots during the soak")
    ap.add_argument("--max-run", type=int, default=300)
    ap.add_argument("--outdir", default=str(HERE / "results" / "v56k_lan_192.168.1.124"))
    a = ap.parse_args()
    a.soak_frames = a.soak * a.soak_fps
    sys.exit(asyncio.run(amain(a)))


if __name__ == "__main__":
    main()
