#!/usr/bin/env python3
"""
v56k_bench.py - Voodoo 5 6000 benchmark campaign runner.

Built for the specpicks.com article comparing the AmigaMerlin driver stack
against our own builds on a real Voodoo 5 6000 AGP (4 x VSA-100, 128 MB).

Why this exists alongside benchmark_runner.py
---------------------------------------------
benchmark_runner.py models a run as (title x resolution x colordepth x fsaa)
but it NEVER APPLIES the fsaa value - it only logs "apply via the card's
driver profile if configured" and then writes the requested level into the CSV
as though it had taken effect.  On a card whose whole selling point is RGSS
anti-aliasing that produces an article's worth of confidently mislabeled rows:
"4x" numbers that were rendered with no AA at all.  This runner applies the
setting, reads it back, and refuses to record a row it could not verify.

The configuration axis
----------------------
On the VSA-100 stack the chip count and the AA level are not two knobs, they
are one: SSTH3_SLI_AA_CONFIGURATION.  The enumeration is not guessed - it is
read out of the driver's own 3dfx Tools descriptors on the card
(Settings\\Glide\\{Single,Dual,Quad}ChipAASLI, each of which carries a "List"
of human labels and a "Tweak Map" of the values they write):

    cfg  chips  samples   tools label
      0    1      1       "Single Chip Only"
      1    1      2       SingleChipAASLI "2-Sample Anti-Aliasing"
      2    2      1       DualChipAASLI  "Fastest Performance"
      3    2      2       DualChipAASLI  "2 Sample Anti-Aliasing"
      4    2      4       DualChipAASLI  "4 Sample Anti-Aliasing"
      5    4      1       QuadChipAASLI  "Fastest Performance"
      6    4      2       QuadChipAASLI  "2 Sample Anti-Aliasing"
      7    4      4       QuadChipAASLI  "4 Sample Anti-Aliasing"
      8    4      8       QuadChipAASLI  "8 Sample Anti-Aliasing"

cfg 8 is the reason this card exists: 8-sample RGSS needs four chips, so no
other 3dfx card - and nothing else of the era - can produce that row.

Applying it, and why both routes
--------------------------------
Glide takes these from the environment; 3dfx Tools persists them in the
display class's Settings\\Glide key.  This runner writes BOTH: the registry
value (so the state is what 3dfx Tools would have set, and is inspectable
after the fact) and an environment variable in the generated launcher (so the
value the game process actually sees is not a guess).  The registry write is
read back, because REGWRITE answers OK for a write that silently created a
subkey instead of setting a value.

Results are appended to the CSV after every single run, so an interrupted
campaign keeps everything it had measured, and re-running skips rows already
present (--resume, the default).
"""

import argparse
import asyncio
import csv
import json
import os
import re
import sys
import time
from datetime import datetime, timezone
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from client.retro_protocol import RetroConnection, RetroProtocolError  # noqa: E402

SECRET = "retro-agent-secret"
PORT = 9898

# The display class key on this box; resolved at runtime but this is the shape.
GLIDE_KEY_TMPL = (r"SYSTEM\CurrentControlSet\Control\Class"
                  r"\{{4D36E968-E325-11CE-BFC1-08002BE10318}}\{inst}\Settings\Glide")

AA_CONFIGS = {
    0: {"chips": 1, "samples": 1, "label": "1chip-noaa"},
    1: {"chips": 1, "samples": 2, "label": "1chip-2xaa"},
    2: {"chips": 2, "samples": 1, "label": "2chip-noaa"},
    3: {"chips": 2, "samples": 2, "label": "2chip-2xaa"},
    4: {"chips": 2, "samples": 4, "label": "2chip-4xaa"},
    5: {"chips": 4, "samples": 1, "label": "4chip-noaa"},
    6: {"chips": 4, "samples": 2, "label": "4chip-2xaa"},
    7: {"chips": 4, "samples": 4, "label": "4chip-4xaa"},
    8: {"chips": 4, "samples": 8, "label": "4chip-8xaa"},
}

# WHAT ACTUALLY WEDGES THIS DRIVER: the NUMBER of topology changes since
# boot, not any particular configuration.
#
# This corrects an earlier reading of the same data. cfg 2 and cfg 8 were
# recorded as "box-killers" because each hung the display driver and took the
# agent with it. Then the card moved to .124 and cfg 5 - the DEFAULT, and a
# configuration that had already produced 150 fps on .191 - hung in exactly the
# same way. Lining up every hang by position in its boot session:
#
#   .191  cfg 0 ok -> cfg 5 ok                 2 changes, fine
#   .191  cfg 0 ok -> cfg 1 ok -> cfg 2 HUNG   3rd change
#   .191  cfg 5 ok -> cfg 8 HUNG               2nd change
#   .124  cfg 0 ok -> cfg 1 ok -> cfg 5 HUNG   3rd change
#   .124  (after reboot) cfg 5 ok              1st, fine
#
# cfg 5 hung as a 3rd change and worked as a 1st and as a 2nd. So no config is
# inherently bad; writing SSTH3_SLI_AA_CONFIGURATION repeatedly within one boot
# is what breaks it, on the second or third write. Everything measured after a
# hang is measured against broken hardware - which is how SIX false "wedges the
# driver" verdicts were produced on .124 in one screening run.
#
# The consequence for method: ONE AA CONFIG PER BOOT. A multi-config sweep in a
# single session cannot produce trustworthy numbers no matter how it is
# ordered. board_alive() exists so a wedge is detected and the campaign stops
# instead of blaming the next config for it.
#
# These two stay excluded by default only because a hang costs a trip to the
# machine, not because they are known-bad: on the evidence they are no worse
# than cfg 5.
HAZARD_CONFIGS = {
    2: "hung as a 3rd topology change on .191; NOT shown to be bad in itself "
       "- measure it in its own boot",
    8: "hung as a 2nd topology change on .191; NOT shown to be bad in itself "
       "- 8-sample AA is the card's headline feature and deserves its own boot",
}

# Never measured at all, in any boot. Kept separate from HAZARD_CONFIGS
# because "we looked and it failed" and "we never looked" must not render the
# same.
SUSPECT_CONFIGS = {
    3: "UNTESTED in a clean boot",
    4: "UNTESTED in a clean boot",
}

CSV_COLS = ["stamp", "title", "engine", "api", "res", "width", "height",
            "colordepth",
            "aa_cfg", "chips", "samples", "aa_label", "avg_fps", "frames",
            "seconds", "gl_renderer", "gl_vendor", "mode_line", "pixelformat",
            "aa_verified", "status", "notes"]


def log(msg):
    print(f"[{datetime.now().strftime('%H:%M:%S')}] {msg}", flush=True)


# --------------------------------------------------------------------------- #
# agent plumbing
# --------------------------------------------------------------------------- #

class Box:
    """One agent connection, reopened per command batch.

    The agent is single-threaded and CLAUDE.md is explicit that a connection
    should be held briefly and closed gracefully, so this opens a connection
    per call rather than keeping one alive across a multi-minute game run.
    """

    def __init__(self, ip):
        self.ip = ip

    async def cmd(self, command, timeout=60.0):
        c = RetroConnection(self.ip, PORT)
        await c.connect(SECRET, timeout=20.0)
        try:
            st, d = await c.send_command(command, timeout=timeout)
            return st, d.decode("ascii", errors="replace")
        finally:
            await c.close()

    async def text(self, command, timeout=60.0):
        st, out = await self.cmd(command, timeout)
        if st != 0:
            raise RetroProtocolError(out.strip())
        return out

    async def exec_(self, cmdline, timeout=90.0):
        st, out = await self.cmd(f"EXEC {cmdline}", timeout)
        return out

    async def upload(self, remote, data):
        if isinstance(data, str):
            data = data.encode("ascii", errors="replace")
        c = RetroConnection(self.ip, PORT)
        await c.connect(SECRET, timeout=20.0)
        try:
            st, d = await c.send_command(f"UPLOAD {remote}", binary_payload=data)
            if st != 0:
                raise RetroProtocolError(d.decode("ascii", errors="replace"))
        finally:
            await c.close()

    async def download(self, remote):
        """Return file bytes, or None when it does not exist.

        DOWNLOAD, never `EXEC type` - reading a file through EXEC has taken a
        Win9x box down twice in this project, and it buffers the whole file
        through the command path even on XP.
        """
        c = RetroConnection(self.ip, PORT)
        await c.connect(SECRET, timeout=20.0)
        try:
            return await c.command_binary(f"DOWNLOAD {remote}", timeout=120.0)
        except RetroProtocolError:
            return None
        finally:
            await c.close()


# --------------------------------------------------------------------------- #
# the card's AA / SLI configuration
# --------------------------------------------------------------------------- #

async def find_display_instance(box):
    """Return the display-class instance subkey driving the desktop.

    HWPROFILE follows EnumDisplayDevices(ATTACHED_TO_DESKTOP) and then that
    adapter's own DeviceKey, which is the only reliable route: VIDEODIAG's
    adapters[0] can be a stale registry key for a card that is no longer
    fitted, and this fleet swaps cards constantly.
    """
    out = await box.text("HWPROFILE")
    prof = json.loads(out)
    for c in prof.get("video_cards", []):
        if c.get("attached_to_desktop"):
            return c.get("instance", "0000"), prof
    return "0000", prof


async def apply_aa_config(box, glide_key, cfg):
    """Write SSTH3_SLI_AA_CONFIGURATION and prove it landed.

    Returns (ok, readback).  Never trust the OK from REGWRITE: given a path
    with the value name folded in it creates a subkey and still answers OK.
    """
    await box.text(f"REGWRITE HKLM {glide_key} SSTH3_SLI_AA_CONFIGURATION REG_SZ {cfg}")
    out = await box.text(f"REGREAD HKLM {glide_key}")
    got = None
    for v in json.loads(out).get("values", []):
        if v["name"].upper() == "SSTH3_SLI_AA_CONFIGURATION":
            got = str(v["data"]).strip()
    return (got == str(cfg)), got


# --------------------------------------------------------------------------- #
# titles
# --------------------------------------------------------------------------- #

# Both id shapes: Quake III / Quake II write "1260 frames, 8.2 seconds: 154.2
# fps"; GLQuake writes "1260 frames 8.2 seconds 154.2 fps" with no punctuation.
def renderer_up(raw):
    """Did the renderer finish coming up in this log?

    The marker is deliberately the renderer IDENTITY rather than "the window
    opened": every engine here prints its GL_RENDERER / bound render device
    only once the driver has answered, which is the line after which a silence
    means "rendering" instead of "stuck".
    """
    return bool(re.search(r"GL_RENDERER:|Bound to |GR_RENDERER:", raw or ""))


FPS_RE = re.compile(
    r"(\d+)\s+frames[,\s]+([\d.]+)\s+seconds[:,\s]+([\d.]+)\s*fps", re.I)


class Quake3:
    """Retail Quake III Arena 1.32c, `demo four` timedemo.

    The retail id binary is used rather than the ioquake3 beside it because
    r_glDriver lets the engine load the 3dfx ICD by name, which makes the
    measured stack unambiguous in the log; ioquake3 dropped that cvar and goes
    through SDL to whatever OPENGL32.DLL resolves to.
    """

    tid = "quake3"
    name = "Quake III Arena"
    engine = "quake3.exe (retail 1.32c)"
    proc = "quake3.exe"
    api = "opengl-icd"

    def supports(self, w, h, depth):
        return None

    def __init__(self, root=r"C:\Games\Quake3-TeamArena"):
        self.root = root
        self.log = rf"{root}\baseq3\qconsole.log"
        self.cfg = rf"{root}\baseq3\bench.cfg"
        self.bat = rf"{root}\V56KBENCH.BAT"

    # r_mode / r_customwidth / r_customheight / r_colorbits and friends are
    # CVAR_LATCH: read once at R_Init.  The first design set them in a cfg
    # exec'd from the command line (which lands AFTER R_Init) and then issued
    # vid_restart to pick them up.  That works, but it means every run builds a
    # renderer twice - once at whatever the saved q3config asked for, then
    # again at the target - and the tear-down/re-init is what hung the driver
    # solid at 4-chip 8x AA, taking the agent down with it.
    #
    # So the latched cvars are set BEFORE R_Init by two routes that agree:
    #   - fleetres.cfg, which the staged autoexec.cfg execs (CLAUDE.md: a seta
    #     there runs after Com_StartupVariable and before R_Init, and therefore
    #     beats the command line - so it has to carry the same values, not be
    #     left holding the launcher's last per-box resolution);
    #   - `+set` on the command line, which Com_StartupVariable applies.
    # Whichever wins, the number is identical, and the renderer is built once.
    def fleetres_cfg(self, w, h, depth):
        zbits = 24 if depth >= 32 else 16
        return "\r\n".join([
            '// written per run by v56k_bench.py - the launcher rewrites this',
            '// file at every start, so overwriting it is harmless.',
            'seta r_glDriver "3dfxogl"',
            'seta r_mode "-1"',
            f'seta r_customwidth "{w}"',
            f'seta r_customheight "{h}"',
            'seta r_customaspect "1"',
            'seta r_customPixelAspect "1"',
            f'seta r_colorbits "{depth}"',
            f'seta r_depthbits "{zbits}"',
            f'seta r_texturebits "{depth}"',
            'seta r_stencilbits "0"',
            'seta r_fullscreen "1"',
            'seta r_picmip "0"',
            'seta r_ext_compress_textures "0"',
            'seta r_vertexLight "0"',
            'seta r_subdivisions "4"',
            '',
        ])

    # Only NON-latched settings and the run itself; no vid_restart.
    def bench_cfg(self):
        return "\r\n".join([
            '// generated per run by v56k_bench.py - do not edit',
            'seta com_maxfps "0"',
            'seta r_swapInterval "0"',
            'seta r_finish "0"',
            'seta cg_drawFPS "1"',
            'seta timedemo "1"',
            'demo four',
            '',
        ])

    def setargs(self, w, h, depth):
        zbits = 24 if depth >= 32 else 16
        return (f'+set r_glDriver 3dfxogl +set r_mode -1 '
                f'+set r_customwidth {w} +set r_customheight {h} '
                f'+set r_customaspect 1 +set r_colorbits {depth} '
                f'+set r_depthbits {zbits} +set r_texturebits {depth} '
                f'+set r_stencilbits 0 +set r_fullscreen 1 +set r_picmip 0')

    def launch_bat(self, w, h, depth, env):
        # `start` does not inherit the caller's working directory here - a bare
        # `cd /d ... && start game.exe` left the engine with fs_basepath C:\,
        # zero pk3s and "Running in restricted demo mode".  So: a .bat that cds
        # and runs the exe directly, with fs paths stated explicitly.
        lines = ["@echo off"]
        for k, v in env.items():
            lines.append(f'set {k}={v}')
        lines += [
            f'cd /d "{self.root}"',
            (f'quake3.exe +set fs_basepath "{self.root}" '
             f'+set fs_homepath "{self.root}" +set logfile 2 '
             f'{self.setargs(w, h, depth)} +exec bench.cfg'),
        ]
        return "\r\n".join(lines) + "\r\n"

    async def prepare(self, box, w, h, depth, env):
        await box.upload(rf"{self.root}\baseq3\fleetres.cfg",
                         self.fleetres_cfg(w, h, depth))
        await box.upload(self.cfg, self.bench_cfg())
        await box.upload(self.bat, self.launch_bat(w, h, depth, env))
        await box.exec_(f'cmd /c del /f /q "{self.log}"')

    async def start(self, box):
        await box.text(f"LAUNCH {self.bat}")

    def parse(self, raw):
        m = None
        for m in FPS_RE.finditer(raw):
            pass                      # keep the LAST timedemo in the log
        if not m:
            return None
        return {"frames": int(m.group(1)), "seconds": float(m.group(2)),
                "avg_fps": float(m.group(3))}

    def attribution(self, raw):
        """Pull the renderer identity from the FINAL renderer init in the log.

        The first init is whatever the saved q3config asked for; the run we are
        measuring is the one after bench.cfg's vid_restart, so the last block
        is the one that describes the measured frames.
        """
        out = {}
        for key, pat in (("gl_renderer", r"GL_RENDERER:\s*(.+)"),
                         ("gl_vendor", r"GL_VENDOR:\s*(.+)"),
                         ("mode_line", r"(MODE:\s*.+)"),
                         ("pixelformat", r"(PIXELFORMAT:\s*.+)")):
            hits = re.findall(pat, raw)
            if hits:
                out[key] = hits[-1].strip()
        return out


class Quake2:
    """Quake II 3.20, `timedemo 1` + `demomap demo1.dm2`.

    Runs through the 3dfx MiniGL (`3dfxgl.dll`, which is what the staged tree
    ships beside the exe) via ref_gl, so this measures the same Glide silicon
    through a different OpenGL front end than Quake III's full ICD.

    id Tech 2 has NO custom-resolution path: gl_mode indexes a fixed table, so
    a resolution outside it cannot be asked for at all.  1280x1024 in
    particular is absent - the table's 1280 entry is 1280x960 - which is a real
    engine limit, not a driver one, and is reported as such rather than being
    silently rounded.
    """

    tid = "quake2"
    name = "Quake II"
    engine = "quake2.exe (3.20)"
    proc = "quake2.exe"
    api = "opengl-minigl"

    # id Tech 2's fixed mode table.
    MODES = {(320, 240): 0, (400, 300): 1, (512, 384): 2, (640, 480): 3,
             (800, 600): 4, (960, 720): 5, (1024, 768): 6, (1152, 864): 7,
             (1280, 960): 8, (1600, 1200): 9}

    def __init__(self, root=r"C:\Games\Quake2Complete"):
        self.root = root
        self.log = rf"{root}\baseq2\qconsole.log"
        self.cfg = rf"{root}\baseq2\bench.cfg"
        self.bat = rf"{root}\V56KBENCH.BAT"

    def supports(self, w, h, depth):
        if (w, h) not in self.MODES:
            return (f"id Tech 2 has no {w}x{h} mode - gl_mode is a fixed table "
                    f"({', '.join(f'{a}x{b}' for a, b in sorted(self.MODES))})")
        return None

    def bench_cfg(self):
        return "\r\n".join([
            '// generated per run by v56k_bench.py',
            'set cl_maxfps "1000"',
            'set gl_swapinterval "0"',
            'set gl_picmip "0"',
            'set gl_finish "0"',
            'set timedemo "1"',
            'demomap demo1.dm2',
            '',
        ])

    def launch_bat(self, w, h, depth, env):
        mode = self.MODES[(w, h)]
        lines = ["@echo off"]
        for k, v in env.items():
            lines.append(f'set {k}={v}')
        lines += [
            f'cd /d "{self.root}"',
            (f'quake2.exe +set basedir "{self.root}" +set vid_ref gl '
             f'+set gl_driver 3dfxgl +set gl_mode {mode} '
             f'+set gl_bitdepth {16 if depth < 32 else 0} '
             f'+set vid_fullscreen 1 +set logfile 2 +exec bench.cfg'),
        ]
        return "\r\n".join(lines) + "\r\n"

    async def prepare(self, box, w, h, depth, env):
        await box.upload(self.cfg, self.bench_cfg())
        await box.upload(self.bat, self.launch_bat(w, h, depth, env))
        await box.exec_(f'cmd /c del /f /q "{self.log}"')

    async def start(self, box):
        await box.text(f"LAUNCH {self.bat}")

    def parse(self, raw):
        m = None
        for m in FPS_RE.finditer(raw):
            pass
        if not m:
            return None
        return {"frames": int(m.group(1)), "seconds": float(m.group(2)),
                "avg_fps": float(m.group(3))}

    def attribution(self, raw):
        out = {}
        for key, pat in (("gl_renderer", r"GL_RENDERER:\s*(.+)"),
                         ("gl_vendor", r"GL_VENDOR:\s*(.+)"),
                         ("mode_line", r"(MODE:\s*.+|setting mode \d+:.*)")):
            hits = re.findall(pat, raw)
            if hits:
                out[key] = hits[-1].strip()
        return out


class GLQuake:
    """GLQuake, `timedemo demo1`.

    The oldest OpenGL path on the box and the one closest to a pure 3dfx MiniGL
    workload.  CLAUDE.md records that GLQuake refuses to go above 1280x960, so
    1600x1200 is declared unsupported here rather than measured and blamed on
    the card.
    """

    tid = "glquake"
    name = "GLQuake"
    engine = "GLQUAKE.EXE"
    proc = "glquake.exe"
    api = "opengl-minigl"

    def __init__(self, root=r"C:\Games\Quake1"):
        self.root = root
        self.log = rf"{root}\id1\qconsole.log"
        self.cfg = rf"{root}\id1\bench.cfg"
        self.bat = rf"{root}\V56KBENCH.BAT"

    def supports(self, w, h, depth):
        if w > 1280 or h > 1024:
            return "GLQuake refuses modes above 1280x960 (measured, CLAUDE.md)"
        return None

    def bench_cfg(self):
        return "\r\n".join(['host_maxfps "1000"', 'timedemo demo1', ''])

    def launch_bat(self, w, h, depth, env):
        lines = ["@echo off"]
        for k, v in env.items():
            lines.append(f'set {k}={v}')
        lines += [
            f'cd /d "{self.root}"',
            (f'GLQUAKE.EXE -width {w} -height {h} -bpp {depth} -fullscreen '
             f'-condebug +exec bench.cfg'),
        ]
        return "\r\n".join(lines) + "\r\n"

    async def prepare(self, box, w, h, depth, env):
        await box.upload(self.cfg, self.bench_cfg())
        await box.upload(self.bat, self.launch_bat(w, h, depth, env))
        await box.exec_(f'cmd /c del /f /q "{self.log}"')

    async def start(self, box):
        await box.text(f"LAUNCH {self.bat}")

    def parse(self, raw):
        m = None
        for m in FPS_RE.finditer(raw):
            pass
        if not m:
            return None
        return {"frames": int(m.group(1)), "seconds": float(m.group(2)),
                "avg_fps": float(m.group(3))}

    def attribution(self, raw):
        out = {}
        for key, pat in (("gl_renderer", r"GL_RENDERER:\s*(.+)"),
                         ("gl_vendor", r"GL_VENDOR:\s*(.+)")):
            hits = re.findall(pat, raw)
            if hits:
                out[key] = hits[-1].strip()
        return out


class Unreal1:
    """Unreal Engine 1 (UT99 / Unreal Gold / Deus Ex), `-benchmark -seconds=N`.

    This is the family that matters most on a Voodoo, because it is the only
    one here with a NATIVE GLIDE render device: GlideDrv.GlideRenderDevice
    talks to glide2x/glide3x directly with no OpenGL in the path at all.  Each
    title is therefore registered twice, `<tid>:glide` and `<tid>:opengl`, so
    the same card can be measured through both APIs.

    The render device and resolution are patched into the game's OWN ini
    (backed up once, first touch) rather than supplied through `-ini=`: UE1
    expects a complete ini and the partial override template that shipped with
    the old harness is not one.

    NOTE on Unreal Gold: its staged System\\ carries a 1,310,720-byte
    glide2x.dll which is the nGlide WRAPPER, and game-local wins at load time -
    so on this box, the one with real Glide silicon, the staged library
    guarantees the card is bypassed.  --neutralize-wrappers renames it aside
    for the run.
    """

    api_devices = {
        "glide": "GlideDrv.GlideRenderDevice",
        "opengl": "OpenGLDrv.OpenGLRenderDevice",
        "d3d": "D3DDrv.D3DRenderDevice",
    }

    def __init__(self, tid, name, root, exe, ini, logname, mapname, api="glide",
                 seconds=60):
        self.tid = f"{tid}:{api}"
        self.name = name
        self.api = api
        self.engine = f"{exe} ({self.api_devices[api].split('.')[0]})"
        self.proc = exe
        self.root = root
        self.exe = exe
        self.ini = rf"{root}\System\{ini}"
        self.log = rf"{root}\System\{logname}"
        self.bat = rf"{root}\System\V56KBENCH.BAT"
        self.mapname = mapname
        self.seconds = seconds
        self.wrapper = rf"{root}\System\glide2x.dll"

    def supports(self, w, h, depth):
        return None

    async def _patch_ini(self, box, w, h, depth):
        raw = await box.download(self.ini)
        if raw is None:
            raise RetroProtocolError(f"cannot read {self.ini}")
        # back up once so the box is recoverable to its staged state
        if await box.download(self.ini + ".v56kbak") is None:
            await box.upload(self.ini + ".v56kbak", raw)
        text = raw.decode("latin-1")
        wanted = {
            "Engine.Engine": {"GameRenderDevice": self.api_devices[self.api]},
            "WinDrv.WindowsClient": {
                "FullscreenViewportX": str(w),
                "FullscreenViewportY": str(h),
                "FullscreenColorBits": str(depth),
                "StartupFullscreen": "True",
                "UseJoystick": "False",
            },
            self.api_devices[self.api].split(".")[0] + "."
            + self.api_devices[self.api].split(".")[1]: {"UseVSync": "False"},
        }
        out, section = [], None
        seen = {s: set() for s in wanted}
        for line in text.splitlines():
            st = line.strip()
            if st.startswith("[") and st.endswith("]"):
                section = st[1:-1]
            elif section in wanted:
                key = st.split("=", 1)[0].strip() if "=" in st else None
                if key and key in wanted[section]:
                    out.append(f"{key}={wanted[section][key]}")
                    seen[section].add(key)
                    continue
            out.append(line)
        # append any key the ini did not already carry
        for sec, kv in wanted.items():
            missing = {k: v for k, v in kv.items() if k not in seen.get(sec, ())}
            if missing:
                out.append(f"[{sec}]")
                out += [f"{k}={v}" for k, v in missing.items()]
        await box.upload(self.ini, ("\r\n".join(out) + "\r\n").encode("latin-1"))

    def launch_bat(self, env):
        lines = ["@echo off"]
        for k, v in env.items():
            lines.append(f'set {k}={v}')
        lines += [
            rf'cd /d "{self.root}\System"',
            (f'{self.exe} {self.mapname}?quickstart=true -benchmark '
             f'-seconds={self.seconds} -nosound'),
        ]
        return "\r\n".join(lines) + "\r\n"

    async def prepare(self, box, w, h, depth, env):
        await self._patch_ini(box, w, h, depth)
        await box.upload(self.bat, self.launch_bat(env))
        await box.exec_(f'cmd /c del /f /q "{self.log}"')

    async def start(self, box):
        await box.text(f"LAUNCH {self.bat}")

    def parse(self, raw):
        # UE1's -benchmark line shape varies by build; accept the usual ones and
        # keep the raw log either way so a miss is calibratable rather than lost.
        for pat in (r"Avg(?:erage)?(?:\s*FPS)?[:=\s]+([\d.]+)",
                    r"([\d.]+)\s*(?:average\s*)?fps",
                    r"Frames\D+(\d+)\D+Time\D+([\d.]+)"):
            m = re.search(pat, raw, re.I)
            if m and m.lastindex == 1:
                return {"avg_fps": float(m.group(1))}
            if m and m.lastindex == 2:
                fr, t = float(m.group(1)), float(m.group(2))
                if t > 0:
                    return {"frames": int(fr), "seconds": t,
                            "avg_fps": round(fr / t, 2)}
        return None

    def attribution(self, raw):
        out = {}
        m = re.findall(r"(?:Bound to|Initialized)\s+(\S*(?:Glide|OpenGL|D3D)\S*)",
                       raw, re.I)
        if m:
            out["gl_renderer"] = m[-1].strip()
        m = re.findall(r"(Resolution[^\r\n]*|Setting res[^\r\n]*)", raw, re.I)
        if m:
            out["mode_line"] = m[-1].strip()
        return out


class SeriousSam:
    """Serious Sam (Serious Engine 1), recorded demo with dem_bProfile.

    sam_iGfxAPI selects the renderer: 0 = OpenGL, 1 = Direct3D.  The engine
    writes its timing summary to SeriousSam.log in the game root.
    """

    proc = "SeriousSam.exe"

    def __init__(self, tid, name, root, demo="auto-demo0001.dem", api="opengl"):
        self.tid = f"{tid}:{api}"
        self.name = name
        self.api = api
        self.engine = f"SeriousSam.exe ({api})"
        self.root = root
        self.log = rf"{root}\SeriousSam.log"
        self.cfg = rf"{root}\Scripts\v56kbench.ini"
        self.bat = rf"{root}\V56KBENCH.BAT"
        self.demo = demo

    def supports(self, w, h, depth):
        return None

    def bench_cfg(self, w, h, depth):
        api = {"opengl": 0, "d3d": 1}[self.api]
        return "\r\n".join([
            '// generated per run by v56k_bench.py',
            'sam_bAutoAdjustAudio=0;',
            f'sam_iGfxAPI={api};',
            'sam_bFullScreenActive=1;',
            f'sam_iScreenSizeI={w};',
            f'sam_iScreenSizeJ={h};',
            f'sam_iDisplayDepth={1 if depth < 32 else 2};',
            'sam_bWaitForVSync=0;',
            'dem_bProfile=1;',
            'dem_iProfileCPU=0;',
            'dem_bOnScreenDisplay=1;',
            f'StartDemoPlay("Demos\\\\{self.demo}", 0, 0);',
            '',
        ])

    def launch_bat(self, env):
        lines = ["@echo off"]
        for k, v in env.items():
            lines.append(f'set {k}={v}')
        lines += [
            f'cd /d "{self.root}"',
            r'Bin\SeriousSam.exe +game SeriousSam +exec Scripts\v56kbench.ini',
        ]
        return "\r\n".join(lines) + "\r\n"

    async def prepare(self, box, w, h, depth, env):
        await box.upload(self.cfg, self.bench_cfg(w, h, depth))
        await box.upload(self.bat, self.launch_bat(env))
        await box.exec_(f'cmd /c del /f /q "{self.log}"')

    async def start(self, box):
        await box.text(f"LAUNCH {self.bat}")

    def parse(self, raw):
        m = re.search(r"Average(?:\s*FPS)?[:=\s]+([\d.]+)", raw, re.I)
        if m:
            return {"avg_fps": float(m.group(1))}
        m = re.search(r"([\d.]+)\s*fps", raw, re.I)
        return {"avg_fps": float(m.group(1))} if m else None

    def attribution(self, raw):
        out = {}
        for key, pat in (("gl_renderer", r"Renderer:?\s*(.+)|Vendor:?\s*(.+)"),
                         ("mode_line", r"(Display mode[^\r\n]*)")):
            hits = re.findall(pat, raw, re.I)
            if hits:
                h = hits[-1]
                out[key] = (h if isinstance(h, str) else
                            next((x for x in h if x), "")).strip()
        return out


def _ut(api):
    return Unreal1("ut", "Unreal Tournament", r"C:\Games\UnrealTournament436",
                   "UnrealTournament.exe", "UnrealTournament.ini",
                   "UnrealTournament.log", "DM-Morpheus", api=api)


def _ugold(api):
    return Unreal1("unrealgold", "Unreal Gold", r"C:\Games\UnrealGold",
                   "Unreal.exe", "Unreal.ini", "Unreal.log",
                   # Morpheus is a UT map; Unreal Gold's DM set is DmDeck16,
                   # DmAriza, DmCurse, ... - checked against the staged tree.
                   "DmDeck16", api=api)


def _deusex(api):
    return Unreal1("deusex", "Deus Ex", r"C:\Games\DeusEx", "DeusEx.exe",
                   "DeusEx.ini", "DeusEx.log", "00_Training", api=api)


TITLES = {
    "quake3": lambda api=None: Quake3(),
    "quake2": lambda api=None: Quake2(),
    "glquake": lambda api=None: GLQuake(),
    "ut": lambda api="glide": _ut(api),
    "unrealgold": lambda api="glide": _ugold(api),
    "deusex": lambda api="glide": _deusex(api),
    "serioussam": lambda api="opengl": SeriousSam(
        "serioussam", "Serious Sam - The First Encounter",
        r"C:\Games\SeriousSamFirstEncounter", api=api),
    "serioussam2": lambda api="opengl": SeriousSam(
        "serioussam2", "Serious Sam - The Second Encounter",
        r"C:\Games\SeriousSamSecondEncounter", api=api),
}


# --------------------------------------------------------------------------- #
# one run
# --------------------------------------------------------------------------- #

# --------------------------------------------------------------------------- #
# game-local DLL safety - a wrong glide beside the exe is not a small mistake
# --------------------------------------------------------------------------- #

# Game-local wins at load time, so a DLL sitting beside a game's exe decides
# which Glide that game gets regardless of what is in system32.  Two are known
# to be staged in this library, and they are NOT the same kind of problem:
#
#   989,027  the clean-room/open glide3x_h5.dll.  FINDINGS 2026-09-04: on this
#            4-chip board it does not hang the game, it HARD-FREEZES THE BOX -
#            100% ping loss, agent gone, physical power cycle.  It is staged
#            game-local in Quake2Complete right now.  Refuse to run rather
#            than discover that again unattended.
#   1,310,720  the nGlide WRAPPER (contains "E:\glide\nglide\logs\log.wri"),
#            staged in UnrealGold\System and Carmageddon2.  It translates
#            Glide to Direct3D, so on the one box with real Glide silicon it
#            guarantees the card is bypassed - which already cost a session on
#            .171.  Harmless to move aside, so move it aside and say so.
DANGEROUS_GLIDE = {989027: ("open-glide3x-h5",
                            "clean-room Glide: hard-freezes this 4-chip board "
                            "(FINDINGS 2026-09-04) - physical power cycle")}
WRAPPER_GLIDE = {1310720: ("nglide-wrapper",
                           "nGlide translates Glide to D3D and shadows the "
                           "real card")}


async def preflight_title_dlls(box, title, allow_open_glide=False):
    """Inspect the DLLs beside this title's exe before measuring it.

    Returns (ok, notes).  ok=False means do not run this title at all.
    """
    notes = []
    root = getattr(title, "root", None)
    if not root:
        return True, ""
    out = await box.exec_(
        f'cmd /c dir /s /b /-c "{root}\\glide2x.dll" "{root}\\glide3x.dll" 2>&1',
        timeout=120)
    for line in out.splitlines():
        path = line.strip()
        if not path.lower().endswith(".dll") or ":" not in path:
            continue
        size_out = await box.exec_(f'cmd /c for %I in ("{path}") do @echo %~zI',
                                   timeout=60)
        try:
            size = int(size_out.strip().splitlines()[0])
        except (ValueError, IndexError):
            notes.append(f"could not size {path}")
            continue
        if size in DANGEROUS_GLIDE:
            tag, why = DANGEROUS_GLIDE[size]
            log(f"    !! REFUSING {title.name}: {path} is the {tag} ({size} B)")
            log(f"       {why}")
            return False, f"blocked: {tag} at {path} - {why}"
        if size in WRAPPER_GLIDE:
            tag, why = WRAPPER_GLIDE[size]
            await box.exec_(f'cmd /c move /Y "{path}" "{path}.v56kbak"', timeout=60)
            still = await box.exec_(f'cmd /c if exist "{path}" echo STILL_THERE',
                                    timeout=60)
            if "STILL_THERE" in still:
                log(f"    !! could not move {tag} aside at {path}")
                return False, f"blocked: {tag} at {path} could not be moved aside"
            log(f"    moved {tag} aside: {path} ({why})")
            notes.append(f"{tag} moved aside at {path}")
    return True, "; ".join(notes)


async def board_alive(box, probe=r"C:\RETRO_AGENT\glideprobe.exe"):
    """Is the BOARD still able to bring Glide up, or is it wedged?

    This is a different question from agent liveness and the difference is
    expensive. Screening the nine AA configs on .124 produced six "wedges the
    driver" verdicts that were all FALSE: one config wedged the board, the
    agent survived, and every later config was then measured against broken
    hardware and blamed for it. cfg 5 was among the six and works perfectly
    from a fresh boot.

    So a failed cell must be followed by asking the board whether IT is the
    broken thing, exactly as a negative result about a Windows file has to be
    re-run case-insensitively before it is reportable.

    Returns True (healthy), False (wedged), or None (could not tell).
    """
    try:
        st, out = await box.cmd(
            f"EXECW 90 {probe} --res 640x480 --noopen "
            r"--dll C:\WINDOWS\system32\glide3x.dll "
            r"--log C:\RETRO_AGENT\probe-health.log", timeout=140)
    except Exception:
        return None
    if "RESULT: probe-ok-noopen" in out:
        return True
    if "timed out" in out or "grGlideInit" in out:
        return False
    return None


async def agent_alive(box, tries=3):
    """Protocol-level liveness.

    A successful TCP connect proves nothing here: when the agent dies its alt
    listener on 9897 stays bound and keeps ACCEPTING while answering nothing,
    so a connect-only check reports a dead agent as healthy.  Only a completed
    PING round trip counts.
    """
    for _ in range(tries):
        try:
            st, out = await box.cmd("PING", timeout=25)
            if "PONG" in out.upper():
                return True
        except Exception:
            pass
        await asyncio.sleep(5)
    return False


async def quiesce(box):
    """Kill the background CPU thieves before measuring.

    These boxes are single core; CLAUDE.md records that a bench with
    retro-infer.exe live reads several fps low.  3dfxMan.exe is the 3dfx Tools
    tray app and belongs in the list on this card.
    """
    try:
        await box.cmd("AI_DISABLE")
    except Exception:
        pass
    for p in ("retro-infer.exe", "rotate_wall.exe", "wuauclt.exe", "3dfxMan.exe",
              "daemon.exe", "wmiprvse.exe", "dwwin.exe", "dumprep.exe"):
        try:
            await box.exec_(f'cmd /c taskkill /f /im "{p}"', timeout=30)
        except Exception:
            pass


async def run_one(box, title, w, h, depth, cfg, glide_key, args):
    meta = AA_CONFIGS[cfg]
    res = f"{w}x{h}"
    row = {c: "" for c in CSV_COLS}
    row.update({
        "stamp": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "title": title.name, "engine": title.engine, "res": res,
        "width": w, "height": h, "colordepth": depth, "aa_cfg": cfg,
        "chips": meta["chips"], "samples": meta["samples"],
        "aa_label": meta["label"], "api": getattr(title, "api", ""),
        "status": "pending",
    })
    log(f"--- {title.name} [{getattr(title,'api','')}]  {res}x{depth}  "
        f"cfg={cfg} ({meta['label']})")

    # An engine that cannot express this mode is a fact about the ENGINE, and
    # recording it as such keeps it out of the card's column.  Checked before
    # the card is reconfigured so a skipped cell costs nothing.
    why = title.supports(w, h, depth)
    if why:
        row["status"] = "unsupported-by-engine"
        row["notes"] = why
        log(f"    -> skipped: {why}")
        return row

    # 1. the card's configuration, applied and proven
    ok, got = await apply_aa_config(box, glide_key, cfg)
    row["aa_verified"] = "yes" if ok else f"NO(read={got})"
    if not ok:
        row["status"] = "aa-apply-failed"
        log(f"    AA config did not take (read back {got!r}) - not recording a number")
        return row

    # 2. stage config + launcher, clear the log
    env = {"SSTH3_SLI_AA_CONFIGURATION": str(cfg), "FX_GLIDE_SWAPINTERVAL": "0"}
    await title.prepare(box, w, h, depth, env)
    await box.exec_(f'cmd /c taskkill /f /im "{title.proc}"', timeout=30)
    await asyncio.sleep(2)

    # 3. go
    try:
        await title.start(box)
    except RetroProtocolError as e:
        row["status"] = f"launch-failed: {e}"
        return row

    # 4. wait for the fps line to appear rather than for a fixed time; the id
    #    engine does not quit after a timedemo, it drops to the menu.
    #
    #    A stalled run is detected by the LOG not growing, not by the clock.
    #    4-chip 8x AA can hang inside Glide/ICD init with the card holding an
    #    exclusive fullscreen context; sitting out the full max_run there costs
    #    minutes per cell and risks the agent, so a quiet log is cut short.
    deadline = time.time() + args.max_run
    parsed, raw = None, ""
    last_size, last_growth = -1, time.time()
    wedged = False
    await asyncio.sleep(12)
    while time.time() < deadline:
        data = await box.download(title.log)
        if data:
            raw = data.decode("ascii", errors="replace")
            parsed = title.parse(raw)
            if parsed:
                break
            if len(data) != last_size:
                last_size, last_growth = len(data), time.time()
            elif time.time() - last_growth > args.stall_after:
                # A quiet log is NOT evidence of a hang: an id engine prints
                # nothing at all between loading the map and finishing the
                # timedemo, so at 4x AA and 1600x1200 a perfectly healthy run
                # is silent for minutes. Only a run that never got the
                # renderer up is wedged; past that point wait out max_run.
                if renderer_up(raw):
                    last_growth = time.time()   # healthy, just slow - keep waiting
                else:
                    wedged = True
                    break
        await asyncio.sleep(6)

    await box.exec_(f'cmd /c taskkill /f /im "{title.proc}"', timeout=30)
    await asyncio.sleep(3)

    if raw:
        (args.outdir / f"{title.tid}_{res}_{depth}_cfg{cfg}.log").write_text(raw)
    if not parsed:
        row["status"] = "no-fps-line(see raw log)"
        # These are real, reportable outcomes for a card/driver, not tool
        # failures - a mode this card cannot bring up belongs in the article.
        if wedged:
            tail = (raw or "").strip().splitlines()[-1:] or [""]
            row["status"] = "gl-init-hung"
            row["notes"] = f"log stopped growing at: {tail[0].strip()[:90]}"
        elif raw and re.search(r"could not set the given mode|failed hard", raw, re.I):
            row["status"] = "mode-rejected-by-card"
        log(f"    -> {row['status']}  {row.get('notes','')}")
        row.update(title.attribution(raw))
        return row

    row.update(parsed)
    row.update(title.attribution(raw))
    row["status"] = "ok"
    log(f"    -> {parsed['avg_fps']} fps   [{row.get('gl_renderer','?')}]")
    return row


# --------------------------------------------------------------------------- #
# campaign
# --------------------------------------------------------------------------- #

def load_done(csv_path):
    done = set()
    rows = []
    if csv_path.exists():
        with csv_path.open(newline="") as fh:
            for r in csv.DictReader(fh):
                rows.append(r)
                if r.get("status") == "ok":
                    done.add((r["title"], r.get("api", ""), r["res"],
                              r["colordepth"], r["aa_cfg"]))
    return done, rows


def migrate_header(csv_path):
    """Rewrite an existing CSV whose header predates a column change.

    append_row writes the header only when the file does not exist, so adding a
    column to CSV_COLS mid-campaign silently writes NEW rows in the NEW order
    underneath the OLD header - every field after the inserted one shifts by
    one and the file reads as garbage while looking perfectly well-formed.
    That happened here when `api` was added, so the migration is not
    hypothetical.  Old rows are preserved and read back through their OWN
    header, then rewritten under the current one.
    """
    if not csv_path.exists():
        return
    with csv_path.open(newline="") as fh:
        rd = csv.reader(fh)
        try:
            have = next(rd)
        except StopIteration:
            return
        if have == CSV_COLS:
            return
        rows = [dict(zip(have, r)) for r in rd]
    backup = csv_path.with_suffix(".csv.pre-migration")
    csv_path.replace(backup)
    with csv_path.open("w", newline="") as fh:
        w = csv.DictWriter(fh, fieldnames=CSV_COLS, extrasaction="ignore")
        w.writeheader()
        for r in rows:
            w.writerow({k: r.get(k, "") for k in CSV_COLS})
    log(f"migrated {csv_path.name} to the current columns "
        f"({len(rows)} row(s) kept; previous file at {backup.name})")


def append_row(csv_path, row):
    new = not csv_path.exists()
    with csv_path.open("a", newline="") as fh:
        w = csv.DictWriter(fh, fieldnames=CSV_COLS, extrasaction="ignore")
        if new:
            w.writeheader()
        w.writerow({k: row.get(k, "") for k in CSV_COLS})


async def amain(args):
    box = Box(args.host)
    inst, prof = await find_display_instance(box)
    glide_key = GLIDE_KEY_TMPL.format(inst=inst)
    gpu = prof.get("gpu", {})
    log(f"box {args.host} {prof.get('hostname')}  agent {prof.get('agent_version')}")
    log(f"gpu  {gpu.get('name')}  {gpu.get('vram_mb')} MB  "
        f"({gpu.get('pci_ven')}:{gpu.get('pci_dev')})")
    log(f"glide settings key: HKLM\\{glide_key}")

    if "121a" not in str(gpu.get("pci_ven", "")).lower():
        log("REFUSING: this is not a 3dfx card - the whole AA axis is VSA-100 specific")
        return 2

    args.outdir.mkdir(parents=True, exist_ok=True)
    csv_path = args.outdir / "results.csv"
    migrate_header(csv_path)
    done, _ = load_done(csv_path)
    if done:
        log(f"resuming: {len(done)} completed run(s) already recorded")

    if not args.no_quiesce:
        log("quiescing background processes")
        await quiesce(box)

    titles = [TITLES[t](api) if api else TITLES[t]() for t, api in args.titles]
    if not args.allow_hazards:
        for c in [c for c in args.configs if c in HAZARD_CONFIGS]:
            log(f"cfg {c} EXCLUDED (measured killer): {HAZARD_CONFIGS[c]}")
        for c in [c for c in args.configs if c in SUSPECT_CONFIGS]:
            log(f"cfg {c} EXCLUDED (untested, not proven bad): {SUSPECT_CONFIGS[c]}")
        args.configs = [c for c in args.configs
                        if c not in HAZARD_CONFIGS and c not in SUSPECT_CONFIGS]
    matrix = [(t, w, h, d, c)
              for t in titles
              for (w, h) in args.resolutions
              for d in args.depths
              for c in args.configs]
    log(f"matrix: {len(matrix)} run(s)")

    checked, blocked = {}, {}
    for i, (t, w, h, d, c) in enumerate(matrix, 1):
        # Once per title: what Glide is sitting beside this game's exe?  A
        # wrong one either bypasses the card silently or takes the box down.
        if t.tid not in checked:
            ok, why = await preflight_title_dlls(box, t, args.allow_open_glide)
            checked[t.tid] = why
            if not ok:
                blocked[t.tid] = why
        if t.tid in blocked:
            row = {k: "" for k in CSV_COLS}
            row.update({"stamp": datetime.now(timezone.utc).isoformat(),
                        "title": t.name, "api": getattr(t, "api", ""),
                        "res": f"{w}x{h}", "width": w, "height": h,
                        "colordepth": d, "aa_cfg": c,
                        "chips": AA_CONFIGS[c]["chips"],
                        "samples": AA_CONFIGS[c]["samples"],
                        "aa_label": AA_CONFIGS[c]["label"],
                        "status": "blocked-unsafe-game-local-dll",
                        "notes": blocked[t.tid]})
            append_row(csv_path, row)
            continue

        key = (t.name, getattr(t, "api", ""), f"{w}x{h}", str(d), str(c))
        if args.resume and key in done:
            log(f"[{i}/{len(matrix)}] skip (already measured) {key}")
            continue
        log(f"[{i}/{len(matrix)}]")
        try:
            row = await run_one(box, t, w, h, d, c, glide_key, args)
        except Exception as e:                      # keep the campaign alive
            row = {k: "" for k in CSV_COLS}
            row.update({"stamp": datetime.now(timezone.utc).isoformat(),
                        "title": t.name, "res": f"{w}x{h}", "colordepth": d,
                        "aa_cfg": c, "chips": AA_CONFIGS[c]["chips"],
                        "samples": AA_CONFIGS[c]["samples"],
                        "aa_label": AA_CONFIGS[c]["label"],
                        "api": getattr(t, "api", ""),
                        "status": f"error: {type(e).__name__}: {e}"})
            log(f"    !! {row['status']}")
        if checked.get(t.tid) and not row.get("notes"):
            row["notes"] = checked[t.tid]
        append_row(csv_path, row)

        # A wedged fullscreen Glide context can take the agent process with it,
        # and on XP nothing restarts it - the Run key only fires at logon.  So
        # liveness is checked with a protocol PING between runs and a dead
        # agent STOPS the campaign: every later cell would fail identically
        # and the CSV would fill with noise that looks like driver results.
        # A cell that failed might be a bad config OR a board already wedged
        # by an earlier cell. Recording the former when it is the latter is how
        # six false verdicts got produced on .124.
        if row.get("status") not in ("ok", "unsupported-by-engine",
                                     "blocked-unsafe-game-local-dll"):
            health = await board_alive(box)
            if health is False:
                log("THE BOARD IS WEDGED - Glide will not initialise at all.")
                log("  Every later cell would fail for this reason and be "
                    "blamed on its own config, so stopping here.")
                log("  Reboot the box, then re-run to resume. One AA config "
                    "per boot is the only sound way to sweep them.")
                row["notes"] = ((row.get("notes") or "") +
                                " | board wedged after this cell").strip()
                append_row(csv_path, row)
                return 4
            if health is None:
                log("    (could not determine board health)")

        if not await agent_alive(box):
            log("AGENT IS NOT ANSWERING - stopping the campaign here.")
            log("  135/139/445 open with 9898 refused means the agent died, not "
                "the box; it needs restarting on the machine.")
            log(f"  {len(matrix) - i} run(s) not attempted; re-run to resume.")
            return 3

    # restore the card to its 4-chip no-AA default so the box is left usable
    await apply_aa_config(box, glide_key, 5)
    log(f"done. results: {csv_path}")
    return 0


def parse_res_list(s):
    out = []
    for part in s.split(","):
        part = part.strip()
        if not part:
            continue
        w, h = part.lower().split("x")
        out.append((int(w), int(h)))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", required=True)
    ap.add_argument("--titles", default="quake3",
                    help="comma list of " + ",".join(TITLES))
    ap.add_argument("--resolutions", default="640x480,800x600,1024x768,1280x1024,1600x1200")
    ap.add_argument("--depths", default="16")
    ap.add_argument("--configs", default="0,1,2,3,4,5,6,7,8",
                    help="SSTH3_SLI_AA_CONFIGURATION values (chips x AA)")
    ap.add_argument("--max-run", type=float, default=420.0,
                    help="seconds to wait for a run's fps line")
    ap.add_argument("--stall-after", type=float, default=100.0,
                    help="give up on a run whose log has not grown for this "
                         "long (a hung GL/Glide init, not a slow demo)")
    ap.add_argument("--outdir", default=None)
    ap.add_argument("--no-resume", dest="resume", action="store_false")
    ap.add_argument("--no-quiesce", action="store_true")
    ap.add_argument("--allow-hazards", "--allow-8xaa", dest="allow_hazards",
                    action="store_true",
                    help="include the configs known or suspected to wedge the "
                         "driver (2, 3, 4, 8 on .191). Each may cost a trip to "
                         "the machine - never set this unattended.")
    ap.add_argument("--allow-open-glide", action="store_true",
                    help="run a title even when the clean-room Glide is staged "
                         "beside it. It hard-freezes this board and needs a "
                         "physical power cycle - never set this unattended.")
    args = ap.parse_args()

    specs = []
    for spec in args.titles.split(","):
        spec = spec.strip()
        if not spec:
            continue
        tid, _, api = spec.partition(":")
        if tid not in TITLES:
            ap.error(f"unknown title {tid!r}; have {sorted(TITLES)}")
        specs.append((tid, api or None))
    args.titles = specs
    args.resolutions = parse_res_list(args.resolutions)
    args.depths = [int(x) for x in args.depths.split(",")]
    args.configs = [int(x) for x in args.configs.split(",")]
    for c in args.configs:
        if c not in AA_CONFIGS:
            ap.error(f"config {c} is not one of {sorted(AA_CONFIGS)}")
    args.outdir = Path(args.outdir) if args.outdir else \
        Path(__file__).resolve().parent / "results" / f"v56k_{args.host}"

    raise SystemExit(asyncio.run(amain(args)))


if __name__ == "__main__":
    main()
