#!/usr/bin/env python3
"""GLQuake timedemo benchmark for a Win9x box driven through the retro agent.

Written for .243 (Win98 SE, Pentium 166 without MMX, Voodoo 2 12 MB) on
2026-09-25, where GLQuake runs from C:\\GAMES\\Quake1\\VOODOO over the Quake II
3.20 3dfx MiniGL. v56k_bench.py is the XP/Voodoo 5 harness and assumes cmd.exe;
this one only uses agent-internal commands plus LAUNCH, because on a
single-threaded 9x agent EXEC of a shell is the pattern that has killed it.

One timedemo per launch. The result is read from GLQuake's own console log
(-condebug -> id1\\qconsole.log), never inferred from timing:
    "969 frames  22.4 seconds  43.3 fps"
After the timedemo, quake.rc's demo loop plays the next demo, so the console
must be opened (TILDE) before `quit` + Enter ends it cleanly
(a force-kill of a fullscreen Glide app can leave the desktop unpainted; the
DESKFIX9 helper repaints it at the end regardless).

Quiesce: retro_chat.exe (the local chat client, three 1 s pollers on 9x) is
stopped for the run and restarted afterwards. Everything else that ran is
recorded in the result so a reader can judge the box state.

    python3 scripts/benchmarks/glquake_win9x_bench.py --host 192.168.1.243
"""
import argparse
import asyncio
import datetime
import json
import os
import re
import sys
import time

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, REPO)
from client.retro_protocol import RetroConnection  # noqa: E402

# GLQuake 1.08 prints TWO spaces between the fields ("969 frames  22.4 seconds
# 43.3 fps") - a single-space pattern missed a finished run on 2026-09-25.
# Rows go in the V5 6000 campaign's CSV format (v56k_bench.CSV_COLS), so every
# score on the fleet reads the same way and the same tools can load it.
def _csv_cols():
    import importlib.util
    spec = importlib.util.spec_from_file_location(
        "v56k_bench", os.path.join(os.path.dirname(os.path.abspath(__file__)), "v56k_bench.py"))
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m.CSV_COLS


def append_csv(path, rows):
    """Append rows (dicts keyed by CSV_COLS) to results.csv, header on create."""
    import csv
    cols = _csv_cols()
    os.makedirs(os.path.dirname(path), exist_ok=True)
    new = not os.path.exists(path)
    with open(path, "a", newline="") as f:
        w = csv.DictWriter(f, fieldnames=cols, extrasaction="ignore")
        if new:
            w.writeheader()
        for r in rows:
            w.writerow({k: r.get(k, "") for k in cols})


FPS_RE = re.compile(r"(\d+) frames\s+([\d.]+) seconds\s+([\d.]+) fps")
SECRET = os.environ.get("RETRO_AGENT_SECRET", "retro-agent-secret")


def names(proclist_json):
    return [p["name"].split("\\")[-1].upper() for p in json.loads(proclist_json)]


async def cmd(c, text, **kw):
    st, d = await c.send_command(text, **kw)
    return st, d


async def running(c, image):
    st, d = await cmd(c, "PROCLIST")
    return image.upper() in names(d)


async def kill_image(c, image):
    st, d = await cmd(c, "PROCLIST")
    for p in json.loads(d):
        if p["name"].split("\\")[-1].upper() == image.upper():
            await cmd(c, "PROCKILL %d" % p["pid"])


async def one_run(c, a, w, h, demo):
    base = a.basedir.rstrip("\\")
    log = base + "\\ID1\\QCONSOLE.LOG"
    await cmd(c, "DELETE " + log)
    line = ("LAUNCH %s\\%s -basedir %s -width %d -height %d -bpp %d -condebug +timedemo %s"
            % (base, a.exe, base, w, h, a.bpp, demo))
    t0 = time.time()
    st, d = await cmd(c, line)
    if st != 0:
        return {"error": "LAUNCH failed: " + d.decode("ascii", "replace")}
    text, m = "", None
    while time.time() - t0 < a.timeout:
        await asyncio.sleep(a.poll)
        st, d = await cmd(c, "DOWNLOAD " + log)
        if st == 1 or (st == 0 and d and not d.startswith(b"ERR")):
            text = d.decode("latin-1", "replace")
            m = FPS_RE.search(text)
            if m:
                break
    renderer = re.search(r"GL_RENDERER: (.*)", text)
    mode = re.search(r"(\d+)x(\d+)x?(\d+)? ?(?:fullscreen)?", text)
    res = {"demo": demo, "asked": "%dx%dx%d" % (w, h, a.bpp),
           "renderer": renderer.group(1).strip() if renderer else None,
           "wall_s": round(time.time() - t0, 1)}
    if m:
        res.update(frames=int(m.group(1)), seconds=float(m.group(2)), fps=float(m.group(3)))
    else:
        res["error"] = "no timedemo line in qconsole.log within %ds" % a.timeout
        res["log_tail"] = text[-600:]
    # Leave cleanly. After a timedemo quake.rc's demo loop plays the next demo,
    # so TILDE opens the console and `quit` from the console exits at once. If
    # nothing was playing, TILDE opened the main menu instead: ESCAPE drops back
    # to the console and the second attempt types into it.
    await asyncio.sleep(5)
    for attempt, keys in enumerate((("UIKEY TILDE", "UIKEY TEXT:quit", "UIKEY RETURN"),
                                    ("UIKEY ESCAPE", "UIKEY TEXT:quit", "UIKEY RETURN"))):
        for k in keys:
            await cmd(c, k)
            await asyncio.sleep(1.5)
        for _ in range(8):
            await asyncio.sleep(2)
            if not await running(c, "GLQUAKE.EXE"):
                break
        else:
            continue
        break
    else:
        await kill_image(c, "GLQUAKE.EXE")
        res["note"] = "had to be killed"
    await asyncio.sleep(a.settle)
    return res


# ---------------------------------------------------------------- Quake II
# The V5 6000 campaign's method (v56k_bench.Quake2), so the numbers are made
# the same way: a per-run bench.cfg (timedemo 1, swapinterval 0, and a
# nextserver that quits after the demo) and a fleetres.cfg carrying the
# renderer, because the staged autoexec.cfg execs fleetres.cfg LAST.
Q2_FPS_RE = re.compile(r"(\d+) frames, ([\d.]+) seconds: ([\d.]+) fps")
Q2_MODES = {(320, 240): 0, (400, 300): 1, (512, 384): 2, (640, 480): 3,
            (800, 600): 4, (960, 720): 5, (1024, 768): 6}


async def one_run_q2(c, a, w, h, demo):
    base = a.basedir.rstrip("\\")
    log = base + "\\baseq2\\qconsole.log"
    mode = Q2_MODES[(w, h)]
    bench = "\r\n".join(["// written per run by glquake_win9x_bench.py",
                           'set cl_maxfps "1000"', 'set gl_swapinterval "0"',
                           'set timedemo "1"', 'set nextserver "killserver; quit"',
                           "demomap %s.dm2" % demo, ""]).encode("ascii")
    fleetres = "\r\n".join(["// written per run by glquake_win9x_bench.py (the launcher rewrites it)",
                              'set vid_ref "gl"', 'set gl_driver "%s"' % a.gl_driver,
                              'set gl_mode "%d"' % mode, 'set vid_fullscreen "1"', ""]).encode("ascii")
    await cmd(c, "DELETE " + log)
    await cmd(c, "UPLOAD %s\\baseq2\\bench.cfg" % base, binary_payload=bench)
    await cmd(c, "UPLOAD %s\\baseq2\\fleetres.cfg" % base, binary_payload=fleetres)
    line = ("LAUNCH %s\\quake2.exe +set basedir %s +set vid_ref gl +set gl_driver %s "
            "+set gl_mode %d +set vid_fullscreen 1 +set logfile 2 +exec bench.cfg"
            % (base, base, a.gl_driver, mode))
    t0 = time.time()
    st, d = await cmd(c, line)
    if st != 0:
        return {"error": "LAUNCH failed: " + d.decode("ascii", "replace")}
    text, m = "", None
    while time.time() - t0 < a.timeout:
        await asyncio.sleep(a.poll)
        st, d = await cmd(c, "DOWNLOAD " + log)
        if st == 1 or (st == 0 and d and not d.startswith(b"ERR")):
            text = d.decode("latin-1", "replace")
            m = Q2_FPS_RE.search(text)
            if m:
                break
    modes = re.findall(r"setting mode \d+:\s*(\d+)\s+(\d+)", text)
    ren = re.search(r"GL_RENDERER: (.*)", text)
    res = {"demo": demo, "asked": "%dx%dx16" % (w, h), "renderer": ren.group(1).strip() if ren else None,
           "mode_ran": "%sx%s" % modes[-1] if modes else None, "wall_s": round(time.time() - t0, 1)}
    if m:
        res.update(frames=int(m.group(1)), seconds=float(m.group(2)), fps=float(m.group(3)))
        if modes and modes[-1] != (str(w), str(h)):
            res["error"] = "asked %dx%d, the timedemo ran at %sx%s" % (w, h, modes[-1][0], modes[-1][1])
    else:
        res["error"] = "no timedemo line in qconsole.log within %ds" % a.timeout
        res["log_tail"] = text[-600:]
    # it quits itself (nextserver); make sure
    for _ in range(15):
        await asyncio.sleep(2)
        if not await running(c, "QUAKE2.EXE"):
            break
    else:
        for k in ("UIKEY TILDE", "UIKEY TEXT:quit", "UIKEY RETURN"):
            await cmd(c, k)
            await asyncio.sleep(1.5)
        await asyncio.sleep(4)
        if await running(c, "QUAKE2.EXE"):
            await kill_image(c, "QUAKE2.EXE")
            res["note"] = "had to be killed"
    await asyncio.sleep(a.settle)
    return res


async def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--host", required=True)
    ap.add_argument("--game", choices=("glquake", "quake2"), default="glquake")
    ap.add_argument("--gl-driver", dest="gl_driver", default="3dfxgl",
                    help="quake2: gl_driver (3dfxgl = the MiniGL beside quake2.exe)")
    ap.add_argument("--basedir", default=r"C:\GAMES\Quake1")
    ap.add_argument("--exe", default=r"VOODOO\GLQUAKE.EXE")
    ap.add_argument("--bpp", type=int, default=16)
    ap.add_argument("--matrix", default="640x480:demo1,demo2,demo3;512x384:demo1;800x600:demo1",
                    help="mode:demo,demo;mode:demo ...")
    ap.add_argument("--runs", type=int, default=2)
    ap.add_argument("--timeout", type=int, default=180)
    ap.add_argument("--poll", type=int, default=10)
    ap.add_argument("--settle", type=int, default=8)
    ap.add_argument("--card", default="voodoo2-12mb")
    ap.add_argument("--csv", default=None,
                    help="append rows (v56k CSV format) here; default "
                         "scripts/benchmarks/results/voodoo2_<host>/results.csv")
    ap.add_argument("--notes", default="", help="appended to every row's notes")
    a = ap.parse_args()

    stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    outdir = os.path.join(REPO, "scripts", "benchmarks", "results",
                          "%s_%s-%s_%s" % (a.host, a.game, a.card, stamp))
    os.makedirs(outdir, exist_ok=True)
    c = RetroConnection(a.host, int(os.environ.get("RETRO_AGENT_PORT", "9898")))
    await c.connect(SECRET, timeout=15.0)
    results, meta = [], {}
    chat_was_running = False
    try:
        st, d = await cmd(c, "HWPROFILE")
        meta["hwprofile"] = json.loads(d)
        st, d = await cmd(c, "PROCLIST")
        chat_was_running = "RETRO_CHAT.EXE" in names(d)
        if chat_was_running:
            await kill_image(c, "RETRO_CHAT.EXE")
        await asyncio.sleep(3)
        st, d = await cmd(c, "PROCLIST")
        meta["processes_during_run"] = sorted(set(names(d)))
        for part in a.matrix.split(";"):
            mode, demos = part.split(":")
            w, h = (int(x) for x in mode.split("x"))
            for demo in demos.split(","):
                for r in range(a.runs):
                    res = await (one_run_q2 if a.game == "quake2" else one_run)(c, a, w, h, demo)
                    res["run"] = r + 1
                    results.append(res)
                    print(json.dumps(res), flush=True)
    finally:
        try:
            await cmd(c, "LAUNCH %s\\%sDESKFIX9.EXE" % (a.basedir.rstrip("\\"),
                                                        "" if a.game == "quake2" else "VOODOO\\"))
            if chat_was_running:
                await asyncio.sleep(5)
                await cmd(c, r"LAUNCH C:\RETRO_AGENT\retro_chat.exe")
        finally:
            await c.close()
    hw = meta.get("hwprofile", {})
    summary = {"host": a.host, "card": a.card, "when": stamp, "exe": a.exe, "bpp": a.bpp,
               "cpu_mhz": (hw.get("cpu") or {}).get("mhz"), "agent": hw.get("agent_version"),
               "results": results, "meta": meta}
    with open(os.path.join(outdir, "results.json"), "w") as f:
        json.dump(summary, f, indent=2)
    osd = hw.get("os") or {}
    gpu = hw.get("gpu") or {}
    ram = hw.get("ram_mb")
    rows = []
    for r in results:
        w, h, bpp = r["asked"].split("x")
        rows.append({
            "stamp": stamp,
            "title": ("Quake II (%s)" if a.game == "quake2" else "GLQuake (%s)") % r["demo"],
            "engine": "quake2.exe (3.20)" if a.game == "quake2" else a.exe,
            "api": ("ref_gl -> %s (3dfx MiniGL)" % a.gl_driver) if a.game == "quake2"
                   else "MiniGL (3dfxgl, Quake II 3.20)",
            "res": "%sx%s" % (w, h), "mode_line": r.get("mode_ran") or "",
            "width": w, "height": h, "colordepth": bpp, "chips": 1, "aa_label": "off",
            "avg_fps": r.get("fps", ""), "frames": r.get("frames", ""),
            "seconds": r.get("seconds", ""), "gl_renderer": r.get("renderer") or "",
            "game_exe": "quake2.exe" if a.game == "quake2" else a.exe, "driver_pkg": "3dfx Voodoo2 reference 3.02.02 (Glide 2.56)",
            "os_build": "%s %s" % (osd.get("name", ""), osd.get("version", "")),
            "agent_ver": hw.get("agent_version", ""),
            "gpu": "%s + %s" % (a.card, gpu.get("name", "")), "cpu_mhz": (hw.get("cpu") or {}).get("mhz", ""),
            "mem_avail_mb": ram or "",
            "status": "ok" if r.get("fps") else "fail",
            "notes": "; ".join(x for x in ("run %d" % r["run"], r.get("error", ""), r.get("note", ""),
                                            "retro_chat stopped for the run", a.notes) if x),
        })
    csv_path = a.csv or os.path.join(REPO, "scripts", "benchmarks", "results",
                                      "voodoo2_%s" % a.host, "results.csv")
    append_csv(csv_path, rows)
    print("csv:", csv_path)
    rows = {}
    for r in results:
        rows.setdefault((r["asked"], r["demo"]), []).append(r.get("fps"))
    md = ["| mode | demo | runs (fps) | mean |", "|---|---|---|---|"]
    for (mode, demo), fps in rows.items():
        good = [x for x in fps if x]
        md.append("| %s | %s | %s | %s |" % (mode, demo, ", ".join("%.1f" % x if x else "fail" for x in fps),
                                             "%.1f" % (sum(good) / len(good)) if good else "-"))
    with open(os.path.join(outdir, "summary.md"), "w") as f:
        f.write("\n".join(md) + "\n")
    print("\n".join(md))
    print("results:", outdir)


if __name__ == "__main__":
    asyncio.run(main())
