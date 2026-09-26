#!/usr/bin/env python3
"""glidelab_run.py - run glidelab.exe (tools/glidelab.c) on a box, collect RESULT.

    glidelab_run.py 192.168.1.124 fill  --res 1600x1200 --cfg 5 --refresh 60
    glidelab_run.py 192.168.1.124 bands --res 1024x768 --cfg 5
    glidelab_run.py 192.168.1.124 cycle --cfg 5 --cycles 20
    glidelab_run.py 192.168.1.124 abandon --cfg 5 --then fill

Uploads out/glidelab.exe to C:\\vcr\\glidelab\\, writes the SLI/AA config the
way the bench runner does (registry + env), runs it through `start /wait` (a
normal STARTUPINFO - the agent's hidden EXEC would hide Glide's window) under
EXECW with a timeout, and prints the RESULT json from the flushed log.
`--then MODE` runs a second mode right after (the check that an `abandon`
left the board usable). `--glide` picks the glide3x.dll (default: our h5
build as staged beside Quake II). `--json-out` appends every RESULT to a file.
"""
import argparse
import asyncio
import json
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
KMD = HERE.parent
REPO = KMD.parents[1]
sys.path.insert(0, str(REPO / "scripts" / "benchmarks"))
import v56k_bench as vb  # noqa: E402

DIR = r"C:\vcr\glidelab"
OUR_GLIDE = r"C:\Games\Quake2Complete\glide3x.dll"


async def run_mode(box, a, mode):
    log = rf"{DIR}\{mode}.log"
    args = [mode, "--res", a.res, "--refresh", str(a.refresh), "--dll", a.glide,
            "--log", log, "--frames", str(a.frames), "--layers", str(a.layers),
            "--cycles", str(a.cycles)]
    if a.cfg is not None:
        args += ["--cfg", str(a.cfg)]
    if a.blend:
        args.append("--blend")
    if a.origin:
        args += ["--origin", a.origin]
    cmd = (rf'cmd /c start "glidelab" /wait "{DIR}\glidelab.exe" ' + " ".join(args))
    await box.exec_(rf'cmd /c del /f /q "{log}"')
    await box.text(f"EXECW {a.timeout} {cmd}", a.timeout + 40)
    data = await box.download(log)
    text = (data or b"").decode("latin1", "replace")
    res = None
    for ln in text.splitlines():
        if ln.startswith("RESULT "):
            try:
                res = json.loads(ln[7:])
            except json.JSONDecodeError:
                res = {"raw": ln}
    if res is None:
        res = {"mode": mode, "error": "no RESULT line", "log_tail": text.splitlines()[-5:]}
    return res


async def main_async(a):
    box = vb.Box(a.host)
    await box.text(rf"MKDIR {DIR}")
    await box.upload(rf"{DIR}\glidelab.exe", (KMD / "out" / "glidelab.exe").read_bytes())
    if a.cfg is not None:
        inst, _ = await vb.find_display_instance(box)
        await vb.apply_aa_config(box, vb.GLIDE_KEY_TMPL.format(inst=inst), a.cfg)
    out = []
    for mode in [a.mode] + ([a.then] if a.then else []):
        r = await run_mode(box, a, mode)
        r["host"] = a.host
        print(json.dumps(r))
        out.append(r)
    if a.json_out:
        with open(a.json_out, "a") as f:
            for r in out:
                f.write(json.dumps(r) + "\n")
    return 0 if all("error" not in r for r in out) else 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("host")
    ap.add_argument("mode", choices=("fill", "bands", "cycle", "abandon"))
    ap.add_argument("--res", default="640x480")
    ap.add_argument("--refresh", type=int, default=60)
    ap.add_argument("--cfg", type=int)
    ap.add_argument("--frames", type=int, default=200)
    ap.add_argument("--layers", type=int, default=4)
    ap.add_argument("--cycles", type=int, default=10)
    ap.add_argument("--blend", action="store_true")
    ap.add_argument("--origin", choices=("upper", "lower"))
    ap.add_argument("--glide", default=OUR_GLIDE)
    ap.add_argument("--then", choices=("fill", "bands", "cycle"))
    ap.add_argument("--timeout", type=int, default=180)
    ap.add_argument("--json-out")
    a = ap.parse_args()
    sys.exit(asyncio.run(main_async(a)))


if __name__ == "__main__":
    main()
