#!/usr/bin/env python3
"""
v56k_glq_shot.py - measure GLQuake WITHOUT -condebug, by photographing the
console line it prints when the timedemo ends.

WHY NOT THE NORMAL ROUTE
------------------------
GLQuake runs fine on AmigaMerlin. What kills it is the runner's own `-condebug`:
the ICD reports 61 extensions in 1,449 bytes and 1997's `Con_DebugLog` formats
that line through a 1 KB buffer, so the stack is overwritten with the string
itself (the saved return address read as ASCII "TENS" - Dr Watson, five times
over). Without `-condebug` there is no qconsole.log to parse, and this Mesa 6.3
ignores MESA_EXTENSION_OVERRIDE, so the string cannot be shortened. The result
still exists - `timedemo` prints "N frames S seconds F fps" to the on-screen
console and stays there - so it is read off a screenshot instead.

WHAT THIS DOES AND DOES NOT PROVE
---------------------------------
- Fullscreen, real mode, real depth: the number is a genuine benchmark cell.
- The fps is read from the image by a person (or a later OCR pass); each cell
  writes `<outdir>/glquake_<res>_<depth>_cfg<N>.png` and a sidecar `.json`
  with everything BUT the fps, so transcription is one number per cell and the
  provenance is on the row.
- A BLACK frame is a FAILURE, said out loud. GDI capture of a fullscreen GL
  window works on XP for the titles measured so far, but the staged
  autoexec.cfg carries an old note claiming GLQuake gives a black frame; this
  script measures the frame's extrema and refuses to call a flat frame a
  capture. Do not "fix" that by running windowed - a windowed timedemo is not
  the fullscreen number and CLAUDE.md is explicit that it does not count.

    python3 v56k_glq_shot.py --host 192.168.1.124 --res 640x480,1024x768 --depths 16,32
"""
import argparse
import asyncio
import json
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parents[1]))
from v56k_bench import Box, GLQuake, find_display_instance  # noqa: E402

DISPLAY_CLASS = r"SYSTEM\CurrentControlSet\Control\Class\{4D36E968-E325-11CE-BFC1-08002BE10318}"


async def read_cfg(box, inst):
    """The SLI/AA setting the box is booted with, read back from the driver's
    Glide key - the same value the sweep applies one-per-boot. Recorded on the
    row because a GLQuake number without its cfg is not comparable to anything."""
    out = await box.text(f"REGREAD HKLM {DISPLAY_CLASS}\\{inst}\\Settings\\Glide")
    for v in json.loads(out).get("values", []):
        if v.get("name", "").upper() == "SSTH3_SLI_AA_CONFIGURATION":
            return str(v.get("data", "?"))
    return "?"


def log(m):
    print(f"[{time.strftime('%H:%M:%S')}] {m}", flush=True)


def frame_stats(bmp_bytes):
    """(min, max, mean) over a BMP's pixel bytes, and whether it is flat."""
    from PIL import Image
    import io
    im = Image.open(io.BytesIO(bmp_bytes)).convert("L")
    px = im.tobytes()
    lo, hi = min(px), max(px)
    mean = sum(px) / len(px)
    return {"width": im.width, "height": im.height, "min": lo, "max": hi,
            "mean": round(mean, 1), "flat": (hi - lo) < 8}


async def one_cell(box, w, h, depth, cfg, outdir, hold):
    t = GLQuake()
    why = t.supports(w, h, depth)
    if why:
        log(f"  {w}x{h}x{depth}: unsupported-by-engine ({why})")
        return {"status": "unsupported-by-engine", "notes": why}
    # Same bat as the runner's, minus -condebug: that flag is the crash.
    bat = "\r\n".join(["@echo off", "set FX_GLIDE_SWAPINTERVAL=0", f'cd /d "{t.root}"',
                       f"GLQUAKE.EXE -width {w} -height {h} -bpp {depth} -fullscreen +exec bench.cfg", ""])
    await box.upload(t.cfg, t.bench_cfg())
    await box.upload(t.bat, bat)
    await box.exec_(f'cmd /c taskkill /f /im "{t.proc}" 2>nul & echo ok')
    t0 = time.time()
    await box.text(f"LAUNCH {t.bat}", timeout=40)
    # demo1 at 100+ fps is ~10 s of timedemo after ~10 s of map load; hold
    # long enough that the console line is up, short enough not to waste a cell
    await asyncio.sleep(hold)
    from client.retro_protocol import RetroConnection
    c = RetroConnection(box.ip, 9898)
    await c.connect("retro-agent-secret", timeout=20)
    try:
        bmp = await c.command_binary("SCREENSHOT 0", timeout=120)
    finally:
        await c.close()
    alive = None
    try:
        import v56k_diag
        alive = await v56k_diag.process_alive(box, t.proc)
    except Exception:
        pass
    await box.exec_(f'cmd /c taskkill /f /im "{t.proc}" 2>nul & echo ok')
    stem = f"glquake_{w}x{h}_{depth}_cfg{cfg}"
    from PIL import Image
    import io
    Image.open(io.BytesIO(bmp)).save(outdir / f"{stem}.png", optimize=True)
    st = frame_stats(bmp)
    rec = {"title": "GLQuake", "api": "opengl-minigl", "res": f"{w}x{h}", "colordepth": depth,
           "aa_cfg": cfg, "engine": "GLQUAKE.EXE (timedemo demo1, no -condebug: it crashes the engine)",
           "capture": st, "process_alive_at_capture": alive, "held_s": hold,
           "stamp": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
           "avg_fps": None, "status": "frame-captured" if not st["flat"] else "frame-black",
           "notes": ("fps to be transcribed from the console frame" if not st["flat"]
                     else f"GDI frame is flat (min {st['min']} max {st['max']}) - the console line was NOT captured; "
                          "this is a capture limit, not a benchmark result")}
    (outdir / f"{stem}.json").write_text(json.dumps(rec, indent=1))
    log(f"  {w}x{h}x{depth}: {rec['status']}  frame min/max/mean {st['min']}/{st['max']}/{st['mean']}  "
        f"alive={alive}  -> {stem}.png")
    return rec


async def main_async(a):
    box = Box(a.host)
    outdir = Path(a.outdir)
    outdir.mkdir(parents=True, exist_ok=True)
    inst, _prof = await find_display_instance(box)
    cfg = await read_cfg(box, inst)
    log(f"box at SLI/AA cfg {cfg}; capturing GLQuake without -condebug")
    out = []
    for res in a.res.split(","):
        w, h = (int(x) for x in res.lower().split("x"))
        for depth in (int(d) for d in a.depths.split(",")):
            out.append(await one_cell(box, w, h, depth, cfg, outdir, a.hold))
    (outdir / "glquake_cells.json").write_text(json.dumps(out, indent=1))
    n_ok = sum(1 for r in out if r.get("status") == "frame-captured")
    log(f"done: {n_ok}/{len(out)} frames captured; transcribe the fps from the PNGs into results")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", required=True)
    ap.add_argument("--res", default="640x480,800x600,1024x768,1280x960")
    ap.add_argument("--depths", default="16,32")
    ap.add_argument("--hold", type=int, default=45, help="seconds from launch to screenshot")
    ap.add_argument("--outdir", default=str(HERE / "results" / "v56k_glquake_192.168.1.124"))
    return asyncio.run(main_async(ap.parse_args()))


if __name__ == "__main__":
    sys.exit(main())
