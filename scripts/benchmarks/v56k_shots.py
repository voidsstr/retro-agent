#!/usr/bin/env python3
"""
v56k_shots.py - matched-scene image-quality captures at each chip/AA setting.

WHY THE ENGINE TAKES THE PICTURE, NOT THE AGENT
-----------------------------------------------
The agent's GDI `SCREENSHOT` does photograph most fullscreen games on XP - that
much was measured across this fleet and is why the "GDI returns black" rule was
demoted to a Windows-7 fact. But a **Glide exclusive-fullscreen surface** is
precisely the case it cannot be trusted for: the 3dfx board renders into its
own framebuffer, and what GDI hands back is not necessarily what was scanned
out. Judging ANTI-ALIASING from a capture that might be resampled, or
palette-mangled, or simply of a different buffer, would be worse than having no
screenshots at all - it would look like evidence.

So every capture here is taken by the ENGINE, through its own screenshot
command, writing a file the engine composed from the frame it drew.

WHY A FIXED FRAME
-----------------
Anti-aliasing can only be judged by comparing the SAME scene at two settings;
edge-quality differences are far subtler than scene differences, so a shot
taken "a few seconds in" lands on a different frame each run and the comparison
is worthless. Demo playback is deterministic, so `demo <name>` + `wait <N
frames>` + `screenshot` returns the identical viewpoint at every AA level, on
every boot. `wait` counts FRAMES, not seconds, which is what makes this work:
the same N is the same scene whether the card manages 25 fps or 120.

WHY THIS BUILDS ON v56k_bench'S LAUNCHERS
-----------------------------------------
The first version of this file carried its own copy of the cvar dialect, and
that copy was wrong in two ways the fps campaign had already paid to learn:

  * it set the LATCHED cvars (r_mode, r_customwidth, r_colorbits ...) in a file
    exec'd from the command line, which lands AFTER R_Init. Those either do
    nothing or need a vid_restart - and a vid_restart is what hung the driver
    solid at 4-chip 8x AA, taking the agent down with it. v56k_bench sets them
    through fleetres.cfg + `+set` so the renderer is built ONCE, correctly.
  * it wrote Quake III's cvars into Quake II, which has no r_mode -1, no
    r_customwidth, and a `wait` that takes NO ARGUMENT and delays exactly one
    frame.

So the launch half of each title is imported from v56k_bench - the code that
has actually produced numbers on this card - and only the "play to a fixed
frame and photograph it" half lives here.

Per-engine facts encoded below, each of which is a real limit:

  * Quake III (retail 1.32c): `wait <N>` takes a frame count; `screenshotJPEG`
    exists and a 1600x1200 TGA is 5.7 MB over SMB1-era networking.
  * Quake II (3.20): `wait` takes no argument and waits ONE frame, so N frames
    is N lines. There is no JPEG screenshot - `screenshot` writes a TGA.
  * RtCW: its id Tech 3 fork HAS NO `r_mode -1` BRANCH (CLAUDE.md, measured) -
    asking for one renders 640x480 rather than erroring, which would silently
    produce a whole column of shots at the wrong resolution. It gets a real
    mode index instead, and only resolutions in the table can be requested.
    It also ships no demo, so the fixed scene is a map spawn point.

ONE CONFIG PER BOOT
-------------------
Same constraint as the fps sweep: writing SSTH3_SLI_AA_CONFIGURATION twice in
one boot wedges the driver. Each config gets a clean boot - and because a
RESOLUTION change is not a topology change, one boot captures every game.

Usage:
    python3 v56k_shots.py --host 192.168.1.124
    python3 v56k_shots.py --host 192.168.1.124 --configs 5,7,8 --games quake3
    python3 v56k_shots.py --host 192.168.1.124 --no-reboot --configs 5
"""

import argparse
import asyncio
import importlib.util
import json
import subprocess
import sys
import time
from datetime import datetime
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
sys.path.insert(0, str(REPO))
from client.retro_protocol import RetroConnection, RetroProtocolError  # noqa: E402


def _load_bench():
    spec = importlib.util.spec_from_file_location("v56k_bench", HERE / "v56k_bench.py")
    mod = importlib.util.module_from_spec(spec)
    sys.modules["v56k_bench"] = mod
    spec.loader.exec_module(mod)
    return mod


bench = _load_bench()

SECRET = "retro-agent-secret"
PROBE = r"C:\RETRO_AGENT\glideprobe.exe"
LABEL = {0: "1chip-noaa", 1: "1chip-2xaa", 2: "2chip-noaa", 3: "2chip-2xaa",
         4: "2chip-4xaa", 5: "4chip-noaa", 6: "4chip-2xaa", 7: "4chip-4xaa",
         8: "4chip-8xaa"}

# id Tech 3's fixed r_mode table, for the engines that have no r_mode -1.
IDTECH3_MODES = {(320, 240): 0, (400, 300): 1, (512, 384): 2, (640, 480): 3,
                 (800, 600): 4, (960, 720): 5, (1024, 768): 6, (1152, 864): 7,
                 (1280, 1024): 8, (1600, 1200): 9, (2048, 1536): 10}


def log(m):
    print(f"[{datetime.now().strftime('%H:%M:%S')}] {m}", flush=True)


# --------------------------------------------------------------------------- #
# board / topology plumbing (same rules as the fps sweep)
# --------------------------------------------------------------------------- #

async def cmd(ip, c, timeout=90):
    con = RetroConnection(ip, 9898)
    await con.connect(SECRET, timeout=20.0)
    try:
        st, d = await con.send_command(c, timeout=timeout)
        return st, d.decode("ascii", errors="replace")
    finally:
        await con.close()


async def glide_key(ip):
    st, out = await cmd(ip, "HWPROFILE", 60)
    prof = json.loads(out)
    inst = "0000"
    for v in prof.get("video_cards", []):
        if v.get("attached_to_desktop"):
            inst = v.get("instance", "0000")
    return (r"SYSTEM\CurrentControlSet\Control\Class"
            r"\{4D36E968-E325-11CE-BFC1-08002BE10318}"
            rf"\{inst}\Settings\Glide")


async def set_aa(ip, key, cfg):
    """Write the topology, then read it back - REGWRITE answers OK for a write
    that created a subkey instead of setting the value."""
    await cmd(ip, f"REGWRITE HKLM {key} SSTH3_SLI_AA_CONFIGURATION REG_SZ {cfg}")
    st, out = await cmd(ip, f"REGREAD HKLM {key}")
    for v in json.loads(out).get("values", []):
        if v["name"].upper() == "SSTH3_SLI_AA_CONFIGURATION":
            return str(v["data"]).strip() == str(cfg)
    return False


async def wait_agent(ip, timeout=900):
    """A completed PING, never a bare connect: a dead agent leaves 9897 bound
    and accepting while answering nothing."""
    await asyncio.sleep(25)
    end = time.time() + timeout
    while time.time() < end:
        try:
            st, out = await cmd(ip, "PING", 20)
            if "PONG" in out.upper():
                return True
        except Exception:
            pass
        await asyncio.sleep(15)
    return False


async def board_ok(ip):
    try:
        st, out = await cmd(ip, f"EXECW 90 {PROBE} --res 640x480 --noopen "
                                r"--dll C:\WINDOWS\system32\glide3x.dll "
                                r"--log C:\RETRO_AGENT\probe-shots.log", 140)
    except Exception:
        return None
    if "RESULT: probe-ok-noopen" in out:
        return True
    if "timed out" in out or "grGlideInit" in out:
        return False
    return None


# --------------------------------------------------------------------------- #
# the shot titles - launch half inherited, capture half here
# --------------------------------------------------------------------------- #

class ShotTitle:
    """Common capture flow: clear the shot dir, prepare the title the way the
    fps campaign prepares it, run it to a fixed frame, retrieve the file.

    The shot directory is emptied FIRST and the newest file taken afterwards,
    so a stale screenshot from a previous config can never be reported as this
    config's - which would be a picture that looks exactly like evidence and is
    not.
    """

    shotdir = None          # relative to root
    unsupported = None

    def supports(self, w, h, depth):
        return None

    async def _run(self, box, timeout):
        await box.text(f"EXECW {timeout} {self.bat}", timeout + 40)

    async def capture(self, ip, w, h, depth, env, timeout=240):
        why = self.supports(w, h, depth)
        if why:
            return None, why
        box = bench.Box(ip)
        shotdir = rf"{self.root}\{self.shotdir}"
        await box.exec_(f'cmd /c if not exist "{shotdir}" mkdir "{shotdir}"')
        await box.exec_(f'cmd /c del /f /q "{shotdir}\\*.*"')
        await box.exec_(f'cmd /c taskkill /f /im "{self.proc}"')
        await asyncio.sleep(2)
        await self.prepare(box, w, h, depth, env)
        try:
            await self._run(box, timeout)
        except Exception as e:
            return None, f"launch failed: {type(e).__name__}: {e}"
        await box.exec_(f'cmd /c taskkill /f /im "{self.proc}"')
        out = await box.exec_(f'cmd /c dir /b /o-d "{shotdir}" 2>&1')
        names = [n.strip() for n in out.replace("\r", "").splitlines()
                 if n.strip() and "." in n
                 and "not find" not in n.lower() and "file not" not in n.lower()]
        if not names:
            return None, "the engine wrote no screenshot"
        data = await box.download(rf"{shotdir}\{names[0]}")
        if not data:
            return None, f"could not download {names[0]}"
        return (names[0], data), None


class Q3Shot(ShotTitle, bench.Quake3):
    """Retail Quake III Arena. Latched cvars go through fleetres.cfg + `+set`
    exactly as the fps runs do, so the renderer is built once and no
    vid_restart is ever issued."""

    tid = "quake3"
    shotdir = r"baseq3\screenshots"

    def __init__(self, wait_frames=400, jpeg=True, **kw):
        bench.Quake3.__init__(self, **kw)
        self.wait_frames, self.jpeg = wait_frames, jpeg
        self.cfg = rf"{self.root}\baseq3\bench.cfg"     # same file the bat execs

    def bench_cfg(self):
        return "\r\n".join([
            '// generated per capture by v56k_shots.py',
            'seta com_maxfps "0"',
            'seta r_swapInterval "0"',
            'seta cg_drawFPS "0"',
            'seta cg_draw2D "0"',        # the HUD is not what is being judged
            'seta timedemo "1"',         # deterministic AND fast
            'demo four',
            f'wait {self.wait_frames}',
            "screenshotJPEG" if self.jpeg else "screenshot",
            'wait 40',
            'quit',
            '',
        ])


class Q2Shot(ShotTitle, bench.Quake2):
    """Quake II 3.20 through the 3dfx MiniGL.

    `wait` here is id Tech 2's, which takes NO ARGUMENT and delays exactly one
    frame - so N frames is N lines, not `wait N`. Writing `wait 300` would
    delay one frame and photograph the first frame of the demo.
    """

    tid = "quake2"
    shotdir = r"baseq2\scrnshot"

    def __init__(self, wait_frames=250, **kw):
        bench.Quake2.__init__(self, **kw)
        self.wait_frames = wait_frames
        self.cfg = rf"{self.root}\baseq2\bench.cfg"

    def bench_cfg(self):
        lines = ['// generated per capture by v56k_shots.py',
                 'set cl_maxfps "1000"', 'set gl_swapinterval "0"',
                 'set gl_picmip "0"', 'set timedemo "1"',
                 'demomap demo1.dm2']
        lines += ["wait"] * self.wait_frames
        lines += ["screenshot", "wait", "wait", "wait", "quit", ""]
        return "\r\n".join(lines)


class RTCWShot(ShotTitle):
    """Return to Castle Wolfenstein.

    TWO engine limits decide the whole shape of this one, and both are real:

    * its id Tech 3 fork has no `r_mode -1` branch (CLAUDE.md, measured on the
      fleet) - it renders 640x480 rather than erroring, which would silently
      fill a whole column with wrong-resolution shots. So a real mode INDEX is
      used and an off-table resolution is declared unsupported instead of
      being quietly rounded.
    * it ships no demo, so there is no `demo <name>` to make the scene
      repeatable. A map spawn point is used instead: the same map, the same
      spawn, the player standing still, which is deterministic for the purpose
      that matters here - the same geometry and the same edges every time.
    """

    tid = "rtcw"
    name = "Return to Castle Wolfenstein"
    engine = "WolfSP.exe"
    proc = "WolfSP.exe"
    api = "opengl-icd"
    shotdir = r"main\screenshots"

    def __init__(self, root=r"C:\Games\ReturnToCastleWolfenstein",
                 mapname="escape1", wait_frames=600):
        self.root = root
        self.mapname = mapname
        self.wait_frames = wait_frames
        self.cfg = rf"{root}\main\v56kshot.cfg"
        self.bat = rf"{root}\V56KSHOT.BAT"

    def supports(self, w, h, depth):
        if (w, h) not in IDTECH3_MODES:
            return (f"RtCW's id Tech 3 fork has no r_mode -1 and no {w}x{h} "
                    f"entry in the mode table")
        return None

    def shot_cfg(self):
        return "\r\n".join([
            '// generated per capture by v56k_shots.py',
            'seta com_maxfps "0"',
            'seta r_swapInterval "0"',
            'seta cg_draw2D "0"',
            'seta cg_drawFPS "0"',
            f'map {self.mapname}',
            f'wait {self.wait_frames}',
            'screenshotJPEG',
            'wait 40',
            'quit',
            '',
        ])

    def launch_bat(self, w, h, depth, env):
        mode = IDTECH3_MODES[(w, h)]
        zbits = 24 if depth >= 32 else 16
        lines = ["@echo off"] + [f'set {k}={v}' for k, v in env.items()]
        lines += [
            f'cd /d "{self.root}"',
            (f'{self.proc} +set fs_basepath "{self.root}" '
             f'+set fs_homepath "{self.root}" +set logfile 2 '
             f'+set r_glDriver 3dfxogl +set r_mode {mode} '
             f'+set r_colorbits {depth} +set r_texturebits {depth} '
             f'+set r_depthbits {zbits} +set r_fullscreen 1 +set r_picmip 0 '
             f'+set sv_cheats 1 +exec v56kshot.cfg'),
        ]
        return "\r\n".join(lines) + "\r\n"

    async def prepare(self, box, w, h, depth, env):
        await box.upload(self.cfg, self.shot_cfg())
        await box.upload(self.bat, self.launch_bat(w, h, depth, env))


GAMES = {
    "quake3": lambda: Q3Shot(),
    "quake2": lambda: Q2Shot(),
    "rtcw": lambda: RTCWShot(),
}


# --------------------------------------------------------------------------- #

def reboot(ip):
    r = subprocess.run(
        [sys.executable, str(REPO / "scripts" / "fleet" / "safe-reboot.py"), ip],
        capture_output=True, text=True, timeout=300)
    return r.returncode == 0, (r.stdout + r.stderr).strip()


def save(outdir, cfg, game, res, name, data):
    ext = Path(name).suffix or ".bin"
    p = outdir / f"{game}_{res}_cfg{cfg}_{LABEL.get(cfg,cfg)}{ext}"
    p.write_bytes(data)
    try:
        from PIL import Image
        im = Image.open(p)
        png = p.with_suffix(".png")
        im.save(png, optimize=True)
        return png, im.size
    except Exception:
        return p, None


async def amain(a):
    outdir = Path(a.outdir) if a.outdir else (HERE / "results" / f"shots_{a.host}")
    outdir.mkdir(parents=True, exist_ok=True)
    key = await glide_key(a.host)
    games = [GAMES[g]() for g in a.games]
    w, h = (int(x) for x in a.res.lower().split("x"))
    log(f"image-quality pass on {a.host}: {len(a.configs)} config(s) x "
        f"{len(games)} game(s) at {a.res}")
    log(f"output -> {outdir}")

    rows = []
    for n, cfg in enumerate(a.configs, 1):
        log("")
        log(f"===== [{n}/{len(a.configs)}] cfg {cfg} ({LABEL.get(cfg,'?')}) =====")
        if not await set_aa(a.host, key, cfg):
            log("  AA write did not stick - skipping")
            rows.append((cfg, "-", "aa-write-failed"))
            continue
        if not a.no_reboot:
            ok, out = reboot(a.host)
            if not ok:
                log("  safe-reboot REFUSED - stopping:")
                for line in out.splitlines()[:5]:
                    log(f"    {line}")
                break
            if not await wait_agent(a.host):
                log("  box did not come back - stopping")
                break
            log("  agent back")
        health = await board_ok(a.host)
        if health is False:
            log("  BOARD WEDGED before capturing - skipping this config")
            rows.append((cfg, "-", "board-wedged-on-boot"))
            continue

        for g in games:
            try:
                res_, err = await g.capture(
                    a.host, w, h, a.depth,
                    {"FX_GLIDE_SWAPINTERVAL": "0"}, a.timeout)
            except Exception as e:
                res_, err = None, f"{type(e).__name__}: {e}"
            if err:
                log(f"  {g.name:<32} FAILED: {err}")
                rows.append((cfg, g.tid, f"FAILED: {err}"))
                # A dead agent here means the wedge took it down; the watchdog
                # restarts it, and the next config's reboot clears the board.
                if not await wait_agent(a.host, timeout=240):
                    log("  agent gone and not recovered - stopping")
                    return 3
                continue
            name, data = res_
            p, size = save(outdir, cfg, g.tid, a.res, name, data)
            log(f"  {g.name:<32} {p.name}  {len(data)} bytes"
                f"{f'  {size[0]}x{size[1]}' if size else ''}")
            rows.append((cfg, g.tid, p.name))

    log("")
    log("captures:")
    for cfg, game, what in rows:
        log(f"  cfg {cfg:<2} {game:<8} {what}")
    got = sum(1 for _, _, w_ in rows if not str(w_).startswith(("FAILED", "aa-", "board-")))
    log(f"{got} capture(s) of {len(rows)} attempted -> {outdir}")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", required=True)
    ap.add_argument("--configs", default="0,1,2,3,4,5,6,7,8")
    ap.add_argument("--games", default="quake3,quake2,rtcw")
    ap.add_argument("--res", default="1024x768",
                    help="one resolution is right for a QUALITY comparison - "
                         "edge quality at a fixed resolution is the point")
    ap.add_argument("--depth", type=int, default=16)
    ap.add_argument("--timeout", type=int, default=240)
    ap.add_argument("--no-reboot", action="store_true",
                    help="capture in the CURRENT boot; only safe for a single "
                         "config, since a second topology write wedges the driver")
    ap.add_argument("--outdir", default=None)
    a = ap.parse_args()
    a.configs = [int(x) for x in a.configs.split(",")]
    a.games = [g.strip() for g in a.games.split(",") if g.strip()]
    bad = [g for g in a.games if g not in GAMES]
    if bad:
        ap.error(f"unknown game(s) {bad}; have {sorted(GAMES)}")
    raise SystemExit(asyncio.run(amain(a)))


if __name__ == "__main__":
    main()
