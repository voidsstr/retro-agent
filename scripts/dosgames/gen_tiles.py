#!/usr/bin/env python3
"""gen_tiles.py — render gameplay preview tiles for DOS games in DOSBox-X.

For each requested catalog entry (kind R = ready-to-run):
  1. extract its zip (from the share mount) into a temp C: root
  2. boot it in DOSBox-X (headless Xvfb :77 via dosbox_run.sh) and let it
     run for --wait seconds (most games sit on their title/demo screen)
  3. screenshot the display, crop the emulator window content, downscale
     to 320x200, quantize to 256 colors
  4. write <STEM8>.PRV: 768-byte RGB palette + 64000 pixel bytes
     (the format DOSGAME.EXE blits in VGA mode 13h)

Usage:
  gen_tiles.py --cat data/GAMES.CAT --out data/tiles --limit 4 [--wait 25]
  gen_tiles.py --title "keen dreams" ...
"""
import argparse, os, re, shutil, struct, subprocess, sys, tempfile, time, zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
SHARE = "/run/user/1000/gvfs/smb-share:server=192.168.1.122,share=files/Games/DOS"
SEARCH_DIRS = [SHARE,
               os.path.join(SHARE, "DOS Games Collection", "1-C"),
               os.path.join(SHARE, "DOS Games Collection", "D-L"),
               os.path.join(SHARE, "DOS Games Collection", "M-R"),
               os.path.join(SHARE, "DOS Games Collection", "S-Z"),
               os.path.join(SHARE, "More Dos Games")]

# The TILE name must use the collision-free stem too. stem8() is the plain
# 8-char truncation that put 1,268 of the 2,982 catalogue rows on a shared
# name: 411 tile names were claimed by more than one game, so gen_tiles.py's
# "skip if it already exists" meant the second game of every colliding pair
# could never get a preview, and DOSGAME.EXE showed the first game's
# screenshot for the second. zip_stem() is the same hashed stem the install
# path already uses, and it is pinned against dosgame.c by
# tests/python/test_dosgame_stem.py.
import os as _os
import sys as _sys
_sys.path.insert(0, _os.path.dirname(_os.path.abspath(__file__)))
from serve_dosgames import zip_stem


def stem8(name):
    s = os.path.splitext(os.path.basename(name))[0]
    return (re.sub(r"[^A-Za-z0-9_]", "_", s)[:8].upper()) or "GAME"

def find_zip(name):
    for d in SEARCH_DIRS:
        p = os.path.join(d, name)
        if os.path.isfile(p):
            return p
    return None

def capture_display(display):
    from PIL import Image
    xwd = subprocess.run(["xwd", "-root", "-silent", "-display", display],
                         capture_output=True).stdout
    hdr = struct.unpack(">25I", xwd[:100])
    hsize, w, h, bpl, ncolors = hdr[0], hdr[4], hdr[5], hdr[12], hdr[19]
    off = hsize + ncolors * 12
    return Image.frombytes("RGB", (w, h), xwd[off:off + bpl * h],
                           "raw", "BGRX", bpl, 1)

def crop_content(img):
    """Crop to the bounding box of non-black content (the emulator frame)."""
    from PIL import ImageOps
    gray = img.convert("L")
    bbox = gray.point(lambda p: 255 if p > 8 else 0).getbbox()
    return img.crop(bbox) if bbox else img

def render_tile(zip_path, exe, out_prv, wait, keep_root=None):
    from PIL import Image
    tmp = tempfile.mkdtemp(prefix="dostile-")
    try:
        gd = os.path.join(tmp, "GAME")
        os.makedirs(gd)
        with zipfile.ZipFile(zip_path) as z:
            z.extractall(gd)
        cmd = exe if exe else "DIR"
        env = dict(os.environ)
        env["DOSBOX_TIMEOUT"] = str(wait + 40)
        run = subprocess.Popen(
            ["bash", os.path.join(HERE, "dosbox_run.sh"), tmp,
             "CD GAME",
             "-c", "AUTOTYPE -w 10 -p 6 enter space enter",
             "-c", cmd],
            env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        time.sleep(wait)
        img = capture_display(":77")
        subprocess.run(["pkill", "-9", "-f", "dosbox-x.exe"],
                       capture_output=True)
        run.wait(timeout=30)
        img = crop_content(img)
        if img.size[0] < 100:
            return False
        img = img.resize((320, 200), Image.LANCZOS)
        # Quality gate: a game that never reached a visible screen within
        # --wait produces an all-black (or near-flat) capture. Writing that
        # would put a black tile in the catalog forever, and the file's
        # existence makes reruns skip it — so reject it here instead.
        gray = img.convert("L")
        lo, hi = gray.getextrema()
        if hi < 30 or len(gray.getcolors(maxcolors=65536) or [1]) < 6:
            return False
        q = img.quantize(colors=256)
        pal = q.getpalette()[:768] + [0] * (768 - len(q.getpalette()[:768]))
        with open(out_prv, "wb") as f:
            f.write(bytes(pal))
            f.write(q.tobytes())
        return True
    finally:
        if keep_root:
            shutil.move(tmp, keep_root)
        else:
            shutil.rmtree(tmp, ignore_errors=True)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cat", default=os.path.join(HERE, "data", "GAMES.CAT"))
    ap.add_argument("--out", default=os.path.join(HERE, "data", "tiles"))
    ap.add_argument("--limit", type=int, default=4)
    ap.add_argument("--wait", type=int, default=25)
    ap.add_argument("--title", action="append", default=[])
    ap.add_argument("--force", action="store_true")
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)

    rows = []
    for line in open(a.cat, encoding="utf-8", errors="replace"):
        if line.startswith("#") or "|" not in line: continue
        f = line.rstrip("\n").split("|")
        if len(f) < 6: continue
        title, zipn, kind, exe = f[0], f[1], f[2], f[3]
        if a.title and not any(t.lower() in title.lower() for t in a.title):
            continue
        if not a.title and kind != "R":
            continue
        rows.append((title, zipn, kind, exe))

    done = 0
    for title, zipn, kind, exe in rows:
        if done >= a.limit: break
        prv = os.path.join(a.out, zip_stem(zipn) + ".PRV")
        if os.path.exists(prv) and not a.force: continue
        zp = find_zip(zipn)
        if not zp:
            print("SKIP (zip not found):", title); continue
        print("render:", title, "->", os.path.basename(prv), flush=True)
        ok = render_tile(zp, exe, prv, a.wait)
        print("  ", "ok" if ok else "FAILED", flush=True)
        done += ok

    print("tiles done:", done)

if __name__ == "__main__":
    main()
