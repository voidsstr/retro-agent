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
    cmd = (rf'cmd /c start "glidelab" /wait "{DIR}\glidelab.exe" ' + " ".join(args))
    budget = session_budget(mode, a.cycles, a.timeout)
    await box.exec_(rf'cmd /c del /f /q "{log}"')
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
    try:
        data = await box.download(log)
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
    return res


async def gate(box, a, modes):
    """(ranges, calc, None) when every mode may be opened, else (.., why):
    mode_sweep's gate on the live `vcrctl info`, or on --monitor-info once
    the box's registry confirms its monitor."""
    return await ms.gate_on_box(box.cmd, a.tool, modes, a.monitor_info,
                                getattr(a, "i_have_checked_the_monitor", False))


async def main_async(a):
    box = vb.Box(a.host)
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
    print(f"plan: {len(runs)} glidelab run(s) at {modes[0]}  {ms.rates(calc, modes[0])}"
          f"\n  monitor {ms.describe(rng)}", flush=True)
    await box.text(rf"MKDIR {DIR}")
    await box.upload(rf"{DIR}\glidelab.exe", (KMD / "out" / "glidelab.exe").read_bytes())
    if a.cfg is not None:
        inst, _ = await vb.find_display_instance(box)
        await vb.apply_aa_config(box, vb.GLIDE_KEY_TMPL.format(inst=inst), a.cfg)
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
    return 0 if all("error" not in r for r in out) else 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("host")
    ap.add_argument("mode", choices=("fill", "bands", "cycle", "abandon"))
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
    ap.add_argument("--then", choices=("fill", "bands", "cycle"))
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
    a = ap.parse_args()
    if a.cycles > MAX_CYCLES:
        ap.error(f"--cycles {a.cycles} REFUSED: at most {MAX_CYCLES} (each open and each "
                 "close re-syncs the monitor)")
    why = bad_refresh(a.refresh)
    if why:
        ap.error(why)
    sys.exit(asyncio.run(main_async(a)))


if __name__ == "__main__":
    main()
