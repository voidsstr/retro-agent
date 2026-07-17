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
from client.retro_protocol import RetroConnection  # noqa: E402

DSN = os.environ.get(
    "SPECPICKS_DATABASE_URL",
    "postgresql://nscadmin:NscP0stgr3s!2026@nscappsdb.postgres.database.azure.com:5432/specpicks?sslmode=require",
)
SECRET = os.environ.get("RETRO_AGENT_SECRET", "retro-agent-secret")
MODE_RES = {3: "640x480", 4: "800x600", 6: "1024x768"}

# Counter-Strike 1.6 (GoldSrc/HLDS client) benchmark defaults. GoldSrc has a
# `timedemo <demo>` console command that plays a .dem and prints the fps line to
# the console; `-condebug` mirrors the console to <mod>\qconsole.log so we can
# scrape it exactly like Q3. Prereqs (stage once, see driver-bench SKILL): a CS
# install at CS16_DIR with a benchmark demo at cstrike\<CS16_DEMO>.dem, and the
# 3dfx GL renderer selected (hl.exe -gl -gldrv, or opengl32/retrogl in the CS
# dir). The Voodoo runs CS in OpenGL via our MesaFX ICD, same as Q3.
CS16_DIR = r"C:\Counter-Strike 1.6"          # override with --cs16dir
CS16_EXE = "hl.exe"
CS16_DEMO = "bench"                            # cstrike\bench.dem
CS16_RES = "640x480"

# system32 file fingerprints -> stack classification (size in bytes)
FPRINT = {
    "3dfxvs.dll": {595180: "retro3dfx (H5-source)", 624896: "AmigaMerlin 2.9"},
    "glide3x.dll": {335872: "retro3dfx (H5-source)", 344064: "AmigaMerlin retail"},
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


async def kill_wait(c, image="quake3.exe"):
    await exw(c, r'cmd /c taskkill /f /im %s 2>nul' % image, 15)
    for _ in range(6):
        r = await exw(c, r'cmd /c tasklist /fi "imagename eq %s" /nh' % image, 12)
        if image.split(".")[0] not in r:
            return
        await asyncio.sleep(3)


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


async def timedemo(ip, q3dir, mode, env):
    """One timedemo run. Returns (fps, gl_renderer)."""
    envcmd = ""
    if env:
        envcmd = "".join("set %s^& " % kv for kv in env.split())
    c = await connect(ip)
    await kill_wait(c)
    await exw(c, r'cmd /c del /f /q C:\q3home\baseq3\qconsole.log 2>nul', 12)
    await asyncio.sleep(2)
    await exw(c, r'cmd /c cd /d "%s" ^&^& %sstart "" quake3.exe +set r_glDriver retrogl '
                 r'+set r_mode %d +set r_fullscreen 1 +set r_colorbits 16 +set fs_homepath C:\q3home '
                 r'+set logfile 2 +set s_initsound 0 +set sv_pure 0 +set timedemo 1 +demo four'
              % (q3dir, envcmd, mode), 15)
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
    await kill_wait(c)
    await c.close()
    return fps, gl


async def cs16_timedemo(ip, cs16dir, demo, env):
    """One CS 1.6 (GoldSrc) timedemo run. Returns (fps, gl_renderer).

    Launches hl.exe in OpenGL with -condebug, autoexec'ing `timedemo <demo>`,
    then scrapes cstrike\\qconsole.log for the fps line (GoldSrc prints
    "<n> frames <s> seconds <fps> fps" just like the Q3 engine it descends from)
    and the GL_RENDERER (carries our [retro3dfx 0.1.N] driver stamp).
    """
    envcmd = "".join("set %s^& " % kv for kv in env.split()) if env else ""
    log = r"%s\cstrike\qconsole.log" % cs16dir
    c = await connect(ip)
    await kill_wait(c, CS16_EXE)
    await exw(c, r'cmd /c del /f /q "%s" 2>nul' % log, 12)
    await asyncio.sleep(2)
    # -condebug -> qconsole.log ; -gl forces OpenGL ; +timedemo runs and prints fps.
    # +sv_cheats/+fps_max 0 keep the demo from being frame-capped.
    await exw(c, r'cmd /c cd /d "%s" ^&^& %sstart "" %s -steam -game cstrike -condebug -gl '
                 r'-w 640 -h 480 +fps_max 0 +sv_cheats 1 +timedemo %s'
              % (cs16dir, envcmd, CS16_EXE, demo), 15)
    await c.close()
    await asyncio.sleep(70)
    c = await connect(ip)
    fps = gl = None
    for _ in range(4):
        txt = await exw(c, r'cmd /c type "%s" 2>nul' % log, 20)
        m = re.search(r"([\d.]+) frames\s+[\d.]+ seconds\s+([\d.]+) fps", txt) \
            or re.search(r"frames, [\d.]+ seconds: ([\d.]+) fps", txt)
        gl = next((l.split("GL_RENDERER:", 1)[1].strip() for l in txt.splitlines() if "GL_RENDERER" in l), gl)
        if m:
            fps = float(m.group(m.lastindex))
            break
        await asyncio.sleep(10)
    await kill_wait(c, CS16_EXE)
    await c.close()
    return fps, gl


async def screenshot(ip, q3dir, outdir):
    """In-engine glReadPixels capture on q3dm1. Returns local png path or None."""
    c = await connect(ip)
    await kill_wait(c)
    await exw(c, r'cmd /c del /f /q C:\q3home\baseq3\screenshots\*.tga 2>nul', 12)
    await exw(c, r'cmd /c cd /d "%s" ^&^& start "" quake3.exe +set r_glDriver retrogl +set r_mode 3 '
                 r'+set r_fullscreen 1 +set r_colorbits 16 +set fs_homepath C:\q3home +set logfile 2 '
                 r'+set s_initsound 0 +set sv_pure 0 +set bot_enable 0 +set com_introplayed 1 '
                 r'+devmap q3dm1 +bind F12 screenshot' % q3dir, 15)
    await c.close()
    await asyncio.sleep(40)
    c = await connect(ip)
    try:
        await c.command_text("UIKEY f12", timeout=10)
        await asyncio.sleep(4)
    except Exception:
        pass
    lst = await exw(c, r'cmd /c dir /b C:\q3home\baseq3\screenshots\*.tga 2>nul', 15)
    tga = lst.strip().splitlines()[0].strip() if lst.strip() and "__ERR__" not in lst else None
    path = None
    if tga and tga.endswith(".tga"):
        data = await c.command_binary(r'DOWNLOAD C:\q3home\baseq3\screenshots\%s' % tga, timeout=90)
        raw = os.path.join(outdir, "quality_shot.tga")
        open(raw, "wb").write(data)
        try:
            from PIL import Image
            path = raw.replace(".tga", ".png")
            Image.open(raw).save(path)
        except Exception:
            path = raw
    await kill_wait(c)
    await c.close()
    return path


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
    ap.add_argument("--game", default="q3", choices=["q3", "cs16", "both"],
                    help="which benchmark(s) to run (default q3)")
    ap.add_argument("--cs16dir", default=CS16_DIR)
    ap.add_argument("--cs16demo", default=CS16_DEMO, help="cstrike\\<name>.dem to timedemo")
    ap.add_argument("--changes", default="", help="fork SHA + description of the driver change under test")
    ap.add_argument("--lever", default="performance", choices=["performance", "quality"])
    ap.add_argument("--notes", default="")
    ap.add_argument("--screenshot", action="store_true", help="also capture the in-engine quality artifact")
    ap.add_argument("--no-db", action="store_true", help="skip specpicks tracking (local JSON only)")
    args = ap.parse_args()

    c = await connect(args.ip)
    specs, cpu = await preflight(c)
    stack = await detect_stack(c, args.q3dir)
    await c.close()
    print("machine: %s | %s" % (cpu, stack["stack_composition"]))

    def ver_of(gl):
        if gl:
            m = re.search(r"\[retro3dfx ([0-9.]+)\]", gl)
            if m:
                return m.group(1)
        return "unknown"

    runs = []
    if args.game in ("q3", "both"):
        for mode in (int(m) for m in args.modes.split(",")):
            label = MODE_RES.get(mode, "mode%d" % mode)
            for run in range(1, args.runs + 1):
                fps, gl = await timedemo(args.ip, args.q3dir, mode, args.env)
                ver = ver_of(gl)
                runs.append({"benchmark": "q3-timedemo-four", "resolution": label, "mode": mode,
                             "run": run, "fps": fps, "gl_renderer": gl, "driver_version": ver,
                             "settings": {"resolution": label, "r_mode": mode, "colorbits": 16,
                                          "demo": "four.dm_66", "q3_version": "1.32"}})
                print("q3 %s run %d: %s fps [driver %s]" % (label, run, fps, ver))
    if args.game in ("cs16", "both"):
        for run in range(1, args.runs + 1):
            fps, gl = await cs16_timedemo(args.ip, args.cs16dir, args.cs16demo, args.env)
            ver = ver_of(gl)
            runs.append({"benchmark": "cs16-timedemo", "resolution": CS16_RES, "mode": None,
                         "run": run, "fps": fps, "gl_renderer": gl, "driver_version": ver,
                         "settings": {"resolution": CS16_RES, "engine": "GoldSrc/hl.exe",
                                      "demo": args.cs16demo + ".dem", "renderer": "OpenGL (retrogl)"}})
            print("cs16 run %d: %s fps [driver %s]" % (run, fps, ver))

    outdir = os.path.join(REPO, "benchmarks")
    shot = await screenshot(args.ip, args.q3dir, outdir) if args.screenshot else None
    if shot:
        print("quality artifact:", shot)

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
