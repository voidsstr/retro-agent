#!/usr/bin/env python3
"""Smoke-test staged titles on whatever OpenGL ICD a box is running.

For each title it launches the FIRST shortcut in the title's staged launch.txt
exactly as the desktop does, waits, and records evidence rather than a verdict:

  * which new processes appeared and whether they are still alive,
  * the lines our voodoo-cleanroom ICD appended to C:\\retrogl.log (loaded by
    which exe, context created, board open, any BAIL) - absent when the title
    never touched OpenGL,
  * the visible window titles (an engine's own error dialog shows up here),

then kills only the processes it started. "Context created and still running
after N s" is the most GDI can prove about a fullscreen Glide game; a rendered
frame needs the engine's own screenshot command.

    cleanroom_smoke.py 192.168.1.124 Quake1 HalfLife1 SeriousSamFirstEncounter ...
"""
import argparse
import asyncio
import json
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from client.retro_protocol import RetroConnection, RetroProtocolError

SECRET = "retro-agent-secret"
TRACE = r"C:\retrogl.log"


class Box:
    def __init__(self, ip):
        self.ip = ip

    async def _conn(self):
        c = RetroConnection(self.ip, 9898)
        await c.connect(SECRET, timeout=20)
        return c

    async def text(self, cmd, timeout=60):
        c = await self._conn()
        try:
            return await c.command_text(cmd, timeout=timeout)
        finally:
            await c.close()

    async def download(self, path):
        c = await self._conn()
        try:
            return await c.command_binary(f"DOWNLOAD {path}", timeout=120)
        except RetroProtocolError:
            return b""
        finally:
            await c.close()

    async def procs(self):
        return {(p["pid"], p["name"]) for p in json.loads(await self.text("PROCLIST"))}


def first_launch(txt):
    for line in txt.splitlines():
        line = line.strip()
        if line and not line.startswith("#"):
            return line.split("\t")[0]
    return None


async def smoke(box, title, wait):
    root = rf"C:\Games\{title}"
    lt = (await box.download(rf"{root}\launch.txt")).decode("latin-1")
    target = first_launch(lt)
    if not target:
        return {"title": title, "result": "no launch.txt entry"}
    trace0 = len(await box.download(TRACE))
    before = await box.procs()
    d, f = (target.rsplit("\\", 1) if "\\" in target else ("", target))
    await box.text(f'EXEC cmd /c start "" /D "{root}\\{d}" "{f}"'.replace("\\\\", "\\"))
    await asyncio.sleep(wait)
    after = await box.procs()
    new = sorted(after - before)
    trace = (await box.download(TRACE))[trace0:].decode("latin-1", "replace").splitlines()
    wins = [w["title"] for w in json.loads(await box.text("WINLIST")).get("windows", [])
            if w.get("title") and "Retro Remote Agent" not in w["title"]
            and w["title"] not in ("Program Manager",) and "cmd.exe" not in w["title"]]
    keep = [l for l in trace if any(k in l for k in (
        "LOADED by", "DrvValidateVersion", "SUCCESS glideContext", "BAIL", "FAILED",
        "fxBestRefresh", "grSstWinOpenExt: win"))]
    for pid, name in new:
        if name.lower() not in ("cmd.exe", "conhost.exe"):
            try:
                await box.text(f"PROCKILL {pid}")
            except Exception:
                pass
    await asyncio.sleep(4)
    return {"title": title, "launch": target, "new_processes": [n for _, n in new],
            "alive_after_s": wait, "icd_loaded": any("LOADED by" in l for l in trace),
            "context_ok": any("SUCCESS glideContext" in l for l in trace),
            "trace": keep[-12:], "windows": wins}


async def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("host")
    ap.add_argument("titles", nargs="+")
    ap.add_argument("--wait", type=int, default=45)
    ap.add_argument("--out")
    a = ap.parse_args()
    box = Box(a.host)
    results = []
    for t in a.titles:
        try:
            r = await smoke(box, t, a.wait)
        except Exception as e:
            r = {"title": t, "result": f"harness error {type(e).__name__}: {e}"}
        results.append(r)
        print(json.dumps(r), flush=True)
    if a.out:
        Path(a.out).write_text(json.dumps(results, indent=1))


if __name__ == "__main__":
    asyncio.run(main())
