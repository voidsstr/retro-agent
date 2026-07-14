#!/usr/bin/env python3
"""
benchmark_runner.py - automated retro GPU benchmark orchestrator.

Drives a retro_agent machine over TCP to run a suite of period benchmarks
(Quake III, UT, Deus Ex, Serious Sam, Giants, 3DMark 99/2000/2001) unattended,
collects the results, parses FPS/scores, and writes a per-run results folder
plus a plain-ASCII summary the chat brain can relay.

Design notes
------------
- Games are GUI: launched with LAUNCH (never EXEC). Completion is detected by
  polling PROCLIST until the game's process exits (auto_quits titles), or by a
  fixed-time run + PROCKILL for engines that loop (Giants/FRAPS).
- Every connection is closed GRACEFULLY (abrupt TCP disconnects crash Win98).
- Fully data-driven: benchmarks.json defines each title's config template,
  launch line, result file, and parser. Edit paths there, not here.
- First-run calibration is expected: exact batch flags / log formats vary by
  build. Parsers are tolerant and always keep the raw output for manual review.

Usage
-----
  python3 benchmark_runner.py --host 192.168.1.143
  python3 benchmark_runner.py --host 192.168.1.143 --card voodoo5-6000 \
      --titles quake3,ut,3dmark2001 --resolutions 1024x768,1600x1200 --fsaa off,4x
  python3 benchmark_runner.py --host 192.168.1.143 --dry-run     # plan only
"""

import argparse
import asyncio
import json
import os
import re
import sys
import time
from pathlib import Path

_REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
if _REPO not in sys.path:
    sys.path.insert(0, _REPO)

from client.retro_protocol import RetroConnection, RetroProtocolError  # noqa: E402

HERE = Path(__file__).resolve().parent
TEMPLATES = HERE / "templates"
RESULTS = HERE / "results"

AGENT_SECRET = os.environ.get("RETRO_AGENT_SECRET", "retro-agent-secret")
AGENT_PORT = int(os.environ.get("RETRO_AGENT_PORT", "9898"))
CONNECT_TIMEOUT = 15.0
CMD_TIMEOUT = 90.0

POLL_INTERVAL = 3.0     # seconds between PROCLIST checks
LOAD_GRACE = 10.0       # give the game this long to appear before deciding "exited"


def log(msg):
    print(f"[{time.strftime('%H:%M:%S')}] {msg}", flush=True)


# --------------------------------------------------------------------------- #
# agent command helpers
# --------------------------------------------------------------------------- #

async def exec_text(conn, cmdline):
    """Run a hidden CLI command (EXEC) and return its captured output."""
    try:
        return await conn.command_text(f"EXEC {cmdline}", timeout=CMD_TIMEOUT)
    except RetroProtocolError as e:
        return f"__ERR__ {e}"


async def path_exists(conn, path):
    """Robust existence check that survives spaces in paths (DIRLIST does not)."""
    out = await exec_text(conn, f'cmd /c if exist "{path}" (echo FOUND) else (echo MISSING)')
    return "FOUND" in out.upper()


async def upload_text(conn, path, text):
    """Write a text file to the agent via UPLOAD (CRLF for DOS tools)."""
    payload = text.replace("\r\n", "\n").replace("\n", "\r\n").encode("ascii", "replace")
    status, data = await conn.send_command(f"UPLOAD {path}", binary_payload=payload,
                                            timeout=CMD_TIMEOUT)
    if status == 0xFF:
        raise RetroProtocolError(data.decode("ascii", "replace"))


async def download_text(conn, path):
    try:
        data = await conn.command_binary(f"DOWNLOAD {path}", timeout=CMD_TIMEOUT)
        return data.decode("ascii", "replace")
    except RetroProtocolError:
        return None


async def proclist(conn):
    try:
        return await conn.command_text("PROCLIST", timeout=CMD_TIMEOUT)
    except RetroProtocolError:
        return ""


async def proc_running(conn, name):
    raw = await proclist(conn)
    return name.lower() in raw.lower()


async def kill_proc(conn, name):
    """Best-effort terminate by image name: find pid in PROCLIST, else taskkill."""
    raw = await proclist(conn)
    pid = None
    try:
        data = json.loads(raw)
        if isinstance(data, list):
            for p in data:
                if isinstance(p, dict):
                    pname = str(p.get("name") or p.get("image") or p.get("exe") or "")
                    if name.lower() in pname.lower():
                        pid = p.get("pid") or p.get("Pid") or p.get("PID")
                        break
    except Exception:
        pass
    if pid is not None:
        try:
            await conn.command_text(f"PROCKILL {pid}", timeout=CMD_TIMEOUT)
            return
        except RetroProtocolError:
            pass
    await exec_text(conn, f"taskkill /f /im {name}")  # XP fallback (no-op on 98)


# --------------------------------------------------------------------------- #
# card detection & combo planning
# --------------------------------------------------------------------------- #

async def detect_card(conn, cards):
    """Match VIDEODIAG output against each card profile's keywords."""
    try:
        diag = await conn.command_text("VIDEODIAG", timeout=CMD_TIMEOUT)
    except RetroProtocolError as e:
        log(f"VIDEODIAG failed: {e}")
        return None, ""
    low = diag.lower()
    best = None
    for card_id, prof in cards.items():
        for kw in prof.get("match", []):
            if kw.lower() in low:
                # prefer the most specific (longest keyword) match
                if best is None or len(kw) > best[1]:
                    best = (card_id, len(kw))
    return (best[0] if best else None), diag


def parse_res(res):
    w, h = res.lower().split("x")
    w, h = int(w), int(h)
    return w, h


def depthbits_for(colordepth):
    return 24 if colordepth >= 32 else 16


def sanitize(s):
    return re.sub(r"[^A-Za-z0-9._-]", "_", s)


# --------------------------------------------------------------------------- #
# result parsers  (tolerant; always keep raw)
# --------------------------------------------------------------------------- #

def parse_quake3(raw):
    m = re.search(r"(\d+)\s+frames\s+([\d.]+)\s+seconds\s+([\d.]+)\s*fps", raw, re.I)
    if m:
        return {"avg": float(m.group(3)), "frames": int(m.group(1)),
                "seconds": float(m.group(2))}
    return {}


def parse_unreal_log(raw):
    out = {}
    m = re.search(r"Average.*?([\d.]+)\s*(?:fps|frames)", raw, re.I)
    if m:
        out["avg"] = float(m.group(1))
    m = re.search(r"Min(?:imum)?.*?([\d.]+)", raw, re.I)
    if m:
        out["min"] = float(m.group(1))
    m = re.search(r"Max(?:imum)?.*?([\d.]+)", raw, re.I)
    if m:
        out["max"] = float(m.group(1))
    return out


def parse_serioussam(raw):
    out = {}
    m = re.search(r"Average(?:\s*FPS)?[:=\s]+([\d.]+)", raw, re.I)
    if m:
        out["avg"] = float(m.group(1))
    m = re.search(r"Min(?:imum)?(?:\s*FPS)?[:=\s]+([\d.]+)", raw, re.I)
    if m:
        out["min"] = float(m.group(1))
    return out


def parse_fraps(raw):
    # FRAPS FPS csv: "Frames, Time (ms), Min, Max, Avg" or a per-second FPS column
    nums = [float(x) for x in re.findall(r"[\d.]+", raw)]
    if not nums:
        return {}
    m = re.search(r"Avg[^\d]*([\d.]+)", raw, re.I)
    out = {}
    if m:
        out["avg"] = float(m.group(1))
    elif nums:
        out["avg"] = round(sum(nums) / len(nums), 2)
    return out


def parse_futuremark(raw):
    out = {}
    m = re.search(r"([\d,]{3,})\s*3DMarks?", raw, re.I) or \
        re.search(r"3DMarks?[:=\s]+([\d,]+)", raw, re.I)
    if m:
        out["score"] = int(m.group(1).replace(",", ""))
    return out


PARSERS = {
    "quake3": parse_quake3,
    "unreal_log": parse_unreal_log,
    "serioussam": parse_serioussam,
    "fraps": parse_fraps,
    "futuremark": parse_futuremark,
}


# --------------------------------------------------------------------------- #
# single run
# --------------------------------------------------------------------------- #

def render_template(name, subs):
    text = (TEMPLATES / name).read_text()
    for k, v in subs.items():
        text = text.replace("{" + k + "}", str(v))
    return text


def split_launch(launch):
    """Split '<...>.exe <args>' into (exe, args), tolerating spaces in the path."""
    idx = launch.lower().find(".exe")
    if idx == -1:
        return launch, ""
    return launch[:idx + 4], launch[idx + 4:].strip()


async def run_one(conn, card, title_id, title, res, colordepth, fsaa, outdir, args):
    w, h = parse_res(res)
    tag = sanitize(f"{card}_{title_id}_{res}_{colordepth}bit_{fsaa}")
    row = {"title": title_id, "name": title["name"], "card": card, "res": res,
           "colordepth": colordepth, "fsaa": fsaa, "api": title.get("api", ""),
           "tag": tag, "status": "pending", "avg": None, "min": None,
           "max": None, "score": None}
    log(f"--- {title['name']}  {res} {colordepth}bit  FSAA={fsaa}")

    if args.dry_run:
        row["status"] = "dry-run (would run)"
        return row

    subs = {"width": w, "height": h, "colordepth": colordepth,
            "depthbits": depthbits_for(colordepth), "tag": tag,
            "demo": title.get("demo", ""), "use32": 1 if colordepth >= 32 else 0,
            "renderdevice": args.renderdevice or "D3DDrv.D3DRenderDevice",
            "api": 0}

    # 1. write per-run config (if the title uses one)
    cfg = title.get("config")
    if cfg:
        try:
            await upload_text(conn, cfg["target"], render_template(cfg["template"], subs))
        except RetroProtocolError as e:
            row["status"] = f"config-upload-failed: {e}"
            return row

    # 2. clear any stale result file
    result_file = title["result_file"].replace("{tag}", tag)
    await exec_text(conn, f'del /f /q "{result_file}"')
    # UT/DeusEx write into a Benchmark subdir that may not exist
    await exec_text(conn, f'cmd /c md "{os.path.dirname(result_file)}"')

    # 3. launch (wrap in cmd/start so cwd is the game's System dir)
    exe, exe_args = split_launch(title["launch"])
    exe_args = exe_args.replace("{tag}", tag)
    launch_cmd = f'cmd /c cd /d "{title["workdir"]}" && start "bench" "{exe}" {exe_args}'
    try:
        await conn.command_text(f"LAUNCH {launch_cmd}", timeout=CMD_TIMEOUT)
    except RetroProtocolError as e:
        row["status"] = f"launch-failed: {e}"
        return row

    # 4. wait for completion
    proc = title["proc_name"]
    max_run = float(title.get("max_run_s", 240))
    started = time.time()
    await asyncio.sleep(LOAD_GRACE)
    finished = False
    while time.time() - started < max_run:
        if title.get("auto_quits", True):
            if not await proc_running(conn, proc):
                finished = True
                break
        await asyncio.sleep(POLL_INTERVAL)
    if not finished:
        log(f"    max_run reached ({max_run}s) - terminating {proc}")
        await kill_proc(conn, proc)
        await asyncio.sleep(3)

    # 5. collect + parse
    await asyncio.sleep(2)  # let the log flush
    raw = await download_text(conn, result_file)
    if raw is None:
        row["status"] = "no-result-file"
        return row
    (outdir / f"{tag}.raw.txt").write_text(raw)
    parsed = PARSERS.get(title.get("parser", ""), lambda r: {})(raw)
    row.update({k: parsed.get(k) for k in ("avg", "min", "max", "score") if k in parsed})
    row["status"] = "ok" if parsed else "parsed-empty(see raw)"
    log(f"    -> {row['status']}  avg={row.get('avg')}  score={row.get('score')}")
    return row


# --------------------------------------------------------------------------- #
# summary
# --------------------------------------------------------------------------- #

def write_outputs(outdir, meta, rows):
    # CSV
    cols = ["title", "name", "card", "res", "colordepth", "fsaa", "api",
            "avg", "min", "max", "score", "status", "tag"]
    lines = [",".join(cols)]
    for r in rows:
        lines.append(",".join(str(r.get(c, "")) if r.get(c) is not None else ""
                              for c in cols))
    (outdir / "results.csv").write_text("\n".join(lines) + "\n")

    # plain-ASCII summary for the chat brain
    s = []
    s.append(f"RETRO GPU BENCHMARK - {meta['card']} on {meta['host']}")
    s.append(f"  {meta['hostname']}  {meta['os']}")
    s.append(f"  run: {meta['stamp']}   titles: {meta['n_titles']}   runs: {len(rows)}")
    s.append("")
    header = f"  {'title':<14}{'res':<11}{'depth':<7}{'fsaa':<6}{'avg fps':>9}{'score':>9}  status"
    s.append(header)
    s.append("  " + "-" * (len(header) - 2))
    for r in rows:
        avg = f"{r['avg']:.1f}" if r.get("avg") is not None else "-"
        sc = str(r["score"]) if r.get("score") is not None else "-"
        s.append(f"  {r['title']:<14}{r['res']:<11}{str(r['colordepth'])+'bit':<7}"
                 f"{r['fsaa']:<6}{avg:>9}{sc:>9}  {r['status']}")
    s.append("")
    s.append(f"  full data: {outdir}/results.csv  (+ per-run raw logs)")
    text = "\n".join(s)
    (outdir / "summary.txt").write_text(text + "\n")
    (outdir / "meta.json").write_text(json.dumps({"meta": meta, "rows": rows}, indent=2))
    return text


# --------------------------------------------------------------------------- #
# main
# --------------------------------------------------------------------------- #

async def amain(args):
    manifest = json.loads(Path(args.manifest).read_text())
    cards = manifest["cards"]
    titles_all = manifest["titles"]

    conn = RetroConnection(args.host, AGENT_PORT)
    try:
        greeting = await conn.connect(AGENT_SECRET, timeout=CONNECT_TIMEOUT)
    except Exception as e:
        log(f"FATAL: cannot connect to {args.host}:{AGENT_PORT} - {e}")
        return 2
    log(f"connected: {greeting.strip()}")

    try:
        # resolve card
        card = args.card
        diag = ""
        if not card:
            card, diag = await detect_card(conn, cards)
        if not card or card not in cards:
            log(f"could not identify GPU (VIDEODIAG did not match a profile). "
                f"Pass --card <{'/'.join(cards)}>.")
            if diag:
                log("VIDEODIAG said:\n" + diag[:400])
            return 3
        log(f"card: {card}  ({cards[card].get('note','')[:60]})")

        # resolve combos
        prof = cards[card]
        resolutions = (args.resolutions.split(",") if args.resolutions
                       else manifest.get("default_resolutions", ["1024x768"]))
        depths = ([int(x) for x in args.depth.split(",")] if args.depth
                  else prof.get("color_depths", [16]))
        fsaas = (args.fsaa.split(",") if args.fsaa
                 else prof.get("fsaa_levels", ["off"]))
        want = (list(titles_all) if args.titles in (None, "all")
                else args.titles.split(","))
        titles = {t: titles_all[t] for t in want if t in titles_all}
        missing_titles = [t for t in want if t not in titles_all]
        if missing_titles:
            log(f"unknown titles ignored: {missing_titles}")

        stamp = time.strftime("%Y%m%d-%H%M%S")
        outdir = RESULTS / f"{sanitize(args.host)}_{card}_{stamp}"
        outdir.mkdir(parents=True, exist_ok=True)
        log(f"output: {outdir}")

        # preflight: which titles are actually installed?
        installed = {}
        for tid, t in titles.items():
            ok = await path_exists(conn, t["exists_check"])
            if not ok and t.get("alt_exists_check"):
                ok = await path_exists(conn, t["alt_exists_check"])
            installed[tid] = ok
            log(f"  install {'OK ' if ok else 'MISS'}  {t['name']}")
        run_titles = {k: v for k, v in titles.items() if installed[k] or args.dry_run}
        if not run_titles:
            log("no benchmarked titles are installed on this machine. "
                "Stage them from the share first.")
            return 4

        # execute
        rows = []
        for tid, t in run_titles.items():
            for res in resolutions:
                for depth in depths:
                    if depth not in prof.get("color_depths", [depth]):
                        continue
                    for fsaa in fsaas:
                        log(f"FSAA={fsaa}: apply via the card's driver profile if "
                            f"configured (else driver-default / manual).")
                        row = await run_one(conn, card, tid, t, res, depth, fsaa,
                                            outdir, args)
                        rows.append(row)

        meta = {"host": args.host, "hostname": conn.hostname,
                "os": conn.os_version, "card": card, "stamp": stamp,
                "n_titles": len(run_titles)}
        summary = write_outputs(outdir, meta, rows)
        print("\n" + summary)
        return 0
    finally:
        await conn.close()  # graceful - never crash Win98 winsock


def main():
    ap = argparse.ArgumentParser(description="Automated retro GPU benchmark runner")
    ap.add_argument("--host", required=True, help="target agent IP")
    ap.add_argument("--card", help="override card id (skip VIDEODIAG detection)")
    ap.add_argument("--titles", help="comma list or 'all' (default: all)")
    ap.add_argument("--resolutions", help="comma list e.g. 1024x768,1600x1200")
    ap.add_argument("--depth", help="comma list of color depths e.g. 16,32")
    ap.add_argument("--fsaa", help="comma list e.g. off,2x,4x (default: card profile)")
    ap.add_argument("--renderdevice", help="Unreal render device override (Glide/D3D)")
    ap.add_argument("--manifest", default=str(HERE / "benchmarks.json"))
    ap.add_argument("--dry-run", action="store_true", help="plan only; no launches")
    args = ap.parse_args()
    try:
        rc = asyncio.run(amain(args))
    except KeyboardInterrupt:
        rc = 130
    sys.exit(rc)


if __name__ == "__main__":
    main()
