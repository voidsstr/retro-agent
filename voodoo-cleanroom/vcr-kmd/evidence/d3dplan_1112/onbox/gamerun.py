#!/usr/bin/env python3
"""gamerun.py - start one game on .124 through a wrapper .bat, photograph it
with the agent's GDI SCREENSHOT 0 at fixed times, then let it exit by itself
or close it civilly (d3dplan_1112/onbox, 2026-11-12).

    gamerun.py --name UT2004_botmatch --dir "C:\\Games\\UT2004\\System" \
        --exe UT2004.exe --args "DM-Antalus?..." --shots 20,40,60,90 --wait-exit 120

Rules this follows (the run's safety rules):
  * the wrapper is uploaded to C:\\vcr\\onbox\\<name>.bat and LAUNCHed
    (cd /d to the game folder, start the exe) - an EXEC of `start` hangs;
  * a screensaver in the foreground is dismissed with UIKEY SHIFT first
    (ScreenSaverIsSecure=0 on this box);
  * after the last frame the game is given --wait-exit seconds to end by
    itself (a benchmark/timedemo quits on its own); then WM_CLOSE by PID
    (taskkill /pid, no /f), 20 s, and only then a forced kill BY PID, verified;
  * never a kill by image name; no vcrctl/ExtEscape while the game runs;
  * afterwards DISPLAYCFG get must read the desktop mode again;
  * an agent that does not answer a protocol PING for 120 s ends the run
    (exit 9) - the caller stops everything.
Writes <outdir>/<name>/run.json and the PNGs.
"""
import argparse
import asyncio
import io
import json
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[5]
sys.path.insert(0, str(ROOT))
from client.retro_protocol import RetroConnection  # noqa: E402

HOST = "192.168.1.124"
SECRET = "retro-agent-secret"
IGNORE = {"cmd.exe", "conhost.exe", "wmiprvse.exe", "dwwin.exe", "dumprep.exe",
          "wuauclt.exe", "ping.exe", "taskkill.exe", "wmic.exe"}


class Dead(Exception):
    pass


def log(msg):
    print(time.strftime("%H:%M:%S ") + msg, flush=True)


async def call(cmd, timeout=60, payload=None):
    """One command on a fresh connection, closed gracefully. Retries until a
    PING answers; 120 s of silence raises Dead."""
    t0 = time.time()
    last = None
    while time.time() - t0 < 120:
        try:
            c = RetroConnection(HOST, 9898)
            await c.connect(SECRET, timeout=15)
            try:
                return await c.send_command(cmd, binary_payload=payload, timeout=timeout)
            finally:
                await c.close()
        except Exception as e:  # noqa: BLE001
            last = e
            log(f"  agent call {cmd[:40]!r} failed: {type(e).__name__}: {e}; retrying")
            await asyncio.sleep(5)
    raise Dead(f"no answer for 120 s (last: {last})")


async def text(cmd, timeout=60):
    st, d = await call(cmd, timeout)
    return st, d.decode("latin1", "replace")


async def procs():
    st, t = await text("PROCLIST", 60)
    try:
        j = json.loads(t)
    except ValueError:
        return {}
    items = j if isinstance(j, list) else j.get("processes", [])
    return {int(x["pid"]): (x.get("name") or "") for x in items if x.get("pid")}


async def winlist():
    st, t = await text("WINLIST", 30)
    try:
        return json.loads(t)
    except ValueError:
        return {}


async def shot(path):
    from PIL import Image
    st, d = await call("SCREENSHOT 0", 120)
    if st == 0xFF:
        return {"error": d.decode("latin1", "replace")[:200]}
    img = Image.open(io.BytesIO(d))
    img.save(path, optimize=True)
    g = img.convert("L")
    return {"file": path.name, "size": img.size, "luma_extrema": g.getextrema(),
            "luma_mean": round(sum(g.histogram()[i] * i for i in range(256)) / (img.size[0] * img.size[1]), 1)}


async def main(a):
    out = Path(a.outdir) / a.name
    out.mkdir(parents=True, exist_ok=True)
    rec = {"name": a.name, "dir": a.dir, "exe": a.exe, "args": a.args,
           "t_start": time.strftime("%Y-%m-%d %H:%M:%S")}
    try:
        st, t = await text("DISPLAYCFG get")
        rec["display_before"] = t.strip()
        wl = await winlist()
        fg = wl.get("foreground") or {}
        rec["foreground_before"] = fg
        if fg.get("class") == "WindowsScreenSaverClass":
            await text("UIKEY SHIFT")
            rec["screensaver_dismissed"] = True
            await asyncio.sleep(3)
        dlg0 = {w.get("hwnd") for w in (wl.get("windows") or []) if w.get("class") == "#32770"}
        before = await procs()
        bat = "\r\n".join(["@echo off", f'cd /d "{a.dir}"',
                           f'start "" "{a.exe}" {a.args}'.rstrip(), "exit", ""])
        (out / f"{a.name}.bat").write_text(bat)
        wrap = rf"C:\vcr\onbox\{a.name}.bat"
        await text(r"MKDIR C:\vcr\onbox")
        st, d = await call(f"UPLOAD {wrap}", payload=bat.encode("latin1"))
        rec["upload"] = d.decode("latin1", "replace").strip()
        t0 = time.time()
        st, t = await text(f"LAUNCH {wrap}")
        rec["launch"] = t.strip()
        log(f"launched {a.name}: {t.strip()[:120]}")
        new = {}
        rec["samples"] = []
        for at in a.shots:
            w = t0 + at - time.time()
            if w > 0:
                await asyncio.sleep(w)
            now = await procs()
            new.update({p: n for p, n in now.items() if p not in before and n.lower() not in IGNORE})
            alive = sorted({n for p, n in new.items() if p in now})
            s = await shot(out / f"{a.name}_{at:03d}s.png")
            wl = await winlist()
            fg = wl.get("foreground") or {}
            dlg = [(w.get("title"), w.get("pid")) for w in (wl.get("windows") or [])
                   if w.get("class") == "#32770" and w.get("hwnd") not in dlg0]
            samp = {"at": at, "t": round(time.time() - t0, 1), "alive": alive, "shot": s,
                    "foreground": {k: fg.get(k) for k in ("class", "title", "pid")},
                    "new_dialogs": dlg}
            rec["samples"].append(samp)
            log(f"  {at:3d}s alive={alive} shot={s.get('luma_extrema')} mean={s.get('luma_mean')} "
                f"fg={fg.get('class')}/{(fg.get('title') or '')[:30]} dialogs={dlg}")
        game = {p: n for p, n in new.items()}
        # let a benchmark / timedemo end by itself
        t_wait = time.time()
        left = {}
        while True:
            now = await procs()
            left = {p: n for p, n in game.items() if p in now}
            if not left or time.time() - t_wait >= a.wait_exit:
                break
            await asyncio.sleep(5)
        rec["exited_by_itself"] = not left
        rec["t_exit_or_close"] = round(time.time() - t0, 1)
        if left and a.console_exit:
            # the engine's own quit (UE: console `exit`), only while the game's
            # own window has the focus - keys go to the FOCUSED window
            wl = await winlist()
            fg = wl.get("foreground") or {}
            rec["console_exit_focus"] = {k: fg.get(k) for k in ("class", "title", "pid")}
            if int(fg.get("pid") or 0) in left:
                for k in ("TILDE", "TEXT:exit", "RETURN"):
                    st, t = await text(f"UIKEY {k}", 30)
                    await asyncio.sleep(1.5)
                t_c = time.time()
                while time.time() - t_c < 30:
                    await asyncio.sleep(4)
                    now = await procs()
                    left = {p: n for p, n in left.items() if p in now}
                    if not left:
                        break
                rec["closed_by_console_exit"] = not left
                log(f"  console exit -> {'gone' if not left else 'still running'}")
            else:
                rec["closed_by_console_exit"] = None
                log(f"  console exit skipped: focus is {fg}")
        if left:
            log(f"  still running after the wait: {left} - WM_CLOSE by pid")
            rec["close"] = []
            for p in left:
                st, t = await text(f"EXEC taskkill /pid {p}", 30)
                rec["close"].append(f"{p}: {t.strip()[:160]}")
            t_c = time.time()
            while time.time() - t_c < 20:
                await asyncio.sleep(4)
                now = await procs()
                left = {p: n for p, n in left.items() if p in now}
                if not left:
                    break
            rec["closed_by_wm_close"] = not left
            if left:
                log(f"  ignored WM_CLOSE: {left} - forcing by pid")
                rec["forced"] = []
                for p in left:
                    st, t = await text(f"EXEC taskkill /f /pid {p}", 30)
                    rec["forced"].append(f"{p}: {t.strip()[:160]}")
                await asyncio.sleep(5)
                now = await procs()
                left = {p: n for p, n in left.items() if p in now}
                for p in left:
                    await text(f"PROCKILL {p}")
                await asyncio.sleep(5)
                now = await procs()
                rec["survived_force"] = sorted({n for p, n in left.items() if p in now})
        # the desktop must come back
        await asyncio.sleep(6)
        st, t = await text("DISPLAYCFG get")
        rec["display_after"] = t.strip()
        wl = await winlist()
        rec["dialogs_after"] = [(w.get("title"), w.get("pid")) for w in (wl.get("windows") or [])
                                if w.get("class") == "#32770" and w.get("hwnd") not in dlg0]
        rec["after_shot"] = await shot(out / f"{a.name}_after.png")
        log(f"  after: display {rec['display_after']} dialogs {rec['dialogs_after']}")
    except Dead as e:
        rec["agent_dead"] = str(e)
        rec["t_dead"] = time.strftime("%Y-%m-%d %H:%M:%S")
        log(f"AGENT NOT ANSWERING: {e}")
        (out / "run.json").write_text(json.dumps(rec, indent=1))
        return 9
    rec["t_end"] = time.strftime("%Y-%m-%d %H:%M:%S")
    (out / "run.json").write_text(json.dumps(rec, indent=1))
    return 0


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--name", required=True)
    ap.add_argument("--dir", required=True)
    ap.add_argument("--exe", required=True)
    ap.add_argument("--args", default="")
    ap.add_argument("--shots", type=lambda s: [int(x) for x in s.split(",")], default=[20, 40, 60, 90])
    ap.add_argument("--wait-exit", type=int, default=0)
    ap.add_argument("--console-exit", action="store_true",
                    help="before WM_CLOSE: TILDE, exit, RETURN - only if the game has the focus")
    ap.add_argument("--outdir", default=str(Path(__file__).resolve().parent / "games"))
    sys.exit(asyncio.run(main(ap.parse_args())))
