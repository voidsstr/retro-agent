#!/usr/bin/env python3
"""
lan_sweep.py - smoke-test EVERY game shortcut on a box's desktop the way a
person starts it: the .lnk's own target, unchanged.

For each shortcut (setup / sound-config / LAN host-join ones skipped: they
need a partner or a person): launch it, find the processes it started by
diffing the process list, and at fixed times take the agent's GDI screenshot
and note which of them are alive; then close them with WM_CLOSE, and force
only after a grace period - recorded, because a forced exit of a fullscreen
Glide game can leave the board mapped (v56k_bench.graceful_kill). After each
title the board is asked whether it can still bring Glide up
(glideprobe --noopen); a wedged board stops the sweep, so one bad title is
not blamed on the next.

A GDI screenshot of an exclusive-fullscreen Glide/OpenGL game on our driver
shows the desktop surface, not the game: for those titles the verdict rests
on "still running, no error window, board healthy" - say so, never call the
picture evidence. lan_check.py is the deep route (engine screenshots,
timedemo, multiplayer) for the priority titles.

    python3 scripts/benchmarks/lan_sweep.py --host 192.168.1.124 \
        --outdir scripts/benchmarks/results/v56k_lan_192.168.1.124/sweep
"""
import argparse
import asyncio
import io
import json
import re
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import lan_check as lc  # noqa: E402  (loads v56k_bench too)

bench = lc.bench
SKIP = re.compile(r"(?i)setup|sound|host .*lan|join .*lan|lan game|join the fleet|join fleet|"
                  r"fleet server|retro agent|retro chat|network config|online|multiplayer server|"
                  r"collection menu|control panel")
# NOT "3dfx": that once skipped the Glide titles themselves ("Quake - 3dfx
# Voodoo", "Hexen II - 3dfx Voodoo") along with the 3dfx Control Panel, which
# "control panel" already covers.

LNK_VBS = r'''Set sh = CreateObject("WScript.Shell")
Set fso = CreateObject("Scripting.FileSystemObject")
For Each d In Array(sh.SpecialFolders("AllUsersDesktop"), sh.SpecialFolders("Desktop"))
  If fso.FolderExists(d) Then
    For Each f In fso.GetFolder(d).Files
      If LCase(fso.GetExtensionName(f.Name)) = "lnk" Then
        Set l = sh.CreateShortcut(f.Path)
        WScript.Echo f.Name & vbTab & l.TargetPath & vbTab & l.Arguments & vbTab & l.WorkingDirectory
      End If
    Next
  End If
Next
'''


async def agent_back(box, limit=180):
    """Wait for the agent to answer again (a restart takes ~20 s; the Run-key
    watchdog relaunches a dead one). True when it answers within `limit`."""
    t = time.time()
    while time.time() - t < limit:
        try:
            st, out = await box.cmd("PING", timeout=15)
            if "PONG" in out.upper():
                return True
        except Exception:
            pass
        await asyncio.sleep(5)
    return False


async def shortcuts(box):
    await box.upload(r"C:\RETRO_AGENT\lnk.vbs", LNK_VBS)
    out = await box.exec_(r"cscript //nologo C:\RETRO_AGENT\lnk.vbs", timeout=90)
    rows = []
    for line in out.splitlines():
        p = line.split("\t")
        if len(p) >= 4 and p[0].lower().endswith(".lnk"):
            rows.append({"name": p[0][:-4], "target": p[1], "args": p[2], "wd": p[3]})
    return rows


async def processes(box):
    try:
        st, out = await box.cmd("PROCLIST", timeout=60)
    except Exception:
        # the agent restarted under us: wait for it, then ask again
        if not await agent_back(box):
            raise
        st, out = await box.cmd("PROCLIST", timeout=60)
    try:
        j = json.loads(out)
    except Exception:
        return {}
    items = j if isinstance(j, list) else j.get("processes", [])
    return {int(x.get("pid")): (x.get("name") or x.get("exe") or "") for x in items if x.get("pid")}


async def dialogs(box):
    """{hwnd: title} of every visible standard dialog (#32770). A game's own
    error box carries the GAME's name as its title - Hidden & Dangerous
    Deluxe's "Unable to initialize graphics ... requires DirectX 8" was titled
    just "Hidden & Dangerous Deluxe" and passed error_windows()'s title test
    - so a dialog that was not there before the launch is reported whatever
    it is called."""
    try:
        st, out = await box.cmd("WINLIST", timeout=30)
        wins = json.loads(out)
    except Exception:
        return {}
    wins = wins if isinstance(wins, list) else wins.get("windows", [])
    return {w.get("hwnd"): (w.get("title") or "(untitled)") for w in wins
            if isinstance(w, dict) and w.get("class") == "#32770" and w.get("visible", True)}


async def foreground(box):
    """The window a keystroke would reach RIGHT NOW (WINLIST "foreground",
    agent 1.91.0+), or None when the agent cannot say."""
    try:
        st, out = await box.cmd("WINLIST", timeout=30)
        fg = json.loads(out).get("foreground")
    except Exception:
        return None
    return fg if isinstance(fg, dict) and "pid" in fg else None


async def key_to(box, pids, key, rec):
    """Send one UIKEY only if the focused window belongs to one of `pids`.

    Keys go to whatever window has the focus. On 2026-09-28 this sweep sent
    ALT+F4 to Aliens vs Predator, which was alive but NOT focused: it reached
    the desktop, opened "Shut Down Windows", the console-quit RETURN confirmed
    it and .124 restarted. An agent that cannot name the focused window (older
    than 1.91.0) gets no keys at all - the force path closes the game instead."""
    fg = await foreground(box)
    if fg is None or int(fg.get("pid") or 0) not in pids:
        where = "unknown (agent has no WINLIST foreground)" if fg is None else \
            "%s \"%s\" pid %s" % (fg.get("class"), fg.get("title"), fg.get("pid"))
        rec.setdefault("keys_skipped", []).append("%s: focus is %s" % (key, where))
        return False
    st, out = await box.cmd(f"UIKEY {key}", timeout=30)
    if st != 0:
        rec.setdefault("keys_refused", []).append("%s: %s" % (key, out.strip()[:120]))
        return False
    return True


async def screenshot(box):
    from client.retro_protocol import RetroConnection
    c = RetroConnection(box.ip, 9898)
    await c.connect(bench.SECRET, timeout=20)
    try:
        return await c.command_binary("SCREENSHOT 0", timeout=90)
    finally:
        await c.close()


async def run_one(box, sc, outdir, shots_at, grace):
    rec = {"shortcut": sc["name"], "target": sc["target"], "args": sc["args"],
           "t0": time.strftime("%H:%M:%S")}
    await bench.quiesce(box)
    before = await processes(box)
    dlg0 = await dialogs(box)
    rec["dialogs"] = {}
    dr0 = await lc.file_size(box, lc.DRWTSN)
    wd = sc["wd"] or str(Path(sc["target"]).parent)
    tgt = sc["target"]
    # a wrapper .bat: a one-line `cmd /c cd /d X && Y` does not change
    # directory under the agent, and a shortcut's working dir is what the
    # game's relative paths assume
    wrap = r"C:\RETRO_AGENT\lansweep.bat"
    call = "call " if tgt.lower().endswith((".bat", ".cmd")) else "start \"\" "
    await box.upload(wrap, "\r\n".join(["@echo off", f'cd /d "{wd}"', f'{call}"{tgt}" {sc["args"]}'.rstrip(), ""]))
    started = time.time()
    await box.text(f"LAUNCH {wrap}")
    rec["samples"] = []
    new = {}
    for at in shots_at:
        w = started + at - time.time()
        if w > 0:
            await asyncio.sleep(w)
        now = await processes(box)
        new.update({p: n for p, n in now.items() if p not in before and n.lower() not in
                    ("cmd.exe", "conhost.exe", "wmiprvse.exe", "dwwin.exe", "dumprep.exe", "wuauclt.exe", "ping.exe")})
        alive = sorted({n for p, n in new.items() if p in now})
        data = await screenshot(box)
        safe = re.sub(r"[^A-Za-z0-9]+", "_", sc["name"]).strip("_")[:60]
        png = outdir / f"{safe}_{at}s.png"
        lc.save_png(data, png)
        rec["samples"].append({"at": at, "alive": alive, "stats": lc.shot_stats(data), "file": png.name})
        rec["dialogs"].update({h: t for h, t in (await dialogs(box)).items() if h not in dlg0})
    rec["error_windows"] = await lc.error_windows(box)
    now = await processes(box)
    left = {p: n for p, n in new.items() if p in now}
    rec["running_at_end"] = sorted(set(left.values()))
    for p in left:
        await box.exec_(f"cmd /c taskkill /pid {p} 2>nul", timeout=30)
    rec["close_via"] = "WM_CLOSE"

    async def settle(secs):
        nonlocal left
        t = time.time()
        while time.time() - t < secs:
            await asyncio.sleep(3)
            now = await processes(box)
            left = {p: n for p, n in left.items() if p in now}
            if not left:
                return True
        return False

    # a game that ignores WM_CLOSE: ALT+F4, then the id/GoldSrc console quit -
    # each key ONLY while the game's own window has the focus (key_to): a key
    # that misses lands on whatever is focused, and ALT+F4 on the desktop is
    # "Shut Down Windows".
    if not await settle(grace) and left:
        if await key_to(box, set(left), "ALT+F4", rec):
            rec["close_via"] = "ALT+F4"
        if not await settle(10) and left:
            for k in ("TILDE", "TEXT:quit", "RETURN"):
                now = await processes(box)
                if not any(p in now for p in left):
                    break
                if not await key_to(box, set(left), k, rec):
                    break
                rec["close_via"] = "console quit"
            await settle(10)
    if left:
        rec["forced"] = sorted(set(left.values()))
        rec["force_output"] = []
        for p in left:
            out = await box.exec_(f"cmd /c taskkill /f /pid {p}", timeout=30)
            rec["force_output"].append(f"{p}: {(out or '').strip()[:160]}")
        await asyncio.sleep(5)
        rec["survived_force"] = await force_close_verified(box, left)
    dr1 = await lc.file_size(box, lc.DRWTSN)
    if dr1 != dr0:
        rec["drwatson_grew"] = dr1 - dr0
    rec["agent_alive"] = await bench.agent_alive(box)
    rec["board"] = await bench.board_alive(box)
    bad = []
    if not new:
        bad.append("started no process")
    elif not rec["running_at_end"]:
        bad.append("exited before the last sample")
    if rec["error_windows"]:
        bad.append("error window: " + "; ".join(rec["error_windows"])[:120])
    if rec["dialogs"]:
        bad.append("dialog: " + "; ".join(sorted(set(rec["dialogs"].values())))[:120])
    if rec.get("drwatson_grew"):
        bad.append("Dr. Watson entry")
    if rec.get("forced"):
        bad.append("had to be forced closed")
    if rec.get("survived_force"):
        bad.append("STILL RUNNING AFTER A FORCED CLOSE: " + ", ".join(rec["survived_force"]))
    if rec.get("keys_skipped") or rec.get("keys_refused"):
        bad.append("close keys withheld - game not focused")
    if rec["board"] is False:
        bad.append("BOARD WEDGED")
    if not rec["agent_alive"]:
        bad.append("AGENT DEAD")
    fail = rec.get("survived_force") or rec["board"] is False or not rec["agent_alive"]
    rec["verdict"] = "PASS" if not bad else ("FAIL: " if fail else "CHECK: ") + "; ".join(bad)
    return rec


async def force_close_verified(box, left):
    """The post-condition of a forced close, not its return code. A forced
    `taskkill /f` once returned while Carmageddon 2 lived on: the sweep called
    it CHECK and ran 48 more titles while the leftover spun one thread at 100%
    CPU for 14 hours (.124, 2026-10-01/02) - every later measurement on the
    box was CPU-starved. A survivor gets the agent's own TerminateProcess by
    PID; whatever is STILL there is returned (image names), and the caller
    must treat it as a failure."""
    now = await processes(box)
    stuck = {p: n for p, n in left.items() if p in now}
    for p in stuck:
        try:
            await box.text(f"PROCKILL {p}")
        except Exception:                       # noqa: BLE001
            pass
    if stuck:
        await asyncio.sleep(5)
        now = await processes(box)
        stuck = {p: n for p, n in stuck.items() if p in now}
    return sorted(set(stuck.values()))


async def amain(a):
    box = bench.Box(a.host)
    out = Path(a.outdir) / time.strftime("%Y%m%d_%H%M%S")
    out.mkdir(parents=True, exist_ok=True)
    rows = await shortcuts(box)
    if a.only:
        # an explicit --only is a request for exactly those shortcuts, SKIP or not
        pat = re.compile(a.only, re.I)
        todo = [r for r in rows if pat.search(r["name"])]
    else:
        todo = [r for r in rows if not SKIP.search(r["name"])]
    if a.exclude:
        pat = re.compile(a.exclude, re.I)
        todo = [r for r in todo if not pat.search(r["name"])]
    lc.log(f"{len(rows)} shortcuts, {len(todo)} to sweep: {[r['name'] for r in todo]}")
    results = []
    for sc in todo:
        lc.log(f"--- {sc['name']}: {sc['target']} {sc['args']}")
        rec = await run_one(box, sc, out, a.shots, a.grace)
        results.append(rec)
        (out / "sweep.json").write_text(json.dumps(results, indent=1))
        lc.log(f"    {rec['verdict']}  samples={[(s['at'], s['alive'], s['stats']) for s in rec['samples']]}")
        if rec["board"] is False or not rec["agent_alive"]:
            lc.log("stopping: board or agent down")
            break
        if rec.get("survived_force"):
            # a leftover that survives TerminateProcess keeps the CPU (or the
            # board) busy for every later title - their results would be
            # measured beside it. Stop, loudly, and say what is still running.
            lc.log("STOPPING: " + ", ".join(rec["survived_force"]) + " is still running after a forced "
                   "close - every later title would run beside it. End it on the box, then re-run "
                   "with --only for the titles left.")
            return 5
    lc.log(f"results -> {out / 'sweep.json'}")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", required=True)
    ap.add_argument("--outdir", default=str(HERE / "results" / "v56k_lan_192.168.1.124" / "sweep"))
    ap.add_argument("--only", help="regex over shortcut names to include")
    ap.add_argument("--exclude", help="regex over shortcut names to leave out")
    ap.add_argument("--shots", type=lambda s: [int(x) for x in s.split(",")], default=[30, 60])
    ap.add_argument("--grace", type=int, default=25)
    a = ap.parse_args()
    sys.exit(asyncio.run(amain(a)))


if __name__ == "__main__":
    main()
