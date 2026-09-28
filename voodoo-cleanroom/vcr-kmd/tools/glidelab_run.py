#!/usr/bin/env python3
"""glidelab_run.py - run glidelab.exe (tools/glidelab.c) on a box, collect RESULT.

    glidelab_run.py 192.168.1.124 fill  --res 1600x1200 --cfg 5 --refresh 60
    glidelab_run.py 192.168.1.124 bands --res 1024x768 --cfg 5
    glidelab_run.py 192.168.1.124 cycle --cfg 5 --cycles 3
    glidelab_run.py 192.168.1.124 abandon --cfg 5 --then fill
    # under the vendor driver, which cannot be asked for the EDID:
    glidelab_run.py 192.168.1.124 fill --res 1024x768 --monitor-info ours-info.json

Uploads out/glidelab.exe to C:\\vcr\\glidelab\\, writes the SLI/AA config the
way the bench runner does (registry + env), runs it through `start /wait` (a
normal STARTUPINFO - the agent's hidden EXEC would hide Glide's window) under
EXECW with a timeout, and prints the RESULT json from the flushed log.
`--then MODE` runs a second mode after it (the check that an `abandon`
left the board usable). `--glide` picks the glide3x.dll (default: our h5
build as staged beside Quake II). `--json-out` appends every RESULT to a file.

EVERY GLIDE SESSION IS A PAIR OF MONITOR RE-SYNCS (grSstWinOpen into the
mode, grSstWinClose - or XP, at exit - back out). On 2026-09-26 a sweep
re-synced .124's 1998 Sony CRT ~250 times at two a second. So:
  - `--res` at `--refresh` (Glide opens at 16 bpp) is checked on the host
    against the monitor's EDID ranges before anything is switched - the same
    gate as glidelab_sweep.py (mode_sweep.py's: the driver's own mode math, no
    slack at the top), on the live `vcrctl info`, or on --monitor-info (a
    `vcrctl info` saved from OUR driver on this box) under another driver,
    once the registry says that monitor is the one on the box (or, when it
    cannot say, with --i-have-checked-the-monitor);
  - a --refresh glidelab.c has no GR_REFRESH code for is refused: glidelab
    would open at 60 Hz instead, silently - a mode the gate never checked -
    and a session whose RESULT reports another "opened_hz" fails;
  - `--cycles` defaults to 2 and more than MAX_CYCLES (3) is refused
    (glidelab.exe paces every open and close 3 s apart and caps the count
    itself as well);
  - `--timeout` is the WORK's budget; the EXECW adds the pace gate's worst
    case for every open and close (a busy switch lock, then the floor), so
    the agent's tree kill never lands on a session only waiting its turn - a
    budget past the agent's clamp is refused;
  - a run that times out (the EXECW's own marker, or the host giving up) or
    fails is cleaned up before anything else may switch: PROCLIST, a
    glidelab.exe that was not running before killed BY PID through `vcrctl
    pace-kill` (paced, stamped), confirmed gone, the pace;
  - a session that lost the focus ("focus_lost"), found the switch lock busy
    ('pace lock busy') or opened another refresh ends the run like a wedge:
    `--then` does not follow it, and glidelab_sweep stops;
  - `--then` waits at least --pace before its session, and does not run at
    all when a survivor of the first could not be confirmed gone.

THE GLIDE STEP TRACE (--trace N, --collect). Our h5 Glide (fork AA-TRACE)
can write every step of grSstWinOpen, the LFB read and the close to a file,
each line flushed to the disk before the hardware access it names, so after
a deep wedge the file's LAST line is the step that froze (level 2 also waits
after each FIFO step of the open until the chips have run it). `--trace N`
passes it to glidelab.exe, which has Glide write <mode>.log.trace next to its
own step log (and, for an AA config, turns the splash plugin off). The old
trace is deleted before the session and DOWNLOADed after it, saved with the
step log under evidence/glidelab/trace/ (or --save-dir); a trace without the
"=== glidetrace" header - the DLL is not a traced build, or it could not
open the file - is a "trace_error" and fails the run. What came back is in
the result's "trace_file" (the last line: the step a wedge froze on); its
"trace" is glidelab's own, the level. A traced run is a diagnosis, not a
benchmark.

`--trace-cfg` (with --trace) adds the fork's PCI config-space dumps
(FX_GLIDE_TRACE_CFG=1: 9 PCI_OP escapes per chip after the SLI/AA escape and
at every READ lock); without it a level-1 trace adds file writes only. An AA
open is traced with neither the splash nor the splash PLUGIN (glidelab sets
FX_GLIDE_NO_SPLASH=1 and FX_GLIDE_NO_PLUGIN=1; the trace's
"splash: plugin present reg=0" line confirms it).

A deep wedge takes the box with it, so nothing can be downloaded until it is
power-cycled: then `--collect` DOWNLOADs the mode's step log, its trace and
the fork's default trace file, saves them and prints what they end with, and
READS every registry place a Glide takes SSTH3_SLI_AA_CONFIGURATION from - a
banner when one holds an AA value, because the next Glide app to start opens
it: the value that froze the box is still armed after the power cycle. That
is all it does: no gate, no upload, no launch, no switch, and no registry
write unless `--restore-cfg N` (0, 2 or 5) is given, which writes N where an
AA value was found and reads it back.

THE GLIDE ON THE BOX IS CHECKED (2026-09-27). The fork commits that add the
SLI/AA guards were local-only, and build-stack.sh clones origin, so a DLL
built anywhere else carries none of them - and one without them froze .124 on
cfg 1. Every run DOWNLOADs `--glide`, prints its md5 and whether it carries
RETRO3DFX_SLIAA_GUARD / RETRO3DFX_AA_TRACE in the plan line, and an AA --cfg
is refused before anything is written or launched when the guard is absent
(glidelab.exe checks GR_EXTENSION itself as well, for an AA value it reads
from the registry). An AA --cfg written to the registry is put back after the
session - the value that was there if it was not AA, else --restore-cfg
(default 5) - and read back; a box that cannot be reached for that gets a
banner naming the command to run after the power cycle.

OUR GLIDE'S OTHER OPT-INS (2026-09-27). Both reach Glide the only way it reads
them - glidelab.exe puts them in its own process environment (glide_env)
before the DLL loads; nothing is written to the registry:
  - `--aa-lfb-read --i-am-at-the-box`: RETRO_GLIDE_AA_LFB_READ=1, which lets
    our Glide READ the LFB in a multi-chip AA mode - the read that froze .124
    on cfg 3 (2026-09-26). Refused without --i-am-at-the-box, here (before
    anything connects) and by glidelab.exe itself; without the flag
    glidelab.exe sets an inherited value to 0, so nothing left on the box can
    opt a run in. The plan prints a banner, and the RESULT says
    "aa_lfb_read":1.
  - `--maplog`: RETRO_GLIDE_MAPLOG=<step log>.maplog - our Glide appends every
    mapping it dereferences (base0, base1, the slaves' registers, with what
    VirtualQuery says). Deleted before the session, DOWNLOADed after it (after
    any cleanup) and saved beside the step log; a missing one, or one with no
    "pid=" line, is a "maplog_error" that fails the run.
"""
import argparse
import asyncio
import hashlib
import json
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
KMD = HERE.parent
REPO = KMD.parents[1]
sys.path.insert(0, str(REPO / "scripts" / "benchmarks"))
sys.path.insert(0, str(HERE))
import v56k_bench as vb  # noqa: E402
import mode_sweep as ms  # noqa: E402  (the monitor gate + cleanup every switching tool shares)

DIR = r"C:\vcr\glidelab"
OUR_GLIDE = r"C:\Games\Quake2Complete\glide3x.dll"
VCRCTL = r"C:\vcr\vcrctl.exe"
IMAGES = ("glidelab.exe",)
# Every switch re-syncs .124's CRT (2026-09-26: a sweep did ~250 at two a
# second and the user heard it), and `cycle` at its old default of 10 was 20
# re-syncs ~0.3 s apart. Refused here rather than left to glidelab.exe's own
# silent-in-the-exit-code cap, so a caller asking for more learns it did not
# get it before anything runs.
MAX_CYCLES = 3
# glidelab.c's HZ[] table: the refreshes it has a GR_REFRESH code for. Any
# other --refresh falls through to GR_REFRESH_60Hz without a word, so the
# session would open a mode the host gate never checked.
GLIDE_HZ = (60, 70, 72, 75, 85, 100, 120)
# glidelab opens at 16 bpp (GR_COLORFORMAT_ARGB, a 16-bit colour buffer): the
# mode the gate checks
GLIDE_BPP = 16
# glidelab.c --trace: 0 off, 1 steps, 2 steps + a sync after each FIFO step
# (GLIDELAB_MAX_TRACE); the fork's own FX_GLIDE_TRACE levels
TRACE_LEVELS = (0, 1, 2)
# the first line a traced Glide writes into the file (fork minihwc.c hwcTraceOpen)
TRACE_HEADER = "=== glidetrace"
# where the fork writes when FX_GLIDE_TRACE_FILE is not given (HWC_TRACE_PATH)
DEFAULT_TRACE = rf"{DIR}\glidetrace.log"
# SSTH3_SLI_AA_CONFIGURATION values that turn anti-aliasing on - glidelab.c's
# aa_config(), i.e. Glide's own table (h5SliAaConfigEnv)
AA_CFGS = frozenset({1, 3, 4, 6, 7, 8})
# what --restore-cfg may write: the configurations without AA
SAFE_CFGS = (0, 2, 5)
# GR_EXTENSION tokens of our fork (glide3/src/diget.c NAPALM_EXT_STR), found as
# bytes in the DLL: a Glide without the guard froze .124 on cfg 1
GUARD_MARKER = b"RETRO3DFX_SLIAA_GUARD"
TRACE_MARKER = b"RETRO3DFX_AA_TRACE"
AA_VALUE = "SSTH3_SLI_AA_CONFIGURATION"
# where our Glide reads settings on XP (fork minihwc.c getRegPath: 3dfxvs when
# Services\3dfxvs\Device0 exists, else banshee), HKCU before HKLM - besides the
# display-class key v56k_bench.apply_aa_config (and --cfg) writes
GLIDE_SERVICE_KEYS = (r"SYSTEM\CurrentControlSet\Services\3dfxvs\Device0\glide",
                      r"SYSTEM\CurrentControlSet\Services\banshee\Device0\glide")


def is_aa(value):
    """True when a registry string is an AA configuration."""
    try:
        return int(str(value).strip()) in AA_CFGS
    except (TypeError, ValueError):
        return False


async def glide_identity(box, path):
    """The Glide DLL on the box: size, md5 and whether it carries the SLI/AA
    guard and the trace (their GR_EXTENSION tokens, found in its bytes).
    DOWNLOAD only. "error" when it could not be read."""
    data, why = await fetch(box, path)
    if data is None:
        return {"path": path, "error": why or "absent", "guard": False, "trace": False}
    return {"path": path, "bytes": len(data), "md5": hashlib.md5(data).hexdigest(),
            "guard": GUARD_MARKER in data, "trace": TRACE_MARKER in data}


def describe_glide(g):
    if g.get("error"):
        return f"glide {g['path']}: NOT READ ({g['error']})"
    return (f"glide {g['path']} md5 {g['md5']} ({g['bytes']} B): "
            f"SLIAA-GUARD {'yes' if g['guard'] else 'NO'}, AA-TRACE {'yes' if g['trace'] else 'no'}")


async def read_aa_value(box, root, key):
    """(value | None, error | None): SSTH3_SLI_AA_CONFIGURATION under root\\key.
    A key the agent cannot open is absent (None, None); a box that does not
    answer is an error."""
    try:
        out = await box.text(f"REGREAD {root} {key}")
    except vb.RetroProtocolError:
        return None, None
    except Exception as e:       # a dead box, a dropped connection
        return None, f"not read: {type(e).__name__}: {e}"
    try:
        values = json.loads(out).get("values", [])
    except (json.JSONDecodeError, AttributeError):
        return None, f"unreadable REGREAD answer: {out[:80]!r}"
    for v in values:
        if str(v.get("name", "")).upper() == AA_VALUE:
            return str(v.get("data", "")).strip(), None
    return None, None


async def aa_registry(box):
    """Every place a Glide on the box takes SSTH3_SLI_AA_CONFIGURATION from:
    the display-class key glidelab_run --cfg / v56k_bench write, and our
    Glide's own keys. -> [{root, key, value, aa[, error]}]"""
    keys = []
    try:
        inst, _ = await vb.find_display_instance(box)
        keys.append(("HKLM", vb.GLIDE_KEY_TMPL.format(inst=inst)))
    except Exception as e:
        rows = [{"root": "HKLM", "key": "(display class)", "value": None, "aa": False,
                 "error": f"HWPROFILE: {type(e).__name__}: {e}"}]
    else:
        rows = []
    for k in GLIDE_SERVICE_KEYS:
        keys += [("HKCU", k), ("HKLM", k)]
    for root, key in keys:
        value, err = await read_aa_value(box, root, key)
        row = {"root": root, "key": key, "value": value, "aa": is_aa(value)}
        if err:
            row["error"] = err
        rows.append(row)
    return rows


async def write_aa_value(box, root, key, cfg):
    """Write cfg and read it back (never trust the OK). -> {..., ok}"""
    rep = {"root": root, "key": key, "wrote": str(cfg)}
    try:
        await box.text(f"REGWRITE {root} {key} {AA_VALUE} REG_SZ {cfg}")
    except Exception as e:
        rep.update(ok=False, error=f"REGWRITE: {type(e).__name__}: {e}")
        return rep
    got, err = await read_aa_value(box, root, key)
    rep.update(readback=got, ok=(got == str(cfg)))
    if err:
        rep["error"] = err
    return rep


def restore_value(before, fallback):
    """What to put back after an AA --cfg run: the value that was there when
    it was not AA, else the fallback (a configuration without AA)."""
    if before is not None and str(before).strip() and not is_aa(before):
        try:
            int(str(before).strip())
            return str(before).strip()
        except ValueError:
            pass
    return str(fallback)


def trace_path(log):
    """The file glidelab --trace has Glide write: its own log's path plus
    ".trace" (glidelab.c: "%s.trace", g_logpath)."""
    return log + ".trace"


def maplog_path(log):
    """The file glidelab --maplog has our Glide append its mappings to: the
    step log's path plus ".maplog"."""
    return log + ".maplog"


def maplog_summary(remote, data):
    """What a downloaded mapping log says: its size, the mapping passes it
    records (fork minihwc.c hwcLogMappings: one "pid=N chips=.." line each)
    and its last line. "error" when there is none."""
    if data is None:
        return {"path": remote, "error": "no mapping log - the glide3x is not one that reads "
                "RETRO_GLIDE_MAPLOG, or it never mapped the board"}
    text = data.decode("latin1", "replace")
    lines = [ln for ln in text.splitlines() if ln.strip()]
    out = {"path": remote, "bytes": len(data), "lines": len(lines),
           # a mapping pass is a "pid=N chips=.." line; the fork's fatal-error
           # path writes "pid=N GLIDE FATAL: .." into the same file
           "passes": sum(1 for ln in lines if ln.startswith("pid=") and " GLIDE FATAL" not in ln),
           "fatal": [ln for ln in lines if " GLIDE FATAL" in ln],
           "last": lines[-1] if lines else None}
    if not out["passes"]:
        out["error"] = "mapping log with no 'pid=' line - not written by our glide3x"
    return out


def aa_lfb_read_refusal(a):
    """None, or why --aa-lfb-read may not run: only with someone at the box."""
    if getattr(a, "aa_lfb_read", False) and not getattr(a, "i_am_at_the_box", False):
        return ("--aa-lfb-read REFUSED without --i-am-at-the-box: RETRO_GLIDE_AA_LFB_READ=1 "
                "lets our Glide READ the LFB in a multi-chip AA mode - the read that froze "
                ".124 on cfg 3 (2026-09-26). Only with someone at the box")
    return None


def trace_summary(remote, data):
    """What a downloaded trace says: present, has the header, how many
    steps, and the LAST one - after a wedge, the step that froze. "error"
    when there is no usable trace."""
    if data is None:
        return {"path": remote, "error": "no trace file - the glide3x is not a traced build, "
                "the trace could not be opened, or it never reached a trace point"}
    text = data.decode("latin1", "replace")
    lines = [ln for ln in text.splitlines() if ln.strip()]
    out = {"path": remote, "bytes": len(data), "lines": len(lines),
           "header": any(ln.startswith(TRACE_HEADER) for ln in lines),
           "last": lines[-1] if lines else None}
    if not out["header"]:
        out["error"] = (f"trace has no '{TRACE_HEADER}' header - not written by a traced "
                        "glide3x (or truncated before its first line)")
    return out


def save_dir(a):
    """Where downloaded logs and traces are kept: in the repo tree, never a
    scratchpad (they are the only record of a wedge)."""
    d = Path(a.save_dir) if getattr(a, "save_dir", None) else KMD / "evidence" / "glidelab" / "trace"
    d.mkdir(parents=True, exist_ok=True)
    return d


def save(a, mode, kind, data):
    """data saved as <stamp>_<host>_<mode>.<kind>; the local path."""
    f = save_dir(a) / f"{time.strftime('%Y%m%d_%H%M%S')}_{a.host}_{mode}.{kind}"
    f.write_bytes(data)
    return str(f)


async def fetch(box, remote):
    """(bytes | None, why-not-read | None): DOWNLOAD, never EXEC type."""
    try:
        return await box.download(remote), None
    except Exception as e:   # a dead box, a dropped connection
        return None, f"not read: {type(e).__name__}: {e}"


def glide_modes(res_list, refresh):
    """The modes Glide sessions at these resolutions open, as the gate and
    modecalc name them."""
    return [f"{r}x{GLIDE_BPP}@{refresh}" for r in dict.fromkeys(res_list)]


def glide_switches(mode, cycles):
    """Paced switches one glidelab session makes: `cycle` opens and closes
    `cycles` times; the rest open once and close once - `abandon` through XP
    at its exit, which the lab's exit hold paces the same way."""
    return 2 * max(1, min(cycles, MAX_CYCLES)) if mode == "cycle" else 2


def session_budget(mode, cycles, work_s):
    """The EXECW for one session: the work plus the pace gate's worst case
    for every open and close (glidelab paces at its own floor)."""
    return ms.execw_budget(glide_switches(mode, cycles), work_s)


def bad_refresh(refresh):
    """None, or why glidelab.exe cannot be asked for this refresh."""
    if refresh in GLIDE_HZ:
        return None
    return (f"--refresh {refresh} REFUSED: glidelab.c has a GR_REFRESH code only for "
            f"{', '.join(map(str, GLIDE_HZ))} Hz and would open any other at 60 Hz, silently")


async def run_mode(box, a, mode):
    """One glidelab session. -> its RESULT, with "error" on any failure;
    "timed_out" when the EXECW timed out or never answered, "wedged" when a
    survivor could not be confirmed gone - both mean: no further session."""
    tool = getattr(a, "tool", VCRCTL)
    pace = max(getattr(a, "pace", 5.0), ms.PACE_FLOOR_S)
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
    level = getattr(a, "trace", 0) or 0
    trace = trace_path(log) if level else None
    if trace:
        args += ["--trace", str(level)]
        if getattr(a, "trace_cfg", False):
            args.append("--trace-cfg")
    why = aa_lfb_read_refusal(a)
    if why:
        # the gate for a caller that built its own namespace: nothing launched
        return {"mode": mode, "error": why, "refused": True}
    if getattr(a, "aa_lfb_read", False):
        # glidelab.exe refuses the first without the second as well
        args += ["--aa-lfb-read", "--i-am-at-the-box"]
    maplog = maplog_path(log) if getattr(a, "maplog", False) else None
    if maplog:
        args += ["--maplog", maplog]
    cmd =(rf'cmd /c start "glidelab" /wait "{DIR}\glidelab.exe" ' + " ".join(args))
    budget = session_budget(mode, a.cycles, a.timeout)
    await box.exec_(rf'cmd /c del /f /q "{log}"')
    if trace:
        # Glide appends: a previous session's trace must not read as this one's
        await box.exec_(rf'cmd /c del /f /q "{trace}"')
    if maplog:
        await box.exec_(rf'cmd /c del /f /q "{maplog}"')     # appended too
    # a glidelab.exe already running is not this session's: a cleanup never
    # kills it (None - PROCLIST did not answer - kills nothing, and says so).
    # Kept on `a` too, for a caller that must clean up after this raised.
    spare = a.spare = await ms.pids_of(box.cmd, IMAGES)
    trouble = None
    try:
        reply = await box.text(f"EXECW {budget} {cmd}", budget + ms.HOST_SLACK_S)
        if ms.EXECW_TIMED_OUT in reply:
            trouble = (f"the agent's EXECW timed out after {budget} s and tried to kill the "
                       "tree (best effort - glidelab may still hold the board and the mode)")
    except Exception as e:  # the host gave up, the connection dropped, EXECW refused
        trouble = f"EXECW did not answer: {type(e).__name__}: {e}"
    log_bytes = None
    try:
        data = log_bytes = await box.download(log)
    except Exception as e:
        data = f"(log not read: {type(e).__name__}: {e})".encode()
    text = (data or b"").decode("latin1", "replace")
    res = None
    for ln in text.splitlines():
        if ln.startswith("RESULT "):
            try:
                res = json.loads(ln[7:])
            except json.JSONDecodeError:
                # a garbled RESULT is not a pass
                res = {"mode": mode, "error": "unparsable RESULT line", "raw": ln}
    if res is None:
        res = {"mode": mode, "error": "no RESULT line", "log_tail": text.splitlines()[-5:]}
    if trouble:
        res["timed_out"] = True
        res["error"] = trouble + (f" (and: {res['error']})" if res.get("error") else "")
    halt = ms.lab_halt(res, text, want_hz=a.refresh)
    if halt:
        # the screen or the pacing was not this session's alone, or Glide
        # opened a mode the gate never checked: a failure, and the end of
        # the run - no --then, and glidelab_sweep stops as on a wedge
        res["halt"] = halt
        res["error"] = halt + (f" (and: {res['error']})" if res.get("error") else "")
    if "error" in res:
        # a session that failed may have left glidelab running, or died
        # without its exit hold (a crash, a kill): nothing else switches until
        # it is gone and the box's pace file says "just now"
        rep = await ms.reap(box.cmd, IMAGES, tool, pace, spare=spare)
        res["cleanup"] = rep
        if not rep["gone"]:
            res["wedged"] = True
            ms.banner("GLIDELAB STILL RUNNING", f"a survivor of '{mode}' could not be confirmed "
                      f"gone ({rep.get('left') or rep.get('error')}) - no further session")
    if trace:
        # after the cleanup: a session that outlived its EXECW may still have
        # been appending to it. Downloading switches nothing.
        # "trace_file": glidelab's own RESULT already says "trace": <level>
        res["trace_file"] = await collect_trace(box, a, mode, trace, log_bytes)
        if res["trace_file"].get("error"):
            res["trace_error"] = res["trace_file"]["error"]
            ms.banner("NO GLIDE TRACE", f"--trace {level} was asked and {trace}: "
                      f"{res['trace_error']}")
    if maplog:
        # after the cleanup too; downloading switches nothing
        data, why_not = await fetch(box, maplog)
        info = maplog_summary(maplog, data)
        if why_not:
            info["error"] = why_not
        if data:
            info["saved"] = save(a, mode, "maplog", data)
        res["maplog_file"] = info
        if info.get("error"):
            res["maplog_error"] = info["error"]
            ms.banner("NO GLIDE MAPPING LOG", f"--maplog was asked and {maplog}: "
                      f"{res['maplog_error']}")
    return res


async def collect_trace(box, a, mode, trace, log_data):
    """DOWNLOAD the session's trace and keep it, with the step log, in the
    repo tree. -> trace_summary() plus where it was saved."""
    data, why = await fetch(box, trace)
    out = trace_summary(trace, data)
    if why:
        out["error"] = why
    if data:
        out["saved"] = save(a, mode, "trace", data)
    if log_data:
        out["log_saved"] = save(a, mode, "log", log_data)
    return out


async def collect(box, a):
    """--collect: after a wedge and a power cycle, DOWNLOAD what the last
    session left on the box - the mode's step log, its trace and the fork's
    default trace file - save them and print what they end with, and READ the
    SLI/AA configuration every Glide key holds (a banner when one is AA: the
    next Glide app opens it). Nothing is uploaded, launched or switched, so no
    gate is needed; the registry is written only with --restore-cfg N, and
    only where an AA value was found."""
    log = rf"{DIR}\{a.mode}.log"
    out = {"mode": a.mode, "host": a.host, "collect": True}
    for key, remote in (("log", log), ("trace", trace_path(log)),
                        ("default_trace", DEFAULT_TRACE)):
        data, why = await fetch(box, remote)
        if why or data is None:
            out[key] = {"path": remote, "error": why or "absent"}
            continue
        if key == "log":
            text = data.decode("latin1", "replace")
            lines = [ln for ln in text.splitlines() if ln.strip()]
            info = {"path": remote, "bytes": len(data), "last": lines[-1] if lines else None}
            results = [ln for ln in lines if ln.startswith("RESULT ")]
            info["result"] = results[-1][7:] if results else None
        else:
            info = trace_summary(remote, data)
        info["saved"] = save(a, a.mode, "log" if key == "log" else key.replace("default_", "default."),
                             data)
        out[key] = info
    # the configuration the box will open next: a wedge's AA value survives
    # the power cycle in the registry (v56k_bench.apply_aa_config: REGWRITE)
    out["registry"] = rows = await aa_registry(box)
    armed = [r for r in rows if r["aa"]]
    restore = getattr(a, "restore_cfg", None)
    if armed and restore is not None:
        out["restored"] = [await write_aa_value(box, r["root"], r["key"], restore) for r in armed]
    print(json.dumps(out), flush=True)
    rc = 0
    if "error" in out["log"]:
        ms.banner("NOTHING COLLECTED", f"{log}: {out['log']['error']}")
        rc = 1
    if armed:
        where = "; ".join(f"{r['root']}\\{r['key']} = {r['value']}" for r in armed)
        if restore is None:
            ms.banner("AA CONFIGURATION ARMED",
                      f"{where} - the next Glide application opens it. Before any Glide app "
                      f"runs: glidelab_run.py {a.host} {a.mode} --collect --restore-cfg 5")
            rc = 1
        elif not all(r.get("ok") for r in out["restored"]):
            ms.banner("AA CONFIGURATION STILL ARMED",
                      f"{where}: --restore-cfg {restore} did not read back - "
                      + json.dumps(out["restored"]))
            rc = 1
    unread = [r for r in rows if r.get("error")]
    if unread:
        ms.banner("REGISTRY NOT READ", "; ".join(f"{r['root']}\\{r['key']}: {r['error']}"
                                                for r in unread))
        rc = 1
    return rc


async def put_back_aa(box, a, key, before):
    """After an AA --cfg session: the display-class value back to what it was
    (when that was not AA) or to --restore-cfg (default 5), read back. A box
    that cannot be reached (a wedge) gets a banner naming the command for
    after the power cycle. -> the write report"""
    fallback = getattr(a, "restore_cfg", None)
    target = restore_value(before, 5 if fallback is None else fallback)
    rep = await write_aa_value(box, "HKLM", key, target)
    rep["before"] = before
    if not rep.get("ok"):
        ms.banner("AA CONFIGURATION LEFT ARMED",
                  f"HKLM\\{key} {AA_VALUE} = {a.cfg} could not be put back to {target} "
                  f"({rep.get('error') or rep.get('readback')}). After the power cycle, before "
                  f"any Glide app runs: glidelab_run.py {a.host} {a.mode} --collect "
                  f"--restore-cfg {target}")
    return rep


async def gate(box, a, modes):
    """(ranges, calc, None) when every mode may be opened, else (.., why):
    mode_sweep's gate on the live `vcrctl info`, or on --monitor-info once
    the box's registry confirms its monitor."""
    return await ms.gate_on_box(box.cmd, a.tool, modes, a.monitor_info,
                                getattr(a, "i_have_checked_the_monitor", False))


async def main_async(a):
    box = vb.Box(a.host)
    if getattr(a, "collect", False):
        return await collect(box, a)
    why = aa_lfb_read_refusal(a)
    if why:
        ms.refuse(why)
        return 2
    a.pace = max(a.pace, ms.PACE_FLOOR_S)
    modes = glide_modes([a.res], a.refresh)      # --then opens the same mode
    runs = [a.mode] + ([a.then] if a.then else [])
    for mode in runs:
        why = ms.budget_refusal(session_budget(mode, a.cycles, a.timeout),
                                f"a glidelab {mode} session with --timeout {a.timeout}")
        if why:
            ms.refuse(why)
            return 2
    rng, calc, why = await gate(box, a, modes)
    if why:
        ms.refuse(why)
        return 2
    # the Glide that will be loaded, by md5 and by what it carries: the fork's
    # guard commits were local-only, so a DLL built elsewhere has none of them
    glide = await glide_identity(box, a.glide)
    print(f"plan: {len(runs)} glidelab run(s) at {modes[0]}  {ms.rates(calc, modes[0])}"
          f"\n  monitor {ms.describe(rng)}\n  {describe_glide(glide)}", flush=True)
    if getattr(a, "aa_lfb_read", False):
        ms.banner("AA LFB READS ALLOWED", "this run passes RETRO_GLIDE_AA_LFB_READ=1 to our "
                  "Glide (process environment only): a READ lock in a multi-chip AA mode goes "
                  "through - the read that froze .124 on cfg 3. --i-am-at-the-box was given")
    if a.cfg in AA_CFGS and not glide["guard"]:
        ms.refuse(f"--cfg {a.cfg} is an AA configuration and {a.glide} "
                  + (f"could not be read to confirm RETRO3DFX_SLIAA_GUARD ({glide['error']})"
                     if glide.get("error") else
                     f"(md5 {glide['md5']}) has no RETRO3DFX_SLIAA_GUARD")
                  + " - a Glide without the SLI/AA guards froze .124 on cfg 1. Stage a guarded "
                    "build (fork 631221b or later) first")
        return 2
    await box.text(rf"MKDIR {DIR}")
    await box.upload(rf"{DIR}\glidelab.exe", (KMD / "out" / "glidelab.exe").read_bytes())
    put_back = None
    if a.cfg is not None:
        inst, _ = await vb.find_display_instance(box)
        key = vb.GLIDE_KEY_TMPL.format(inst=inst)
        # an AA value written here survives every reboot: note what was there,
        # to put it back after the session (put_back_aa)
        before, _ = await read_aa_value(box, "HKLM", key) if a.cfg in AA_CFGS else (None, None)
        await vb.apply_aa_config(box, key, a.cfg)
        if a.cfg in AA_CFGS:
            put_back = (key, before)
    try:
        out = await run_sessions(box, a, runs)
    finally:
        if put_back:
            rep = await put_back_aa(box, a, *put_back)
            print(json.dumps({"host": a.host, "aa_restore": rep}), flush=True)
    if put_back and not rep.get("ok"):
        return 1
    # a trace that was asked for and not produced fails the run too: the
    # diagnosis it was for did not happen
    return 0 if all("error" not in r and "trace_error" not in r and "maplog_error" not in r
                    for r in out) else 1


async def run_sessions(box, a, runs):
    """The glidelab session(s) of one run, in order; each RESULT printed as it
    comes. -> the RESULTs"""
    out = []
    for i, mode in enumerate(runs):
        if i:
            if out[-1].get("wedged") or out[-1].get("halt"):
                r = {"mode": mode, "error": "not run: the previous session's glidelab could not "
                     "be confirmed gone" if out[-1].get("wedged") else
                     f"not run: the previous session ended the run ({out[-1]['halt']})"}
                r["host"] = a.host
                print(json.dumps(r))
                out.append(r)
                break
            # a floor between the sessions on the host as well: glidelab
            # paces its own opens, but a failed first run's exit may not have
            await asyncio.sleep(a.pace)
        r = await run_mode(box, a, mode)
        r["host"] = a.host
        print(json.dumps(r))
        out.append(r)
    if a.json_out:
        with open(a.json_out, "a") as f:
            for r in out:
                f.write(json.dumps(r) + "\n")
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("host")
    ap.add_argument("mode", choices=("fill", "bands", "texmem", "edges", "cycle", "abandon"))
    ap.add_argument("--res", default="640x480")
    ap.add_argument("--refresh", type=int, default=60,
                    help=f"one of {', '.join(map(str, GLIDE_HZ))} (glidelab.c's table)")
    ap.add_argument("--cfg", type=int)
    ap.add_argument("--frames", type=int, default=200)
    ap.add_argument("--layers", type=int, default=4)
    ap.add_argument("--cycles", type=int, default=2)
    ap.add_argument("--blend", action="store_true")
    ap.add_argument("--origin", choices=("upper", "lower"))
    ap.add_argument("--glide", default=OUR_GLIDE)
    ap.add_argument("--then", choices=("fill", "bands", "texmem", "cycle"))
    ap.add_argument("--timeout", type=int, default=180,
                    help="seconds of WORK per session; the EXECW adds the pace gate's worst case "
                         "for every open and close")
    ap.add_argument("--pace", type=float, default=5.0,
                    help="seconds between sessions and after a cleanup (min 3)")
    ap.add_argument("--monitor-info", help="a `vcrctl info` saved from OUR driver on this box: "
                    "the monitor's EDID ranges when the installed driver is not ours (used "
                    "only when the registry says that monitor is the one on the box)")
    ap.add_argument(ms.CHECKED_FLAG, action="store_true",
                    help="with --monitor-info: go on when the box's registry cannot confirm the "
                         "monitor - only after looking at it (a different one is still refused)")
    ap.add_argument("--tool", default=VCRCTL,
                    help="vcrctl.exe on the box (info, modes, pace-kill, pace-mark)")
    ap.add_argument("--json-out")
    ap.add_argument("--trace", type=int, choices=TRACE_LEVELS, default=0,
                    help="our h5 Glide's step trace (FX_GLIDE_TRACE): 1 steps, 2 steps plus a "
                         "sync after each FIFO step of the open; saved with the step log")
    ap.add_argument("--trace-cfg", action="store_true",
                    help="with --trace: also the fork's PCI config-space dumps "
                         "(FX_GLIDE_TRACE_CFG=1 - PCI_OP escapes of their own)")
    ap.add_argument("--collect", action="store_true",
                    help="only DOWNLOAD the MODE's step log and trace left by the last session "
                         "(after a wedge and power cycle) and READ the Glide SLI/AA registry "
                         "values - nothing is run or switched")
    ap.add_argument("--restore-cfg", type=int, choices=SAFE_CFGS,
                    help="with --collect: write this where an AA SSTH3_SLI_AA_CONFIGURATION was "
                         "found, and read it back; for a run with an AA --cfg: what to put back "
                         "when the value before it was absent or AA (default 5)")
    ap.add_argument("--aa-lfb-read", action="store_true",
                    help="RETRO_GLIDE_AA_LFB_READ=1 in glidelab's environment: our Glide lets a "
                         "READ lock through in a multi-chip AA mode (it froze .124 on cfg 3). "
                         "Only with --i-am-at-the-box")
    ap.add_argument("--i-am-at-the-box", action="store_true",
                    help="the confirmation --aa-lfb-read needs: someone is at the box to "
                         "power-cycle it")
    ap.add_argument("--maplog", action="store_true",
                    help="RETRO_GLIDE_MAPLOG=<step log>.maplog: our Glide's mapping log, "
                         "brought home and saved beside the step log")
    ap.add_argument("--save-dir", help="where downloaded logs/traces are kept "
                    "(default: evidence/glidelab/trace/)")
    a = ap.parse_args()
    if a.cycles > MAX_CYCLES:
        ap.error(f"--cycles {a.cycles} REFUSED: at most {MAX_CYCLES} (each open and each "
                 "close re-syncs the monitor)")
    if a.trace_cfg and not a.trace:
        ap.error("--trace-cfg REFUSED without --trace 1 or 2 (the dumps are part of a trace)")
    why = bad_refresh(a.refresh) or aa_lfb_read_refusal(a)
    if why:
        ap.error(why)
    sys.exit(asyncio.run(main_async(a)))


if __name__ == "__main__":
    main()
