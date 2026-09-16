#!/usr/bin/env python3
"""
v56k_diag.py - the flight recorder and crash capture for the Voodoo 5 6000
running AmigaMerlin, so a benchmarking failure says WHY instead of "it hung".

WHY THIS EXISTS
---------------
The vintage H5 driver we build ourselves carries a registry-ring flight
recorder (`RLog00..RLog31` + `RLogSeq` under the miniport `Device0` key, gated
by `Retro3dfxLog=1`). **AmigaMerlin 3.1-R11 is a RETAIL driver and has no such
ring** - measured 2026-09-16 on .124: no `RLog*` under the display class, the
`3dfxvs` service, or `Device0`. So the ring cannot be read; it has to be
recreated from OUTSIDE the driver. Two surfaces do that, and both are used here:

1. **The per-chip scanout recorder** - `retro-3dfx/tools/v56k/fxscan2.c`, which
   rides the `HWCEXT_GET_SLAVE_REGS` escape the SHIPPING driver already answers
   (verified against AmigaMerlin on .124: escape 0x3df3, 121A:0009, numChips 4).
   `fxscan2 ring` samples every watched register on all four VSA-100s and logs
   each change, fflushing every line, so a wedge still leaves the last state on
   disk. It is the only recorder that can see a **Glide fullscreen** session at
   all: the display driver releases the hardware on `DrvAssertMode(DISABLE)` and
   Glide programs the card directly, so nothing in the driver is on that path.
   NOTE: start it BEFORE the game - the mode transition is the interesting part.

2. **Dr Watson** - which is what actually cracked the Quake III "hang". The
   crash is not a hang at all:

       glide3x!grDrawTriangle+0x2d      <- int3, STATUS_BREAKPOINT (80000003)
       3dfxogl+0xc65a1                  <- AmigaMerlin's Mesa ICD
       3dfxogl+0xba55a / 0xb90af / 0xb9608
       quake3+0x6208d

   AmigaMerlin's retail `glide3x.dll` ships a **hardcoded breakpoint** inside
   `grDrawTriangle`, reached by a NULL test (`jz`) and a low-bit test (`jnz`)
   on a state pointer. The ICD trips it. With no debugger attached the box
   LOOKS wedged because the crash dialog sits behind the exclusive fullscreen
   surface - the same modal-behind-fullscreen trap already in FINDINGS.md.

That last point is why `errors_quiet()` exists: suppressing the dialog converts
a whole-box wedge into an ordinary failed cell that the sweep can carry on past,
and the dump is still written for us to read.

USAGE
    python3 v56k_diag.py --host 192.168.1.124 dump          # per-chip registers
    python3 v56k_diag.py --host 192.168.1.124 watson        # decode last crash
    python3 v56k_diag.py --host 192.168.1.124 quiet         # suppress crash dialogs
    python3 v56k_diag.py --host 192.168.1.124 capture       # full bundle
    python3 v56k_diag.py --host 192.168.1.124 ring --secs 70 --interval 60
"""
import argparse
import asyncio
import re
import shutil
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
sys.path.insert(0, str(REPO))
sys.path.insert(0, str(HERE))
from v56k_bench import Box  # noqa: E402

# fxscan2 is the OTHER lane's tool and stays single-sourced there: this module
# BUILDS it and deploys the exe, it never copies or edits the C. Editing
# retro-3dfx driver code is forbidden; tools/ is not driver code, and reading
# and building it is explicitly allowed.
FXSCAN_SRC = Path("/home/voidsstr/development/retro-3dfx/tools/v56k/fxscan2.c")
DEPLOY_DIR = r"C:\RETRO_AGENT\v56k-deploy"
FXSCAN_EXE = rf"{DEPLOY_DIR}\fxscan2.exe"
RING_TXT = r"C:\fxring.txt"
WATSON_DIR = r"C:\Documents and Settings\All Users\Application Data\Microsoft\Dr Watson"
WATSON_LOG = rf"{WATSON_DIR}\drwtsn32.log"
WATSON_DMP = rf"{WATSON_DIR}\user.dmp"


def log(msg):
    print(msg, flush=True)


def build_fxscan(out=None):
    """Build fxscan2.exe from the retro-3dfx source. Returns the local path."""
    out = Path(out or (HERE / "bin" / "fxscan2.exe"))
    out.parent.mkdir(parents=True, exist_ok=True)
    if not FXSCAN_SRC.exists():
        raise SystemExit(f"fxscan2 source not found at {FXSCAN_SRC}")
    if out.exists() and out.stat().st_mtime >= FXSCAN_SRC.stat().st_mtime:
        return out
    cc = shutil.which("i686-w64-mingw32-gcc")
    if not cc:
        raise SystemExit("i686-w64-mingw32-gcc not found - cannot build fxscan2")
    subprocess.run([cc, "-O2", "-Wall", "-o", str(out), str(FXSCAN_SRC), "-lgdi32"], check=True)
    return out


async def ensure_fxscan(box):
    """Upload fxscan2.exe if the box does not already have this exact build."""
    local = build_fxscan()
    want = local.stat().st_size
    await box.exec_(f'cmd /c if not exist "{DEPLOY_DIR}" mkdir "{DEPLOY_DIR}"')
    got = await box.exec_(f'cmd /c for %I in ("{FXSCAN_EXE}") do @echo %~zI')
    cur = got.strip().splitlines()[-1].strip() if got.strip() else ""
    if cur == str(want):
        return False
    await box.upload(FXSCAN_EXE, local.read_bytes())
    return True


async def dump(box, diff=False):
    """Per-chip scanout register table straight from the shipping driver.

    Trap: read this DURING the failing state. After a game exits the slaves keep
    the game's geometry - nothing restores them - so a desktop dump shows a
    divergence that is not one, and a stale-but-equal dump hides a real one.
    """
    await ensure_fxscan(box)
    # EXECW is its own agent command, NOT an argument to EXEC. Box.exec_ already
    # prefixes EXEC, so routing it through there produced the cmd.exe error
    # "'EXECW' is not recognized" in a 96-byte dump that looked like a real
    # capture - make the failure visible instead of banking it.
    out = await box.text(f'EXECW 40 {FXSCAN_EXE} {"diff" if diff else "dump"}', timeout=90)
    if "not recognized" in out or "escape" not in out:
        raise RuntimeError(f"fxscan2 did not run: {out.strip()[:200]}")
    return out


async def errors_quiet(box):
    """Stop a crash from LOOKING like a wedge.

    An int3 in glide3x raises a breakpoint exception; the resulting dialog sits
    behind the exclusive fullscreen surface and the whole box reads as hung.
    These four values keep the dump (we want it) and drop the UI (we do not).
    Returns the post-condition, read back - never the `OK`.
    """
    sets = [
        (r"HKLM\SOFTWARE\Microsoft\PCHealth\ErrorReporting", "DoReport", "0"),
        (r"HKLM\SOFTWARE\Microsoft\PCHealth\ErrorReporting", "ShowUI", "0"),
        (r"HKLM\SYSTEM\CurrentControlSet\Control\Windows", "ErrorMode", "2"),
        (r"HKLM\SOFTWARE\Microsoft\DrWatson", "VisualNotification", "0"),
    ]
    for key, name, val in sets:
        await box.exec_(f'cmd /c reg add "{key}" /v {name} /t REG_DWORD /d {val} /f')
    out = {}
    for key, name, _ in sets:
        got = await box.exec_(f'cmd /c reg query "{key}" /v {name} 2>nul')
        m = re.search(rf"{name}\s+REG_DWORD\s+(0x[0-9a-fA-F]+)", got)
        out[name] = m.group(1) if m else "UNREADABLE"
    return out


async def watson_clear(box):
    """Delete the crash log so the NEXT crash is unambiguously the next crash."""
    await box.exec_(f'cmd /c del /f /q "{WATSON_LOG}" "{WATSON_DMP}" 2>nul & echo ok')


async def watson_fetch(box, outdir):
    """Download Dr Watson's log + dump. Returns (log_path, bytes) or (None, 0)."""
    outdir = Path(outdir)
    outdir.mkdir(parents=True, exist_ok=True)
    got = {}
    for remote, name in ((WATSON_LOG, "drwtsn32.log"), (WATSON_DMP, "user.dmp")):
        try:
            data = await box.download(remote)
        except Exception:
            data = None
        if data:
            (outdir / name).write_bytes(data)
            got[name] = len(data)
    return got


def read_watson_text(path):
    """drwtsn32.log is ANSI when Dr Watson APPENDS to an existing file and
    UTF-16LE with a BOM when it CREATES one - which is every log after
    watson_clear(). Decoding a fresh log as latin-1 finds no markers at all and
    reports "no crash" for a crash that is right there (measured on the GLQuake
    exit, 67,522 B, 33,760 NULs). Sniff the BOM; never assume."""
    b = Path(path).read_bytes()
    if b[:2] in (b"\xff\xfe", b"\xfe\xff"):
        return b.decode("utf-16", errors="replace")
    if len(b) > 64 and b[1:64:2].count(b"\x00") > 24:
        return b.decode("utf-16-le", errors="replace")
    return b.decode("latin-1", errors="replace")


def watson_decode(path):
    """Pull the useful facts out of a drwtsn32.log: the app, the exception, the
    faulting function and the stack. This is what named glide3x!grDrawTriangle."""
    txt = read_watson_text(path)
    recs = []
    for m in re.finditer(r"Application exception occurred:\s*\n\s*App:\s*(.+?)\n\s*When:\s*(.+?)\n\s*Exception number:\s*(.+?)\n", txt):
        recs.append({"app": m.group(1).strip(), "when": m.group(2).strip(), "exception": m.group(3).strip()})
    funcs = [m.group(1).strip() for m in re.finditer(r"function:\s*(.+)", txt)]
    frames = []
    i = txt.lower().find("stack back trace")
    if i >= 0:
        for line in txt[i:i + 3000].splitlines():
            fm = re.match(r"\s*[0-9a-fA-F]{8}\s+[0-9a-fA-F]{8}\s+(?:[0-9a-fA-F]{8}\s+){0,3}(\S+)", line)
            if fm and not fm.group(1).startswith("*"):
                frames.append(fm.group(1))
    return {"records": recs, "fault_function": funcs[0] if funcs else None, "frames": frames[:12]}


async def ring_start(box, secs=70, interval_ms=60, outfile=RING_TXT):
    """Arm the per-chip flight recorder, DETACHED, before the game launches.

    LAUNCH (not EXEC) because this must outlive the call and keep sampling
    through the mode switch and the whole session.
    """
    await ensure_fxscan(box)
    await box.exec_(f'cmd /c del /f /q "{outfile}" 2>nul & echo ok')
    await box.text(f'LAUNCH cmd /c {FXSCAN_EXE} ring {secs} {interval_ms} {outfile}', timeout=40)


async def ring_fetch(box, outdir, name="fxring.txt", outfile=RING_TXT):
    outdir = Path(outdir)
    outdir.mkdir(parents=True, exist_ok=True)
    try:
        data = await box.download(outfile)
    except Exception:
        data = None
    if not data:
        return None, 0
    (outdir / name).write_bytes(data)
    return outdir / name, len(data)


def ring_summary(path):
    """Condense a ring log: which chips moved, which registers, CRTC heartbeat."""
    txt = Path(path).read_text(encoding="latin-1", errors="replace")
    changes, hb = {}, []
    for line in txt.splitlines():
        m = re.match(r"\s*(\d+)\s+(\d)\s+(\S+)\s+([0-9A-Fa-f]{8})\s*->\s*([0-9A-Fa-f]{8})", line)
        if m:
            changes.setdefault((m.group(2), m.group(3)), []).append((m.group(1), m.group(4), m.group(5)))
            continue
        if " HB " in line or line.strip().startswith("HB"):
            hb.append(line.strip())
    return {"changed": {f"chip{c} {r}": v for (c, r), v in sorted(changes.items())},
            "heartbeats": hb[-6:], "stalled": _hb_stalled(hb)}


def _hb_stalled(hb):
    """A CRTC that stops advancing is a wedge signature the register diff misses."""
    vals = []
    for line in hb[-4:]:
        vals.append(tuple(re.findall(r"c\d=\s*(\d+)", line)))
    return len(vals) >= 2 and len(set(vals)) == 1


async def capture(box, outdir, label="capture"):
    """Everything worth having after a failure, in one bundle."""
    outdir = Path(outdir)
    outdir.mkdir(parents=True, exist_ok=True)
    report = {"label": label}
    try:
        report["dump"] = await dump(box)
        (outdir / f"{label}-fxscan-dump.txt").write_text(report["dump"])
    except Exception as e:
        report["dump_error"] = f"{type(e).__name__}: {e}"
    got = await watson_fetch(box, outdir)
    report["watson_files"] = got
    if "drwtsn32.log" in got:
        report["watson"] = watson_decode(outdir / "drwtsn32.log")
    try:
        report["glide_reg"] = await box.exec_(
            r'cmd /c reg query "HKLM\SYSTEM\CurrentControlSet\Control\Class'
            r'\{4D36E968-E325-11CE-BFC1-08002BE10318}\0001\Settings\Glide" 2>nul')
    except Exception:
        pass
    ringp, n = await ring_fetch(box, outdir, f"{label}-fxring.txt")
    if ringp:
        report["ring_bytes"] = n
        report["ring"] = ring_summary(ringp)
    return report


async def main_async(a):
    box = Box(a.host)
    if a.cmd == "dump":
        log(await dump(box, diff=a.diff))
    elif a.cmd == "quiet":
        log(f"crash-dialog suppression (read back): {await errors_quiet(box)}")
    elif a.cmd == "watson":
        got = await watson_fetch(box, a.outdir)
        if not got:
            log("no Dr Watson log on the box (no crash recorded since it was last cleared)")
            return 0
        log(f"fetched {got}")
        d = watson_decode(Path(a.outdir) / "drwtsn32.log")
        for r in d["records"]:
            log(f"  app={r['app']}\n  when={r['when']}\n  exception={r['exception']}")
        log(f"  fault function: {d['fault_function']}")
        for f in d["frames"]:
            log(f"    {f}")
    elif a.cmd == "clear":
        await watson_clear(box)
        log("Dr Watson log cleared")
    elif a.cmd == "ring":
        await ring_start(box, a.secs, a.interval)
        log(f"ring armed for {a.secs}s @ {a.interval}ms -> {RING_TXT}")
    elif a.cmd == "ringfetch":
        p, n = await ring_fetch(box, a.outdir)
        log(f"ring -> {p} ({n} B)" if p else "no ring file on the box")
        if p:
            log(str(ring_summary(p)))
    elif a.cmd == "capture":
        import json
        rep = await capture(box, a.outdir, a.label)
        (Path(a.outdir) / f"{a.label}-report.json").write_text(json.dumps(rep, indent=1))
        log(json.dumps({k: v for k, v in rep.items() if k != "dump"}, indent=1)[:3000])
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("cmd", choices=["dump", "quiet", "watson", "clear", "ring", "ringfetch", "capture"])
    ap.add_argument("--host", required=True)
    ap.add_argument("--outdir", default=str(HERE / "results" / "v56k_diag"))
    ap.add_argument("--label", default="capture")
    ap.add_argument("--secs", type=int, default=70)
    ap.add_argument("--interval", type=int, default=60)
    ap.add_argument("--diff", action="store_true")
    return asyncio.run(main_async(ap.parse_args()))


if __name__ == "__main__":
    sys.exit(main())


# Windows that BLOCK a benchmark and never go away on their own. Each one has
# cost a stalled run on .124: the engine sits on a modal while the runner waits
# out max_run, times out, retries, and burns attempts x max_run on a cell that
# could never have produced a number.
BLOCKING_MODALS = (
    "critical error",      # UE1's own GPF box - e.g. UOpenGlRenderDevice::SetRes
    "cd check",            # Serious Sam: "Please insert the game CD"
    "please insert",
    "found new hardware",  # the wizard that froze a Quake III run
    "drwtsn32", "dr. watson",
    "has encountered a problem",
)


async def blocking_modal(box):
    """Return the title of a blocking modal that is up, or None.

    Cheap enough to poll: one WINLIST. This is how a doomed cell is failed in
    seconds instead of attempts x max_run.
    """
    try:
        out = await box.text("WINLIST", timeout=30)
    except Exception:
        return None
    for t in re.findall(r'"title"\s*:\s*"((?:[^"\\]|\\.)*)"', out):
        if any(k in t.lower() for k in BLOCKING_MODALS):
            return t.encode().decode("unicode_escape", errors="replace")
    return None


async def process_alive(box, image_name):
    """Is a process with this image name running? One PROCLIST; None if it
    could not be asked (a dead agent must not read as a dead game)."""
    import json
    try:
        procs = json.loads(await box.text("PROCLIST", timeout=30))
    except Exception:
        return None
    want = image_name.lower()
    return any((p.get("name") or "").lower() == want for p in procs)
