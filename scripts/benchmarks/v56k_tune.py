#!/usr/bin/env python3
"""
v56k_tune.py - apply the per-box game tuning for .124 (Voodoo 5 6000, our
clean-room stack, Athlon XP 2 GHz) to the games' OWN config files.

Every file here is per-box state the game writes itself - none of them is in
the staged library (checked 2026-10-02), so GAMESYNC never puts them back, and
a re-image loses them: re-run this afterwards. Re-run it after every ICD
update as well: id Tech 3 games remember the GL renderer string in
r_lastValidRenderer, our ICD's string carries its build number, and a game that
sees a "new card" resets its graphics to the low "recommended" preset (found
2026-10-02: RtCW at r_picmip 2 with vertex lighting, SoF2 at r_picmip 3 behind
a "New Video card detected" box). This script writes r_lastValidRenderer from
the ICD that is actually deployed.

Idempotent: a value already right is not rewritten. Each file is backed up
once, beside itself, as <name>.pretune, before its first change. The answer
says, per file, what changed - "0 changed" on a tuned box.

    python3 scripts/benchmarks/v56k_tune.py --host 192.168.1.124 [--dry-run]
"""
import argparse
import asyncio
import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parents[1]))
from client.retro_protocol import RetroConnection  # noqa: E402

SECRET = "retro-agent-secret"
G = r"C:\Games"
TRILINEAR = "GL_LINEAR_MIPMAP_LINEAR"
# the renderer string is built at run time from Glide's board name; the build
# tag comes from the deployed DLL (see renderer_string)
RENDERER_FMT = "Mesa Glide v0.62 Voodoo5 6000 (tm) [voodoo-cleanroom {ver}]"
RENDERER = object()       # placeholder value: "the deployed ICD's renderer string"

# Quake II engine: full-colour textures instead of the 8-bit paletted upload,
# trilinear filtering. Their config.cfg uses `set`.
Q2 = {"gl_ext_palettedtexture": "0", "gl_texturemode": TRILINEAR}
# id Tech 3: the full-quality preset - full-size 32-bit textures, lightmaps,
# trilinear, uncompressed (the VSA-100 has FXT1, not the S3TC these ask for)
Q3HQ = {"r_picmip": "0", "r_texturebits": "32", "r_colorbits": "32", "r_vertexLight": "0",
        "r_ext_compressed_textures": "0", "r_textureMode": TRILINEAR, "r_subdivisions": "4",
        "r_lastValidRenderer": RENDERER}

TUNING = [
    (rf"{G}\Quake2Complete\baseq2\config.cfg", Q2),
    (rf"{G}\Quake2Complete\xatrix\config.cfg", Q2),
    (rf"{G}\Quake2Complete\rogue\config.cfg", Q2),
    (rf"{G}\Quake2Complete\ctf\config.cfg", Q2),
    (rf"{G}\SiNGold\base\players\blade\config.cfg", Q2),
    (rf"{G}\SiNGold\2015\players\blade\config.cfg", Q2),
    (rf"{G}\SoldierOfFortune\user\config.cfg", {"gl_texturemode": TRILINEAR}),
    (rf"{G}\Quake3-TeamArena\baseq3\q3config.cfg", Q3HQ),
    (rf"{G}\JediAcademy\base\jaconfig.cfg", Q3HQ),
    (rf"{G}\JediAcademy\base\jampconfig.cfg", Q3HQ),
    (rf"{G}\SoldierOfFortune2\base\sof2sp.cfg", Q3HQ),
    (rf"{G}\SoldierOfFortune2\base\mp\sof2mp.cfg", Q3HQ),
    (rf"{G}\ReturnToCastleWolfenstein\main\wolfconfig.cfg", Q3HQ),
    (rf"{G}\ReturnToCastleWolfenstein\main\wolfconfig_mp.cfg", Q3HQ),
]


def set_cvars(text, wanted):
    """Return (new_text, changes). A cvar line keeps its own verb (seta/set);
    a missing cvar is appended as `seta` (`set` in a file that never uses
    seta - Quake II's config.cfg)."""
    changes = []
    out = text
    verb_default = "seta" if re.search(r"(?m)^\s*seta\s", text) else "set"
    nl = "\r\n" if "\r\n" in text else "\n"
    for name, value in wanted.items():
        # a line ends at \r?\n: stop BEFORE the \r so a CRLF file keeps its CR
        # (Quake II writes CRLF; without this its every cvar read as missing)
        pat = re.compile(r'(?im)^([ \t]*)(seta|set)([ \t]+)' + re.escape(name) +
                         r'([ \t]+)"?([^"\r\n]*?)"?[ \t]*(?=\r?$)')
        m = pat.search(out)
        if m:
            if m.group(5) == value:
                continue
            out = out[:m.start()] + f'{m.group(1)}{m.group(2)}{m.group(3)}{name}{m.group(4)}"{value}"' + out[m.end():]
            changes.append((name, m.group(5), value))
        else:
            if out and not out.endswith(("\n", "\r")):
                out += nl
            out += f'{verb_default} {name} "{value}"{nl}'
            changes.append((name, None, value))
    return out, changes


async def call(host, cmd, payload=None, raw=False, timeout=60):
    c = RetroConnection(host, 9898)
    await c.connect(SECRET, timeout=15)
    try:
        st, data = await (c.send_command(cmd, timeout=timeout, binary_payload=payload)
                          if payload is not None else c.send_command(cmd, timeout=timeout))
        return data if raw else data.decode("latin-1", "replace")
    finally:
        await c.close()


async def renderer_string(host):
    """The deployed system ICD's GL_RENDERER: the build tag from its bytes."""
    dll = await call(host, r"DOWNLOAD C:\WINDOWS\system32\retroicd.dll", raw=True, timeout=120)
    m = re.search(rb"\[voodoo-cleanroom (\d+\.\d+\.\d+)\]", dll)
    if not m:
        raise SystemExit("system32\\retroicd.dll carries no [voodoo-cleanroom x.y.z] tag - not our ICD?")
    return RENDERER_FMT.format(ver=m.group(1).decode())


async def amain(args):
    rend = await renderer_string(args.host)
    print(f"deployed ICD renderer: {rend}")
    total = 0
    for path, wanted in TUNING:
        want = {k: (rend if v is RENDERER else v) for k, v in wanted.items()}
        data = await call(args.host, f"DOWNLOAD {path}", raw=True)
        text = data.decode("latin-1", "replace")
        if text.startswith("Cannot open file"):
            print(f"  -- {path}: not on the box ({text.strip()}) - skipped")
            continue
        new, changes = set_cvars(text, want)
        total += len(changes)
        if not changes:
            print(f"  ok {path}: 0 changed")
            continue
        for name, old, value in changes:
            print(f"  ~~ {path}: {name} {old!r} -> {value!r}")
        if args.dry_run:
            continue
        bak = path + ".pretune"
        if (await call(args.host, f"DOWNLOAD {bak}", raw=True)).startswith(b"Cannot open file"):
            await call(args.host, f"UPLOAD {bak}", payload=data)
        await call(args.host, f"UPLOAD {path}", payload=new.encode("latin-1"))
        back = (await call(args.host, f"DOWNLOAD {path}", raw=True)).decode("latin-1", "replace")
        if back != new:
            raise SystemExit(f"{path}: the upload did not land - read back differs")
    print(f"{'would change' if args.dry_run else 'changed'} {total} value(s)")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--host", default="192.168.1.124")
    ap.add_argument("--dry-run", action="store_true")
    raise SystemExit(asyncio.run(amain(ap.parse_args())))


if __name__ == "__main__":
    main()
