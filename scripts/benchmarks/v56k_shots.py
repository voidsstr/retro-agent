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
out. Judging anti-aliasing quality from a capture that might be resampled, or
palette-mangled, or simply of a different buffer, would be worse than having no
screenshots: it would look like evidence.

So every capture here is taken by the ENGINE, through its own screenshot
command, writing a file the engine itself composed from the frame it drew.

WHY A FIXED FRAME OF A DEMO
---------------------------
Anti-aliasing can only be judged by comparing the SAME scene at different
settings. A screenshot taken "a few seconds in" lands on a different frame each
run and the comparison is then worthless - edge quality differences are subtler
than scene differences. Demo playback is deterministic, so `demo <name>` +
`wait <N frames>` + `screenshot` returns the identical viewpoint every time,
at every AA level, on every boot.

`wait` counts FRAMES, not seconds, which is what makes this work: the same N
is the same scene whether the card is managing 25 fps or 120.

ONE CONFIG PER BOOT
-------------------
Same constraint as the fps sweep: writing SSTH3_SLI_AA_CONFIGURATION twice in
one boot wedges the driver. So each config gets a clean boot - and because a
resolution change is NOT a topology change, one boot can capture every game at
that config.

Usage:
    python3 v56k_shots.py --host 192.168.1.124
    python3 v56k_shots.py --host 192.168.1.124 --configs 5,7,8 --games quake3
    python3 v56k_shots.py --host 192.168.1.124 --no-reboot --configs 5   # current boot
"""

import argparse
import asyncio
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

SECRET = "retro-agent-secret"
PROBE = r"C:\RETRO_AGENT\glideprobe.exe"
LABEL = {0: "1chip-noaa", 1: "1chip-2xaa", 2: "2chip-noaa", 3: "2chip-2xaa",
         4: "2chip-4xaa", 5: "4chip-noaa", 6: "4chip-2xaa", 7: "4chip-4xaa",
         8: "4chip-8xaa"}


def log(m):
    print(f"[{datetime.now().strftime('%H:%M:%S')}] {m}", flush=True)


# --------------------------------------------------------------------------- #
# agent plumbing
# --------------------------------------------------------------------------- #

async def cmd(ip, c, timeout=90):
    con = RetroConnection(ip, 9898)
    await con.connect(SECRET, timeout=20.0)
    try:
        st, d = await con.send_command(c, timeout=timeout)
        return st, d.decode("ascii", errors="replace")
    finally:
        await con.close()


async def upload(ip, remote, data):
    if isinstance(data, str):
        data = data.encode("ascii", errors="replace")
    con = RetroConnection(ip, 9898)
    await con.connect(SECRET, timeout=20.0)
    try:
        st, d = await con.send_command(f"UPLOAD {remote}", binary_payload=data,
                                       timeout=180)
        if st != 0:
            raise RetroProtocolError(d.decode("ascii", errors="replace"))
    finally:
        await con.close()


async def download(ip, remote):
    con = RetroConnection(ip, 9898)
    await con.connect(SECRET, timeout=20.0)
    try:
        return await con.command_binary(f"DOWNLOAD {remote}", timeout=180)
    except RetroProtocolError:
        return None
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
    await cmd(ip, f"REGWRITE HKLM {key} SSTH3_SLI_AA_CONFIGURATION REG_SZ {cfg}")
    st, out = await cmd(ip, f"REGREAD HKLM {key}")
    for v in json.loads(out).get("values", []):
        if v["name"].upper() == "SSTH3_SLI_AA_CONFIGURATION":
            return str(v["data"]).strip() == str(cfg)
    return False


async def wait_agent(ip, timeout=900):
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
# the games
# --------------------------------------------------------------------------- #

class IdTech:
    """Quake III / Quake II / RtCW - all take a cfg, a demo and `wait`.

    `screenshotJPEG` is preferred over `screenshot` where it exists: a 1600x1200
    TGA is 5.7 MB and has to come back over SMB1-era networking, and JPEG at
    default quality is more than enough to judge edge geometry.
    """

    def __init__(self, tid, name, root, exe, gamedir, demo, shotdir,
                 wait_frames, jpeg=True, extra_cvars=(), demo_cmd="demo"):
        self.tid, self.name, self.root, self.exe = tid, name, root, exe
        self.gamedir, self.demo, self.shotdir = gamedir, demo, shotdir
        self.wait_frames, self.jpeg = wait_frames, jpeg
        self.extra_cvars, self.demo_cmd = extra_cvars, demo_cmd
        self.cfg_path = rf"{root}\{gamedir}\v56kshot.cfg"
        self.bat = rf"{root}\V56KSHOT.BAT"

    def cfg(self, w, h, depth):
        shot = "screenshotJPEG" if self.jpeg else "screenshot"
        lines = ["// generated per capture by v56k_shots.py",
                 'seta r_glDriver "3dfxogl"',
                 'seta com_maxfps "0"', 'seta r_mode "-1"',
                 f'seta r_customwidth "{w}"', f'seta r_customheight "{h}"',
                 f'seta r_colorbits "{depth}"',
                 f'seta r_texturebits "{depth}"',
                 f'seta r_depthbits "{24 if depth>=32 else 16}"',
                 'seta r_fullscreen "1"', 'seta r_picmip "0"',
                 'seta r_swapInterval "0"', 'seta cg_drawFPS "0"',
                 'seta timedemo "1"']
        lines += list(self.extra_cvars)
        lines += [f'{self.demo_cmd} {self.demo}',
                  f'wait {self.wait_frames}',
                  shot,
                  'wait 40',
                  'quit', '']
        return "\r\n".join(lines)

    def bat_text(self, env):
        out = ["@echo off"] + [f"set {k}={v}" for k, v in env.items()]
        out += [f'cd /d "{self.root}"',
                (f'{self.exe} +set fs_basepath "{self.root}" '
                 f'+set fs_homepath "{self.root}" +set logfile 2 '
                 f'+exec v56kshot.cfg')]
        return "\r\n".join(out) + "\r\n"

    async def capture(self, ip, w, h, depth, env, timeout=180):
        shotdir = rf"{self.root}\{self.shotdir}"
        await cmd(ip, f'EXEC cmd /c del /f /q "{shotdir}\\*.*"', 60)
        await upload(ip, self.cfg_path, self.cfg(w, h, depth))
        await upload(ip, self.bat, self.bat_text(env))
        await cmd(ip, f'EXEC cmd /c taskkill /f /im "{self.exe}"', 40)
        await asyncio.sleep(2)
        await cmd(ip, f"EXECW {timeout} {self.bat}", timeout + 40)
        await cmd(ip, f'EXEC cmd /c taskkill /f /im "{self.exe}"', 40)
        st, out = await cmd(ip, f'EXEC cmd /c dir /b "{shotdir}" 2>&1', 60)
        names = [n.strip() for n in out.replace("\r", "").splitlines()
                 if n.strip() and "." in n and "not find" not in n.lower()
                 and "File Not" not in n]
        if not names:
            return None, "engine wrote no screenshot"
        data = await download(ip, rf"{shotdir}\{names[0]}")
        if not data:
            return None, f"could not download {names[0]}"
        return (names[0], data), None


GAMES = {
    "quake3": lambda: IdTech(
        "quake3", "Quake III Arena", r"C:\Games\Quake3-TeamArena",
        "quake3.exe", "baseq3", "four", r"baseq3\screenshots", 400),
    "quake2": lambda: IdTech(
        "quake2", "Quake II", r"C:\Games\Quake2Complete",
        "quake2.exe", "baseq2", "demo1.dm2", r"baseq2\scrnshot", 300,
        jpeg=False, demo_cmd="demomap",
        extra_cvars=('set vid_ref "gl"', 'set gl_driver "3dfxgl"',
                     'set gl_picmip "0"', 'set vid_fullscreen "1"')),
    "rtcw": lambda: IdTech(
        "rtcw", "Return to Castle Wolfenstein",
        r"C:\Games\ReturnToCastleWolfenstein", "WolfSP.exe", "main",
        "demo1", r"main\screenshots", 300),
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
    # Convert to PNG for the article if Pillow can read it (TGA/JPEG both fine).
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
    log(f"image-quality pass on {a.host}: {len(a.configs)} config(s) x "
        f"{len(games)} game(s) at {a.res}")
    log(f"output -> {outdir}")

    w, h = (int(x) for x in a.res.lower().split("x"))
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
            log("  BOARD WEDGED before capturing - stopping")
            break

        for g in games:
            try:
                res_, err = await g.capture(a.host, w, h, a.depth,
                                            {"SSTH3_SLI_AA_CONFIGURATION": str(cfg),
                                             "FX_GLIDE_SWAPINTERVAL": "0"},
                                            a.timeout)
            except Exception as e:
                res_, err = None, f"{type(e).__name__}: {e}"
            if err:
                log(f"  {g.name:<32} FAILED: {err}")
                rows.append((cfg, g.tid, err))
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
    ap.add_argument("--timeout", type=int, default=180)
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
