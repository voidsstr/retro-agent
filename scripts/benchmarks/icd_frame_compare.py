#!/usr/bin/env python3
"""icd_frame_compare.py - prove an ICD change renders the same pixels.

A frame-rate row says a change is faster; it cannot say the frame is still
right. Quake II's timedemo advances the demo exactly one message per rendered
frame, so every frame of `demomap demo1.dm2` is the same picture every run.
This launches the all-ours Quake II lane (our ICD + our h5 Glide, game-local)
once per variant, has the engine itself `screenshot` the demo's LAST frame
(glReadPixels through the ICD), downloads the TGA and compares the variants
pixel for pixel.

Not frame N via a chain of `wait`s: that stalls the command buffer the demo
needs for its own precache commands, and playback dies with
"CM_InlineModel: bad number" (measured, 2026-09-25).

    icd_frame_compare.py 192.168.1.124 \\
        --variant on= --variant off=FX_NO_TRI_BATCH=1

Each --variant is NAME=[KEY=VAL[,KEY=VAL...]], applied as the game's
environment. Exit status 0 = every variant matched the first one.
"""
import argparse
import asyncio
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parents[1]))
import v56k_bench as vb  # noqa: E402


class Quake2Frame(vb.Quake2AllOurs):
    """The all-ours Quake II lane, but the run screenshots the demo's last
    frame and then quits."""

    def bench_cfg(self):
        return "\r\n".join([
            '// generated per run by icd_frame_compare.py',
            'set cl_maxfps "1000"', 'set gl_swapinterval "0"', 'set gl_picmip "0"',
            'set gl_finish "0"', 'set timedemo "1"',
            'demomap demo1.dm2',
            # after demomap: SV_Map clears nextserver (see Quake2.bench_cfg)
            'set nextserver "screenshot; killserver; quit"',
            '',
        ])


async def run_variant(box, name, env, w, h, depth, outdir):
    t = Quake2Frame()
    shots = rf"{t.root}\baseq2\scrnshot"
    await box.exec_(rf'cmd /c if exist "{shots}" rd /s /q "{shots}"')
    await t.identify(box)
    await t.prepare(box, w, h, depth, env)
    await t.start(box)
    import v56k_diag
    for _ in range(40):                        # up to ~4 min: the engine quits itself
        await asyncio.sleep(6)
        if await v56k_diag.process_alive(box, t.proc) is False:
            break
    else:
        await vb.graceful_kill(box, t.proc)
    d = await box.download(rf"{shots}\quake00.tga")    # 3.20 writes TGA
    out = Path(outdir) / f"lastframe_{name}.tga"
    out.write_bytes(d or b"")
    return out, len(d or b"")


def image_pixels(path):
    from PIL import Image
    im = Image.open(path).convert("RGB")
    return im.size, im.tobytes()


async def main_async(a):
    box = vb.Box(a.host)
    Path(a.outdir).mkdir(parents=True, exist_ok=True)
    w, h = (int(x) for x in a.res.split("x"))
    results = []
    for spec in a.variant:
        name, _, rest = spec.partition("=")
        env = dict(kv.split("=", 1) for kv in rest.split(",") if kv)
        path, n = await run_variant(box, name, env, w, h, a.depth, a.outdir)
        print(f"{name}: {path} ({n} bytes)")
        results.append((name, path, n))
    if any(n == 0 for _, _, n in results):
        print("FAIL: a variant produced no screenshot")
        return 2
    base_name, base_path, _ = results[0]
    size0, px0 = image_pixels(base_path)
    ok = True
    for name, path, _ in results[1:]:
        size, px = image_pixels(path)
        if size != size0:
            print(f"FAIL: {name} is {size}, {base_name} is {size0}")
            ok = False
            continue
        diff = sum(1 for i in range(0, len(px), 3) if px[i:i + 3] != px0[i:i + 3])
        print(f"{name} vs {base_name}: {diff} of {size[0] * size[1]} pixels differ")
        ok = ok and diff == 0
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("host")
    ap.add_argument("--variant", action="append", required=True)
    ap.add_argument("--res", default="640x480")
    ap.add_argument("--depth", type=int, default=16)
    ap.add_argument("--outdir", default=str(HERE / "results" / "icd_frame_compare"))
    a = ap.parse_args()
    sys.exit(asyncio.run(main_async(a)))


if __name__ == "__main__":
    main()
