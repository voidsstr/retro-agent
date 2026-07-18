#!/usr/bin/env python3
"""driver-bench runner: benchmark a 3dfx fleet machine, track everything in specpicks.

One command = preflight -> stack detection -> Q3 timedemo matrix -> optional
quality screenshot -> machine upsert + per-run rows in the specpicks DB ->
JSON drop in benchmarks/.

Examples:
  python3 run_bench.py --ip 192.168.1.124
  python3 run_bench.py --ip 192.168.1.124 --modes 3,6 --runs 2 \
      --changes "abc1234: SSE cliptest" --screenshot
"""
import argparse, asyncio, json, os, re, sys, time

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
sys.path.insert(0, REPO)
sys.path.insert(0, os.path.join(REPO, "scripts"))
from client.retro_protocol import RetroConnection  # noqa: E402
from specpicks_dsn import resolve_dsn  # noqa: E402

DSN = resolve_dsn()
SECRET = os.environ.get("RETRO_AGENT_SECRET", "retro-agent-secret")
MODE_RES = {3: "640x480", 4: "800x600", 6: "1024x768"}

# Counter-Strike 1.6 (GoldSrc/HLDS client) benchmark defaults. GoldSrc has a
# `timedemo <demo>` console command that plays a .dem and prints the fps line to
# the console; `-condebug` mirrors the console to <mod>\qconsole.log so we can
# scrape it exactly like Q3. Prereqs (stage once, see driver-bench SKILL): a CS
# install at CS16_DIR with a benchmark demo at cstrike\<CS16_DEMO>.dem, and the
# 3dfx GL renderer selected (hl.exe -gl -gldrv, or opengl32/retrogl in the CS
# dir). The Voodoo runs CS in OpenGL via our MesaFX ICD, same as Q3.
CS16_DIR = r"C:\Program Files\Counter-strike" # override with --cs16dir (found on .124)
CS16_EXE = "hl.exe"
CS16_DEMO = "bench"                            # cstrike\bench.dem
CS16_RES = "640x480"

# Quake II (idTech2 GL) benchmark defaults. Q2 has `timedemo 1` + `demomap
# <demo>`; with `logfile 2` it mirrors the console (incl. the "<n> frames,
# <s> seconds: <fps> fps" line + GL_RENDERER) to baseq2\qconsole.log. It loads
# our ICD via the `gl_driver` cvar (the deployed retrogl.dll). Needs ref_gl.dll
# present (stock GL renderer) and demo1.dm2 in baseq2\.
Q2_DIR = r"C:\Games\Quake2"                    # override with --q2dir (found on .124)
Q2_EXE = "quake2.exe"
Q2_DEMO = "demo1.dm2"
Q2_GLDRIVER = "3dfxgl"                         # gl_driver: 3dfxgl (works) | retrogl (our ICD, hangs in ref_gl - see fxwgl task)
Q2_MODES = {3: "640x480", 4: "800x600", 5: "960x720", 6: "1024x768"}

# ---------------------------------------------------------------------------
# Quality / video-card settings — recorded IN FULL on every run, and swept
# ---------------------------------------------------------------------------
# Every Q3 renderer knob that changes what the GPU/driver does, with the value
# the run uses. The full resolved set is written into each run's `settings`
# jsonb, so the DB always states exactly which quality knobs were on/off for a
# given fps. Named PROFILES set them explicitly on the launch line; a run can
# sweep several profiles (--quality-sweep) to cover permutations. When you
# optimize, benchmark across the profiles so a change is judged at every quality
# level, not just one.
QUALITY_DEFAULT = {
    "r_colorbits": "16",           # Voodoo3 framebuffer is 16bpp (no 32-bit render)
    "r_texturebits": "16",
    "r_textureMode": "GL_LINEAR_MIPMAP_NEAREST",  # bilinear + nearest-mip (Q3 default)
    "r_picmip": "1",               # texture detail reduction (0 = full detail)
    "r_ext_compressed_textures": "0",
    "r_overBrightBits": "1",
    "r_mapOverBrightBits": "2",
    "r_vertexLight": "0",          # 0 = lightmaps, 1 = vertex lighting (cheaper)
    "r_dynamiclight": "1",
    "r_subdivisions": "4",         # bezier-patch tessellation (lower = finer/heavier)
    "r_lodCurveError": "250",
    "r_lodBias": "0",              # mipmap LOD bias (negative = sharper, 3dfx trick)
    "r_detailtextures": "1",
    "r_flares": "0",
    "r_fastsky": "0",
    "r_finish": "0",
}
# Voodoo3 has no T-buffer, so no FSAA / motion blur / anisotropic — recorded as a
# constant so the DB is explicit about what the card cannot do (vs left off).
QUALITY_FIXED = {"fsaa": "none (Voodoo3 has no T-buffer)", "anisotropic": "none"}
# Named quality profiles = overrides on QUALITY_DEFAULT.
QUALITY_PROFILES = {
    "default": {},                 # stock Q3 defaults (above)
    "fast": {"r_picmip": "3", "r_vertexLight": "1", "r_dynamiclight": "0",
             "r_subdivisions": "20", "r_ext_compressed_textures": "1",
             "r_fastsky": "1", "r_detailtextures": "0", "r_lodCurveError": "10000"},
    "high": {"r_picmip": "0", "r_textureMode": "GL_LINEAR_MIPMAP_LINEAR",  # trilinear
             "r_ext_compressed_textures": "0", "r_dynamiclight": "1",
             "r_subdivisions": "4"},
    "max": {"r_picmip": "0", "r_textureMode": "GL_LINEAR_MIPMAP_LINEAR",
            "r_ext_compressed_textures": "0", "r_dynamiclight": "1",
            "r_subdivisions": "2", "r_lodBias": "-0.5", "r_detailtextures": "1",
            "r_lodCurveError": "10000"},
}


def resolve_quality(profile):
    """Return (launch_cvar_string, full_settings_dict) for a named quality profile."""
    cv = dict(QUALITY_DEFAULT)
    cv.update(QUALITY_PROFILES.get(profile, {}))
    extra = " ".join("+set %s %s" % (k, v) for k, v in cv.items())
    return extra, {**cv, **QUALITY_FIXED, "quality_profile": profile}

# system32 file fingerprints -> stack classification (size in bytes)
FPRINT = {
    "3dfxvs.dll": {595180: "retro3dfx (H5-source)", 624896: "AmigaMerlin 2.9"},
    "glide3x.dll": {335872: "retro3dfx (H5-source)", 344064: "AmigaMerlin retail"},
    # pure-3dfx lane (3dfx-driver-optimized): our renamed, WFP-safe display driver
    "3dfxv5d.dll": {595180: "3dfx-driver-optimized (H5-source, WFP-safe rename)"},
}


async def exw(c, cmd, secs):
    try:
        return await c.command_text("EXECW %d %s" % (secs, cmd), timeout=secs + 10)
    except Exception as e:
        return "__ERR__ " + str(e)


async def connect(ip):
    c = RetroConnection(ip, 9898)
    await c.connect(SECRET, timeout=10)
    return c


# pure-3dfx lane: after killing a Glide game the Voodoo is left in its fullscreen
# Glide mode; restoring the desktop mode IMMEDIATELY (before GDI repaints over the
# stale Glide framebuffer) avoids the garble. Set by --restore-mode; None = off.
RESTORE_DESKTOP = None
SETMODE_EXE = r'C:\RETRO_AGENT\3dfx-driver\setmode.exe'


async def kill_wait(c, image="quake3.exe"):
    await exw(c, r'cmd /c taskkill /f /im %s 2>nul' % image, 15)
    gone = False
    for _ in range(6):
        r = await exw(c, r'cmd /c tasklist /fi "imagename eq %s" /nh' % image, 12)
        if image.split(".")[0] not in r:
            gone = True
            break
        await asyncio.sleep(3)
    # restore the desktop mode immediately after the game is gone (3dfx lane)
    if RESTORE_DESKTOP:
        await exw(c, r'cmd /c %s %s' % (SETMODE_EXE, RESTORE_DESKTOP), 15)
    return gone


async def preflight(c):
    """Return (specs, gpu_ok, cpu_str). Aborts caller if not 3dfx / agent too old."""
    specs = {}
    si = json.loads(await c.command_text("SYSINFO", timeout=20))
    specs["sysinfo"] = si
    ver = si.get("agent_version", "0")
    if tuple(int(x) for x in ver.split(".")[:2]) < (1, 6):
        raise SystemExit("agent %s < 1.6.0 (EXECW required) - update the agent first" % ver)
    try:
        specs["videodiag"] = json.loads(await c.command_text("VIDEODIAG", timeout=25))
    except Exception:
        specs["videodiag"] = {}
    try:
        specs["pciscan"] = (await c.command_text("PCISCAN", timeout=30))[:4000]
    except Exception:
        specs["pciscan"] = ""
    blob = json.dumps(specs).lower()
    if "121a" not in blob and "3dfx" not in blob and "voodoo" not in blob:
        raise SystemExit("no 3dfx GPU detected on this machine - driver-bench is 3dfx-only for now")
    mhz = await exw(c, r'cmd /c reg query "HKLM\HARDWARE\DESCRIPTION\System\CentralProcessor\0" /v ~MHz', 15)
    m = re.search(r"0x([0-9a-fA-F]+)", mhz)
    cpu_mhz = int(m.group(1), 16) if m else None
    specs["cpu_mhz"] = cpu_mhz
    cpu = "%s ~%sMHz" % (si.get("cpu", {}).get("architecture", "x86"), cpu_mhz or "?")
    return specs, cpu


async def detect_stack(c, q3dir):
    """Classify the live driver stack from system32 fingerprints."""
    out = {"display_driver": "unknown", "glide3x": "unknown", "icd": "retro3dfx-gl (MesaFX 6.2 fork)"}
    for fn, table in FPRINT.items():
        # single % — this runs on a cmd /c command line, not inside a .bat
        r = await exw(c, r'cmd /c for %A in ("%SystemRoot%\system32\{0}") do @echo %~zA'.format(fn), 15)
        try:
            size = int(r.strip().splitlines()[-1])
        except Exception:
            size = None
        key = "display_driver" if fn == "3dfxvs.dll" else "glide3x"
        out[key] = table.get(size, "unrecognized (%s B)" % size)
    ours = [v for v in (out["display_driver"], out["glide3x"]) if "retro3dfx" in v]
    if len(ours) == 2:
        comp = "ALL-RETRO3DFX: our kernel display driver + our glide3x + our MesaFX ICD"
    elif ours:
        comp = "HYBRID: " + ", ".join("%s=%s" % (k, v) for k, v in out.items() if k != "icd")
    else:
        comp = "HYBRID: retail display+glide (%s / %s) + our MesaFX ICD" % (out["display_driver"], out["glide3x"])
    out["stack_composition"] = comp
    return out


DRWAT = (r'C:\Documents and Settings\All Users\Application Data'
         r'\Microsoft\Dr Watson\drwtsn32.log')


async def capture_crash_logs(c):
    """When a run yields no fps, grab the driver diagnostic logs so a failed
    benchmark still produces actionable output (pure-3dfx lane: our glide3x /
    3dfxogl write C:\\glide3x.log / C:\\3dfxogl.log; Dr Watson names the module
    + fault address). Returns a dict (empty strings if absent)."""
    out = {}
    out["glide3x_log"] = (await exw(c, r'cmd /c type C:\glide3x.log 2>nul', 20))[-2500:]
    out["ogl_log"] = (await exw(c, r'cmd /c type C:\3dfxogl.log 2>nul', 20))[-2000:]
    dw = await exw(c, r'cmd /c type "%s" 2>nul | findstr /i '
                      r'"function fault-> glide3x 3dfxogl grSstWinOpen ->0"' % DRWAT, 25)
    out["drwatson"] = dw[-1500:]
    return {k: v for k, v in out.items() if v and "__ERR__" not in v}


async def timedemo(ip, q3dir, mode, env, gldriver="retrogl", extra="", capture=False):
    """One timedemo run. Returns (fps, gl_renderer, crash_logs).

    gldriver: r_glDriver value ("retrogl" = MesaFX lane; "3dfxogl" = pure-3dfx
    ICD lane). extra: extra +set cvars appended. capture: on no-fps, pull the
    driver crash logs (for the pure-3dfx debug cycle)."""
    envcmd = ""
    if env:
        envcmd = "".join("set %s^& " % kv for kv in env.split())
    c = await connect(ip)
    await kill_wait(c)
    await exw(c, r'cmd /c del /f /q C:\q3home\baseq3\qconsole.log C:\glide3x.log C:\3dfxogl.log 2>nul', 12)
    await asyncio.sleep(2)
    await exw(c, r'cmd /c cd /d "%s" ^&^& %sstart "" quake3.exe +set r_glDriver %s '
                 r'+set r_mode %d +set r_fullscreen 1 +set r_colorbits 16 +set fs_homepath C:\q3home '
                 r'+set logfile 2 +set s_initsound 0 +set com_introPlayed 1 +set sv_pure 0 %s'
                 r'+set timedemo 1 +demo four'
              % (q3dir, envcmd, gldriver, mode, (extra + " " if extra else "")), 15)
    await c.close()
    await asyncio.sleep(78)
    c = await connect(ip)
    fps = gl = None
    for _ in range(4):
        log = await exw(c, r'cmd /c type C:\q3home\baseq3\qconsole.log 2>nul', 20)
        m = re.search(r"frames, [\d.]+ seconds: ([\d.]+) fps", log)
        gl = next((l.split("GL_RENDERER:", 1)[1].strip() for l in log.splitlines() if "GL_RENDERER" in l), gl)
        if m:
            fps = float(m.group(1))
            break
        await asyncio.sleep(10)
    crash = await capture_crash_logs(c) if (capture and fps is None) else None
    await kill_wait(c)
    await c.close()
    return fps, gl, crash


async def cs16_timedemo(ip, cs16dir, demo, env):
    """One CS 1.6 (GoldSrc) timedemo run. Returns (fps, gl_renderer).

    Plays a PRE-RECORDED demo (cstrike\\<demo>.dem) via `+timedemo <demo>` in
    fullscreen OpenGL (our 3dfx ICD) and scrapes qconsole.log for the fps line
    ("<n> frames <s> seconds <fps> fps", same shape as Q3 — GoldSrc descends
    from it). -condebug writes qconsole.log to the *working dir* (the CS root),
    NOT cstrike\\ — we read root first, then cstrike\\ as a fallback for other
    builds.

    Method note (BCShield): the Romania/BC no-Steam build is anti-cheat-protected;
    `timerefresh` is BLOCKED (halts the cfg command buffer), so a pre-recorded
    demo + `+timedemo` is the only automatable fps path. Record the demo ONCE
    (spawn via chooseteam/menuselect, `record <demo>`, wait, `stop` + flush waits,
    `quit`) — see the driver-bench SKILL. `quit`/`record`/`stop` are NOT blocked.
    """
    envcmd = "".join("set %s^& " % kv for kv in env.split()) if env else ""
    logs = (r"%s\qconsole.log" % cs16dir, r"%s\cstrike\qconsole.log" % cs16dir)
    c = await connect(ip)
    await kill_wait(c, CS16_EXE)
    for lg in logs:
        await exw(c, r'cmd /c del /f /q "%s" 2>nul' % lg, 12)
    await asyncio.sleep(2)
    # -gl forces OpenGL (our ICD) ; -full = our fullscreen Glide ; +timedemo plays
    # the demo (loads its own map) and prints the fps line. No -steam (non-Steam
    # build) / no +sv_cheats (demo playback needs neither).
    await exw(c, r'cmd /c cd /d "%s" ^&^& %sstart "" %s -game cstrike -gl -w 640 -h 480 -full '
                 r'-condebug -console -noipx -nojoy +timedemo %s'
              % (cs16dir, envcmd, CS16_EXE, demo), 15)
    await c.close()
    await asyncio.sleep(45)
    c = await connect(ip)
    fps = gl = None
    for _ in range(4):
        txt = ""
        for lg in logs:
            txt += await exw(c, r'cmd /c type "%s" 2>nul' % lg, 20)
        # ignore the "-1 frames 1.000 seconds -1.000 fps" demo-load priming lines
        cand = [mm for mm in re.finditer(r"(-?[\d.]+) frames\s+[\d.]+ seconds\s+(-?[\d.]+) fps", txt)
                if float(mm.group(2)) > 0] \
            or [mm for mm in re.finditer(r"frames, [\d.]+ seconds: ([\d.]+) fps", txt)]
        gl = next((l.split("GL_RENDERER:", 1)[1].strip() for l in txt.splitlines() if "GL_RENDERER" in l), gl)
        if cand:
            fps = float(cand[-1].group(cand[-1].lastindex))
            break
        await asyncio.sleep(10)
    await kill_wait(c, CS16_EXE)
    await c.close()
    return fps, gl


async def quake2_timedemo(ip, q2dir, demo, gl_mode, gl_driver, env):
    """One Quake II (idTech2 GL) timedemo run. Returns (fps, gl_renderer).

    Launches quake2.exe with vid_ref gl + our ICD via gl_driver, plays `demo`
    under timedemo, and scrapes baseq2\\qconsole.log (logfile 2) for the fps
    line (same "frames, ... seconds: ... fps" shape as Q3) and GL_RENDERER
    (carries the [retro3dfx 0.1.N] stamp)."""
    envcmd = "".join("set %s^& " % kv for kv in env.split()) if env else ""
    log = r"%s\baseq2\qconsole.log" % q2dir
    c = await connect(ip)
    await kill_wait(c, Q2_EXE)
    await exw(c, r'cmd /c del /f /q "%s" 2>nul' % log, 12)
    # Q2's ref_gl can ONLY create a GL context on the Voodoo3 when the DESKTOP is
    # 16-bit (a 32bpp desktop -> qwglCreateContext failed; Q3 works at 32bpp
    # because it uses its own GL init, not ref_gl's). Switch to 16bpp, restore after.
    desk = None
    try:
        cur = json.loads(await c.command_text("DISPLAYCFG get", timeout=15))
        desk = (cur.get("width") or 1024, cur.get("height") or 768, cur.get("refresh") or 75)
        await c.command_text("DISPLAYCFG set %d %d 16 %d" % desk, timeout=20)
        await asyncio.sleep(3)
    except Exception:
        pass
    await exw(c, r'cmd /c cd /d "%s" ^&^& %sstart "" %s +set vid_ref gl +set gl_driver %s '
                 r'+set gl_bitdepth 16 +set gl_mode %d +set vid_fullscreen 1 +set logfile 2 +set s_initsound 0 '
                 r'+set timedemo 1 +map %s'
              % (q2dir, envcmd, Q2_EXE, gl_driver, gl_mode, demo), 15)
    await c.close()
    await asyncio.sleep(60)
    c = await connect(ip)
    fps = gl = None
    for _ in range(4):
        txt = await exw(c, r'cmd /c type "%s" 2>nul' % log, 20)
        m = re.search(r"frames,?\s+[\d.]+ seconds:?\s+([\d.]+) fps", txt) \
            or re.search(r"([\d.]+) frames\s+[\d.]+ seconds\s+([\d.]+) fps", txt)
        gl = next((l.split("GL_RENDERER:", 1)[1].strip() for l in txt.splitlines() if "GL_RENDERER" in l), gl)
        if m:
            fps = float(m.group(m.lastindex))
            break
        await asyncio.sleep(10)
    await kill_wait(c, Q2_EXE)
    # restore the desktop to 32-bit (kill Q2 FIRST so the Glide surface is gone)
    if desk:
        try:
            await c.command_text("DISPLAYCFG set %d %d 32 %d" % desk, timeout=20)
        except Exception:
            pass
    await c.close()
    return fps, gl


# scenes for quality capture: (label, extra-cvars). "menu" exercises the 2D
# proportional-font path (text/menu quality); "q3dm1" a textured 3D world.
QUALITY_SCENES = {
    "menu":  "",                     # main menu (proportional font = text-quality probe)
    "q3dm1": "+devmap q3dm1",        # in-game 3D world + HUD
}


async def screenshot(ip, q3dir, outdir, gldriver="retrogl", scene="q3dm1", extra=""):
    """glReadPixels quality capture. Uses Q3's command-line `+screenshot` (NOT
    UIKEY, which can't reach fullscreen Glide) so it works headless: load the
    scene, wait for it to settle, capture a TGA, quit. Returns local PNG path or
    None. `scene` in QUALITY_SCENES ("menu" for the text/font-quality probe,
    "q3dm1" for a 3D scene); `extra` appends cvars (e.g. a quality profile)."""
    sc = QUALITY_SCENES.get(scene, "+devmap " + scene)
    c = await connect(ip)
    await kill_wait(c)
    await exw(c, r'cmd /c del /f /q C:\q3home\baseq3\screenshots\*.tga 2>nul', 12)
    # start /wait + explicit +screenshot +quit -> deterministic, no input injection
    await exw(c, (r'cmd /c cd /d "%s" ^&^& start /wait "" quake3.exe +set r_glDriver %s +set r_mode 3 '
                 r'+set r_fullscreen 1 +set r_colorbits 16 +set fs_homepath C:\q3home +set logfile 2 '
                 r'+set s_initsound 0 +set sv_pure 0 +set bot_enable 0 +set com_introPlayed 1 %s %s '
                 r'+wait 200 +screenshot +wait 60 +quit') % (q3dir, gldriver, sc, extra), 130)
    await c.close()
    await asyncio.sleep(3)
    c = await connect(ip)
    await kill_wait(c)   # also restores desktop mode (3dfx lane)
    lst = await exw(c, r'cmd /c dir /b C:\q3home\baseq3\screenshots\*.tga 2>nul', 15)
    tga = lst.strip().splitlines()[0].strip() if lst.strip() and "__ERR__" not in lst else None
    path = None
    if tga and tga.endswith(".tga"):
        data = await c.command_binary(r'DOWNLOAD C:\q3home\baseq3\screenshots\%s' % tga, timeout=90)
        raw = os.path.join(outdir, "quality_%s.tga" % scene)
        open(raw, "wb").write(data)
        try:
            from PIL import Image
            path = raw.replace(".tga", ".png")
            Image.open(raw).save(path)
            os.remove(raw)
        except Exception:
            path = raw
    await c.close()
    return path


async def deploy_dlls(ip, files, q3dir):
    """Deploy user-mode driver DLLs (glide3x.dll / 3dfxogl.dll) with NO reboot:
    kill the game, upload each, copy to system32 AND the game dir. Display
    driver / miniport are NOT handled here (they need the WFP-safe reboot flow
    in deploy-3dfx-driver). Returns list of (name, bytes)."""
    stage = r"C:\RETRO_AGENT\3dfx-driver"
    c = await connect(ip)
    out = []
    try:
        await kill_wait(c)
        await exw(c, r'cmd /c if not exist %s mkdir %s' % (stage, stage), 12)
        for f in files:
            name = os.path.basename(f)
            data = open(f, "rb").read()
            await c.send_command(r'UPLOAD %s\%s' % (stage, name), binary_payload=data, timeout=60)
            await exw(c, r'cmd /c copy /Y %s\%s C:\WINDOWS\system32\%s ^& copy /Y %s\%s "%s\%s"'
                      % (stage, name, name, stage, name, q3dir, name), 20)
            out.append((name, len(data)))
            print("deployed %s (%d B)" % (name, len(data)))
    finally:
        await c.close()
    return out


def track(machine_specs, cpu, ip, stack, runs, args, shot_path):
    import psycopg2
    conn = psycopg2.connect(DSN)
    conn.autocommit = True
    cur = conn.cursor()
    si = machine_specs.get("sysinfo", {})
    os_d = si.get("os", {}) if isinstance(si.get("os"), dict) else {}
    mem = si.get("memory", {}) if isinstance(si.get("memory"), dict) else {}
    gpu = stack.get("display_driver", "3dfx")
    cur.execute(
        """INSERT INTO retro_benchmark_machines (ip, hostname, os, cpu, ram_mb, gpu, specs)
           VALUES (%s,%s,%s,%s,%s,%s,%s)
           ON CONFLICT (ip) DO UPDATE SET hostname=EXCLUDED.hostname, os=EXCLUDED.os,
             cpu=EXCLUDED.cpu, ram_mb=EXCLUDED.ram_mb, gpu=EXCLUDED.gpu,
             specs=EXCLUDED.specs, updated_at=now() RETURNING id""",
        (ip, si.get("hostname"), ("%s %s" % (os_d.get("product", ""), os_d.get("version", ""))).strip() or None,
         cpu, mem.get("total_mb"), "3dfx Voodoo (%s)" % gpu, json.dumps(machine_specs)))
    mid = cur.fetchone()[0]
    n = 0
    for r in runs:
        cur.execute(
            """INSERT INTO retro_benchmark_runs
               (machine_id, benchmark, settings, driver_stack, driver_version, result_fps, result, lever, notes, source)
               VALUES (%s,%s,%s,%s,%s,%s,%s,%s,%s,'driver-bench-skill')""",
            (mid, r.get("benchmark", "q3-timedemo-four"),
             json.dumps({**r.get("settings", {}), "run_index": r["run"], "env": args.env or "none"}),
             json.dumps({**stack, "icd_version": r["driver_version"], "gl_renderer": r.get("gl_renderer"),
                         "changes": args.changes or None}),
             r["driver_version"], r.get("fps"), json.dumps(r), args.lever, args.notes))
        n += 1
    if shot_path:
        cur.execute(
            """INSERT INTO retro_benchmark_runs
               (machine_id, benchmark, settings, driver_stack, driver_version, result_fps, result, lever, notes, source)
               VALUES (%s,'q3-screenshot-q3dm1',%s,%s,%s,NULL,%s,'quality',%s,'driver-bench-skill')""",
            (mid, json.dumps({"resolution": "640x480", "map": "q3dm1", "capture": "in-engine glReadPixels"}),
             json.dumps({**stack, "changes": args.changes or None}),
             runs[0]["driver_version"] if runs else "unknown",
             json.dumps({"artifact": shot_path}),
             "quality artifact - pixel-diff vs the machine's baseline in benchmarks/"))
        n += 1
    conn.close()
    return mid, n


async def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--ip", required=True)
    ap.add_argument("--modes", default="3,6", help="r_mode list (3=640x480,4=800x600,6=1024x768)")
    ap.add_argument("--runs", type=int, default=2, help="runs per mode (2nd is official)")
    ap.add_argument("--env", default="", help='launcher env, e.g. "FX_GLIDE_SWAPINTERVAL=0"')
    ap.add_argument("--q3dir", default=r"C:\Quake III Arena\Quake3")
    ap.add_argument("--q2dir", default=Q2_DIR)
    ap.add_argument("--q2demo", default=Q2_DEMO, help="baseq2 demo, e.g. demo1.dm2")
    ap.add_argument("--q2modes", default="3,6", help="Q2 gl_mode list (3=640x480,6=1024x768)")
    ap.add_argument("--q2driver", default=Q2_GLDRIVER, help="Q2 gl_driver (3dfxgl works; retrogl=our ICD)")
    ap.add_argument("--game", default="q3", choices=["q3", "cs16", "q2", "both", "all"],
                    help="which benchmark(s) to run (default q3)")
    ap.add_argument("--cs16dir", default=CS16_DIR)
    ap.add_argument("--cs16demo", default=CS16_DEMO, help="cstrike\\<name>.dem to timedemo")
    ap.add_argument("--changes", default="", help="fork SHA + description of the driver change under test")
    ap.add_argument("--lever", default="performance", choices=["performance", "quality"])
    ap.add_argument("--quality", default="default", choices=list(QUALITY_PROFILES),
                    help="quality profile applied + recorded per run (default/fast/high/max)")
    ap.add_argument("--quality-sweep", default="", dest="quality_sweep",
                    help='comma list of quality profiles to permute (or "all"); '
                         "overrides --quality. Every run records the full resolved cvar set.")
    ap.add_argument("--notes", default="")
    # pure-3dfx lane (3dfx-driver-optimized on .143): our own OpenGL ICD, not MesaFX
    ap.add_argument("--gldriver", default="retrogl",
                    help="r_glDriver value: retrogl (MesaFX lane) | 3dfxogl (pure-3dfx ICD lane)")
    ap.add_argument("--driver-version", default="", dest="driver_version",
                    help="explicit driver version when GL_RENDERER carries no [retro3dfx X.Y] stamp "
                         "(pure-3dfx lane, e.g. 3dfxopt-0.1.0)")
    ap.add_argument("--stack-name", default="", dest="stack_name",
                    help="override stack name (e.g. 3dfx-driver-optimized); also captures driver crash "
                         "logs on a no-fps run")
    ap.add_argument("--screenshot", action="store_true", help="also capture in-engine quality artifacts")
    ap.add_argument("--shots", default="q3dm1,menu",
                    help="comma list of quality scenes to capture with --screenshot "
                         "(menu = 2D proportional-font/text probe; q3dm1 = 3D world; or any map name)")
    ap.add_argument("--no-db", action="store_true", help="skip specpicks tracking (local JSON only)")
    ap.add_argument("--deploy", nargs="*", default=[],
                    help="local paths of user-mode DLLs (glide3x.dll/3dfxogl.dll) to deploy "
                         "(no reboot) BEFORE benchmarking")
    ap.add_argument("--restore-mode", dest="restore_mode", default="",
                    help="desktop mode to restore after each Glide game exits, e.g. '1024 768 32 85' "
                         "(3dfx lane; avoids the stuck-Glide-mode garble). Default on for --stack-name.")
    args = ap.parse_args()

    global RESTORE_DESKTOP
    if args.restore_mode:
        RESTORE_DESKTOP = args.restore_mode
    elif args.stack_name:                      # pure-3dfx lane default
        RESTORE_DESKTOP = "1024 768 32 85"

    if args.deploy:
        await deploy_dlls(args.ip, args.deploy, args.q3dir)

    c = await connect(args.ip)
    specs, cpu = await preflight(c)
    stack = await detect_stack(c, args.q3dir)
    if args.stack_name:                       # pure-3dfx lane: override classification
        stack["stack_composition"] = args.stack_name
        stack["icd"] = "3dfxogl (H5-source OpenGL ICD)" if args.gldriver == "3dfxogl" else stack["icd"]
    await c.close()
    print("machine: %s | %s" % (cpu, stack["stack_composition"]))

    def ver_of(gl):
        if args.driver_version:               # explicit (pure-3dfx ICD has no retro3dfx stamp)
            return args.driver_version
        if gl:
            m = re.search(r"\[retro3dfx ([0-9.]+)\]", gl)
            if m:
                return m.group(1)
        return "unknown"

    capture = bool(args.stack_name)           # pull driver crash logs on no-fps in the debug lane
    # quality profiles to run (permutation coverage): --quality-sweep wins,
    # "all" = every named profile, else the single --quality.
    q_profiles = (list(QUALITY_PROFILES) if args.quality_sweep.strip() == "all"
                  else [p.strip() for p in args.quality_sweep.split(",") if p.strip()]
                  or [args.quality])
    runs = []
    if args.game in ("q3", "both", "all"):
        for mode in (int(m) for m in args.modes.split(",")):
            label = MODE_RES.get(mode, "mode%d" % mode)
            for qp in q_profiles:
                qextra, qsettings = resolve_quality(qp)
                for run in range(1, args.runs + 1):
                    fps, gl, crash = await timedemo(args.ip, args.q3dir, mode, args.env,
                                                    gldriver=args.gldriver, extra=qextra,
                                                    capture=capture)
                    ver = ver_of(gl)
                    rec = {"benchmark": "q3-timedemo-four", "resolution": label, "mode": mode,
                           "run": run, "fps": fps, "gl_renderer": gl, "driver_version": ver,
                           "settings": {"resolution": label, "r_mode": mode,
                                        "demo": "four.dm_66", "q3_version": "1.32", **qsettings}}
                    if crash:
                        rec["crash_logs"] = crash
                    runs.append(rec)
                    print("q3 %s [%s] run %d: %s fps [driver %s]%s"
                          % (label, qp, run, fps, ver, "  CRASH(logs captured)" if crash else ""))
    if args.game in ("cs16", "both", "all"):
        for run in range(1, args.runs + 1):
            fps, gl = await cs16_timedemo(args.ip, args.cs16dir, args.cs16demo, args.env)
            ver = ver_of(gl)
            runs.append({"benchmark": "cs16-timedemo", "resolution": CS16_RES, "mode": None,
                         "run": run, "fps": fps, "gl_renderer": gl, "driver_version": ver,
                         "settings": {"resolution": CS16_RES, "engine": "GoldSrc/hl.exe",
                                      "demo": args.cs16demo + ".dem",
                                      "renderer": "OpenGL (our ICD)", "colorbits": "16",
                                      "gl_texturemode": "GL_LINEAR_MIPMAP_LINEAR",
                                      "gl_max_size": "256", "gl_picmip": "0",
                                      "gl_round_down": "0", "gl_overbright": "1",
                                      "r_mmx": "1", "fps_max": "0",
                                      "fsaa": "none (Voodoo3 has no T-buffer)"}})
            print("cs16 run %d: %s fps [driver %s]" % (run, fps, ver))
    if args.game in ("q2", "all"):
        for mode in (int(m) for m in args.q2modes.split(",")):
            label = Q2_MODES.get(mode, "mode%d" % mode)
            for run in range(1, args.runs + 1):
                fps, gl = await quake2_timedemo(args.ip, args.q2dir, args.q2demo,
                                                mode, args.gldriver, args.env)
                ver = ver_of(gl)
                runs.append({"benchmark": "q2-timedemo", "resolution": label, "mode": mode,
                             "run": run, "fps": fps, "gl_renderer": gl, "driver_version": ver,
                             "settings": {"resolution": label, "gl_mode": mode,
                                          "engine": "idTech2/quake2.exe", "demo": args.q2demo,
                                          "renderer": "OpenGL (our ICD via gl_driver)",
                                          "gl_driver": args.gldriver, "colorbits": "16",
                                          "vid_fullscreen": "1",
                                          "fsaa": "none (Voodoo3 has no T-buffer)"}})
                print("q2 %s run %d: %s fps [driver %s]" % (label, run, fps, ver))

    outdir = os.path.join(REPO, "benchmarks")
    # Quality capture (--screenshot): grab BOTH a 3D scene AND the menu (the 2D
    # proportional-font path = text-quality probe), so a driver change is judged
    # on quality, not just fps. Read the PNGs to eyeball font/texture quality.
    shots = {}
    if args.screenshot:
        for scene in args.shots.split(","):
            scene = scene.strip()
            if not scene:
                continue
            p = await screenshot(args.ip, args.q3dir, outdir, args.gldriver, scene=scene)
            if p:
                shots[scene] = p
                print("quality artifact [%s]: %s" % (scene, p))
    shot = shots.get("q3dm1") or (next(iter(shots.values()), None))

    ver = next((r["driver_version"] for r in runs if r["driver_version"] != "unknown"), "unknown")
    drop = os.path.join(outdir, "%s_%s_driver-bench-%s.json" % (args.ip, time.strftime("%Y-%m-%d_%H%M"), ver))
    json.dump({"machine_ip": args.ip, "ts": time.strftime("%Y-%m-%dT%H:%M:%S"), "cpu": cpu,
               "stack": stack, "env": args.env or "none", "changes": args.changes,
               "q3_timedemo": runs, "screenshot": shot}, open(drop, "w"), indent=1)
    print("json drop:", drop)

    if not args.no_db:
        mid, n = track(specs, cpu, args.ip, stack, runs, args, shot)
        print("specpicks: machine id=%d, %d rows inserted" % (mid, n))


if __name__ == "__main__":
    asyncio.run(main())
