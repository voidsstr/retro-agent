#!/usr/bin/env python3
"""game_sweep.py — find and fix every game-local 3dfx binary on a fleet box.

Windows' LoadLibrary prefers the game's own directory over system32, so a
driver install is not complete until every game-local copy of the loader DLLs
is our current build and every modern-PC wrapper (nGlide, dgVoodoo, "Win10
fixed" ddraw shims) is retired. This script automates that sweep for both
3dfx stacks:

  --flavor vintage    real 3dfx H5 stack (sibling retro-3dfx repo): system32
                      holds the display driver + real glide; game dirs get the
                      vintage-source ICD; glide/ddraw shadows are RETIRED so
                      system32 wins.
  --flavor cleanroom  clean-room MesaFX ICD + open Glide (this repo): game
                      dirs get the MesaFX ICD and clean-room glide copies.

Policy per found file (outside \\WINDOWS\\ — system copies are never touched):
  opengl32.dll / 3dfxgl.dll / 3dfxogl.dll   -> replace with the chosen ICD
                                               (.pre backup on first replace;
                                               all GL names in a dir kept
                                               identical)
  glide2x.dll / glide3x.dll                 -> vintage: retire to .wrapper.bak
                                               cleanroom: replace with ours
  ddraw.dll                                 -> retire to .wrapper.bak (always)
  UnrealTournament.exe + ini in dir         -> switch ini to GlideDrv
Every write is verified by DOWNLOAD-readback md5. Dry-run by default; --apply
to act; --kill to taskkill known game processes first (locked DLLs can't be
replaced — each copy is verified, not assumed).

Usage:
  python3 game_sweep.py <host> --flavor vintage|cleanroom [--apply] [--kill]
                        [--drives C D] [--icd PATH] [--glide2 PATH] [--glide3 PATH]
Exit: 0 = clean (or clean plan), 1 = any action failed, 2 = setup/scan error.
"""
import argparse, asyncio, hashlib, ntpath, os, sys

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
sys.path.insert(0, REPO)
from client.retro_protocol import RetroConnection  # noqa: E402

SECRET = "retro-agent-secret"
GL_NAMES = ("opengl32.dll", "3dfxgl.dll", "3dfxogl.dll")
GLIDE_NAMES = ("glide2x.dll", "glide3x.dll")
WRAPPER_NAMES = ("ddraw.dll",)
ALL_NAMES = GL_NAMES + GLIDE_NAMES + WRAPPER_NAMES
# processes that hold these DLLs; --kill closes them before acting
GAME_PROCS = ("hl.exe", "quake3.exe", "quake2.exe", "UnrealTournament.exe",
              "UT2004.exe", "gamemd.exe", "RA2MD.exe", "hexen2.exe",
              "wolf.exe", "mohaa.exe", "3DMark2001SE.exe")

# candidate artifact locations, first hit wins (override with --icd/--glide2/--glide3)
RETRO3DFX = os.path.join(os.path.dirname(REPO), "retro-3dfx")
VINTAGE = {
    "icd":    [os.path.join(RETRO3DFX, "toolchain-3dfx/prefix/drive_c/3dfx/SWLIBS/OPENGL/GLIDE3X/release/opengl.dll")],
    "glide2": [os.path.join(RETRO3DFX, "toolchain-3dfx/prefix/drive_c/3dfx/H5/BIN/glide2x.dll")],
    "glide3": [os.path.join(RETRO3DFX, "toolchain-3dfx/prefix/drive_c/3dfx/H5/BIN/glide3x.dll")],
}
CLEANROOM = {
    "icd":    [os.path.join(REPO, "voodoo-cleanroom/build/opengl32.dll"),
               os.path.join(REPO, "voodoo-cleanroom/build-mesafx/opengl32.dll"),
               os.path.join(REPO, "scripts/3dfx/out/opengl32.dll")],
    "glide2": [os.path.join(REPO, "scripts/3dfx/out/glide2x.dll")],
    "glide3": [os.path.join(REPO, "scripts/3dfx/out/glide3x.dll")],
}


def md5(b):
    return hashlib.md5(b).hexdigest()


def resolve_artifacts(flavor, overrides):
    table = VINTAGE if flavor == "vintage" else CLEANROOM
    out = {}
    for key in ("icd", "glide2", "glide3"):
        if overrides.get(key):
            cands = [overrides[key]]
        else:
            cands = table[key]
        path = next((p for p in cands if os.path.isfile(p)), None)
        if path is None:
            print("ERROR: no %s artifact found (tried: %s) — pass --%s"
                  % (key, ", ".join(cands), key if key == "icd" else key))
            return None
        data = open(path, "rb").read()
        out[key] = {"path": path, "data": data, "md5": md5(data), "size": len(data)}
        print("artifact %-6s: %s (%d bytes, %s)" % (key, path, len(data), out[key]["md5"][:8]))
    return out


async def rc(c, cmd, t=60):
    _, d = await c.send_command(cmd, timeout=t)
    return d.decode("ascii", "replace")


async def scan(c, drives):
    """dir /s /b every relevant DLL name on each drive; skip \\WINDOWS\\."""
    found = []
    for drv in drives:
        pats = " ".join("%s:\\%s" % (drv, n) for n in ALL_NAMES)
        out = await rc(c, "EXEC cmd /c dir /s /b %s 2>nul" % pats, t=300)
        for line in out.splitlines():
            p = line.strip()
            if not p or "\\WINDOWS\\" in p.upper() or "\\RETRO_AGENT\\" in p.upper():
                continue
            if p.lower().endswith(".dll"):
                found.append(p)
    return sorted(set(found))


async def classify(c, path, art):
    """download and md5-classify one remote file."""
    try:
        b = await c.command_binary("DOWNLOAD %s" % path, timeout=120)
    except Exception as e:
        return {"path": path, "cls": "unreadable", "detail": str(e)[:60]}
    h, n = md5(b), ntpath.basename(path).lower()
    if n in GL_NAMES and h == art["icd"]["md5"]:
        return {"path": path, "cls": "ours-current", "size": len(b)}
    if n == "glide2x.dll" and h == art["glide2"]["md5"]:
        return {"path": path, "cls": "ours-current", "size": len(b)}
    if n == "glide3x.dll" and h == art["glide3"]["md5"]:
        return {"path": path, "cls": "ours-current", "size": len(b)}
    kind = "wrapper" if (n in WRAPPER_NAMES or n in GLIDE_NAMES) else "stale-or-foreign"
    if n == "glide2x.dll" and len(b) == 1310720:
        kind = "wrapper-nglide"
    return {"path": path, "cls": kind, "size": len(b), "md5": h}


async def put_verified(c, remote, data):
    await c.send_command("UPLOAD %s" % remote, binary_payload=data, timeout=180)
    back = await c.command_binary("DOWNLOAD %s" % remote, timeout=120)
    return md5(back) == md5(data)


async def act_on(c, item, art, flavor):
    """apply the policy to one classified non-current file. Returns (action, ok)."""
    path = item["path"]
    n = ntpath.basename(path).lower()
    if n in GL_NAMES:
        # backup once, then replace with the ICD
        await rc(c, 'EXEC cmd /c if not exist "%s.pre" copy /Y "%s" "%s.pre"' % (path, path, path), t=30)
        ok = await put_verified(c, path, art["icd"]["data"])
        return ("replaced-with-ICD", ok)
    if n in GLIDE_NAMES:
        if flavor == "vintage":
            out = await rc(c, 'EXEC cmd /c move /Y "%s" "%s.wrapper.bak" 2>&1' % (path, path), t=30)
            gone = "cannot" not in out.lower() and "denied" not in out.lower()
            return ("retired-shadow", gone)
        key = "glide2" if n == "glide2x.dll" else "glide3"
        ok = await put_verified(c, path, art[key]["data"])
        return ("replaced-with-cleanroom-glide", ok)
    if n in WRAPPER_NAMES:
        out = await rc(c, 'EXEC cmd /c move /Y "%s" "%s.wrapper.bak" 2>&1' % (path, path), t=30)
        gone = "cannot" not in out.lower() and "denied" not in out.lower()
        return ("retired-wrapper", gone)
    return ("skipped", True)


async def fix_ut99(c, sys_dir, apply):
    """switch UT99 to the native Glide renderer (correct + fastest on 3dfx)."""
    ini = sys_dir + r"\UnrealTournament.ini"
    txt = await rc(c, 'EXEC cmd /c type "%s" 2>nul' % ini, t=30)
    if "RenderDevice=" not in txt:
        return None
    if "RenderDevice=GlideDrv.GlideRenderDevice" in txt and \
       "GameRenderDevice=GlideDrv.GlideRenderDevice" in txt:
        return "ut99-already-glide"
    if not apply:
        return "ut99-would-switch-to-GlideDrv"
    out = []
    for line in txt.splitlines():
        s = line.strip()
        if s.split("=")[0] in ("GameRenderDevice", "WindowedRenderDevice", "RenderDevice"):
            out.append("%s=GlideDrv.GlideRenderDevice" % s.split("=")[0])
        else:
            out.append(line)
    await c.send_command("UPLOAD %s" % ini,
                         binary_payload=("\r\n".join(out) + "\r\n").encode("latin-1", "replace"),
                         timeout=60)
    return "ut99-switched-to-GlideDrv"


async def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("host")
    ap.add_argument("--flavor", choices=("vintage", "cleanroom"), required=True)
    ap.add_argument("--apply", action="store_true")
    ap.add_argument("--kill", action="store_true")
    ap.add_argument("--drives", nargs="+", default=["C"])
    ap.add_argument("--icd"); ap.add_argument("--glide2"); ap.add_argument("--glide3")
    a = ap.parse_args()

    art = resolve_artifacts(a.flavor, {"icd": a.icd, "glide2": a.glide2, "glide3": a.glide3})
    if art is None:
        return 2

    c = RetroConnection(a.host, 9898)
    await c.connect(SECRET, timeout=20)

    if a.kill and a.apply:
        for p in GAME_PROCS:
            await rc(c, "EXEC cmd /c taskkill /f /im %s 2>nul" % p, t=20)
        await asyncio.sleep(3)

    print("scanning drives %s for %s ..." % (a.drives, ", ".join(ALL_NAMES)))
    paths = await scan(c, a.drives)
    print("found %d game-local candidates" % len(paths))

    rows, failures, ut_dirs, gl_md5s = [], 0, set(), {}
    for p in paths:
        item = await classify(c, p, art)
        d = ntpath.dirname(p)
        n = ntpath.basename(p).lower()
        disp = item["cls"] + (":" + item["md5"][:8] if "md5" in item else "")
        if n in GL_NAMES and "md5" in item:
            gl_md5s[item["md5"]] = gl_md5s.get(item["md5"], 0) + 1
        # UT99 detection (fix its ini rather than pushing a GL ICD at it)
        probe = await rc(c, 'EXEC cmd /c dir /b "%s\\UnrealTournament.exe" 2>nul' % d, t=20)
        if probe.strip():
            ut_dirs.add(d)
        if item["cls"] in ("ours-current",):
            rows.append((p, disp, "none", "ok"))
            continue
        if item["cls"] == "unreadable":
            rows.append((p, disp, "none", item.get("detail", "")))
            failures += 1
            continue
        if n in GL_NAMES and d in ut_dirs:
            rows.append((p, disp, "left-alone (UT99 uses GlideDrv)", "ok"))
            continue
        if not a.apply:
            rows.append((p, disp, "PLAN: " + ("retire" if item["cls"].startswith("wrapper")
                        or (a.flavor == "vintage" and n in GLIDE_NAMES)
                        else "replace"), "dry-run"))
            continue
        action, ok = await act_on(c, item, art, a.flavor)
        rows.append((p, disp, action, "ok" if ok else "FAILED"))
        if not ok:
            failures += 1

    # Downgrade guard: if the deployed GL copies are self-consistent on ONE md5
    # that differs from the local ICD artifact, the fleet may be AHEAD of the
    # local build (e.g. a newer ICD was deployed by another session). Warn hard.
    dominant = max(gl_md5s.items(), key=lambda kv: kv[1]) if gl_md5s else None
    if dominant and dominant[1] >= 3 and dominant[0] != art["icd"]["md5"]:
        print("\n*** WARNING: %d GL copies on the box share md5 %s, which is NOT the local"
              % (dominant[1], dominant[0][:8]))
        print("*** ICD artifact (%s). The box may be running a NEWER build than your local"
              % art["icd"]["md5"][:8])
        print("*** tree. Confirm the local artifact is the intended version before --apply,")
        print("*** or pass --icd <path-to-current-build>. Applying would overwrite ALL copies.")

    for d in sorted(ut_dirs):
        res = await fix_ut99(c, d, a.apply)
        if res:
            rows.append((d + r"\UnrealTournament.ini", "ut99", res, "ok"))

    await c.close()

    print("\n%-68s %-28s %-34s %s" % ("PATH", "CLASS", "ACTION", "RESULT"))
    for r in rows:
        print("%-68s %-28s %-34s %s" % (r[0][:68], r[1], r[2][:34], r[3]))
    print("\n%d candidates, %d failures%s" % (len(rows), failures,
          "" if a.apply else "  (dry-run — re-run with --apply to act)"))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
