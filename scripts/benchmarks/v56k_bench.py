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

# Every row records WHAT WAS RUNNING, not just how fast it ran. A benchmark
# whose software versions are not in the row cannot be compared against a later
# one, and "which build was that?" is unanswerable a week later - so the game
# binary, the driver files and the OS/agent are captured per run, by md5 and
# size as well as by version string. (A version RESOURCE can be absent or
# stale; an md5 cannot.) migrate_header() rewrites an older CSV so these can be
# added mid-campaign without shifting every field.
CSV_COLS = ["stamp", "title", "engine", "api", "res", "width", "height",
            "colordepth",
            "aa_cfg", "chips", "samples", "aa_label", "avg_fps", "frames",
            "seconds", "gl_renderer", "gl_vendor", "mode_line", "pixelformat",
            "aa_verified",
            "game_exe", "game_size", "game_md5",
            "driver_pkg", "driver_ver", "glide3x_md5", "icd_md5",
            "os_build", "agent_ver", "gpu",
            "mem_avail_mb", "mem_load_pct",
            "status", "notes"]

# The files whose identity decides what a number means on this box.
VERSION_FILES = {
    "glide3x":  r"C:\WINDOWS\system32\glide3x.dll",
    "glide2x":  r"C:\WINDOWS\system32\glide2x.dll",
    "icd":      r"C:\WINDOWS\system32\3dfxOGL.dll",
    "display":  r"C:\WINDOWS\system32\3dfxvs.dll",
    "miniport": r"C:\WINDOWS\system32\drivers\3dfxvsm.sys",
}


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


async def file_identity(box, path):
    """size + md5 of one file on the box.

    md5 rather than a version resource because a driver DLL here may carry no
    version at all, or carry the version of the package it was copied FROM -
    AmigaMerlin ships rebranded 3dfx binaries. A hash is what actually
    distinguishes two builds.
    """
    out = await box.exec_(f'cmd /c if exist "{path}" '
                          f'(for %I in ("{path}") do @echo %~zI) else (echo -)')
    size = out.strip().splitlines()[-1].strip() if out.strip() else "-"
    md5 = "-"
    try:
        data = await box.download(path)
        if data:
            import hashlib
            md5 = hashlib.md5(data).hexdigest()
            size = str(len(data))
    except Exception:
        pass
    return {"path": path, "size": size, "md5": md5}


async def collect_versions(box, glide_key):
    """Everything that decides what a benchmark number MEANS on this box.

    Captured once per campaign and written beside the CSV as versions.json, and
    the identifying fields are also stamped onto every row - a sidecar can be
    separated from its data, a row cannot.
    """
    v = {"probed": datetime.now(timezone.utc).isoformat(), "files": {}}
    for name, path in VERSION_FILES.items():
        v["files"][name] = await file_identity(box, path)
    try:
        prof = json.loads(await box.text("HWPROFILE"))
        v["agent_ver"] = prof.get("agent_version")
        v["os"] = prof.get("os") or prof.get("os_version")
        # A CSV cell holding a dict repr is not a version anyone can read or
        # sort on, so flatten it for the row while versions.json keeps the full
        # structure.
        o = v["os"]
        v["os_str"] = (" ".join(str(o.get(k, "")) for k in
                                ("product", "version", "service_pack")).strip()
                       if isinstance(o, dict) else str(o or ""))
        v["hostname"] = prof.get("hostname")
        for c in prof.get("video_cards", []):
            if c.get("attached_to_desktop"):
                v["gpu"] = {"name": c.get("name"), "driver_version":
                            c.get("driver_version"), "vram_mb": c.get("vram_mb"),
                            "pci": f"{c.get('pci_ven')}:{c.get('pci_dev')}"}
    except Exception as e:
        v["hwprofile_error"] = f"{type(e).__name__}: {e}"
    # The driver PACKAGE as the display class records it - this is what a reader
    # means by "which driver", and it is not derivable from any single file.
    try:
        cls = glide_key.rsplit("\\Settings\\Glide", 1)[0]
        out = await box.text(f"REGREAD HKLM {cls}")
        for val in json.loads(out).get("values", []):
            if val["name"] in ("DriverVersion", "DriverDesc", "ProviderName",
                               "InfPath", "DriverDate"):
                v.setdefault("display_class", {})[val["name"]] = val["data"]
    except Exception:
        pass
    return v


async def title_identity(box, title):
    """The GAME binary actually launched - the user's explicit requirement that
    the version of the game under test is tracked, not just the driver."""
    exe = getattr(title, "proc", None)
    root = getattr(title, "root", None)
    if not exe or not root:
        return {}
    for cand in (rf"{root}\{exe}", rf"{root}\System\{exe}"):
        ident = await file_identity(box, cand)
        if ident["md5"] != "-":
            ident["engine"] = getattr(title, "engine", "")
            return ident
    return {"path": exe, "size": "-", "md5": "-",
            "engine": getattr(title, "engine", "")}


async def apply_aa_config(box, glide_key, cfg):
    """Write SSTH3_SLI_AA_CONFIGURATION and prove THE WRITE LANDED.

    Returns (ok, readback).  Never trust the OK from REGWRITE: given a path
    with the value name folded in it creates a subkey and still answers OK.

    ⚠️ THIS IS NOT EVIDENCE THE CARD IS ANTI-ALIASING, and for a while this
    function's result was recorded in a column called `aa_verified`, which read
    as though it were. Measured 2026-09-15 on .124: the value writes, reads
    back, survives a reboot, and changes NOTHING - Quake III's own screenshot
    of a fixed demo frame at 4 chips/no AA and 4 chips/4x AA is byte-identical
    (same md5, zero pixel difference), and 2x AA on one chip costs 0.99x at a
    fill-bound 1024x768 where it must cost roughly half. Adding
    FX_GLIDE_AA_SAMPLE=4 produced the same md5 a third time.

    The only honest post-condition for "AA is applied" is that THE RENDERING
    CHANGED: a fps delta against the matching no-AA cell, or a pixel
    difference between the two screenshots. So the column now records
    `reg-readback-only`, which is what this actually checked all along.
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

    def verify_mode(self, raw, w, h, depth):
        modes = re.findall(r"MODE:\s*-?\d+,\s*(\d+)\s*x\s*(\d+)", raw)
        if not modes:
            return "no 'MODE:' line in the log"
        mw, mh = map(int, modes[-1])
        return None if (mw, mh) == (w, h) else f"asked {w}x{h}, the demo ran at {mw}x{mh}"

    # Only NON-latched settings and the run itself; no vid_restart.
    def bench_cfg(self):
        return "\r\n".join([
            '// generated per run by v56k_bench.py - do not edit',
            'seta com_maxfps "0"',
            'seta r_swapInterval "0"',
            'seta r_finish "0"',
            'seta cg_drawFPS "1"',
            'seta timedemo "1"',
            # exit cleanly when the demo ends: a taskkill'd Glide process can
            # leave the display driver handing its stale board mapping to the
            # next Glide app, which then faults in grGlideInit (2026-09-24)
            'set nextdemo "quit"',
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
            # clean exit after the demo (see Quake3.bench_cfg): killserver
            # first so the timedemo line is printed by the disconnect
            'set nextserver "killserver; quit"',
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
             f'+set gl_driver {self.gl_driver} +set gl_mode {mode} '
             f'+set gl_bitdepth {16 if depth < 32 else 32} '
             f'+set vid_fullscreen 1 +set logfile 2 +exec bench.cfg'),
        ]
        return "\r\n".join(lines) + "\r\n"

    # The AmigaMerlin ICD straight from system32, by name - the same route
    # Quake III takes. The game-local 3dfxgl.dll is whatever the library
    # ships (the stock V1/V2 MiniGL, which cannot drive a VSA-100 and drops
    # the engine to ref_soft at 320x240) or a hand-placed ICD copy that the
    # next GAMESYNC restores away; neither is a stable thing to measure.
    gl_driver = "3dfxogl"

    def fleetres_cfg(self, w, h, depth):
        """The staged autoexec.cfg sets gl_driver "opengl32" and then execs
        fleetres.cfg LAST - so this file, not the command line, decides the
        mode, depth and driver the timedemo runs at. Written per run with the
        same values the command line carries, so the renderer is built once."""
        return "\r\n".join([
            '// written per run by v56k_bench.py - the launcher rewrites this',
            '// file at every start, so overwriting it is harmless.',
            f'set gl_mode "{self.MODES[(w, h)]}"',
            f'set gl_bitdepth "{16 if depth < 32 else 32}"',
            f'set gl_driver "{self.gl_driver}"',
            'set vid_ref "gl"',
            'set vid_fullscreen "1"',
            '',
        ])

    def verify_mode(self, raw, w, h, depth):
        """None when the LAST renderer init ran at w x h and depth, else why."""
        modes = re.findall(r"setting mode \d+:\s*(\d+)\s+(\d+)", raw)
        if not modes:
            return "no 'setting mode' line in the log"
        mw, mh = map(int, modes[-1])
        if (mw, mh) != (w, h):
            return f"asked {w}x{h}, the timedemo ran at {mw}x{mh}"
        bits = re.findall(r"using gl_bitdepth of (\d+)", raw)
        want = 16 if depth < 32 else 32
        if bits and int(bits[-1]) != want:
            return f"asked {want}-bit, ran at {bits[-1]}-bit"
        return None

    async def prepare(self, box, w, h, depth, env):
        await box.upload(rf"{self.root}\baseq2\fleetres.cfg",
                         self.fleetres_cfg(w, h, depth))
        # `gl_driver 3dfxgl` loads whatever file sits beside quake2.exe under
        # that name. The staged library ships the real 3dfx MiniGL (142,848 B);
        # the driver-install sweep on .124 replaced it with a copy of the
        # AmigaMerlin ICD (2,646,009 B). Label the row by what is actually
        # loaded, measured at prepare time, not by what the class assumed.
        # The label is EVIDENCE, never a default. Twice on .124 this probe came
        # back empty and the row kept the class default "opengl-minigl" - the
        # specific name of a driver the cell did not run on - while its own
        # gl_renderer said "Mesa Glide v0.63", which no MiniGL can report.
        await self.identify(box)
        await box.upload(self.cfg, self.bench_cfg())
        await box.upload(self.bat, self.launch_bat(w, h, depth, env))
        await box.exec_(f'cmd /c del /f /q "{self.log}"')

    async def identify(self, box):
        """Measure which 3dfxgl.dll sits beside quake2.exe, ONCE. The runner
        calls this before the matrix is keyed, so the resume key, the row and
        the label all agree; prepare() only falls back to it."""
        if getattr(self, "_identified", False):
            return
        self._identified = True
        if self.gl_driver == "3dfxogl":
            self.api = "opengl-icd"
            self.engine = "quake2.exe (3.20; gl_driver 3dfxogl = the AmigaMerlin ICD in system32)"
            return
        size = ""
        for _ in range(3):
            out = await box.exec_(rf'cmd /c for %I in ("{self.root}\3dfxgl.dll") do @echo %~zI')
            size = (out.strip().splitlines() or [""])[-1].strip()
            if size.isdigit():
                break
            await asyncio.sleep(2)
        if size == "2646009":
            self.api, self.engine = "opengl-icd-gamelocal", "quake2.exe (3.20; game-local 3dfxgl.dll = AmigaMerlin ICD copy)"
        elif size == "142848":
            self.api, self.engine = "opengl-minigl", "quake2.exe (3.20; 3dfx MiniGL 3dfxgl.dll)"
        elif size.isdigit():
            self.api, self.engine = f"opengl-3dfxgl-{size}B", f"quake2.exe (3.20; 3dfxgl.dll {size} B, unidentified)"
        else:
            self.api, self.engine = "opengl-3dfxgl-unmeasured", "quake2.exe (3.20; 3dfxgl.dll size probe returned nothing)"

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
        seen_sections = set()
        skipping_dup = False
        for line in text.splitlines():
            st = line.strip()
            if st.startswith("[") and st.endswith("]"):
                section = st[1:-1]
                # A second copy of a section we manage (left by an earlier
                # append) is dropped wholesale: UE1 honours the first copy and
                # the duplicate only confuses the next reader.
                skipping_dup = section in wanted and section in seen_sections
                seen_sections.add(section)
                if skipping_dup:
                    continue
            elif skipping_dup:
                continue
            elif section in wanted:
                key = st.split("=", 1)[0].strip() if "=" in st else None
                if key and key in wanted[section]:
                    out.append(f"{key}={wanted[section][key]}")
                    seen[section].add(key)
                    continue
            out.append(line)
        # A key the section did not carry is inserted INTO that section's first
        # copy, right under its header. Appending a fresh "[Section]" block at
        # the end is what left UnrealTournament.ini with two
        # [WinDrv.WindowsClient] sections on .124 - UE1 reads the first one and
        # the values in the second are silently inert.
        for sec, kv in wanted.items():
            missing = {k: v for k, v in kv.items() if k not in seen.get(sec, ())}
            if not missing:
                continue
            header = f"[{sec}]"
            if header in out:
                idx = out.index(header) + 1
                out[idx:idx] = [f"{k}={v}" for k, v in missing.items()]
            else:
                out.append(header)
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
        # UE1 writes System\Running.ini while a session is live and removes it
        # on a CLEAN exit. A benchmark harness kills the game, so the marker
        # survives and the NEXT launch stops on a modal "Unreal Tournament
        # Recovery Mode" dialog instead of starting - which looks exactly like
        # a launch failure, and leaves a 0-byte log with nothing to parse.
        # CLAUDE.md records this file surviving a full redeploy for the same
        # reason. Clearing it is part of preparing the run, not cleanup.
        await box.exec_(rf'cmd /c del /f /q "{self.root}\System\Running.ini"')

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
        """The staged title is a DISC-MOUNT launcher: the fleet template mounts
        _disc\\<image>, waits for the drive letter, then starts the game. The
        first harness launched Bin\\SeriousSam.exe directly and got the CD
        check the library was staged to prevent - a harness fault that was
        first written up as a library one. So the bench launcher is generated
        from the SAME template and spec, with the bench's args and env and the
        resolution block replaced (v56kbench.ini pins the mode)."""
        import importlib.util, json as _json
        repo = Path(__file__).resolve().parents[2]
        from pathlib import PureWindowsPath
        spec_file = repo / "provisioning" / "discmount" / "specs" / f"{PureWindowsPath(self.root).name}-Play.json"
        gen = repo / "scripts" / "fleet" / "make-mount-launcher.py"
        if not spec_file.exists() or not gen.exists():
            raise RuntimeError(f"no disc-mount spec/generator for {self.root} ({spec_file.name})")
        spec = dict(_json.loads(spec_file.read_text()))
        spec["title"] = spec.get("title", self.name) + " - V56K bench"
        spec["fleetres_block"] = "rem (bench: the resolution comes from Scripts\\v56kbench.ini)"
        spec["prelaunch"] = "\r\n".join(f"set {k}={v}" for k, v in env.items()) or "rem (none)"
        v = dict(spec.get("vars", {}))
        v["GTITLE"] = v.get("GTITLE", self.name) + " - V56K bench"
        v["GAMEARGS"] = "+exec Scripts\\v56kbench.ini"
        spec["vars"] = v
        mspec = importlib.util.spec_from_file_location("make_mount_launcher", gen)
        mod = importlib.util.module_from_spec(mspec); mspec.loader.exec_module(mod)
        return mod.crlf(mod.substitute(mod.load_template(mod.DEFAULT_TEMPLATE), spec))

    async def prepare(self, box, w, h, depth, env):
        await box.upload(self.cfg, self.bench_cfg(w, h, depth))
        await box.upload(self.bat, self.launch_bat(env))
        await box.exec_(f'cmd /c del /f /q "{self.log}" "{self.root}\\mount-error.txt" 2>nul & echo ok')

    async def start(self, box):
        await box.text(f"LAUNCH {self.bat}")

    async def mount_error(self, box):
        """The template writes mount-error.txt when the mounter is missing or no
        drive appeared - two different failures it reports differently."""
        data = await box.download(rf"{self.root}\mount-error.txt")
        return data.decode("latin-1", "replace").strip()[:200] if data else None

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



DEMOS = Path(__file__).resolve().parent / "demos"   # UTbench.dem and wolfbench.dm_60, committed with the runner


class UT99Bench(Unreal1):
    def supports(self, w, h, depth):
        base = getattr(super(), "supports", None)
        why = base(w, h, depth) if callable(base) else None
        if why:
            return why
        if self.api == "glide" and depth >= 32:
            # Glide 2.x has no 32-bit framebuffer; the engine takes the request
            # and renders 16-bit anyway. Measured: every "32-bit" GlideDrv row
            # matched its 16-bit twin to within noise (57.82 vs 56.01 ...).
            return "GlideDrv is Glide 2.x: no 32-bit framebuffer - a 32-bit request renders 16-bit"
        return None

    """Unreal Tournament 436 through the fleet's proven UTbench.dem route.

    UE1's `-benchmark -seconds=N` never exits on this build (measured: 100% CPU
    past two and a half minutes with the log locked), so the timedemo is driven
    the way `.claude/skills/driver-bench/run_bench.py` proved on 25 earlier
    fleet runs: bind F9 to `timedemo 1|demoplay UTbench.dem` and F10 to `Exit`,
    launch to the menu, press F9, wait for the demo, press F10. UE1 accepts a
    synthetic keystroke in exclusive fullscreen (id Tech 3 does not), and the
    CLEAN exit is what flushes the locked log and leaves no Running.ini behind.

    The summary line UE1 prints on a finished timedemo:
        N frames rendered in S seconds. Min A Max B Avg C fps.

    The render device is patched into UnrealTournament.ini by the parent class
    (glide -> GlideDrv, opengl -> OpenGLDrv over the AmigaMerlin ICD, d3d ->
    D3DDrv over its D3D HAL) together with FullscreenColorBits, so a 32-bit
    request reaches every device - and the log records what each device
    actually did with it. UE1's GlideDrv is expected to stay 16-bit whatever
    the ini says; that is a result to record, not a failure to hide.
    """

    demo = "UTbench.dem"

    def __init__(self, api="glide"):
        super().__init__("ut99", "Unreal Tournament 436", r"C:\Games\UnrealTournament436",
                         "UnrealTournament.exe", "UnrealTournament.ini", "bench.log",
                         "DM-Gothic", api=api)
        self.engine = f"UnrealTournament.exe 436 ({self.api_devices[api].split('.')[0]}, UTbench.dem)"
        self.user_ini = rf"{self.root}\System\User.ini"
        self.demo_path = rf"{self.root}\System\{self.demo}"

    def launch_bat(self, env):
        lines = ["@echo off"] + [f'set {k}={v}' for k, v in env.items()]
        lines += [rf'cd /d "{self.root}\System"',
                  f'{self.exe} -log=bench.log -nosound']
        return "\r\n".join(lines) + "\r\n"

    async def _ensure_binds(self, box):
        raw = await box.download(self.user_ini)
        if raw is None:
            raise RetroProtocolError(f"cannot read {self.user_ini}")
        txt = raw.decode("latin-1")
        new = re.sub(r"(?m)^F9=.*$", f"F9=timedemo 1|demoplay {self.demo}", txt, count=1)
        new = re.sub(r"(?m)^F10=.*$", "F10=Exit", new, count=1)
        if "F9=" not in new:
            new = new.replace("[Engine.Input]", f"[Engine.Input]\r\nF9=timedemo 1|demoplay {self.demo}\r\nF10=Exit", 1)
        if new != txt:
            await box.upload(self.user_ini, new.encode("latin-1"))

    async def _stage_demo(self, box):
        chk = await box.exec_(f'cmd /c if exist "{self.demo_path}" (echo Y) else (echo N)')
        if "Y" not in chk:
            await box.upload(self.demo_path, (DEMOS / self.demo).read_bytes())

    async def prepare(self, box, w, h, depth, env):
        await self._patch_ini(box, w, h, depth)
        await self._ensure_binds(box)
        await self._stage_demo(box)
        await box.upload(self.bat, self.launch_bat(env))
        await box.exec_(f'cmd /c del /f /q "{self.log}"')
        await box.exec_(rf'cmd /c del /f /q "{self.root}\System\Running.ini"')

    async def start(self, box):
        # The whole timedemo is driven here; the runner's log poll then finds
        # the flushed bench.log. Timings from the fleet route: ~20 s to the
        # menu, the demo runs ~90 s, F10 exits cleanly.
        await box.text(f"LAUNCH {self.bat}")
        await asyncio.sleep(24)
        await box.text("UIKEY F9")
        await asyncio.sleep(105)
        await box.text("UIKEY F10")
        await asyncio.sleep(8)

    def parse(self, raw):
        # UE1 prints a summary every time timedemo is toggled, so the log holds a
        # "3 frames rendered in 0.11 seconds ... Avg 25.62 fps" blip from the
        # toggle BEFORE the demo's own "2937 frames rendered in 44.19 seconds
        # ... Avg 66.44 fps" (measured 2026-09-16). The demo is the summary with
        # the most frames, and only a run of at least 500 frames counts.
        best = None
        for m in re.finditer(r"(\d+) frames rendered in ([\d.]+) seconds\.\s*Min [\d.]+ Max [\d.]+ Avg ([\d.]+) fps", raw):
            frames = int(m.group(1))
            if frames >= 500 and (best is None or frames > best[0]):
                best = (frames, float(m.group(2)), float(m.group(3)))
        if not best:
            return None
        return {"frames": best[0], "seconds": best[1], "avg_fps": best[2]}

    def attribution(self, raw):
        out = {}
        m = re.findall(r"GL_RENDERER\)?:?\s*(.+)", raw)
        if m:
            out["gl_renderer"] = m[-1].strip()[:120]
        else:
            m = re.findall(r"(?:Bound to|Initialized)\s+(\S*(?:Glide|OpenGL|D3D)\S*)", raw, re.I)
            if m:
                out["gl_renderer"] = m[-1].strip()
        # what the device actually did with the requested depth
        m = re.findall(r"(?im)^.*(?:Glide|OpenGL|D3D|Direct3D).*\b(\d{2})[- ]?bit.*$", raw)
        if m:
            out["pixelformat"] = re.findall(r"(?im)^(.*(?:Glide|OpenGL|D3D|Direct3D).*\b\d{2}[- ]?bit.*)$", raw)[-1].strip()[:120]
        m = re.findall(r"(?im)^(.*(?:Resolution|Setting res|SetRes|Mode:).*)$", raw)
        if m:
            out["mode_line"] = m[-1].strip()[:120]
        # GlideDrv reports what Glide 2 sees of the board: on the V5 6000 via
        # AmigaMerlin's glide2x it is "Type=0, fbRam=27262976 ... nTexelfx=2
        # Sli=0" - one chip's worth, SLI flag clear. Worth keeping on the row.
        m = re.findall(r"(?im)^.*(Found Glide:.*|Glide info:.*)$", raw)
        if m:
            out["pixelformat"] = (out.get("pixelformat", "") + " | " + " ; ".join(x.strip() for x in m)).strip(" |")[:200]
        return out


class RTCW:
    """Return to Castle Wolfenstein multiplayer engine, `timedemo 1 +demo wolfbench`.

    id Tech 3 fork with NO `r_mode -1` branch (it renders 640x480 rather than
    erroring), so a real mode index is used and an off-table resolution is
    declared unsupported. The demo `wolfbench.dm_60` was recorded on this fleet
    for the driver-bench skill and is committed beside the runner. The saved
    wolfconfig forces `r_glDriver "gl/openglv5.dll"` (the file staged with the
    game), and a saved cvar OVERRIDES a +set on this engine, so the config is
    patched to name the AmigaMerlin ICD by its registered name, `3dfxogl`, the
    same way Quake III loads it - backed up once, first touch.
    """

    tid = "rtcw"
    name = "Return to Castle Wolfenstein"
    engine = "WolfMP.exe (wolfbench.dm_60)"
    proc = "WolfMP.exe"
    api = "opengl-icd"
    MODES = {(320, 240): 0, (400, 300): 1, (512, 384): 2, (640, 480): 3, (800, 600): 4,
             (960, 720): 5, (1024, 768): 6, (1152, 864): 7, (1280, 1024): 8, (1600, 1200): 9}

    def __init__(self, root=r"C:\Games\ReturnToCastleWolfenstein", api="amigamerlin"):
        # "amigamerlin" -> the registered 3dfxogl ICD (Mesa 6.3), the same file
        # Quake III measures; "openglv5" -> the 3dfx-era OpenGL ICD staged with
        # the game in gl\ (GL_VENDOR METABYTE/WICKED3D, GL_VERSION 1.1) - a
        # second driver's number on the same card, labelled as such.
        self.api = {"amigamerlin": "opengl-icd", "openglv5": "opengl-3dfx-openglv5"}.get(api, api)
        self.gldriver = {"amigamerlin": "3dfxogl", "openglv5": "gl/openglv5.dll"}.get(api, api)
        self.tid = "rtcw" if api == "amigamerlin" else f"rtcw:{api}"
        self.engine = f"WolfMP.exe (wolfbench.dm_60, r_glDriver {self.gldriver})"
        self.root = root
        self.log = rf"{root}\main\rtcwconsole.log"
        self.bat = rf"{root}\V56KBENCH.BAT"
        self.demo_path = rf"{root}\main\demos\wolfbench.dm_60"
        self.cfgs = (rf"{root}\main\wolfconfig_mp.cfg", rf"{root}\main\wolfconfig.cfg")

    def supports(self, w, h, depth):
        if (w, h) not in self.MODES:
            return (f"RtCW's id Tech 3 fork has no r_mode -1 and no {w}x{h} entry "
                    f"in its mode table (1280x1024, not 1280x960)")
        return None

    def launch_bat(self, w, h, depth, env):
        mode = self.MODES[(w, h)]
        zbits = 24 if depth >= 32 else 16
        lines = ["@echo off"] + [f'set {k}={v}' for k, v in env.items()]
        lines += [f'cd /d "{self.root}"',
                  (f'{self.proc} +set fs_basepath "{self.root}" +set fs_homepath "{self.root}" '
                   f'+set logfile 2 +set r_glDriver {self.gldriver} +set r_mode {mode} +set r_fullscreen 1 '
                   f'+set r_colorbits {depth} +set r_texturebits {depth} +set r_depthbits {zbits} '
                   f'+set r_picmip 0 +set r_swapInterval 0 +set com_maxfps 0 +set sv_pure 0 '
                   f'+set s_initsound 0 +set timedemo 1 +demo wolfbench')]
        return "\r\n".join(lines) + "\r\n"

    async def _pin_gldriver(self, box):
        """r_glDriver is CVAR_LATCH|ARCHIVE. Measured 2026-09-16: with no
        wolfconfig on disk and `+set r_glDriver 3dfxogl` on the command line the
        engine still loaded the game's staged gl/openglv5.dll (GL_VENDOR
        METABYTE/WICKED3D - the 3dfx-era ICD) and logged "r_glDriver will be
        changed upon restarting": something exec'd after the +set set it back.
        So the value goes into every config the engine reads - patched where
        the file exists, CREATED where it does not - and the runner's
        attribution records which library actually loaded either way."""
        for cfg in self.cfgs:
            raw = await box.download(cfg)
            if raw is None:
                await box.upload(cfg, f'seta r_glDriver "{self.gldriver}"\r\n'.encode("latin-1"))
                continue
            txt = raw.decode("latin-1")
            if re.search(r'(?im)^seta?\s+r_glDriver\s+', txt):
                new = re.sub(r'(?im)^(seta?\s+r_glDriver\s+)"[^"]*"', rf'\g<1>"{self.gldriver}"', txt)
            else:
                new = txt.rstrip("\r\n") + f'\r\nseta r_glDriver "{self.gldriver}"\r\n'
            if new != txt:
                if await box.download(cfg + ".v56kbak") is None:
                    await box.upload(cfg + ".v56kbak", raw)
                await box.upload(cfg, new.encode("latin-1"))

    async def prepare(self, box, w, h, depth, env):
        chk = await box.exec_(f'cmd /c if exist "{self.demo_path}" (echo Y) else (echo N)')
        if "Y" not in chk:
            await box.exec_(rf'cmd /c if not exist "{self.root}\main\demos" mkdir "{self.root}\main\demos"')
            await box.upload(self.demo_path, (DEMOS / "wolfbench.dm_60").read_bytes())
        await self._pin_gldriver(box)
        await box.upload(self.bat, self.launch_bat(w, h, depth, env))
        await box.exec_(f'cmd /c del /f /q "{self.log}"')

    def verify_driver(self, raw):
        """r_glDriver is CVAR_LATCH: pinning 3dfxogl does not take on the FIRST
        launch after a change (measured 2026-09-16 - the config read 3dfxogl yet
        R_Init loaded the game-staged gl/openglv5.dll and only latched the
        change). Retiring the wrong ICD to force a fallback WEDGED the box (the
        agent died on a fullscreen GL-init error, 2026-09-16), so instead the
        runner READS BACK which ICD actually loaded and says so, rather than
        forcing it blind. Returns (ok, note): ok is False when the loaded
        GL_VENDOR does not match the driver this cell asked for."""
        vends = re.findall(r"(?im)^GL_VENDOR:\s*(.+)$", raw)
        rends = re.findall(r"(?im)^GL_RENDERER:\s*(.+)$", raw)
        vend = vends[-1].strip() if vends else ""
        rend = rends[-1].strip() if rends else ""
        if not vend and not rend:
            return True, ""
        is_wicked = "wicked" in vend.lower() or "metabyte" in vend.lower()
        is_mesa = "mesa" in rend.lower() or "brian paul" in vend.lower()
        if self.api == "opengl-3dfx-openglv5":
            if is_wicked:
                return True, ""
            return False, f"driver-mismatch: asked for gl/openglv5.dll (Wicked3D), engine loaded GL_VENDOR '{vend[:40]}' / '{rend[:40]}'"
        # AmigaMerlin: a POSITIVE match. "not Wicked3D" would pass a GDI Generic
        # software fallback as if it were the ICD.
        if is_mesa:
            return True, ""
        got = "gl/openglv5.dll (Wicked3D)" if is_wicked else f"GL_VENDOR '{vend[:40]}' / '{rend[:40]}'"
        return False, f"driver-mismatch: asked for {self.gldriver} (the Mesa ICD), engine loaded {got}"

    async def start(self, box):
        await box.text(f"LAUNCH {self.bat}")

    def parse(self, raw):
        m = None
        for m in FPS_RE.finditer(raw):
            pass
        if not m:
            return None
        return {"frames": int(m.group(1)), "seconds": float(m.group(2)), "avg_fps": float(m.group(3))}

    def attribution(self, raw):
        out = {}
        for key, pat in (("gl_renderer", r"GL_RENDERER:\s*(.+)"), ("gl_vendor", r"GL_VENDOR:\s*(.+)"),
                         ("mode_line", r"(MODE:\s*.+)"), ("pixelformat", r"(PIXELFORMAT:\s*.+)")):
            hits = re.findall(pat, raw)
            if hits:
                out[key] = hits[-1].strip()
        return out


class CS16:
    """Counter-Strike 1.6 (GoldSrc; the staged BC Romania build 4554 with the
    BCShield anti-cheat), `+timedemo cs16_bench` on de_dust.

    What the driver-bench skill learned the hard way, carried over:
    - GoldSrc IGNORES -w/-h for the fullscreen mode: it takes
      HKCU\\Software\\Valve\\Half-Life\\Settings ScreenWidth/ScreenHeight/
      ScreenBPP. So the mode is written there per cell and READ BACK - a
      REGWRITE "OK" is not evidence (CLAUDE.md, registry.c:284).
    - -condebug writes qconsole.log to the CS ROOT (the working dir), not
      cstrike\\.
    - BCShield blocks `timerefresh`, so a pre-recorded demo + `+timedemo` is
      the only automatable fps path; `record`/`stop`/`quit`/`wait` are allowed.
    - The engine prints "-1 frames 1.000 seconds -1.000 fps" priming lines
      while a demo loads; FPS_RE cannot match a negative count, so only the
      real result line is taken.

    The demo is recorded ONCE per box by `_ensure_demo()` - in the SOFTWARE
    renderer, windowed, so recording never touches the 3dfx card - from a
    listen server whose cstrike\\listenserver.cfg joins a team, walks the
    player round the map and records it. The .dem is kept in demos/ and
    uploaded to every later box, so every row plays the same frames.
    """

    tid = "cs16"
    name = "Counter-Strike 1.6"
    engine = "hl.exe (GoldSrc build 4554, cs16_bench.dem on de_dust)"
    proc = "hl.exe"
    api = "opengl-icd"
    DEMO = "cs16_bench"
    SETTINGS = r"Software\Valve\Half-Life\Settings"
    # GoldSrc prints no GL_RENDERER line on its own; these mark "the demo is
    # playing", after which a quiet log means rendering, not a hang.
    up_re = r"(?i)playing demo|demo from|timedemo|de_dust|GL_RENDERER"

    def __init__(self, root=r"C:\Games\CounterStrike16"):
        self.root = root
        self.log = rf"{root}\qconsole.log"
        self.bat = rf"{root}\V56KBENCH.BAT"
        self.recbat = rf"{root}\V56KREC.BAT"
        self.demo_path = rf"{root}\cstrike\{self.DEMO}.dem"
        self.lscfg = rf"{root}\cstrike\listenserver.cfg"

    def supports(self, w, h, depth):
        return None     # GoldSrc takes any mode the driver lists

    def launch_bat(self, w, h, depth, env):
        # MESA_FORCE_SSE=1: the AmigaMerlin ICD is Mesa-based and, at
        # wglCreateContext, runs Mesa's SSE-exception probe - a deliberate
        # divps by zero it expects its unhandled-exception filter to swallow.
        # GoldSrc's own handler catches the trap first and shuts the engine
        # down, so without this every GL launch on the V5 dies before the
        # first frame (caught under ntsd on .124, 2026-09-23).
        lines = ["@echo off", "set MESA_FORCE_SSE=1"]
        for k, v in env.items():
            lines.append(f"set {k}={v}")
        lines += [
            f'cd /d "{self.root}"',
            (f"hl.exe -game cstrike -gl -full -w {w} -h {h} -condebug -noipx -nojoy "
             f"+fps_max 999 +gl_vsync 0 +timedemo {self.DEMO}"),
        ]
        return "\r\n".join(lines) + "\r\n"

    @staticmethod
    def record_cfg():
        """listenserver.cfg that records the benchmark demo. `wait` defers the
        rest of the buffer one frame; nested aliases keep the command buffer
        small (a flat 2,000-line wait chain overflows it)."""
        w10 = ";".join(["wait"] * 10)
        w100 = ";".join(["w10"] * 10)
        return "\r\n".join([
            "// generated by v56k_bench.py CS16._ensure_demo - deleted after recording",
            f'alias w10 "{w10}"',
            f'alias w100 "{w100}"',
            "w100", "chooseteam", "w10;w10", "menuselect 5", "w10;w10", "menuselect 5",
            "w100;w100;w100",                      # spawn
            f"record {CS16.DEMO}",
            "+forward", "w100;w100", "-forward",
            "+right", "w10;w10;w10;w10", "-right",
            "+forward", "w100;w100", "-forward",
            "+left", "w10;w10;w10;w10;w10;w10", "-left",
            "+forward", "w100;w100;w100", "-forward",
            "+right", "w10;w10;w10", "-right",
            "+forward", "w100;w100", "-forward",
            "stop",
            "w100;w100",                           # flush - else "Corrupt demo file"
            "quit",
            "",
        ])

    async def _demo_size(self, box):
        out = await box.exec_(rf'cmd /c for %I in ("{self.demo_path}") do @echo %~zI')
        s = (out.strip().splitlines() or [""])[-1].strip()
        return int(s) if s.isdigit() else 0

    async def _ensure_demo(self, box):
        if await self._demo_size(box) > 20000:
            return
        local = DEMOS / f"{self.DEMO}.dem"
        if local.exists():
            await box.upload(self.demo_path, local.read_bytes())
            if await self._demo_size(box) > 20000:
                return
        # Record it: software renderer, windowed - never touches the card.
        await box.upload(self.lscfg, self.record_cfg())
        await box.upload(self.recbat, "\r\n".join([
            "@echo off", f'cd /d "{self.root}"',
            ("hl.exe -game cstrike -soft -window -w 640 -h 480 -noipx -nojoy "
             "+sv_lan 1 +maxplayers 2 +mp_freezetime 0 +map de_dust"), ""]))
        await box.exec_(f'cmd /c del /f /q "{self.demo_path}" 2>nul')
        await box.text(f"LAUNCH {self.recbat}")
        t0 = time.time()
        await asyncio.sleep(20)
        while time.time() - t0 < 300:
            out = await box.exec_('cmd /c tasklist /fi "imagename eq hl.exe" /nh')
            if "hl.exe" not in out.lower():
                break
            await asyncio.sleep(10)
        await box.exec_('cmd /c taskkill /f /im hl.exe 2>nul')
        await box.exec_(f'cmd /c del /f /q "{self.lscfg}" 2>nul')
        size = await self._demo_size(box)
        if size <= 20000:
            raise RetroProtocolError(f"recording {self.DEMO}.dem failed ({size} B)")
        data = await box.download(self.demo_path)
        if data:
            DEMOS.mkdir(exist_ok=True)
            local.write_bytes(data)

    async def identify(self, box):
        """Which OpenGL driver will hl.exe load? A game-local opengl32.dll wins
        over the registered ICD; label by what is really there."""
        if getattr(self, "_identified", False):
            return
        self._identified = True
        out = await box.exec_(rf'cmd /c for %I in ("{self.root}\opengl32.dll") do @echo %~zI')
        size = (out.strip().splitlines() or [""])[-1].strip()
        if size.isdigit() and size != "0":
            self.api = f"opengl-gamelocal-{size}B"
            self.engine += f"; game-local opengl32.dll {size} B"
        else:
            self.api = "opengl-icd"
            self.engine += "; system opengl32 -> registered ICD"
        await self._ensure_demo(box)

    async def _set_mode(self, box, w, h, depth):
        for name, val in (("ScreenWidth", w), ("ScreenHeight", h), ("ScreenBPP", depth),
                          ("EngineModeW", w), ("EngineModeH", h), ("EngineModeBPP", depth),
                          ("ScreenWindowed", 0), ("EngineModeWindowed", 0)):
            await box.text(f"REGWRITE HKCU {self.SETTINGS} {name} REG_DWORD {val}")
        got = json.loads(await box.text(f"REGREAD HKCU {self.SETTINGS}"))
        vals = {v["name"]: v["data"] for v in got.get("values", [])}
        if (vals.get("ScreenWidth"), vals.get("ScreenHeight"), vals.get("ScreenBPP")) != (w, h, depth):
            raise RetroProtocolError(f"GoldSrc mode did not stick: {vals}")

    async def prepare(self, box, w, h, depth, env):
        await self.identify(box)
        await self._set_mode(box, w, h, depth)
        await box.upload(self.bat, self.launch_bat(w, h, depth, env))
        await box.exec_(f'cmd /c del /f /q "{self.log}" "{self.root}\\cstrike\\qconsole.log" 2>nul')

    async def start(self, box):
        await box.text(f"LAUNCH {self.bat}")

    def parse(self, raw):
        m = None
        for m in FPS_RE.finditer(raw):
            pass
        if not m or float(m.group(3)) <= 0:
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


# --------------------------------------------------------------------------- #
# the clean-room lane: OUR MesaFX ICD (voodoo-cleanroom), game-local
# --------------------------------------------------------------------------- #

# Roadmap 17.1 Step 1: our ICD over AmigaMerlin's own Glide + display driver,
# loaded by name from beside the game's exe - nothing in system32 changes, so
# removing one file restores the retail stack. The DLL is the retail-linked
# build (imports the glide3x.dll already in system32).
CLEANROOM_ICD = Path(os.environ.get(
    "V56K_CLEANROOM_ICD",
    Path(__file__).resolve().parents[2].parent.parent.parent
    / "voodoo-cleanroom" / "out" / "opengl32_retail.dll"))
if not CLEANROOM_ICD.exists():   # running from the main tree, not a worktree
    CLEANROOM_ICD = (Path(__file__).resolve().parents[2]
                     / "voodoo-cleanroom" / "out" / "opengl32_retail.dll")
CLEANROOM_TRACE = r"C:\retrogl.log"


def cleanroom_version(data):
    m = re.search(rb"\[voodoo-cleanroom (\d+\.\d+\.\d+)\]", data)
    return m.group(1).decode() if m else "unknown"


class _Cleanroom:
    """Mixin: put our ICD beside the exe as retrogl.dll and make sure the
    bytes on the box are the bytes we mean (md5 compared, never trusted)."""
    icd_name = "retrogl"

    async def stage_icd(self, box):
        import hashlib
        data = CLEANROOM_ICD.read_bytes()
        want = hashlib.md5(data).hexdigest()
        dest = rf"{self.root}\{self.icd_name}.dll"
        have = await box.download(dest)
        if not have or hashlib.md5(have).hexdigest() != want:
            await box.upload(dest, data)
            have = await box.download(dest)
            if not have or hashlib.md5(have).hexdigest() != want:
                raise RetroProtocolError(f"{dest}: upload did not land intact")
        ver = cleanroom_version(data)
        self.api = f"opengl-cleanroom-{ver}"
        # the tracer from the previous run would be misread as this run's
        await box.exec_(f'cmd /c del /f /q "{CLEANROOM_TRACE}"')
        return ver


class Quake2Cleanroom(_Cleanroom, Quake2):
    tid = "quake2:retrogl"
    api = "opengl-cleanroom"

    gl_driver = "retrogl"

    def verify_driver(self, raw):
        r = re.findall(r"GL_RENDERER:\s*(.+)", raw)
        if r and "voodoo-cleanroom" in r[-1]:
            return True, ""
        return False, f"GL_RENDERER is '{(r[-1] if r else '?').strip()[:60]}', not our ICD"

    async def identify(self, box):
        if getattr(self, "_identified", False):
            return
        self._identified = True
        ver = await self.stage_icd(box)
        self.engine = f"quake2.exe (3.20; game-local retrogl.dll = voodoo-cleanroom {ver} over AmigaMerlin glide3x)"


class Quake3Cleanroom(_Cleanroom, Quake3):
    tid = "quake3:retrogl"
    api = "opengl-cleanroom"

    def verify_driver(self, raw):
        return Quake2Cleanroom.verify_driver(self, raw)

    def fleetres_cfg(self, w, h, depth):
        return super().fleetres_cfg(w, h, depth).replace(
            'seta r_glDriver "3dfxogl"', f'seta r_glDriver "{self.icd_name}"')

    def setargs(self, w, h, depth):
        return super().setargs(w, h, depth).replace(
            "+set r_glDriver 3dfxogl", f"+set r_glDriver {self.icd_name}")

    async def identify(self, box):
        if getattr(self, "_identified", False):
            return
        self._identified = True
        ver = await self.stage_icd(box)
        self.engine = f"quake3.exe (retail 1.32c; game-local retrogl.dll = voodoo-cleanroom {ver} over AmigaMerlin glide3x)"

    async def prepare(self, box, w, h, depth, env):
        await self.identify(box)
        await super().prepare(box, w, h, depth, env)


class RTCWCleanroom(_Cleanroom, RTCW):
    """RtCW on OUR ICD: retrogl.dll beside WolfMP.exe, r_glDriver retrogl.
    r_glDriver is latched, so the first launch after the change can still load
    the previous driver - verify_driver reads the renderer back and refuses."""

    def __init__(self):
        RTCW.__init__(self, api="retrogl")
        self.tid = "rtcw:retrogl"
        self.api = "opengl-cleanroom"

    async def prepare(self, box, w, h, depth, env):
        ver = await self.stage_icd(box)
        self.engine = f"WolfMP.exe (wolfbench.dm_60; game-local retrogl.dll = voodoo-cleanroom {ver} over AmigaMerlin glide3x)"
        await RTCW.prepare(self, box, w, h, depth, env)

    def verify_driver(self, raw):
        return Quake2Cleanroom.verify_driver(self, raw)

TITLES = {
    "cs16": lambda api=None: CS16(),
    "quake3": lambda api=None: (Quake3Cleanroom() if api == "retrogl" else Quake3()),
    "quake2": lambda api=None: (Quake2Cleanroom() if api == "retrogl" else Quake2()),
    "glquake": lambda api=None: GLQuake(),
    "ut": lambda api="glide": _ut(api),
    "ut99": lambda api="glide": UT99Bench(api),
    "rtcw": lambda api="amigamerlin": (RTCWCleanroom() if api == "retrogl"
                                       else RTCW(api=api or "amigamerlin")),
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
    # .124 raises a "Found New Hardware Wizard" after every boot since the
    # card swap (observed 2026-09-15/16). A modal dialog steals focus from a
    # fullscreen game and can eat the synthetic keystrokes the UE1 route
    # depends on, so it is closed by window title before every run.
    try:
        await box.exec_('cmd /c taskkill /f /fi "windowtitle eq Found New Hardware Wizard"', timeout=30)
    except Exception:
        pass


async def run_one(box, title, w, h, depth, cfg, glide_key, args, versions=None):
    meta = AA_CONFIGS[cfg]
    res = f"{w}x{h}"
    row = {c: "" for c in CSV_COLS}
    # What was running, stamped on the row itself. A sidecar can be separated
    # from its data; a row cannot.
    v = versions or {}
    files = v.get("files", {})
    dc = v.get("display_class", {})
    ti = v.get("titles", {}).get(getattr(title, "tid", ""), {})
    row.update({
        "game_exe": ti.get("path", ""), "game_size": ti.get("size", ""),
        "game_md5": ti.get("md5", ""),
        "driver_pkg": dc.get("DriverDesc", ""),
        "driver_ver": dc.get("DriverVersion", ""),
        "glide3x_md5": files.get("glide3x", {}).get("md5", ""),
        "icd_md5": files.get("icd", {}).get("md5", ""),
        "os_build": v.get("os_str") or v.get("os", ""),
        "agent_ver": v.get("agent_ver", ""),
        "gpu": (v.get("gpu") or {}).get("name", ""),
    })
    row.update({
        "stamp": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "title": title.name, "engine": title.engine, "res": res,
        "width": w, "height": h, "colordepth": depth, "aa_cfg": cfg,
        "chips": meta["chips"], "samples": meta["samples"],
        "aa_label": meta["label"], "api": getattr(title, "api", ""),
        "status": "pending",
    })
    # Free memory BEFORE the cell. A single cell is healthy in isolation, so the
    # thing that kills the agent on the ~5th cell of a boot is cumulative, and
    # this box has 255 MB. A monotonic decline across cells is the evidence that
    # separates a resource leak from a driver fault; without it both look like
    # "it died again". Cheap: SYSINFO is one call the runner already makes.
    try:
        import json as _json
        _si = _json.loads(await box.text("SYSINFO", timeout=30))
        _m = _si.get("memory", {})
        row["mem_avail_mb"] = _m.get("avail_mb", "")
        row["mem_load_pct"] = _m.get("load_percent", "")
    except Exception:
        pass
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
    # "the value read back", NOT "the card is anti-aliasing" - see
    # apply_aa_config. Anything downstream that wants the latter has to compare
    # this row against the matching no-AA cell.
    row["aa_verified"] = "reg-readback-only" if ok else f"NO(read={got})"
    if not ok:
        row["status"] = "aa-apply-failed"
        log(f"    AA config did not take (read back {got!r}) - not recording a number")
        return row

    # 2. stage config + launcher, clear the log
    env = {"SSTH3_SLI_AA_CONFIGURATION": str(cfg), "FX_GLIDE_SWAPINTERVAL": "0"}
    extra = dict(e.split("=", 1) for e in (getattr(args, "env", None) or []))
    env.update(extra)
    await title.prepare(box, w, h, depth, env)
    # prepare() may have MEASURED the identity (Quake II probes 3dfxgl.dll);
    # the row was built before that, so refresh it - the class default
    # "opengl-minigl" reached a row that ran on the Mesa ICD this way.
    row["api"] = getattr(title, "api", row["api"])
    row["engine"] = getattr(title, "engine", row["engine"])
    if extra:   # an A/B knob belongs to the row, not to the operator's memory
        row["engine"] += " [env " + " ".join(f"{k}={v}" for k, v in extra.items()) + "]"
    await box.exec_(f'cmd /c taskkill /f /im "{title.proc}"', timeout=30)
    await asyncio.sleep(2)

    # 3. go
    try:
        # A crash record on the box belongs to whichever cell made it. Keep any
        # that is there (labelled pre-<this cell>), then clear, so the record a
        # failure fetches is THIS cell's - not an earlier title's.
        try:
            import v56k_diag
            pre = await v56k_diag.watson_fetch(box, args.outdir / "diag", f"pre-{title.tid}_{res}_{depth}_cfg{cfg}")
            if pre:
                log(f"    (an earlier crash record was on the box - kept as pre-{title.tid}_{res}_{depth}_cfg{cfg})")
            await v56k_diag.watson_clear(box)
        except Exception:
            pass
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
    modal = None
    exited = None
    t_launch = time.time()
    last_poll = 0.0
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
                if renderer_up(raw) or (getattr(title, "up_re", None)
                                        and re.search(title.up_re, raw)):
                    last_growth = time.time()   # healthy, just slow - keep waiting
                else:
                    wedged = True
                    break
        # A game whose PROCESS IS GONE with no fps line can never produce one.
        # GLQuake crashed 5 s after GL init (a 1997 buffer vs a 1449-byte
        # GL_EXTENSIONS string) and the runner waited out the full max_run
        # five times over, 7 minutes each, for a log that would never grow.
        # Process and modal checks every 30 s, not every 6 s: each is a
        # connection to the single-threaded agent while the timed run is on,
        # and a bench with background work reads low. Tri-state on purpose:
        # None means the AGENT could not be asked, which must never read as
        # the GAME being gone - one slow PROCLIST mid-timedemo would otherwise
        # fail a healthy cell.
        if time.time() - t_launch > 20 and time.time() - last_poll >= 30:
            last_poll = time.time()
            try:
                import v56k_diag
                if exited is None:
                    alive = await v56k_diag.process_alive(box, title.proc)
                    if alive is False:
                        exited = True
                        break
                if modal is None:
                    modal = await v56k_diag.blocking_modal(box)
                    if modal:
                        break
            except Exception:
                pass
        await asyncio.sleep(6)

    # A game hung inside the driver can take longer than 30 s to die (CS 1.6
    # at 1600x1200 on one VSA-100, 2026-09-24): the kill then times out and
    # the cell used to become an anonymous "error: TimeoutError". It is a
    # real outcome - record it as such, and give the box time to settle.
    unkillable = False
    try:
        await box.exec_(f'cmd /c taskkill /f /im "{title.proc}"', timeout=30)
    except (asyncio.TimeoutError, TimeoutError):
        unkillable = True
        await asyncio.sleep(45)
    await asyncio.sleep(3)

    if raw:
        (args.outdir / f"{title.tid}_{res}_{depth}_cfg{cfg}.log").write_text(raw)
    if not parsed and raw:
        # the per-cell log above is overwritten by the next attempt at the same
        # cell, so a failure's log would be lost exactly when it matters
        faildir = args.outdir / "diag"
        faildir.mkdir(parents=True, exist_ok=True)
        (faildir / f"{title.tid}_{res}_{depth}_cfg{cfg}-FAILED-{time.strftime('%H%M%S')}.log").write_text(raw)
    if not parsed:
        row["status"] = "no-fps-line(see raw log)"
        if modal:
            # Name the dialog. "no-fps-line" invites another attempt; "blocked
            # by a modal" tells the operator the cell can never pass as staged.
            row["status"] = "blocked-by-modal"
            row["notes"] = f"blocking dialog: {modal[:80]}"
        elif exited:
            # The process died before printing a result: a crash, which the
            # diagnostic capture below will name from Dr Watson.
            row["status"] = "process-exited"
            row["notes"] = "game process gone before any fps line"
        me = getattr(title, "mount_error", None)
        if me is not None:
            try:
                err = await me(box)
                if err:
                    row["status"] = "mount-failed"
                    row["notes"] = f"disc-mount launcher reported: {err}"
            except Exception:
                pass
        # These are real, reportable outcomes for a card/driver, not tool
        # failures - a mode this card cannot bring up belongs in the article.
        if unkillable:
            tail = (raw or "").strip().splitlines()[-1:] or [""]
            row["status"] = "hung-unkillable"
            row["notes"] = (f"{title.proc} ignored taskkill for 30 s; log "
                            f"stopped at: {tail[0].strip()[:80]}")
        elif wedged:
            tail = (raw or "").strip().splitlines()[-1:] or [""]
            row["status"] = "gl-init-hung"
            row["notes"] = f"log stopped growing at: {tail[0].strip()[:90]}"
        elif raw and re.search(r"could not set the given mode|failed hard", raw, re.I):
            row["status"] = "mode-rejected-by-card"
        log(f"    -> {row['status']}  {row.get('notes','')}")
        row.update(title.attribution(raw))
        # A failed cell is the ONLY time the card will tell us why, so take the
        # flight recorder and the crash dump now. AmigaMerlin has no registry
        # ring of its own (measured: no RLog* anywhere), so this is Dr Watson +
        # the per-chip scanout dump - which is what named
        # glide3x!grDrawTriangle+0x2d as the Quake III "hang".
        try:
            import v56k_diag
            rep = await v56k_diag.capture(
                box, args.outdir / "diag", f"{title.tid}_{res}_{depth}_cfg{cfg}")
            w = (rep.get("watson") or {}).get("records") or []
            if w:
                row["notes"] = ((row.get("notes", "") + "; ") if row.get("notes") else "") + \
                    f"crash: {w[-1]['exception']} in {rep['watson'].get('fault_function')}"
                log(f"       crash: {w[-1]['exception']} in {rep['watson'].get('fault_function')}")
        except Exception as e:
            log(f"       (diagnostic capture failed: {type(e).__name__}: {e})")
        return row

    row.update(parsed)
    row.update(title.attribution(raw))
    row["status"] = "ok"
    # What the engine REPORTED beats what a file-size probe guessed. A Mesa
    # renderer string is the AmigaMerlin ICD (or a copy of it); a 3dfx MiniGL
    # never says "Mesa". Reconcile the label, and say that it was reconciled.
    rend = (row.get("gl_renderer") or "")
    if "Mesa" in rend and getattr(title, "tid", "") == "quake2" and str(row.get("api", "")).startswith(("opengl-minigl", "opengl-3dfxgl-")):
        row["notes"] = ((row.get("notes", "") + "; ") if row.get("notes") else "") + \
            f"api relabelled from '{row['api']}' to opengl-icd-gamelocal: GL_RENDERER '{rend[:40]}' is the Mesa ICD"
        row["api"] = "opengl-icd-gamelocal"
        if getattr(title, "tid", "") == "quake2":
            row["engine"] = "quake2.exe (3.20; game-local 3dfxgl.dll = AmigaMerlin ICD copy, identified from GL_RENDERER)"
    # A title may READ BACK which driver actually loaded (RtCW's r_glDriver is
    # latched, so a pinned driver can lose the first launch after a change). A
    # mismatch is recorded visibly - never published as a clean number for the
    # wrong driver - per the make-failure-visible rule.
    verify = getattr(title, "verify_driver", None)
    if verify is not None:
        ok_drv, drv_note = verify(raw)
        if not ok_drv:
            row["status"] = "driver-mismatch"
            row["notes"] = (row.get("notes", "") + "; " if row.get("notes") else "") + drv_note
    # And which MODE it ran at. Every Quake II row until 2026-09-24 ran at
    # 640x480x16 whatever it was labelled: the staged autoexec.cfg execs
    # fleetres.cfg, which reset gl_mode and restarted the renderer at the
    # box's own mode before the timedemo. The log said so; nothing read it.
    vmode = getattr(title, "verify_mode", None)
    if vmode is not None:
        bad = vmode(raw, w, h, depth)
        if bad:
            row["status"] = "wrong-mode"
            row["notes"] = (row.get("notes", "") + "; " if row.get("notes") else "") + bad
    log(f"    -> {parsed['avg_fps']} fps   [{row.get('gl_renderer','?')}]"
        f"  free={row.get('mem_avail_mb','?')}MB"
        + ("  !! " + row["notes"] if row["status"] in ("driver-mismatch", "wrong-mode") else ""))
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


def _where(e):
    """'func:line' of the innermost traceback frame inside this script."""
    import traceback
    here = os.path.basename(__file__)
    frames = [f for f in traceback.extract_tb(e.__traceback__)
              if os.path.basename(f.filename) == here]
    return " > ".join(f"{f.name}:{f.lineno}" for f in frames[-3:])


def append_row(csv_path, row):
    new = not csv_path.exists()
    with csv_path.open("a", newline="") as fh:
        w = csv.DictWriter(fh, fieldnames=CSV_COLS, extrasaction="ignore")
        if new:
            w.writeheader()
        w.writerow({k: row.get(k, "") for k in CSV_COLS})


def _lock_path(ip):
    return Path(__file__).resolve().parent / "results" / f".box-{ip}.lock"


def _take_box_lock(ip, outdir):
    import os
    p = _lock_path(ip)
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text(json.dumps({"pid": os.getpid(), "started": datetime.now(timezone.utc).isoformat(),
                             "outdir": str(outdir)}))
    return p


async def amain(args):
    _take_box_lock(args.host, args.outdir)
    box = Box(args.host)
    inst, prof = await find_display_instance(box)
    glide_key = GLIDE_KEY_TMPL.format(inst=inst)
    gpu = prof.get("gpu", {})
    log(f"box {args.host} {prof.get('hostname')}  agent {prof.get('agent_version')}")
    # A crash must fail the CELL, not the box: the Windows crash dialog sits
    # behind the exclusive fullscreen surface and reads as a wedge. Idempotent,
    # read back; HKLM so it survives reboots (not a re-image).
    try:
        import v56k_diag
        log(f"crash dialogs suppressed: {await v56k_diag.errors_quiet(box)}")
    except Exception as e:
        log(f"(could not suppress crash dialogs: {type(e).__name__})")
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

    # ---- what is under test, captured before a single frame is drawn ------ #
    log("capturing software versions (driver files, game binaries, OS, agent)")
    versions = await collect_versions(box, glide_key)
    versions["titles"] = {}
    for t in titles:
        versions["titles"][t.tid] = await title_identity(box, t)
        ti = versions["titles"][t.tid]
        log(f"  {t.name}: {ti.get('path','?')}  {ti.get('size','?')} B  "
            f"md5 {str(ti.get('md5','?'))[:12]}")
    for n in ("glide3x", "icd", "display"):
        f = versions["files"].get(n, {})
        log(f"  {n}: {f.get('size','?')} B  md5 {str(f.get('md5','?'))[:12]}")
    (args.outdir / "versions.json").write_text(json.dumps(versions, indent=2))
    log(f"versions -> {args.outdir / 'versions.json'}")

    checked, blocked = {}, {}
    for i, (t, w, h, d, c) in enumerate(matrix, 1):
        # Once per title: what Glide is sitting beside this game's exe?  A
        # wrong one either bypasses the card silently or takes the box down.
        if t.tid not in checked:
            ident = getattr(t, "identify", None)
            if ident is not None:
                try:
                    await ident(box)       # so the resume key below uses the MEASURED label
                except Exception as e:
                    log(f"    (identity probe failed for {t.tid}: {type(e).__name__})")
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
            row = await run_one(box, t, w, h, d, c, glide_key, args, versions)
            if row.get("status") == "process-exited" and "glide3x!" in (row.get("notes") or ""):
                # AmigaMerlin's Glide sometimes gets a dead board mapping in
                # grGlideInit (ioRegs -> unmapped); the very next launch works.
                # Keep the failed row - it is a real event - and try once more.
                append_row(csv_path, row)
                log("    glide3x init fault - retrying the cell once")
                await asyncio.sleep(10)
                row = await run_one(box, t, w, h, d, c, glide_key, args, versions)
        except Exception as e:                      # keep the campaign alive
            row = {k: "" for k in CSV_COLS}
            row.update({"stamp": datetime.now(timezone.utc).isoformat(),
                        "title": t.name, "res": f"{w}x{h}", "colordepth": d,
                        "aa_cfg": c, "chips": AA_CONFIGS[c]["chips"],
                        "samples": AA_CONFIGS[c]["samples"],
                        "aa_label": AA_CONFIGS[c]["label"],
                        "api": getattr(t, "api", ""),
                        "status": f"error: {type(e).__name__}: {e}",
                        # a bare TimeoutError says nothing: name the call
                        # that timed out (innermost frame in this file)
                        "notes": _where(e)})
            log(f"    !! {row['status']}  at {row['notes']}")
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
    ap.add_argument("--env", action="append", default=[], metavar="KEY=VAL",
                    help="extra environment for the game's launch .bat (repeatable); "
                         "recorded in the row's notes so an A/B is never anonymous")
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

    try:
        raise SystemExit(asyncio.run(amain(args)))
    finally:
        try:
            _lock_path(args.host).unlink()
        except OSError:
            pass


if __name__ == "__main__":
    main()
