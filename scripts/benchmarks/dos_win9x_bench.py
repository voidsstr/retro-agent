#!/usr/bin/env python3
"""Timedemo benchmarks for DOS titles on a Win98 box, run in a Win98 DOS box.

Only titles whose -timedemo EXITS BY ITSELF are timed (Doom-engine family):
each job is a small batch that feeds stdin from KEY.TXT (the Hexen "Press any
key" screen otherwise blocks forever), redirects stdout to a file (the engine
prints "timed N gametics in M realtics" there on exit) and writes an .END
marker. The host LAUNCHes the batch, waits for the marker with agent-internal
DOWNLOADs (never EXEC - see CLAUDE.md), and parses the result:

    fps = gametics * 35 / realtics      (Doom's tic rate is 35 Hz)

Rows are appended in the fleet's v56k CSV format with api "Win98 DOS box", so
they never mix with real-DOS or Windows-native rows.

    python3 scripts/benchmarks/dos_win9x_bench.py --host 192.168.1.243 \
        --job "Doom (demo3)|C:\\DOOM|DOOM.EXE -timedemo demo3 -nosound -nomusic" --runs 2
"""
import argparse
import asyncio
import datetime
import json
import os
import re
import sys
import time

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
sys.path.insert(0, REPO)
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from client.retro_protocol import RetroConnection  # noqa: E402
from glquake_win9x_bench import append_csv, cmd, kill_image, names  # noqa: E402

TIMED_RE = re.compile(r"timed\s+(\d+)\s+gametics\s+in\s+(\d+)\s+realtics", re.I)
BENCH = "C:\\BENCH"


def job_bat(tag, directory, command):
    drive = directory[:2]
    return ("@echo off\r\n%s\r\ncd %s\r\n%s < %s\\KEY.TXT > %s\\RES\\%s.TXT\r\n"
            "echo END> %s\\RES\\%s.END\r\n" % (drive, directory[2:] or "\\", command, BENCH, BENCH, tag, BENCH, tag))


# Quake 1 (DOS QUAKE.EXE): "969 frames 30.5 seconds 31.8 fps" - written to
# id1\qconsole.log with -condebug (it prints nothing to stdout), so a Quake job
# names that log as its result file (the 4th field of --job).
QUAKE_RE = re.compile(r"(\d+)\s+frames\s+([\d.]+)\s+seconds\s+([\d.]+)\s+fps", re.I)


def quake_cfg(demo, waits=4000):
    """A cfg that timedemos `demo` and then QUITS: DOS Quake returns to its
    console after a timedemo and never exits by itself, and killing a DOS
    program on Win9x is not an option. Each `wait` holds the rest of the
    buffer one host frame; the chain outlasts the demo, then `quit` runs.

    The bare `startdemos` FIRST is load-bearing: quake.rc's own
    `startdemos demo1 demo2 demo3` runs after this cfg - i.e. after the whole
    wait chain - so cls.demonum is still 0 when the demo ends, and the demo's
    closing svc_disconnect (Host_EndGame) then goes to CL_NextDemo ("No demos
    listed with startdemos") instead of CL_Disconnect -> CL_FinishTimeDemo:
    no score is ever printed and DOS Quake 1.08 exits (measured on .243,
    2026-09-28). An empty startdemos sets demonum to -1, as id's own flow does."""
    lines = ["startdemos",
             'alias w10 "wait;wait;wait;wait;wait;wait;wait;wait;wait;wait"',
             'alias w100 "w10;w10;w10;w10;w10;w10;w10;w10;w10;w10"',
             'alias w1000 "w100;w100;w100;w100;w100;w100;w100;w100;w100;w100"',
             "timedemo %s" % demo] + ["w1000"] * (waits // 1000) + ["quit", ""]
    return "\r\n".join(lines)


def parse(text):
    q = QUAKE_RE.search(text)
    if q:
        return {"frames": int(q.group(1)), "seconds": float(q.group(2)), "fps": float(q.group(3))}
    m = TIMED_RE.search(text)
    if not m:
        return None
    tics, real = int(m.group(1)), int(m.group(2))
    return {"frames": tics, "seconds": round(real / 35.0, 2), "fps": round(tics * 35.0 / real, 1) if real else None}


class Link:
    """The agent connection, re-opened when a poll times out. A Win9x agent
    before 1.89.1 runs a LAUNCHed DOS job INSIDE its own console and goes deaf
    for the job's whole run the moment another client connects (measured on
    .243 2026-09-28) - it answers again when the job ends. So a timed-out poll
    is a reason to reconnect and keep waiting for the .END marker, not to die."""

    def __init__(self, host):
        self.host, self.c, self.stalls = host, None, 0

    async def open(self, deadline):
        while True:
            try:
                c = RetroConnection(self.host, 9898)
                await c.connect(os.environ.get("RETRO_AGENT_SECRET", "retro-agent-secret"), timeout=20.0)
                self.c = c
                return c
            except Exception:
                if time.time() > deadline:
                    raise
                await asyncio.sleep(10)

    async def drop(self):
        try:
            if self.c:
                await self.c.close()
        except Exception:
            pass
        self.c = None


async def run_job(link, tag, directory, command, timeout, result_file=None):
    c = link.c
    await cmd(c, "DELETE %s\\RES\\%s.END" % (BENCH, tag))
    await cmd(c, "DELETE %s\\RES\\%s.TXT" % (BENCH, tag))
    if result_file:
        await cmd(c, "DELETE %s\\%s" % (directory, result_file))
    await cmd(c, "UPLOAD %s\\%s.BAT" % (BENCH, tag), binary_payload=job_bat(tag, directory, command).encode("ascii"))
    t0 = time.time()
    st, d = await cmd(c, "LAUNCH %s\\%s.BAT" % (BENCH, tag))
    if st != 0:
        return {"error": "LAUNCH failed: " + d.decode("ascii", "replace")}
    stalled = 0
    while time.time() - t0 < timeout:
        await asyncio.sleep(5)
        try:
            st, d = await cmd(link.c, "DOWNLOAD %s\\RES\\%s.END" % (BENCH, tag))
        except Exception:
            stalled += 1
            link.stalls += 1
            await link.drop()
            await link.open(t0 + timeout)
            continue
        if st == 1:
            break
    else:
        return {"error": "no .END after %d s - the job may be waiting on the screen" % timeout}
    c = link.c
    st, d = await cmd(c, "DOWNLOAD %s\\%s" % (directory, result_file) if result_file
                      else "DOWNLOAD %s\\RES\\%s.TXT" % (BENCH, tag))
    text = d.decode("latin-1") if st == 1 else ""
    res = parse(text) or {"error": "no 'timed ... gametics' line", "tail": text[-300:]}
    res["wall_s"] = round(time.time() - t0, 1)
    if stalled:
        res["agent_stalled_polls"] = stalled
    return res


async def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--host", required=True)
    ap.add_argument("--job", action="append", required=True,
                    help='"title|C:\\dir|EXE args" (repeatable)')
    ap.add_argument("--runs", type=int, default=2)
    ap.add_argument("--timeout", type=int, default=300)
    ap.add_argument("--csv", default=None)
    ap.add_argument("--notes", default="")
    a = ap.parse_args()
    stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    outdir = os.path.join(REPO, "scripts", "benchmarks", "results", "%s_dosbox_%s" % (a.host, stamp))
    os.makedirs(outdir, exist_ok=True)
    link = Link(a.host)
    c = await link.open(time.time() + 60)
    results, meta, chat = [], {}, False
    try:
        st, d = await cmd(c, "HWPROFILE"); meta["hwprofile"] = json.loads(d)
        st, d = await cmd(c, "PROCLIST"); chat = "RETRO_CHAT.EXE" in names(d)
        if chat:
            await kill_image(c, "RETRO_CHAT.EXE")
        await cmd(c, "MKDIR %s\\RES" % BENCH)
        await cmd(c, "UPLOAD %s\\KEY.TXT" % BENCH, binary_payload=b"\r\n\r\n\r\n\r\n")
        await asyncio.sleep(3)
        st, d = await cmd(c, "PROCLIST"); meta["processes_during_run"] = sorted(set(names(d)))
        for i, j in enumerate(a.job):
            parts = j.split("|")
            title, directory, command = parts[:3]
            result_file = parts[3] if len(parts) > 3 and parts[3] else None
            for r in range(a.runs):
                tag = "J%dR%d" % (i, r)
                res = await run_job(link, tag, directory, command, a.timeout, result_file)
                c = link.c
                res.update({"title": title, "dir": directory, "command": command, "run": r + 1})
                results.append(res)
                print(json.dumps(res), flush=True)
                await asyncio.sleep(3)
    finally:
        try:
            if chat:
                await cmd(link.c, r"LAUNCH C:\RETRO_AGENT\retro_chat.exe")
        finally:
            await link.drop()
    hw = meta.get("hwprofile", {})
    json.dump({"host": a.host, "when": stamp, "results": results, "meta": meta},
              open(os.path.join(outdir, "results.json"), "w"), indent=2)
    osd = hw.get("os") or {}
    rows = [{
        "stamp": stamp, "title": r["title"], "engine": r["command"].split()[0],
        "api": "Win98 DOS box", "res": "320x200", "width": 320, "height": 200, "colordepth": 8,
        "avg_fps": r.get("fps", ""), "frames": r.get("frames", ""), "seconds": r.get("seconds", ""),
        "game_exe": r["dir"] + "\\" + r["command"].split()[0],
        "os_build": "%s %s" % (osd.get("name", ""), osd.get("version", "")),
        "agent_ver": hw.get("agent_version", ""), "gpu": "Cirrus GD5436 (VGA)",
        "cpu_mhz": (hw.get("cpu") or {}).get("mhz", ""), "mem_avail_mb": hw.get("ram_mb", ""),
        "status": "ok" if r.get("fps") else "fail",
        "notes": "; ".join(x for x in ("run %d" % r["run"], r["command"], r.get("error", ""),
                                        "retro_chat stopped" if chat else "", a.notes) if x),
    } for r in results]
    csv_path = a.csv or os.path.join(REPO, "scripts", "benchmarks", "results", "voodoo2_%s" % a.host, "results.csv")
    append_csv(csv_path, rows)
    print("csv:", csv_path)
    for r in results:
        print("%-28s run %d: %s" % (r["title"], r["run"], "%.1f fps" % r["fps"] if r.get("fps") else r.get("error")))


if __name__ == "__main__":
    asyncio.run(main())
