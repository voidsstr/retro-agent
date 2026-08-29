#!/usr/bin/env python3
"""Quake II timedemo A/B on .171 (Voodoo 2 / Intel 865G). DB-free: writes JSON.

Usage:  python3 q2bench171.py <gl_driver> <gl_mode> <swapinterval> [runs]
        gl_driver     3dfxgl | 3dfxvgl | retrogl | opengl32
        gl_mode       2=512x384  3=640x480  4=800x600   (NOT 6: 1024x768 needs
                      4.72MB and a single Voodoo2 has a 4MB framebuffer)
        swapinterval  0 = vsync OFF, 1 = vsync ON
"""
import asyncio, json, os, re, sys, time

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
sys.path.insert(0, REPO)
from client.retro_protocol import RetroConnection  # noqa: E402

HOST = os.environ.get("BENCH_HOST", "192.168.1.171")
SECRET = os.environ.get("RETRO_AGENT_SECRET", "retro-agent-secret")
OUTDIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "bench-results")


async def conn():
    for attempt in range(3):
        try:
            c = RetroConnection(HOST, 9898)
            await c.connect(SECRET, timeout=15.0)
            return c
        except Exception as e:
            if attempt == 2:
                raise
            print(f"  connect retry ({e})")
            await asyncio.sleep(3)


async def ex(c, cmd, timeout=60):
    s, d = await c.send_command(f"EXECW {timeout} {cmd}")
    return d.decode("ascii", "replace")


async def quiesce(c):
    for k, v in (("DoReport", 0), ("ShowUI", 0)):
        await ex(c, f'cmd /c reg add "HKLM\\SOFTWARE\\Microsoft\\PCHealth\\ErrorReporting" '
                    f'/v {k} /t REG_DWORD /d {v} /f', 20)
    await ex(c, 'cmd /c reg add "HKLM\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\AeDebug" '
                '/v Auto /t REG_SZ /d 0 /f', 20)
    try:
        await c.send_command("AI_DISABLE")
    except Exception:
        pass
    for img in ("retro-infer.exe", "rotate_wall.exe", "wuauclt.exe", "3dfxMan.exe",
                "daemon.exe", "wmiprvse.exe", "dwwin.exe", "dumprep.exe", "quake2.exe"):
        await ex(c, f"cmd /c taskkill /f /im {img}", 20)


async def wait_gone(c, image="quake2.exe", limit=90):
    for _ in range(limit):
        out = await ex(c, f'cmd /c tasklist /fi "imagename eq {image}" /nh', 20)
        if image.lower() not in out.lower():
            return True
        await asyncio.sleep(2)
    return False


async def one_run(driver, mode, swap):
    c = await conn()
    try:
        await ex(c, "cmd /c del /f /q C:\\Games\\Quake2Complete\\baseq2\\qconsole.log", 20)
        await ex(c, f"cmd /c C:\\RETRO_AGENT\\bench\\q2run.bat {driver} {mode} {swap}", 30)
    finally:
        await c.close()
    await asyncio.sleep(75)
    c = await conn()
    try:
        log = await ex(c, "cmd /c type C:\\Games\\Quake2Complete\\baseq2\\qconsole.log", 30)
        await ex(c, "cmd /c taskkill /f /im quake2.exe", 20)
        await wait_gone(c)
    finally:
        await c.close()
    fps = re.search(r"frames,\s*[\d.]+ seconds:\s*([\d.]+) fps", log)
    rend = re.search(r"GL_RENDERER:\s*(.+)", log)
    swapext = ("enabling WGL_EXT_swap_control" in log, "WGL_EXT_swap_control not found" in log)
    return {
        "fps": float(fps.group(1)) if fps else None,
        "gl_renderer": rend.group(1).strip() if rend else None,
        "swap_control_enabled": swapext[0],
        "swap_control_missing": swapext[1],
        "log_tail": log[-2500:],
    }


async def main():
    if len(sys.argv) < 4:
        print(__doc__)
        return 2
    driver, mode, swap = sys.argv[1], sys.argv[2], sys.argv[3]
    runs = int(sys.argv[4]) if len(sys.argv) > 4 else 5
    if mode == "6":
        print("REFUSED: gl_mode 6 (1024x768) needs 4.72MB; a single Voodoo2 has 4MB.")
        return 2
    c = await conn()
    try:
        await quiesce(c)
    finally:
        await c.close()
    results = []
    for i in range(runs):
        r = await one_run(driver, mode, swap)
        print(f"  run {i+1}/{runs}: {r['fps']} fps  [{r['gl_renderer']}]")
        results.append(r)
    good = [r["fps"] for r in results[1:] if r["fps"]]   # discard warm-up run 1
    good.sort()
    median = good[len(good) // 2] if good else None
    os.makedirs(OUTDIR, exist_ok=True)
    stamp = time.strftime("%Y%m%d-%H%M%S")
    path = os.path.join(OUTDIR, f"q2-{driver}-mode{mode}-vsync{swap}-{stamp}.json")
    with open(path, "w") as f:
        json.dump({"host": HOST, "benchmark": "quake2-timedemo-demo1",
                   "gl_driver": driver, "gl_mode": mode, "swapinterval": swap,
                   "runs": results, "median_fps_runs2plus": median}, f, indent=2)
    print(f"MEDIAN (runs 2..{runs}): {median} fps   -> {path}")
    return 0


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
