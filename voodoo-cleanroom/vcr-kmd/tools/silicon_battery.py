#!/usr/bin/env python3
"""silicon_battery.py - every vcr-kmd check that can run on a real box, in one
go, after `deploy_box.py install`:

    silicon_battery.py 192.168.1.124 --label vcrkmd-<date> [--golden <vendor golden>]
                       [--vendor-cursor amigamerlin-3.1-r11] [--skip modes,d3d]

Steps (each a subprocess of the existing tool, so each can also run alone):
  info       vcrctl info (the driver is ours and bound, it read the monitor's
             EDID and builds its mode list inside it). The monitor gate:
             it always runs, and the battery stops here (rc 3) otherwise
  modes      mode_sweep.py (--golden: the vendor's modes + register check)
  cursor     cursor_golden.py --compare (the hardware cursor vs the vendor's)
  gdi        gdilab (2D engine: fills, copies, scrolls, clip, engine/CPU order)
  ddraw      ddlab caps/flip/blt at 16 and 32 bpp
  d3d        d3dprobe caps; render fullscreen 16 bpp at 640x480 and 1024x768
  d3dperf    d3dprobe perf fullscreen, no vsync

Every step's JSON (or its tail) goes to evidence/silicon/<label>.jsonl as
{"step", "rc", "result"}. Between steps the agent is PINGed; if it stopped
answering, the battery stops there and says which step it was - the flight
recorder (vcrphases.py --prev after a power cycle) says the rest.

THE MONITOR. Every mode switch is a re-sync of a 1998 CRT - and TWO timing
changes on the cable, since our driver passes through its 31.5 kHz VGA reset
on every mode change (old -> VGA -> new, ~30 ms apart). The battery builds
its whole schedule first, prints the number of switches AND of timing changes
from that same schedule, and waits PACE seconds before every fullscreen run;
the modes step visits LIVE_MODES only (the register check of all 123 vendor
modes is tools/golden_compare.py, on the dev host, with no monitor).

A failure in a fullscreen-driving step (ddraw, d3d, d3dperf) ends EVERY
fullscreen run after it, for the rest of the battery: a driver that just
failed an exclusive-mode set is not driven through the rest. So does a lab
that reports it lost the focus while it held a mode ("focus_lost") or found
the box's switch lock busy ('pace lock busy' - something else is switching),
in any step. A sub-run that TIMED OUT (the battery's own timeout, rc -9, or
the agent's EXECW-timeout marker in its output) is cleaned up before
anything else runs - PROCLIST for ddlab.exe / d3dprobe.exe / vcrctl.exe, and
only a survivor that was NOT running before that sub-run started (the
battery lists them first; with no list it kills nothing) is killed, by PID,
through `vcrctl pace-kill`; then the stamp and the pace - and ends every
later switching run the same way. Each sub-run's timeout covers the runner's
EXECW (the pace gate's worst case per switch included) and a whole cleanup
after it, so the battery never cuts a runner off on its own way out. The
first version of this battery ran every vendor mode with a return to the
desktop after each, ~250 re-syncs at two a second (.124, 2026-09-26); it was
stopped at ~126.
"""
import argparse
import asyncio
import json
import re
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
KMD = HERE.parent
sys.path.insert(0, str(KMD.parents[1]))
sys.path.insert(0, str(HERE))
from client.retro_protocol import RESP_ERROR, RetroConnection  # noqa: E402
import mode_sweep  # noqa: E402  (the monitor gate every switching tool shares)

STEPS = ("info", "modes", "cursor", "gdi", "ddraw", "d3d", "d3dperf")

# The modes games use, ordered by horizontal frequency so the monitor walks its
# bands one way (31.5 -> 87.5 kHz; the desktop's 1280x1024@85 is 91 kHz).
LIVE_MODES = ("640x480x8@60", "640x480x16@85", "800x600x16@85", "1024x768x16@85",
              "1024x768x32@100", "1600x1200x16@70", "1600x1200x32@70")
PACE = 5.0                       # seconds of rest before every mode switch
VCRCTL = r"C:\vcr\vcrctl.exe"
# the steps whose sub-runs take the screen exclusively: a failure in any of
# them latches the battery off fullscreen for good
FULLSCREEN_STEPS = ("ddraw", "d3d", "d3dperf")
# what a timed-out sub-run may leave running on the box, holding a mode
ORPHANS = ("ddlab.exe", "d3dprobe.exe", "vcrctl.exe")
# the work budget each lab runner is given (its --timeout): its EXECW adds the
# pace gate's worst case per switch on top
LAB_WORK_S = {"ddlab_run": 120, "d3dprobe_run": 180, "lab_run": 240}
# One civil cleanup after a runner, at worst: a pace-kill of a survivor (its
# EXECW and the host's slack), the bounded confirm, PROCLISTs, a pace-mark
# (EXEC, up to 90 s), the pace
CLEANUP_S = (mode_sweep.PACE_KILL_EXECW_S + mode_sweep.HOST_SLACK_S + 10 + 30 + 90 + PACE)


def runner_timeout(execw_s, floor_s, extra_s=0):
    """How long the battery lets a sub-run take before it SIGTERMs it: the
    runner's EXECW, the host's slack on it, whatever else the runner does
    after (`extra_s`) and a whole cleanup - the battery must not cut a runner
    off in the middle of its own way out, which is exactly when a lab may
    still hold a mode. Never under the step's old floor."""
    return max(floor_s, int(execw_s + mode_sweep.HOST_SLACK_S + extra_s + CLEANUP_S))


def lab_timeout(runner, switches):
    return runner_timeout(mode_sweep.execw_budget(switches, LAB_WORK_S[runner]), 300)


def log(m):
    print(f"[{time.strftime('%H:%M:%S')}] {m}", flush=True)


async def ping(host, port):
    try:
        c = RetroConnection(host, port)
        await c.connect("retro-agent-secret", timeout=15)
        st, d = await c.send_command("PING", timeout=15)
        await c.close()
        return d == b"PONG"
    except Exception:
        return False


async def agent_raw(host, port, cmd, timeout=90, payload=None):
    c = RetroConnection(host, port)
    await c.connect("retro-agent-secret", timeout=20)
    try:
        if payload is not None:
            return await c.send_command(cmd, binary_payload=payload, timeout=timeout)
        return await c.send_command(cmd, timeout=timeout)
    finally:
        await c.close()


async def agent(host, port, cmd, timeout=90, payload=None):
    st, d = await agent_raw(host, port, cmd, timeout, payload)
    return d.decode("latin1", "replace")


def _text(b):
    """A timed-out subprocess hands back what it printed so far as BYTES even
    under text=True; decoded, it is the evidence of where the tool hung."""
    if isinstance(b, bytes):
        return b.decode("utf-8", "replace")
    return b or ""


def run(args, timeout):
    t0 = time.time()
    p = subprocess.Popen([sys.executable] + args, cwd=KMD, stdout=subprocess.PIPE,
                         stderr=subprocess.PIPE, text=True)
    try:
        so, se = p.communicate(timeout=timeout)
        rc = p.returncode
    except subprocess.TimeoutExpired as e:
        so, se = _text(e.stdout), _text(e.stderr)
        # SIGTERM first, not SIGKILL: mode_sweep traps it and stops its list
        # on the box (one paced restore) instead of leaving vcrctl switching
        p.terminate()
        try:
            so2, se2 = p.communicate(timeout=120)
        except subprocess.TimeoutExpired:
            p.kill()
            so2, se2 = p.communicate()
        # a retried communicate() returns everything, the partial output too
        so, se = _text(so2) or so, _text(se2) or se
        se += f"\n[battery: timed out after {timeout} s, terminated]"
        rc = -9
    out = _text(so) + _text(se)
    lines = [ln for ln in out.splitlines() if ln.strip()]
    res = []
    for ln in lines:
        if ln.startswith("{"):
            try:
                res.append(json.loads(ln))
            except ValueError:
                pass
    res = res or lines[-6:]
    # the agent's EXECW-timeout marker must survive into the result, wherever
    # it was printed: it is what says the sub-run left the box unfinished
    if mode_sweep.EXECW_TIMED_OUT in out and mode_sweep.EXECW_TIMED_OUT not in json.dumps(res):
        res = list(res) + [ln for ln in lines if mode_sweep.EXECW_TIMED_OUT in ln][:1]
    return rc, res, time.time() - t0


def timed_out(rc, res):
    """The battery killed the sub-run (-9), the agent's EXECW did (its marker,
    which the lab runners keep as "execw"), or a runner's EXECW never
    answered ("timed_out")."""
    return (rc == -9 or mode_sweep.EXECW_TIMED_OUT in json.dumps(res)
            or any(isinstance(r, dict) and r.get("timed_out") for r in res))


def box_call(h, port):
    """mode_sweep's call(cmd, timeout) -> (status, bytes), one connection each."""
    async def call(cmd, timeout):
        return await agent_raw(h, port, cmd, timeout)
    return call


def snapshot(h, port):
    """The ORPHANS already running before a sub-run starts: not that
    sub-run's, so its cleanup never kills them - another session's lab is
    not collateral. None when PROCLIST did not answer: the cleanup then
    kills nothing at all, and says so."""
    try:
        return asyncio.run(mode_sweep.pids_of(box_call(h, port), ORPHANS))
    except Exception:
        return None


def clean_up(h, port, spare):
    """After a timed-out sub-run: its lab or vcrctl may still hold a mode.
    PROCLIST, and each survivor that is not in `spare` - the snapshot taken
    before the sub-run started - killed BY PID through `vcrctl pace-kill`
    (paced, the revert stamped); then the stamp and the pace
    (mode_sweep.reap). -> the report."""
    try:
        return asyncio.run(mode_sweep.reap(box_call(h, port), ORPHANS, VCRCTL, PACE, spare=spare))
    except Exception as e:  # the report says it failed; the latch still holds
        return {"gone": False, "error": f"{type(e).__name__}: {e}"}


def refused_before_switching(res):
    """A runner's pre-launch refusal - {"refused": true} from ddlab_run /
    d3dprobe_run / lab_run's gate - as opposed to a run that went fullscreen
    and failed. Only the second may latch the battery's switching off."""
    return any(isinstance(r, dict) and r.get("refused") is True for r in (res or ()))


def halt_reason(res):
    """Why a sub-run's results end every later switch of the battery, or
    None: a lab lost the focus while it held a mode, or found the box's
    switch lock busy (mode_sweep.lab_halt)."""
    return next((w for w in map(mode_sweep.lab_halt, res or ()) if w), None)


def schedule(a, h, pp):
    """Every sub-run after the info step, in the order it will run. The plan
    printed before the first switch and the loop that runs the steps both
    read THIS list, so the count the operator is shown is the count the
    monitor gets.

    switches: logical mode changes the sub-run makes - a fullscreen run is
    two, in and out; the mode list is one per mode plus its single restore -
    an upper bound, since mode_sweep drops modes the driver does not offer
    and never adds any. Each is TWO timing changes on the cable (the driver's
    VGA reset, then the new mode), which the plan prints as well."""
    # mode_sweep: its one EXECW, then - if the summary is lost - the civil
    # wait for vcrctl, the cleanup and one restore
    sweep_s = runner_timeout(
        mode_sweep.modeseq_budget(len(LIVE_MODES), PACE), 900,
        2 * mode_sweep.switch_s(PACE) + mode_sweep.MODESEQ_STEP_S + mode_sweep.CIVIL_MARGIN_S
        + mode_sweep.RESTORE_EXECW_S + mode_sweep.HOST_SLACK_S)
    lab_t = ["--timeout", str(LAB_WORK_S["lab_run"])]
    d3d_t = ["--timeout", str(LAB_WORK_S["d3dprobe_run"])]
    dd_t = ["--timeout", str(LAB_WORK_S["ddlab_run"])]
    plan = {
        "modes": (["tools/mode_sweep.py", h] + pp + (["--golden", a.golden] if a.golden else [])
                  + ["--modes", ",".join(LIVE_MODES), "--pace", str(PACE)], sweep_s),
        "cursor": (["tools/cursor_golden.py", h, "--label", a.label, "--compare", a.vendor_cursor], 300),
        "gdi": (["tools/lab_run.py", "gdilab", h] + pp + lab_t,
                max(600, lab_timeout("lab_run", 0))),
        "d3dperf": (["tools/d3dprobe_run.py", h] + pp + ["perf", "--full", "--novsync", "--res",
                                                         "640x480", "--frames", "300"] + d3d_t,
                    max(600, lab_timeout("d3dprobe_run", 2))),
    }
    subs = []

    def add(step, args, timeout, switches=0, fullscreen=False):
        subs.append({"step": step, "args": args, "timeout": timeout, "switches": switches,
                     "fullscreen": fullscreen})

    add("modes", *plan["modes"], switches=len(LIVE_MODES) + 1)
    add("cursor", *plan["cursor"])
    add("gdi", *plan["gdi"])
    for bpp in (16, 32):
        for m in ("caps", "flip", "blt"):
            full = m != "caps"          # flip and blt go fullscreen
            add("ddraw", ["tools/ddlab_run.py", h] + pp + [m, "--res", "800x600", "--bpp", str(bpp),
                                                           "--frames", "60"] + dd_t,
                max(400, lab_timeout("ddlab_run", 2 if full else 0)), 2 if full else 0, full)
    add("d3d", ["tools/d3dprobe_run.py", h] + pp + ["caps"] + d3d_t, lab_timeout("d3dprobe_run", 0))
    for res in ("640x480", "1024x768"):
        add("d3d", ["tools/d3dprobe_run.py", h] + pp + ["render", "--full", "--res", res,
                                                        "--bpp", "16"] + d3d_t,
            max(600, lab_timeout("d3dprobe_run", 2)), 2, True)
    add("d3dperf", *plan["d3dperf"], switches=2, fullscreen=True)
    return subs


def blank_edid_serial(text):
    """The info JSON carries the monitor's whole EDID, and its 0xFF display
    descriptor is the unit's serial number: the evidence committed to the
    repo keeps the model, never the serial (the EDID fixtures are blanked the
    same way). The four 18-byte descriptors sit at 54, 72, 90, 108."""
    def blank(hexs):
        b = bytearray.fromhex(hexs)
        for off in (54, 72, 90, 108):
            if len(b) >= off + 18 and b[off:off + 3] == b"\0\0\0" and b[off + 3] == 0xFF:
                b[off + 5:off + 18] = b"\n" + b" " * 12
        return b.hex()
    return re.sub(r'("edid"\s*:\s*")([0-9a-fA-F]{256})"', lambda m: m.group(1) + blank(m.group(2)) + '"',
                  text)


def info_step(h, port, jl):
    """Put THIS build's vcrctl on the box, then the monitor gate. -> rc, or
    0 to go on. Nothing after this step may switch a mode unless both pass."""
    def record(rc, result):
        with open(jl, "a") as f:
            f.write(json.dumps({"step": "info", "rc": rc, "result": result}) + "\n")

    try:
        # the steps below need THIS build's vcrctl (modeseq, paced, with the
        # stop file) - never whatever an older run left. An UPLOAD over a
        # vcrctl that is still running fails, and the old unpaced binary
        # would stay: so read it back rather than trusting the answer.
        exe = (KMD / "out" / "vcrctl.exe").read_bytes()
        asyncio.run(agent(h, port, r"MKDIR C:\vcr"))
        st, d = asyncio.run(agent_raw(h, port, rf"UPLOAD {VCRCTL}", 120, exe))
        _, back = asyncio.run(agent_raw(h, port, rf"DOWNLOAD {VCRCTL}", 120))
        if st == RESP_ERROR or back != exe:
            why = (f"UPLOAD of vcrctl.exe did not land ({d.decode('latin1', 'replace').strip()[:120]}"
                   f"; read back {len(back)} bytes, built {len(exe)})")
            mode_sweep.banner("BATTERY STOPPED before any switch", why)
            record(3, why)
            return 3
        txt = blank_edid_serial(asyncio.run(agent(h, port, rf"EXEC {VCRCTL} info")))
    except Exception as e:
        why = f"info step failed: {type(e).__name__}: {e}"
        mode_sweep.banner("BATTERY STOPPED before any switch", why)
        record(3, why)
        return 3
    rng, why = mode_sweep.monitor_gate(mode_sweep.jline(txt))
    if why:
        mode_sweep.banner("BATTERY STOPPED before any switch", why)
        record(3, [txt.strip()[:1500], why])
        return 3
    log(f"   monitor {mode_sweep.describe(rng)} - the driver filters its modes by it")
    record(0, [txt.strip()[:1500]])
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("host")
    ap.add_argument("--port", type=int, default=9898)
    ap.add_argument("--label", required=True)
    ap.add_argument("--golden", help="mode_sweep --golden capture")
    ap.add_argument("--vendor-cursor", default="amigamerlin-3.1-r11")
    ap.add_argument("--skip", default="", help="comma list of steps to leave out (not info: "
                    "it is the monitor gate)")
    a = ap.parse_args()
    skip = set(filter(None, a.skip.split(",")))
    if "info" in skip:
        log("'info' is the monitor gate and always runs - not skipped")
        skip.discard("info")
    out = KMD / "evidence" / "silicon"
    out.mkdir(parents=True, exist_ok=True)
    jl = out / f"{a.label}.jsonl"
    h, pp = a.host, ["--port", str(a.port)]
    subs = [s for s in schedule(a, h, pp) if s["step"] not in skip]
    todo = [st for st in STEPS if st == "info" or any(s["step"] == st for s in subs)]
    switches = sum(s["switches"] for s in subs)
    log(f"plan: {', '.join(todo)} - at most {switches} mode switches = "
        f"{mode_sweep.TIMING_CHANGES_PER_SWITCH * switches} timing changes on the cable (each "
        f"switch passes through the driver's 31.5 kHz VGA reset, ~30 ms), "
        f"{PACE:.0f}s apart at least")
    bad = skipped = 0
    # battery-wide: once set, no later run that switches modes is started
    stop_switching = None

    def rest():
        time.sleep(PACE)

    for step in todo:
        if not asyncio.run(ping(h, a.port)):
            log(f"AGENT SILENT before '{step}' - stopping; the previous step took it down")
            with open(jl, "a") as f:
                f.write(json.dumps({"step": step, "rc": None, "result": "agent silent before this step"}) + "\n")
            return 3
        log(f"== {step}")
        if step == "info":
            rc = info_step(h, a.port, jl)
            if rc:
                return rc
            continue
        for s in (x for x in subs if x["step"] == step):
            name = f"{Path(s['args'][0]).stem} {' '.join(s['args'][4:])}".strip()
            # after a failure in ddraw / d3d / d3dperf, or any timed-out
            # sub-run, a driver that may have just failed (or hung in) an
            # exclusive-mode set is not switched again for the REST OF THE
            # BATTERY - each fullscreen run is two switches, four timing changes
            if (s["fullscreen"] or s["switches"]) and stop_switching:
                why = f"not run: {stop_switching} - no more mode switches this battery"
                log(f"   skip {name}: {why}")
                skipped += 1
                with open(jl, "a") as f:
                    f.write(json.dumps({"step": step, "rc": None, "skipped": why,
                                        "args": s["args"][1:]}) + "\n")
                continue
            if s["fullscreen"]:
                # the labs hold every fullscreen mode >= 3 s themselves
                # (tools/vcr_pace.h); this rest() is margin on top of that
                rest()
            # what runs already is not this sub-run's: its cleanup spares it
            before = snapshot(h, a.port)
            rc, res, dt = run(s["args"], s["timeout"])
            halt = halt_reason(res)
            # a lab that lost the focus or the switch lock failed, whatever
            # its runner's exit code says
            bad += rc != 0 or bool(halt)
            row = {"step": step, "rc": rc, "seconds": round(dt), "result": res}
            if timed_out(rc, res):
                log(f"   {name} TIMED OUT - cleaning up before anything else runs")
                row["cleanup"] = clean_up(h, a.port, before)
                stop_switching = stop_switching or f"'{name}' timed out"
            elif halt:
                # in ANY step: the pacing or the screen was not ours alone
                row["halt"] = halt
                stop_switching = stop_switching or f"'{name}': {halt}"
            elif rc != 0 and step in FULLSCREEN_STEPS and refused_before_switching(res):
                # the runner's own gate said no BEFORE uploading or switching
                # anything: the driver was never asked, so nothing says it is
                # unfit to switch - record it, keep the latch for real failures
                row["refused"] = True
            elif rc != 0 and step in FULLSCREEN_STEPS:
                stop_switching = stop_switching or f"'{name}' failed"
            log(f"   rc {rc} ({dt:.0f}s): {json.dumps(res)[:300]}")
            with open(jl, "a") as f:
                f.write(json.dumps(row) + "\n")
    log(f"done: {bad} failing sub-step(s), {skipped} switching run(s) skipped after a failure "
        f"or timeout -> {jl}")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
