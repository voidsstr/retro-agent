#!/usr/bin/env python3
"""
v56k_audit.py - is each title on the box READY to be benchmarked, and through
which driver files?

Read-only. Answers, per title, the questions that have produced a wrong number
or a wasted boot before: is the exe there; which game-local OpenGL/Glide/DDraw
DLLs sit beside it (game-local wins at load time, so a stray nGlide or an old
ICD silently replaces the driver under test); is the demo the runner plays
present; what colour depth and render device does the config carry; and, for
the id Tech 3 titles, which r_glDriver the saved config forces (a saved
`r_glDriver "gl/openglv5.dll"` overrides a `+set` on the command line).

    python3 v56k_audit.py --host 192.168.1.124 [--json out.json]
"""
import argparse
import asyncio
import hashlib
import json
import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
sys.path.insert(0, str(REPO))
from client.retro_protocol import RetroConnection  # noqa: E402

SECRET = "retro-agent-secret"

# The identities that matter. Sizes are what has been seen on this fleet; the
# md5s of the AmigaMerlin files are the ones recorded in versions.json.
KNOWN = {
    1310720: "nGlide wrapper glide2x (bypasses the card)",
    989027: "open glide3x_h5 (hard-froze host 1)",
    2646009: "AmigaMerlin 3dfxOGL.dll (Mesa 6.3 ICD)",
    344064: "AmigaMerlin glide3x.dll",
    94208: "AmigaMerlin glide2x.dll",
    142848: "3dfxgl.dll MiniGL (Quake II staged)",
    352256: "gl/openglv5.dll as staged with RtCW (not AmigaMerlin's)",
}
DLLS = ("opengl32.dll", "3dfxgl.dll", "3dfxogl.dll", "glide2x.dll", "glide3x.dll", "ddraw.dll", "openglv5.dll")

TITLES = {
    "quake3":      {"root": r"C:\Games\Quake3-TeamArena", "exe": "quake3.exe", "cfg": r"baseq3\q3config.cfg", "keys": ["r_glDriver", "r_colorbits", "r_mode", "r_texturebits", "r_depthbits"]},
    "quake2":      {"root": r"C:\Games\Quake2Complete", "exe": "quake2.exe", "cfg": r"baseq2\config.cfg", "keys": ["gl_driver", "vid_ref", "gl_mode", "gl_bitdepth"]},
    "glquake":     {"root": r"C:\Games\Quake1", "exe": "GLQUAKE.EXE", "cfg": r"id1\config.cfg", "keys": ["vid_mode", "gl_"]},
    "ut":          {"root": r"C:\Games\UnrealTournament436", "exe": r"System\UnrealTournament.exe", "cfg": r"System\UnrealTournament.ini",
                    "keys": ["GameRenderDevice", "RenderDevice", "WindowedRenderDevice", "FullscreenColorBits", "FullscreenViewportX", "FullscreenViewportY", "UseVSync"],
                    "demo": r"System\UTbench.dem", "ini2": r"System\User.ini", "ini2_keys": ["F9=", "F10=", "F11="]},
    "unrealgold":  {"root": r"C:\Games\UnrealGold", "exe": r"System\Unreal.exe", "cfg": r"System\Unreal.ini",
                    "keys": ["GameRenderDevice", "RenderDevice", "WindowedRenderDevice", "FullscreenColorBits", "FullscreenViewportX", "FullscreenViewportY"]},
    "deusex":      {"root": r"C:\Games\DeusEx", "exe": r"SYSTEM\DEUSEX.EXE", "cfg": r"SYSTEM\DeusEx.ini",
                    "keys": ["GameRenderDevice", "RenderDevice", "WindowedRenderDevice", "FullscreenColorBits", "FullscreenViewportX", "FullscreenViewportY"]},
    "rtcw":        {"root": r"C:\Games\ReturnToCastleWolfenstein", "exe": "WolfMP.exe", "exe2": "WolfSP.exe", "cfg": r"main\wolfconfig_mp.cfg", "cfg2": r"main\wolfconfig.cfg",
                    "keys": ["r_glDriver", "r_colorbits", "r_mode", "r_texturebits"], "demo": r"main\demos\wolfbench.dm_60"},
    "serioussam":  {"root": r"C:\Games\SeriousSamFirstEncounter", "exe": r"Bin\SeriousSam.exe", "cfg": r"Scripts\PersistentSymbols.ini", "keys": ["gfx_iDisplayDepth", "gfx_pixResWidth", "gfx_pixResHeight", "gfx_strAPI", "gfx_iRefreshRate"]},
    "serioussam2": {"root": r"C:\Games\SeriousSamSecondEncounter", "exe": r"Bin\SeriousSam.exe", "cfg": r"Scripts\PersistentSymbols.ini", "keys": ["gfx_iDisplayDepth", "gfx_pixResWidth", "gfx_pixResHeight", "gfx_strAPI", "gfx_iRefreshRate"]},
}


async def cmd(c, x, t=90):
    st, d = await c.send_command(x, timeout=t)
    return st, d.decode("ascii", errors="replace")


async def dl(c, path):
    try:
        return await c.command_binary(f"DOWNLOAD {path}", timeout=120)
    except Exception:
        return None


async def audit(ip):
    c = RetroConnection(ip, 9898)
    await c.connect(SECRET, timeout=20.0)
    out = {}
    try:
        # system32 driver files, for the "which file is really in play" comparison
        sysd = {}
        for f in ("opengl32.dll", "3dfxOGL.dll", "3dfxgl.dll", "glide2x.dll", "glide3x.dll", "3dfxvs.dll"):
            st, o = await cmd(c, f'EXEC cmd /c for %I in ("C:\\WINDOWS\\system32\\{f}") do @echo %~zI')
            sysd[f] = o.strip().splitlines()[-1].strip() if o.strip() else "-"
        out["system32"] = sysd
        for tid, t in TITLES.items():
            r = {"root": t["root"]}
            st, o = await cmd(c, f'EXEC cmd /c if exist "{t["root"]}\\{t["exe"]}" (echo Y) else (echo N)')
            r["exe"] = "present" if "Y" in o else "MISSING"
            if t.get("exe2"):
                st, o = await cmd(c, f'EXEC cmd /c if exist "{t["root"]}\\{t["exe2"]}" (echo Y) else (echo N)')
                r["exe2"] = "present" if "Y" in o else "MISSING"
            # game-local DLLs, any depth up to 3 under the root
            st, o = await cmd(c, f'EXEC cmd /c dir /s /b /-c "{t["root"]}\\*.dll" 2>nul', 120)
            local = []
            for line in o.replace("\r", "").splitlines():
                name = line.rsplit("\\", 1)[-1].lower()
                if name in DLLS or name.endswith(".wrapper.bak") or name.endswith(".3dfxbak"):
                    st2, sz = await cmd(c, f'EXEC cmd /c for %I in ("{line.strip()}") do @echo %~zI')
                    size = sz.strip().splitlines()[-1].strip() if sz.strip() else "?"
                    ident = KNOWN.get(int(size), "") if size.isdigit() else ""
                    local.append({"path": line.strip().replace(t["root"], "."), "size": size, "known_as": ident})
            r["game_local_dlls"] = local
            if t.get("demo"):
                st, o = await cmd(c, f'EXEC cmd /c if exist "{t["root"]}\\{t["demo"]}" (echo Y) else (echo N)')
                r["demo"] = {"path": t["demo"], "present": "Y" in o}
            for key in ("cfg", "cfg2", "ini2"):
                if not t.get(key):
                    continue
                data = await dl(c, f'{t["root"]}\\{t[key]}')
                if data is None:
                    r[key] = {"path": t[key], "present": False}
                    continue
                txt = data.decode("latin-1", errors="replace")
                keys = t["ini2_keys"] if key == "ini2" else t["keys"]
                found = {}
                for k in keys:
                    for line in txt.splitlines():
                        s = line.strip()
                        if s.lower().startswith(k.lower()) or (k.endswith("=") and s.startswith(k)):
                            found.setdefault(k, []).append(s[:100])
                r[key] = {"path": t[key], "present": True, "values": found}
            out[tid] = r
    finally:
        await c.close()
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", required=True)
    ap.add_argument("--json", default=None)
    a = ap.parse_args()
    out = asyncio.run(audit(a.host))
    if a.json:
        Path(a.json).write_text(json.dumps(out, indent=1))
    print("system32:", out.get("system32"))
    for tid, r in out.items():
        if tid == "system32":
            continue
        print(f"\n== {tid}  {r['root']}  exe:{r['exe']}" + (f" exe2:{r['exe2']}" if "exe2" in r else ""))
        for d in r.get("game_local_dlls", []):
            flag = "  <-- WRAPPER/STALE" if d["known_as"] and ("wrapper" in d["known_as"] or "froze" in d["known_as"]) else ""
            print(f"   dll {d['path']}  {d['size']} B  {d['known_as']}{flag}")
        if "demo" in r:
            print(f"   demo {r['demo']['path']}: {'present' if r['demo']['present'] else 'MISSING'}")
        for key in ("cfg", "cfg2", "ini2"):
            if key in r:
                if not r[key]["present"]:
                    print(f"   {key} {r[key]['path']}: MISSING")
                else:
                    for k, vals in r[key]["values"].items():
                        print(f"   {key} {k}: {vals[:3]}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
