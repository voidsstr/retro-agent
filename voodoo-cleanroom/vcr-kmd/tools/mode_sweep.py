#!/usr/bin/env python3
"""mode_sweep.py - switch through a SHORT list of modes and prove each one.

The whole list runs in ONE process on the box, `vcrctl modeseq` under EXECW:
per mode it switches (ChangeDisplaySettings), draws a GDI pattern and reads it
back, and snapshots the chip's registers; after the last mode it restores the
desktop ONCE. The host then reads the flight recorder (`vcrctl log <after>`)
and splits it at each mode's boundary. A mode passes only when the switch
succeeded, the CURRENT mode reads back as the one asked for, GDI read back
what it drew, and the driver logged no WARN/ERROR while in it.

With --golden (a golden_capture.py file from the same box):
  - only modes the vendor capture holds are visited - a subset to compare
    against, NOT a monitor filter: the vendor's list is a fixed 3dfx table
    (README, THE MONITOR) and knows nothing of the monitor on the box;
  - after each switch our driver's live registers (read back from the chip)
    must equal the vendor's for that mode: every CRTC timing byte, CR1A/CR1B,
    misc, pllCtrl0, dacMode, vidScreenSize, and vidProcCfg minus the
    tiled-desktop and hardware-cursor bits ours does not use yet.

    mode_sweep.py 192.168.1.124 --modes 640x480x16@85,1024x768x32@85
    mode_sweep.py 192.168.1.124 --golden golden/amigamerlin-3.1-r11_192.168.1.124.json \\
        --modes 640x480x16@85,1024x768x32@85
    mode_sweep.py 127.0.0.1 --port 19910 --test-bed --filter 16 --limit 8

EVERY SWITCH IS A MONITOR RE-SYNC - two timing changes on the cable, in fact:
our driver passes through its 31.5 kHz VGA reset on every mode change, ~30 ms
before the new timing. On 2026-09-26 this tool took .124's 1998
Sony CRT through 123 modes with a return to the desktop after each - ~250
re-syncs at two a second, every relay click audible. So a live sweep is small,
paced and inside the monitor:
  - nothing is switched unless `vcrctl info` says the driver read the EDID
    (edid_ok 1), builds its mode list inside a range (mon_filter 1), and that
    range is THIS monitor's own (mon_src 1 EDID or 2 same monitor) - not the
    envelope or the default the driver falls back to without one;
  - every mode is checked again HERE: tools/modecalc.c (the driver's own mode
    math) gives the horizontal frequency and pixel clock the chip will
    program, and a mode outside the EDID's ranges is refused - by the
    driver's own rule (half a unit of rounding slack, the dot clock exact),
    applied to the PROGRAMMED timing;
  - at least --pace seconds before every switch, the restore included
    (default 5; vcrctl enforces 3 whatever it is asked);
  - more than --max-live modes (default 12) is refused without --allow-many,
    and so is a run whose EXECW could outlive the agent's 900 s clamp - the
    budget counts the gate's worst case for every switch (queued behind
    another tool's switch lock for VCR_PACE_LOCK_MS, then the pace);
  - SIGINT/SIGTERM does not abandon the box mid-list: the first uploads a
    stop file (C:\\vcr\\modeseq.stop), which makes vcrctl stop before its next
    switch and restore once, paced; the second only stops waiting for the
    reply; nothing is killed before a THIRD;
  - when modeseq's summary is not heard (a second signal, a dropped
    connection, a host timeout, the agent's EXECW timeout) nothing is killed
    first: the stop file goes up (unless it already has), vcrctl gets a
    bounded wait to stop on its own, and only after that - or a third signal
    - is it killed, by PID, through `vcrctl pace-kill` (paced and stamped on
    the box), and never a vcrctl that predates the sweep. Then the host
    waits the pace, and the desktop is restored ONCE, and only if it is not
    already the current mode.
The register check for EVERY vendor mode needs no monitor at all:
tools/golden_compare.py runs the driver's mode math against the capture on
the dev host (123/123 identical, 2026-09-26).
"""
import argparse
import asyncio
import ipaddress
import json
import math
import re
import signal
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parents[2]))
sys.path.insert(0, str(HERE))
from client.retro_protocol import RESP_ERROR, RetroConnection  # noqa: E402
import vcrlog  # noqa: E402

SECRET = "retro-agent-secret"
# vcrctl modeseq looks for this before every switch, deletes it, and restores
# once (paced): the one way to end a running list without a burst of switches
STOP_FILE = r"C:\vcr\modeseq.stop"
# The agent clamps EXECW at 900 s and then kills the process tree - which can
# leave vcrctl alive holding a temporary mode. A list that could run that long
# is refused rather than cut off.
EXECW_CEILING = 800
# What the agent appends to an EXECW reply when it killed the child's tree at
# the timeout (agent/src/exec.c). The tree kill is best effort, so a reply
# carrying it says the tool may still be running, holding a mode.
EXECW_TIMED_OUT = "[EXECW: timed out"
# How much longer the host waits for an EXECW's reply than the agent's own
# budget, so it hears the agent's answer (and its timeout marker) rather
# than timing out first and skipping the cleanup.
HOST_SLACK_S = 30


def _pace_h(name, fallback_ms):
    """A timing from tools/vcr_pace.h, in seconds - read, not copied: the
    host's budgets must cover what the gate on the box can actually wait, and
    a copy that drifts is how an EXECW's tree kill lands in the middle of a
    hold (a kill reverts the mode unpaced). The fallback is the documented
    value, for a tools/ directory without the header."""
    try:
        m = re.search(r"^#define\s+%s\s+(\d+)u?\b" % name, (HERE / "vcr_pace.h").read_text(), re.M)
    except OSError:
        m = None
    return (int(m.group(1)) if m else fallback_ms) / 1000.0


# vcr_pace.h's floor: no tool on the box switches sooner than this after the
# last switch; the host never waits less either.
PACE_FLOOR_S = max(3.0, _pace_h("VCR_PACE_MIN_MS", 3000))
# ... and how long a tool queues behind another tool's switch lock before it
# goes on without it (vcrctl) or fails 'pace lock busy' (the labs). Every
# switch can cost this before its floor even starts.
PACE_LOCK_S = _pace_h("VCR_PACE_LOCK_MS", 20000)
# An exit hold's stamp, and a paced kill's, is written this far AHEAD (the
# revert lands after it): the next switch waits its floor from there
PACE_EXIT_LAG_S = _pace_h("VCR_PACE_EXIT_LAG_MS", 5000)
# the most a caller's pace may raise the floor to - vcrctl refuses more
PACE_MAX_S = _pace_h("VCR_PACE_MAX_MS", 30000)
# Our driver keeps its VGA reset on every mode change, so one logical switch
# is TWO timing changes on the cable: old -> 31.5 kHz VGA -> new, ~30 ms
# apart. A plan that counts only switches under-states what the tube gets.
TIMING_CHANGES_PER_SWITCH = 2
# How long a modeseq told to stop may take to leave on its own, beyond the
# two paced switches it may still make (the wait before its next switch,
# where it looks for the stop file, and the wait before its restore - each
# queued behind the lock first, see switch_s): a mode already under test
# (the GDI test and snapshot - MODETEST_S), and a margin for the restore
# itself and the exit. Killing it sooner is what the stop file is for
# avoiding.
MODETEST_S = 20
MODESEQ_STEP_S = MODETEST_S + 5
CIVIL_MARGIN_S = 15
POLL_S = 3
# modeseq's start-up before its first wait, and the summary after the restore
MODESEQ_START_S = 60


def switch_s(pace=PACE_FLOOR_S):
    """The longest ONE paced switch can take on the box: queued behind
    another tool's switch lock for VCR_PACE_LOCK_MS, then the floor - or the
    caller's longer pace - since the last stamp, which an exit hold or a
    paced kill writes VCR_PACE_EXIT_LAG_MS ahead. A hold before a give-back
    is the same wait (vcr_pace_hold_to_exit takes the lock too)."""
    return PACE_LOCK_S + PACE_EXIT_LAG_S + max(pace, PACE_FLOOR_S)


def execw_budget(switches, work_s, pace=PACE_FLOOR_S):
    """An EXECW budget for a tool that makes `switches` paced switches (and
    holds) and needs `work_s` for everything else: the gate's worst case per
    switch on top of the work, so the agent's tree kill - which reverts the
    mode unpaced - never lands on a tool that is only waiting its turn."""
    return int(math.ceil(switches * switch_s(pace) + work_s))


def modeseq_budget(n, pace):
    """`vcrctl modeseq`'s EXECW for n modes: every switch - each mode, and the
    restore - can queue behind another tool's switch lock and then waits
    `pace`; each mode has up to MODETEST_S of GDI test and snapshot; a failed
    restore leaves the exit hold (one more such wait); plus start-up."""
    return execw_budget(n, n * MODETEST_S, pace) + execw_budget(2, MODESEQ_START_S, pace)


def budget_refusal(budget, what):
    """None, or why an EXECW of `budget` s may not be started: the agent
    clamps EXECW at 900 s and then kills the tree, which can leave a tool
    holding a mode. The plan is refused, not cut off."""
    if budget <= EXECW_CEILING:
        return None
    return (f"{what} needs up to {budget} s in one EXECW (the pace gate's worst case for every "
            f"switch included); the agent kills at 900 s, which can leave a tool holding a mode - "
            f"keep it under {EXECW_CEILING} s")


# `vcrctl restore`: one paced switch and - if it fails - the exit hold before
# the process may go (a second), plus start-up and exit. A restore is a
# switch, so never plain EXEC: its fixed 60 s can kill it waiting its turn.
RESTORE_EXECW_S = execw_budget(2, 15)
# `vcrctl pace-kill <pid>`: queue for the switch lock, wait out the floor,
# kill, up to 10 s for the process to be gone, stamp the revert - one switch,
# that wait, and start-up
PACE_KILL_EXECW_S = execw_budget(1, 10 + 10)
# Signals before anything is killed: the first stops the list civilly, the
# second only stops waiting for the reply, the third may kill.
KILL_AFTER_SIGNALS = 3
VCRCTL_IMAGES = ("vcrctl.exe",)

CRTC_IGNORE = {0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x14}   # cursor/start address
VPC_IGNORE = (1 << 24) | (1 << 27)                          # tiled desktop, hw cursor


def against_golden(snap, cap):
    """Differences between our live registers and the vendor's for one mode."""
    ours = snap["chips"][0]
    oio = [int(x, 16) for x in ours["io"]]
    vio = [int(x, 16) for x in cap["io"]]
    diffs = []
    for off, name, mask in ((0x40, "pllCtrl0", 0xffff), (0x4c, "dacMode", 0x1f),
                            (0x98, "vidScreenSize", 0xffffff),
                            (0x5c, "vidProcCfg", ~VPC_IGNORE & 0xffffffff)):
        if (oio[off // 4] & mask) != (vio[off // 4] & mask):
            diffs.append(f"{name} {oio[off // 4]:08x}/{vio[off // 4]:08x}")
    oc = bytes.fromhex(ours["crtc"])
    vc = bytes.fromhex(cap["vga"]["crtc"])
    for i in list(range(0x19)) + [0x1a, 0x1b]:
        if i not in CRTC_IGNORE and oc[i] != vc[i]:
            diffs.append(f"CR{i:02x} {oc[i]:02x}/{vc[i]:02x}")
    if ours["misc"] != cap["vga"]["misc"]:
        diffs.append(f"misc {ours['misc']}/{cap['vga']['misc']}")
    return diffs


def jline(text):
    for ln in reversed(text.strip().splitlines()):
        if ln.startswith("{"):
            try:
                return json.loads(ln)
            except json.JSONDecodeError:
                return None
    return None


def json_lines(text):
    """Every JSON object line in `text`. A garbled line is skipped, never
    fatal: the restore decision reads this, and a parse error must not be
    the reason a box is left in a temporary mode."""
    out = []
    for ln in (text or "").splitlines():
        ln = ln.strip()
        if not ln.startswith("{"):
            continue
        try:
            j = json.loads(ln)
        except ValueError:
            continue
        if isinstance(j, dict):
            out.append(j)
    return out


def banner(head, why):
    """A refusal, or a restore that did not happen, must be impossible to miss:
    a quiet one reads like a pass."""
    bar = "!" * 76
    print(f"{bar}\n{head}: {why}\n{bar}", flush=True)


def refuse(why):
    banner("REFUSED - no mode was switched", why)


# ---- the monitor gate (used by every tool here that switches modes:
# ---- mode_sweep, silicon_battery, golden_capture, glidelab_run/_sweep,
# ---- ddlab_run, d3dprobe_run, lab_run, sli_golden/_sweep, sli_shot) --------

# vcr_edid.h's VCR_MON_SRC_*: where the limits the driver filters by came from
MON_SRC_NAMES = {0: "none", 1: "edid", 2: "same-monitor persisted", 3: "envelope",
                 4: "default"}
# the two that are THIS monitor's own ranges: its EDID's, read this boot, or -
# for an EDID with no range descriptor - the range this same model persisted
MON_SRC_OWN = (1, 2)


def monitor_gate(info, where="vcrctl info"):
    """(the monitor's EDID ranges, None), or (None, why nothing may switch).

    Three things are required. edid_ok: the driver read an EDID over DDC this
    boot. mon_filter: its mode list is limited to a range at all
    (Diag\\EdidFilter=0 lists every timing in its table - 1600x1200@85 at
    106 kHz included, past .124's 96 kHz CRT). mon_src: that range is THIS
    monitor's - its EDID's own (1), or what this same model persisted on an
    earlier boot when its EDID has no range descriptor (2).

    The driver no longer offers every timing when it lacks an EDID: a failed
    DDC read gets the envelope of every monitor the box has seen (3) or a safe
    default (4), and so does a range-less EDID from a monitor it never saw
    with a range. Those keep ITS list conservative, but they are not a
    measurement of the tube on the cable, and a host plan is checked against
    the numbers it is given - so a plan is never built on them. An info with
    no mon_src comes from a driver or vcrctl older than that split and
    cannot say which it is."""
    if not isinstance(info, dict) or not info.get("ok"):
        return None, (f"{where} gave no answer - not the vcr-kmd driver, or no vcrctl - "
                      "so nothing says which timings this monitor accepts")
    if info.get("edid_ok") != 1:
        return None, (f"{where}: edid_ok={info.get('edid_ok')} - the driver has no EDID for "
                      "this monitor")
    if info.get("mon_filter") != 1:
        return None, (f"{where}: mon_filter={info.get('mon_filter')} - the driver's mode list is "
                      "NOT limited to a monitor range (Diag\\EdidFilter=0)")
    if "mon_src" not in info:
        return None, (f"{where} has no mon_src - it comes from a driver / vcrctl older than the "
                      "EDID fallback, so it cannot say whether its range is this monitor's own "
                      "or the envelope / default it falls back to; install the current build "
                      "(and save the info again)")
    src = info.get("mon_src")
    if src not in MON_SRC_OWN:
        return None, (f"{where}: mon_src={src} ({MON_SRC_NAMES.get(src, 'unknown')}) - the "
                      "driver's range is not this monitor's own (only 1 edid or 2 "
                      "same-monitor persisted are), so a host check against it proves nothing "
                      "about the tube on the cable")
    try:
        hmin, hmax = (int(x) for x in info["mon_h_khz"])
        vmin, vmax = (int(x) for x in info["mon_v_hz"])
        pmax = int(info["mon_max_pixclk_khz"])
    except (KeyError, TypeError, ValueError):
        return None, f"{where}: no usable mon_h_khz / mon_v_hz / mon_max_pixclk_khz"
    if not (0 < hmin <= hmax and 0 < vmin <= vmax and pmax > 0):
        return None, (f"{where}: implausible monitor ranges H {hmin}-{hmax} kHz, "
                      f"V {vmin}-{vmax} Hz, {pmax} kHz")
    return {"h_khz": (hmin, hmax), "v_hz": (vmin, vmax), "pixclk_khz": pmax,
            "monitor": str(info.get("monitor", "?")).strip()}, None


NOT_OURS_HINT = ("under the vendor driver, pass --monitor-info FILE: a `vcrctl info` saved "
                 "from OUR driver on this box and monitor")
CHECKED_FLAG = "--i-have-checked-the-monitor"

# ---- --monitor-info: is the saved info still THIS box's monitor? ------------
#
# Under another driver nothing on the box answers for the EDID, so the gate
# reads a `vcrctl info` saved from OUR driver. A saved file says nothing about
# NOW: the fleet's hardware moves between boxes, and a monitor swapped since
# the file was written would be driven by another tube's ranges. XP records
# each monitor devnode's EDID under Enum\DISPLAY whatever the driver, and the
# devnode that is present now carries a volatile Control key - the rule
# agent/shared/edid.h's edid_from_pnp reads the active monitor by.

EDID_HEADER = bytes.fromhex("00ffffffffffff00")
DISPLAY_ENUM = r"SYSTEM\CurrentControlSet\Enum\DISPLAY"
# a box with more monitor devnodes than this is not read further: a bound on
# the REGREADs, not a limit anything real reaches
MAX_MON_INSTANCES = 64


def _hexbytes(s):
    """Bytes from hex text with or without separators - REGREAD gives
    "00 FF FF ...", vcrctl info "00ffff..." - or None."""
    if not isinstance(s, str):
        return None
    try:
        return bytes.fromhex("".join(s.split()))
    except ValueError:
        return None


def edid_model(edid):
    """(PnP id, product code) from an EDID's bytes 8-11, or None when it is
    not an EDID. The MODEL, not the unit - the identity the driver keys a
    monitor's persisted range on (vcr_mon_id): two units of one model are the
    same tube as far as the ranges go."""
    if not isinstance(edid, (bytes, bytearray)) or len(edid) < 12 or bytes(edid[:8]) != EDID_HEADER:
        return None
    i = (edid[8] << 8) | edid[9]
    pnp = "".join(chr(ord("@") + ((i >> s) & 0x1f)) for s in (10, 5, 0))
    if not all("A" <= ch <= "Z" for ch in pnp):
        return None
    return pnp, edid[10] | (edid[11] << 8)


def model_str(m):
    """As vcrctl info's "monitor" names it: PnP id + product, %04x."""
    return f"{m[0]}{m[1]:04x}"


def saved_monitor(saved, where):
    """(model, None) for the monitor a saved `vcrctl info` describes, or
    (None, why). The EDID bytes decide; the "monitor" string must agree, or
    the file is not one monitor's info (hand-edited, or two spliced)."""
    m = edid_model(_hexbytes(saved.get("edid")))
    if not m:
        return None, (f"{where} carries no EDID ('edid'), so nothing says which monitor its "
                      "ranges belong to - save it again from the current vcrctl")
    name = str(saved.get("monitor", "")).split()
    if not name or name[0].lower() != model_str(m).lower():
        return None, (f"{where}: its 'monitor' ({saved.get('monitor')!r}) and its EDID "
                      f"({model_str(m)}) disagree - not one monitor's info")
    return m, None


async def _regread(call, path, value=None):
    """REGREAD over the agent -> its JSON, or None: the key did not open, the
    value is not there (the agent then answers malformed JSON), no answer."""
    cmd = f'REGREAD HKLM "{path}"' + (f" {value}" if value else "")
    try:
        st, txt = await call(cmd, 30)
        j = json.loads(_text(txt))
    except Exception:
        return None
    return j if st != RESP_ERROR and isinstance(j, dict) else None


async def present_monitors(call):
    """([{"key", "model"}], None): every monitor devnode XP has present NOW,
    with the model its registry EDID names (None: no EDID there); or (None,
    why) when the registry could not be read - which is never "no monitor"."""
    root = await _regread(call, DISPLAY_ENUM)
    if not root or not isinstance(root.get("subkeys"), list):
        return None, f"REGREAD HKLM\\{DISPLAY_ENUM} gave no key list"
    out, n = [], 0
    for mid in root["subkeys"]:
        k = await _regread(call, f"{DISPLAY_ENUM}\\{mid}")
        if not k or not isinstance(k.get("subkeys"), list):
            return None, f"cannot list HKLM\\{DISPLAY_ENUM}\\{mid}"
        for inst in k["subkeys"]:
            n += 1
            if n > MAX_MON_INSTANCES:
                return None, f"more than {MAX_MON_INSTANCES} monitor devnodes - not read further"
            path = f"{DISPLAY_ENUM}\\{mid}\\{inst}"
            ik = await _regread(call, path)
            if not ik or not isinstance(ik.get("subkeys"), list):
                return None, f"cannot read HKLM\\{path}"
            if not any(str(s).lower() == "control" for s in ik["subkeys"]):
                continue                     # not present now: an old monitor's devnode
            ev = await _regread(call, path + "\\Device Parameters", "EDID")
            data = (ev.get("value") or {}).get("data") if ev else None
            out.append({"key": f"{mid}\\{inst}", "model": edid_model(_hexbytes(data))})
    return out, None


async def monitor_fresh(call, saved, where, checked=False):
    """None when the saved info may stand in for the monitor on the box now,
    else why not. The present monitor's registry EDID must name the model the
    saved info does: a DIFFERENT model is refused outright (the flag cannot
    override a registry that says so). A registry that cannot say - it did
    not answer, or no present monitor carries an EDID (XP's Default Monitor:
    the driver read none), or one of several present ones does not - is
    refused too (fail closed), unless the operator has looked at the box and
    says so with --i-have-checked-the-monitor."""
    want, why = saved_monitor(saved, where)
    if why:
        return why
    mons, unread = await present_monitors(call)
    if mons is not None:
        known = [m for m in mons if m["model"]]
        wrong = [m for m in known if m["model"] != want]
        if wrong:
            now = ", ".join(model_str(m["model"]) for m in wrong)
            return (f"the monitor on the box now is {now} "
                    f"(registry {', '.join(m['key'] for m in wrong)}), not the "
                    f"{model_str(want)} {where} describes - its ranges are another tube's")
        blind = [m["key"] for m in mons if not m["model"]]
        if not known:
            unread = ("no present monitor carries an EDID in the registry"
                      + (f" ({', '.join(blind)})" if blind else " (none is present)"))
        elif blind:
            unread = (f"present monitor(s) without an EDID ({', '.join(blind)}) beside the "
                      f"{model_str(want)} - nothing says which one the modes reach")
        else:
            print(f"monitor check: the present monitor is {model_str(want)}, as {where} says "
                  f"({', '.join(m['key'] for m in known)})", flush=True)
            return None
    if checked:
        banner("MONITOR NOT CONFIRMED BY THE BOX", f"{unread} - going on because {CHECKED_FLAG} "
               f"says the {model_str(want)} of {where} is the monitor on the cable")
        return None
    return (f"{unread}: cannot confirm the monitor on the box is the {model_str(want)} {where} "
            f"describes - refused (fail closed); once you have looked at the box, {CHECKED_FLAG}")


def _load_info(path):
    """A saved `vcrctl info`: the raw line, or pretty-printed JSON."""
    txt = Path(path).read_text()
    try:
        j = json.loads(txt)
    except ValueError:
        j = jline(txt)
    return j if isinstance(j, dict) else None


async def choose_monitor_info(call, live, saved_path=None, checked=False):
    """(the info to gate on, where it came from, why not). The live `vcrctl
    info` whenever the installed driver is ours. Under another driver (the
    vendor's, for a golden capture or a benchmark) nothing on the box answers
    for the EDID, so the gate reads saved_path - a `vcrctl info` saved from
    OUR driver - once the registry confirms the monitor on the box is the one
    it describes (monitor_fresh). The host range check is then the only
    guard: that driver's list is its own fixed table, not the monitor's."""
    if (isinstance(live, dict) and live.get("ok")) or not saved_path:
        return live, "vcrctl info", None
    where = f"--monitor-info {saved_path}"
    try:
        saved = _load_info(saved_path)
    except OSError as e:
        return None, where, f"{where}: {e.strerror}"
    if saved is None:
        return None, where, f"{where}: no `vcrctl info` JSON in it"
    banner("NOT OUR DRIVER", f"its mode list is not limited to the monitor - every mode is "
           f"checked on the host against {saved_path} instead")
    why = await monitor_fresh(call, saved, where, checked)
    return (None, where, why) if why else (saved, where, None)


async def box_json(call, cmd, timeout=60):
    """One agent command's last JSON line, or None (no answer counts as none)."""
    try:
        _, txt = await call(cmd, timeout)
    except Exception:
        return None
    return jline(_text(txt))


async def gate_on_box(call, tool, modes, saved_path=None, checked=False, live=None):
    """The whole gate for a tool about to switch into `modes` on a box:
    (ranges, calc, None), or (None, None, why). `vcrctl info` is asked unless
    `live` is given; under another driver the saved --monitor-info stands in,
    checked against the registry first."""
    if live is None:
        live = await box_json(call, f"EXEC {tool} info")
    info, where, why = await choose_monitor_info(call, live, saved_path, checked)
    if why:
        return None, None, why
    rng, calc, why = gate_modes(info, modes, where)
    if why and not (isinstance(live, dict) and live.get("ok")) and not saved_path:
        why = f"{why} - {NOT_OURS_HINT}"
    return rng, calc, why


def highest_refresh(listed, w, h, bpp=None):
    """The highest refresh `listed` (vcrctl modes) holds at WxH - at that
    depth, or at any when bpp is None - or None. XP's 0/1 Hz "driver
    default" entries are not refreshes."""
    best = None
    for m in listed or ():
        mm = re.fullmatch(r"(\d+)x(\d+)x(\d+)@(\d+)", str(m))
        if not mm:
            continue
        mw, mh, mb, hz = (int(x) for x in mm.groups())
        if (mw, mh) == (w, h) and (bpp is None or mb == bpp) and hz > 1:
            best = hz if best is None else max(best, hz)
    return best


async def driver_picks(call, tool, specs):
    """The modes to gate for runs that switch at refresh 0 - SetDisplayMode
    with no rate, a fullscreen D3D device at D3DPRESENT_RATE_DEFAULT, Quake II
    through our ICD - where the driver / XP picks the rate. specs: [(w, h,
    bpp, any_depth)]. The mode checked is the HIGHEST refresh the driver
    lists at that size (at that depth, or at any for a caller that picks
    across depths): the worst the pick can land on. -> (modes, None) or
    (None, why)."""
    j = await box_json(call, f"EXEC {tool} modes")
    listed = (j or {}).get("modes")
    if not isinstance(listed, list):
        return None, ("vcrctl modes gave no mode list - so nothing says which refresh the driver "
                      "would pick")
    out = []
    for w, h, bpp, any_depth in specs:
        hz = highest_refresh(listed, w, h, None if any_depth else bpp)
        if hz is None:
            return None, (f"the driver lists no {w}x{h}" + ("" if any_depth else f"x{bpp}")
                          + " mode with a refresh - the run would ask for a mode nobody checked")
        out.append(f"{w}x{h}x{bpp}@{hz}")
    return out, None


async def fullscreen_gate(call, tool, specs, saved_path=None, checked=False):
    """The gate for runs that switch at refresh 0 (driver_picks' specs):
    the monitor first - the live `vcrctl info`, or --monitor-info checked
    against the registry - then the highest refresh the driver lists at each
    size, then the host range check. -> (ranges, calc, modes, None) or
    (None, None, None, why)."""
    live = await box_json(call, f"EXEC {tool} info")
    info, where, why = await choose_monitor_info(call, live, saved_path, checked)
    if not why:
        _, why = monitor_gate(info, where)
    if why:
        if not (isinstance(live, dict) and live.get("ok")) and not saved_path:
            why = f"{why} - {NOT_OURS_HINT}"
        return None, None, None, why
    modes, why = await driver_picks(call, tool, specs)
    if why:
        return None, None, None, why
    rng, calc, why = gate_modes(info, modes, where)
    return (None, None, None, why) if why else (rng, calc, modes, None)


def lab_halt(res, text="", want_hz=None):
    """Why no further switch may follow a lab's run, or None:
      - "focus_lost": its fullscreen / exclusive window lost the focus -
        something else took the screen (a dialog, another session's tool)
        while it held a mode, and whatever that is may switch too;
      - 'pace lock busy': it queued VCR_PACE_LOCK_MS behind another tool's
        switch lock and gave up - something else is switching this box;
      - 'pace floor never passed': the stamp kept moving for
        VCR_PACE_WAIT_CAP_MS - something else keeps switching this box;
      - an "opened_hz" other than asked (glidelab): Glide opened a refresh
        the host gate never checked."""
    blob = (json.dumps(res) if isinstance(res, (dict, list)) else str(res or "")) + (text or "")
    if isinstance(res, dict) and res.get("focus_lost"):
        return "the lab lost the focus while it held a mode (focus_lost)"
    if "pace lock busy" in blob.lower():
        return "the pace lock was busy - another tool is switching this box"
    if "pace floor never passed" in blob.lower():
        return "the pace floor never passed - something else keeps switching this box"
    # 0 / 1 is XP's "hardware default" answer, not a refresh: unknown is not
    # a mismatch (unmeasured on silicon whether a Glide session reports one)
    if want_hz is not None and isinstance(res, dict) and res.get("opened_hz") not in (None, 0, 1) \
            and res.get("opened_hz") != want_hz:
        return (f"Glide opened {res.get('opened_hz')} Hz, asked {want_hz} - a mode the gate "
                "never checked")
    return None


def describe(rng):
    return (f"{rng['monitor']}: H {rng['h_khz'][0]}-{rng['h_khz'][1]} kHz, "
            f"V {rng['v_hz'][0]}-{rng['v_hz'][1]} Hz, <= {rng['pixclk_khz'] / 1000:.0f} MHz")


def modecalc():
    """What the driver's own mode math programs for every timing, by mode -
    tools/modecalc.c built for the host by golden_compare's helper."""
    import golden_compare
    return golden_compare.ours()


def out_of_range(modes, rng, calc):
    """{mode: why} for every mode the host cannot prove inside the monitor.

    The DRIVER'S rule (common/vcr_modes.c vcr_mode_check): half a unit of
    slack at both ends for EDID's whole-number rounding (a "120 Hz" monitor's
    own 640x480@120 runs at 120.015 Hz; a "31 kHz" floor is a 31.47 kHz
    640x480), the dot clock exact - applied here to the timing that is
    actually PROGRAMMED (modecalc), which in 2X mode is a hair above the
    nominal one the driver checks. A host check stricter than the driver
    refused modes the driver lists (every refresh-0 fullscreen run at
    640x480 on the Sony, where the highest listed rate is 120.015 Hz) and
    reported the refusal as a driver failure; one looser than the driver
    would bless what the driver was built to keep off the tube."""
    (hmin, hmax), (vmin, vmax), pmax = rng["h_khz"], rng["v_hz"], rng["pixclk_khz"]
    bad = {}
    for m in modes:
        o = calc.get(m)
        if o is None:
            bad[m] = "no timing in common/vcr_modes.c, so its frequencies are unknown"
            continue
        hf, vf, px = o.get("hkhz_x1000"), o.get("vhz_x1000"), o.get("khz")
        if not all(isinstance(x, int) for x in (hf, vf, px)):
            bad[m] = "tools/modecalc.c gave no scan rates for it"
        elif hf > hmax * 1000 + 500 or hf + 500 < hmin * 1000:
            bad[m] = f"H {hf / 1000:.3f} kHz outside {hmin}-{hmax}"
        elif vf > vmax * 1000 + 500 or vf + 500 < vmin * 1000:
            bad[m] = f"V {vf / 1000:.3f} Hz outside {vmin}-{vmax}"
        elif px > pmax:
            bad[m] = f"pixel clock {px / 1000:.1f} MHz above {pmax / 1000:.0f}"
    return bad


def gate_modes(info, modes, where="vcrctl info"):
    """The whole gate for a list of modes: (ranges, calc, None) when every
    one may be switched to, else (None, None, why). calc is modecalc()'s
    table, for the caller's plan."""
    rng, why = monitor_gate(info, where)
    if why:
        return None, None, why
    try:
        calc = modecalc()
    except Exception as e:  # no compiler, a build error: no numbers, no switch
        return None, None, f"cannot compute the timings on the host ({type(e).__name__}: {e})"
    bad = out_of_range(modes, rng, calc)
    if bad:
        return None, None, (f"not provably inside the monitor ({describe(rng)}): "
                            + "; ".join(f"{m} {w}" for m, w in bad.items()))
    return rng, calc, None


def rates(calc, m):
    o = (calc or {}).get(m)
    if not o or not isinstance(o.get("hkhz_x1000"), int):
        return ""
    return (f"{o['hkhz_x1000'] / 1000:6.2f} kHz {o['vhz_x1000'] / 1000:6.2f} Hz "
            f"{o['khz'] / 1000:6.1f} MHz")


def is_loopback(host):
    try:
        return ipaddress.ip_address(host).is_loopback
    except ValueError:
        return host == "localhost"


# ---- after a run that did not end civilly (every tool here that launches one:
# ---- mode_sweep, golden_capture, glidelab_run/_sweep, the lab runners, the
# ---- battery) -----------------------------------------------------------------

def _text(d):
    return d.decode("latin1", "replace") if isinstance(d, (bytes, bytearray)) else (d or "")


async def pids_of(call, images, spare=None):
    """PIDs of running `images` (exe names, any case), minus `spare`; None
    when PROCLIST did not answer - which is NOT proof that none is running.

    call(cmd, timeout) -> (status, text or bytes), one agent command."""
    want = {i.lower() for i in images}
    try:
        _, txt = await call("PROCLIST", 30)
        procs = json.loads(_text(txt))
    except Exception:  # no answer, or not a list: unknown, never "none"
        return None
    if not isinstance(procs, list):
        return None
    return [p.get("pid") for p in procs if isinstance(p, dict)
            and str(p.get("name", "")).lower() in want
            and p.get("pid") not in (spare or ())]


async def pace_kill(call, tool, pid):
    """Kill ONE process through `vcrctl pace-kill <pid>`, under EXECW: vcrctl
    queues for the box's switch lock, waits out the floor since the last
    switch, kills the PID and stamps the revert XP makes as it dies. A
    PROCKILL or `taskkill` reverted the mode the instant it landed - maybe a
    moment after a switch, two re-syncs back to back - and recorded nothing.
    Never plain EXEC: the kill itself waits its turn, and EXEC's fixed 60 s
    could cut it off mid-wait. -> (killed and stamped, the answer or why not)."""
    try:
        # bounded here too: a transport that ignores its timeout must not
        # hold the cleanup - and every switch after it - forever
        st, txt = await asyncio.wait_for(
            call(f"EXECW {PACE_KILL_EXECW_S} {tool} pace-kill {pid}",
                 PACE_KILL_EXECW_S + HOST_SLACK_S), PACE_KILL_EXECW_S + 2 * HOST_SLACK_S)
    except Exception as e:
        return False, f"{type(e).__name__}: {e}"
    txt = _text(txt)
    j = next((x for x in reversed(json_lines(txt)) if x.get("cmd") == "pace-kill"), None)
    if EXECW_TIMED_OUT in txt:
        return False, f"pace-kill itself timed out after {PACE_KILL_EXECW_S} s"
    if st == RESP_ERROR or j is None:
        return False, txt.strip()[-160:] or "no answer"
    if not j.get("ok"):
        return False, str(j.get("error") or json.dumps(j))[:160]
    if str(j.get("pid")) != str(pid):
        return False, f"pace-kill answered for pid {j.get('pid')}, not {pid}"
    return True, j


async def reap(call, images, tool, pace, spare=None):
    """The civil cleanup after a tool that may not have ended on its own (an
    EXECW timeout, a dropped connection, a host timeout, a crash):

      1. PROCLIST for `images`. Only a process that was NOT running before
         the caller launched its own is a target: `spare` is the caller's
         pre-launch list, and what is on it - another session's - is left
         alone. With no list (None: the caller's PROCLIST did not answer)
         nothing says whose a survivor is, so NOTHING is killed; the report
         says so and "gone" stays False (the caller then restores nothing);
      2. each target is killed BY PID through `vcrctl pace-kill` (pace_kill:
         paced, and the revert stamped on the box). No WM_CLOSE first: these
         tools do not quit on it, so it only ever cost a grace;
      3. confirm each one gone, bounded;
      4. `vcrctl pace-mark`, unless every target was pace-killed (each of
         those stamped already): a tool that died on its own (a crash, the
         agent's tree kill) skipped its exit hold, so XP reverted its mode
         unstamped - without the stamp the next tool could switch at once;
      5. wait `pace` on the host as well, before anything else switches.

    Never retried beyond the bounded confirm: a cleanup that loops is its own
    burst. -> a report dict; "gone" is True only when every process of ours
    was seen to be gone."""
    want = {i.lower() for i in images}
    rep = {"images": sorted(want), "found": [], "killed": [], "left": [], "spared": [],
           "gone": False, "pace_mark": False}
    say = lambda m: print(f"  cleanup: {m}", flush=True)  # noqa: E731
    stamped = False

    async def running():
        try:
            _, txt = await call("PROCLIST", 30)
            procs = json.loads(_text(txt))
            return {p.get("pid") for p in procs
                    if isinstance(p, dict) and str(p.get("name", "")).lower() in want}
        except Exception:
            return None

    live = await running()
    if live is None:
        rep["error"] = "PROCLIST did not answer - cannot tell what is still running"
        say(rep["error"])
    else:
        rep["found"] = sorted(live)
        if spare is None:
            # whose they are is unknown: another session's lab is not collateral
            targets, unknown = set(), set(live)
            if unknown:
                rep["whose"] = "unknown (no list from before the launch) - nothing killed"
                say(f"{', '.join(sorted(want))} running as {sorted(unknown)}: no list from "
                    "before the launch says whose, so none is killed")
        else:
            targets, unknown = live - set(spare), set()
            rep["spared"] = sorted(live & set(spare))
        if rep["spared"]:
            say(f"left alone (running before this launch - not ours): {rep['spared']}")
        for pid in sorted(targets):
            ok, detail = await pace_kill(call, tool, pid)
            if ok:
                rep["killed"].append(pid)
                continue
            rep.setdefault("kill_errors", []).append(f"{pid}: {detail}")
            # No other route, even for 'pace lock busy' (a tool hung WHILE
            # holding the switch lock): a plain kill reverts its mode unpaced.
            # The survivor is reported, the box stays as it is, and a person
            # decides - the rare hung holder costs a visit, never a burst.
        # the kill is asynchronous: look a few times, bounded. A PROCLIST that
        # does not answer leaves the last KNOWN state standing - never "gone"
        # by default
        left = set(targets)
        for k in range(5):
            if not left:
                break
            if k:
                await asyncio.sleep(2)
            now = await running()
            if now is not None:
                left &= now
        rep["left"] = sorted(left | unknown)
        rep["gone"] = not rep["left"]
        stamped = bool(targets) and set(rep["killed"]) == targets
        if rep["killed"]:
            say(f"killed by PID through pace-kill (paced, stamped): {rep['killed']}")
        if left:
            say(f"STILL RUNNING after the kill: {sorted(left)}")
    if stamped:
        rep["pace_mark"] = True
        rep["stamped_by"] = "pace-kill"
    else:
        try:
            _, txt = await call(f"EXEC {tool} pace-mark", 90)
            j = jline(_text(txt))
            rep["pace_mark"] = bool(j and j.get("ok"))
            if not rep["pace_mark"]:
                rep["pace_mark_error"] = _text(txt).strip()[-160:]
        except Exception as e:
            rep["pace_mark_error"] = f"{type(e).__name__}: {e}"
        if not rep["pace_mark"]:
            say(f"pace-mark FAILED ({rep.get('pace_mark_error')}) - the host still waits the pace")
    # the box's stamp paces the tools there; this paces the host's own next
    # switch, whichever tool it comes from
    rest = max(pace, PACE_FLOOR_S)
    await asyncio.sleep(rest)
    rep["rested_s"] = rest
    return rep


async def restore_once(call, tool):
    """ONE `vcrctl restore`, under EXECW with a budget - a restore is a paced
    switch, and plain EXEC's 60 s could kill it mid-wait - and never retried:
    a loop of restores is a burst of re-syncs. -> its answer, or {"ok":
    false, ...}."""
    try:
        _, txt = await asyncio.wait_for(
            call(f"EXECW {RESTORE_EXECW_S} {tool} restore", RESTORE_EXECW_S + HOST_SLACK_S),
            RESTORE_EXECW_S + 2 * HOST_SLACK_S)
    except Exception as e:
        return {"ok": False, "error": f"{type(e).__name__}: {e}"}
    txt = _text(txt)
    r = next((x for x in reversed(json_lines(txt)) if x.get("cmd") == "restore"), None)
    if EXECW_TIMED_OUT in txt:
        return dict(r or {}, ok=False, timed_out=True,
                    error=f"the restore's EXECW timed out after {RESTORE_EXECW_S} s")
    return r or {"ok": False, "error": txt.strip()[-200:] or "no answer"}


# ---- the run ----------------------------------------------------------------

async def box_cmd(a, cmd, timeout=60, payload=None):
    """One command on a FRESH connection. After an EXECW that timed out or
    was abandoned, the sweep's own connection may still owe a reply."""
    c = RetroConnection(a.host, a.port)
    await c.connect(SECRET, timeout=20)
    try:
        if payload is not None:
            st, d = await c.send_command(cmd, binary_payload=payload, timeout=timeout)
        else:
            st, d = await c.send_command(cmd, timeout=timeout)
        return st, d.decode("ascii", "replace")
    finally:
        await c.close()


async def send_stop(a):
    """Create STOP_FILE on the box. -> None, or why it could not be done."""
    try:
        st, d = await box_cmd(a, f"UPLOAD {STOP_FILE}", 30, payload=b"")
        return None if st != RESP_ERROR else d.strip() or "UPLOAD refused"
    except Exception as e:
        return f"{type(e).__name__}: {e}"


def restore_needed(seqsum):
    """Did this run leave the box in a mode it did not give back?"""
    if seqsum is None:
        return True         # it died, timed out, or was never heard from
    if "restore" not in seqsum:
        # "busy" (another sweep has the box) and usage errors answer before
        # the first switch; any other summary without a restore is unknown
        return not (seqsum.get("busy") or seqsum.get("error"))
    return seqsum.get("restore") != 0


def box_caller(a):
    """reap()'s call: one command per fresh connection (see box_cmd)."""
    return lambda cmd, timeout: box_cmd(a, cmd, timeout)


async def fallback_restore(a, pace, desktop=None, spare=None, stop_sent=False, intr=None):
    """modeseq's summary was not heard - a second signal, a dropped
    connection, a host timeout, the agent's EXECW timeout - or it did not
    report giving the mode back. vcrctl is usually STILL RUNNING and pacing
    (the agent's tree kill is best effort; a host timeout kills nothing), and
    a kill now would skip its hold: XP drops the test mode at once, unstamped,
    perhaps a moment after a switch - two re-syncs back to back - and the old
    `taskkill /im` also killed any other session's vcrctl. So, in order:

      1. the stop file, unless it is already up: modeseq looks for it before
         every switch, ends the list and restores ONCE, paced;
      2. wait for vcrctl to leave on its own, bounded (2 x switch_s(pace) +
         MODESEQ_STEP_S + CIVIL_MARGIN_S: the wait before its next switch -
         the lock, then the pace - a mode already under test, the same wait
         before its restore, the restore); a third signal cuts it short;
      3. only then kill it, BY PID through `vcrctl pace-kill` - never one of
         `spare`, the vcrctl PIDs running before this sweep launched its own,
         and nothing at all when that list is unknown - and confirm it gone;
      4. the stamp (pace-kill's, or `vcrctl pace-mark`) and the pace on the
         host (reap());
      5. restore ONCE, under EXECW, and only if the desktop mode (`desktop`,
         read before the list) is not the current one. A restore is a switch
         too; retrying one is how a monitor gets a burst of them.

    -> the restore's answer ({"ok": true, "skipped": ...} when none was
    needed), or None when it was not safe to restore at all."""
    print("fallback: modeseq's summary was not heard, or it did not give the mode back",
          flush=True)
    call = box_caller(a)
    if not stop_sent:
        why = await send_stop(a)
        print(f"  fallback: stop file {STOP_FILE} "
              + ("uploaded" if why is None else f"NOT uploaded: {why}"), flush=True)
    bound = 2 * switch_s(pace) + MODESEQ_STEP_S + CIVIL_MARGIN_S
    ours = None
    if intr is not None:
        intr.waiting = True
    try:
        for k in range(int(bound // POLL_S) + 1):
            ours = await pids_of(call, VCRCTL_IMAGES, spare)
            if ours == []:
                break
            if intr is not None and intr.force.is_set():
                print("  fallback: no longer waiting for vcrctl to stop on its own", flush=True)
                break
            if k == 0:
                print(f"  fallback: waiting up to {bound:.0f} s for vcrctl to stop on its own "
                      f"({'PROCLIST did not answer' if ours is None else f'pid {ours}'})"
                      + (f"; {KILL_AFTER_SIGNALS} signals in all to kill it now" if intr else ""),
                      flush=True)
            await asyncio.sleep(POLL_S)
    finally:
        if intr is not None:
            intr.waiting = False
    if ours == []:
        print("  fallback: vcrctl has left on its own", flush=True)
    # still running: pace-killed by PID now. Gone already: reap finds nothing
    # to kill and only stamps the pace - its exit may have been the agent's
    # kill. An unknown pre-launch list (spare None) kills nothing at all.
    rep = await reap(call, VCRCTL_IMAGES, a.tool, pace, spare=spare)
    if not rep["gone"]:
        banner("NOT RESTORED", f"vcrctl could not be confirmed gone ({rep}) - a restore now "
               "would fight it; XP gives the desktop mode back when it exits")
        return None
    if rep["spared"]:
        banner("NOT RESTORED", f"a vcrctl that was running before this sweep (pid "
               f"{rep['spared']}, not ours) still is - a restore now could fight it")
        return None
    # nothing of ours is left to consume the stop file, and one left up would
    # end the next run before its first switch
    try:
        await box_cmd(a, f"DELETE {STOP_FILE}", 30)
    except Exception:
        pass
    current = None
    try:
        _, out = await box_cmd(a, f"EXEC {a.tool} modes", 90)
        current = (jline(out) or {}).get("current")
    except Exception as e:
        print(f"  fallback: cannot read the current mode ({type(e).__name__}: {e})", flush=True)
    if desktop and current == desktop:
        r = {"ok": True, "skipped": f"{current} is the desktop mode already - no restore"}
        print("fallback restore:", json.dumps(r), flush=True)
        return r
    r = await restore_once(call, a.tool)
    print("fallback restore:", json.dumps(r), flush=True)
    if not (r and r.get("ok")):
        banner("NOT RESTORED", "the fallback restore failed - look at the box; it was not retried")
    return r


class Interrupts:
    """SIGINT/SIGTERM while the box is switching must not abandon it mid-list:
    the 2026-09-26 sweep on .124 was stopped part-way (~126 of ~250 re-syncs),
    and a host that just dies mid-EXECW leaves vcrctl to run its list to the
    end. So a signal escalates, one step at a time, and nothing is killed
    before the THIRD:
      1st  asks vcrctl to stop (STOP_FILE) - it restores once, paced;
      2nd  stops waiting for the EXECW's reply; the fallback takes over and
           first waits, bounded, for vcrctl to stop on its own;
      3rd  cuts that wait short - vcrctl is killed by PID, the pace stamped,
           at most one restore.
    After the wait, while the kill / restore decision runs, signals are
    noted, not obeyed - that restore is what leaves the monitor in its
    desktop mode."""

    def __init__(self):
        self.seen = []
        self.stop = asyncio.Event()
        self.force = asyncio.Event()     # the third signal: stop waiting, kill
        self.execw = None
        self.restoring = False           # the fallback / restore decision is running
        self.waiting = False             # ... in its bounded wait for vcrctl to leave
        self.loop = asyncio.get_running_loop()

    def __enter__(self):
        for s in (signal.SIGINT, signal.SIGTERM):
            self.loop.add_signal_handler(s, self.on_signal, s.name)
        return self

    def __exit__(self, *exc):
        for s in (signal.SIGINT, signal.SIGTERM):
            self.loop.remove_signal_handler(s)

    def on_signal(self, name):
        self.seen.append(name)
        n = len(self.seen)
        if self.restoring and not self.waiting:
            print(f"\n{name}: noted - the restore decision finishes first", flush=True)
            return
        if n >= KILL_AFTER_SIGNALS:
            print(f"\n{name} (#{n}): no longer waiting for vcrctl to stop on its own - it is "
                  "killed by PID, the pace stamped, then at most one restore", flush=True)
            self.force.set()
        elif self.waiting:
            print(f"\n{name}: noted - still waiting for vcrctl to stop on its own "
                  f"({KILL_AFTER_SIGNALS - n} more to kill it instead)", flush=True)
            return
        elif n == 1:
            print(f"\n{name}: stopping the list on the box - vcrctl stops before its next "
                  "switch and restores once, paced", flush=True)
            self.stop.set()
            return
        if self.execw is not None and not self.execw.done():
            if n < KILL_AFTER_SIGNALS:
                print(f"\n{name} again: no longer waiting for the reply (the stop file stands; "
                      "nothing is killed before a third signal)", flush=True)
            self.execw.cancel()


async def main_async(a):
    if a.test_bed and not is_loopback(a.host):
        refuse(f"--test-bed is for the VM test bed behind a loopback port; {a.host} is a real box")
        return 2
    c = RetroConnection(a.host, a.port)
    await c.connect(SECRET, timeout=20)
    tool = a.tool
    events = vcrlog.load_events()

    async def run(cmd, timeout=60):
        st, d = await c.send_command(f"EXEC {tool} {cmd}", timeout=timeout)
        return d.decode("ascii", "replace")

    results, lines, seqsum, intr = [], [], None, None
    restore_failed = False
    try:
        info = jline(await run("info"))
        after = info.get("log_next_seq", 0) if info and info.get("ok") else 0
        listed = jline(await run("modes")) or {}
        if not isinstance(listed.get("modes"), list):
            print("vcrctl modes gave no mode list - nothing to sweep")
            return 1
        # the mode to come back to: a fallback restores only when this is not
        # what the box shows afterwards (a restore is one more switch)
        desktop = listed.get("current")
        # XP adds its VGA driver's 4 bpp modes to the list; they are not ours
        modes = [m for m in listed["modes"] if int(m.split("@")[0].split("x")[2]) >= 8]
        golden = {}
        if a.golden:
            g = json.load(open(a.golden))
            golden = {c["tag"]: c for c in g["captures"] if c.get("ok") and "vga" in c}
            modes = [m for m in modes if m in golden]
        if a.filter:
            modes = [m for m in modes if m.split("@")[0].endswith("x" + a.filter)]
        if a.limit:
            modes = modes[:a.limit]
        if a.modes:
            want = [m.strip() for m in a.modes.split(",") if m.strip()]
            junk = [m for m in want if not re.fullmatch(r"\d+x\d+x\d+@\d+", m)]
            if junk:
                refuse("not WxHxBPP@HZ: " + ", ".join(junk))
                return 2
            missing = [m for m in want if m not in modes]
            if missing:
                print(f"not offered (driver list{' / vendor golden' if golden else ''}): "
                      + ", ".join(missing))
            modes = [m for m in want if m in modes]
        if not modes:
            print("0 modes to sweep - nothing switched")
            return 1
        if len(modes) > a.max_live and not a.allow_many:
            refuse(f"{len(modes)} live modes is {len(modes) + 1} monitor re-syncs; the limit is "
                   f"{a.max_live} modes (--allow-many to override). For the register check of "
                   "every vendor mode use tools/golden_compare.py - it needs no monitor.")
            return 2
        rng = calc = None
        if a.test_bed:
            banner("TEST BED", f"{a.host} is a VM with no monitor - the EDID gate is not applied")
        else:
            rng, calc, why = gate_modes(info, modes)
            if why:
                refuse(why)
                return 2
        pace = max(a.pace, 3.0)
        if pace > PACE_MAX_S:
            # vcrctl refuses it too (before any switch); say so here, plainly
            refuse(f"--pace {a.pace:g} is above vcr_pace.h's {PACE_MAX_S:g} s maximum")
            return 2
        # the gate's worst case for every switch, the tests, start-up
        # (modeseq_budget): the agent's tree kill at this budget would revert
        # the mode unpaced, so it must cover all of it, and a list that needs
        # more than the ceiling is refused
        budget = modeseq_budget(len(modes), pace)
        why = budget_refusal(budget, f"{len(modes)} modes at {pace:.0f} s")
        if why:
            refuse(f"{why} (fewer modes, or a shorter --pace)")
            return 2
        switches = len(modes) + 1
        print(f"plan: {len(modes)} modes in one vcrctl process, {pace:.0f} s before every "
              f"switch: {switches} mode switches incl. the restore = "
              f"{TIMING_CHANGES_PER_SWITCH * switches} timing changes on the cable (each "
              f"switch passes through the 31.5 kHz VGA reset), up to {budget} s (a busy "
              f"switch lock can add {PACE_LOCK_S:.0f} s per switch)"
              + (f"\n  monitor {describe(rng)}" if calc else ""))
        for m in modes:
            print(f"  {m:>18}  {rates(calc, m)}")
        # a stop file left by an interrupted earlier run would end this one
        # before its first switch (an absent file answers an error: fine)
        await c.send_command(f"DELETE {STOP_FILE}", timeout=30)
        # the vcrctl PIDs already running are not ours: a fallback kill spares
        # them (None - PROCLIST did not answer - means it cannot tell, and says so)
        spare = await pids_of(box_caller(a), VCRCTL_IMAGES)
        argv = " ".join(" ".join(m.replace("@", "x").split("x")) for m in modes)
        cmd = f"EXECW {budget} {tool} modeseq {int(pace * 1000)} {argv}"

        with Interrupts() as intr:
            out, trouble, stop_sent, timed_out = "", None, None, False
            try:
                intr.execw = asyncio.create_task(c.send_command(cmd,
                                                                timeout=budget + HOST_SLACK_S))
                waiter = asyncio.create_task(intr.stop.wait())
                await asyncio.wait({intr.execw, waiter}, return_when=asyncio.FIRST_COMPLETED)
                waiter.cancel()
                if not intr.execw.done():
                    why = await send_stop(a)
                    stop_sent = why is None
                    print(f"stop file {STOP_FILE} " + ("uploaded" if stop_sent
                                                       else f"NOT uploaded: {why}"), flush=True)
                    await asyncio.wait({intr.execw})
                if intr.execw.cancelled():
                    trouble = "stopped waiting for vcrctl after a second signal"
                elif intr.execw.exception() is not None:
                    e = intr.execw.exception()
                    trouble = f"EXECW failed: {type(e).__name__}: {e}"
                else:
                    out = intr.execw.result()[1].decode("ascii", "replace")
                    if EXECW_TIMED_OUT in out:
                        timed_out = True
                        trouble = ("the agent's EXECW timed out and tried to kill vcrctl's "
                                   "process tree (best effort - it may still be running)")
            finally:
                # The restore decision comes first, straight after the EXECW,
                # whatever happened to it: the box may be in a temporary mode.
                intr.restoring = True
                lines = json_lines(out)
                seqsum = next((j for j in lines if j.get("cmd") == "modeseq"), None)
                if trouble:
                    print(f"modeseq: {trouble}", flush=True)
                # a timed-out run goes through the fallback even with a summary:
                # a vcrctl that outlived the tree kill still holds the mutex
                if restore_needed(seqsum) or timed_out:
                    r = await fallback_restore(a, pace, desktop=desktop, spare=spare,
                                               stop_sent=bool(stop_sent), intr=intr)
                    restore_failed = not (r and r.get("ok"))
        if seqsum and seqsum.get("busy"):
            refuse("another modeseq is running on the box - one sweep at a time")
            return 2
        if stop_sent and not (seqsum and seqsum.get("stopped")):
            # vcrctl had already finished: the file would stop the next run
            await box_cmd(a, f"DELETE {STOP_FILE}", 30)
        tests = {j.get("index"): j for j in lines if j.get("cmd") == "modetest"}
        ran = seqsum.get("ran", len(modes)) if seqsum else len(modes)
        _, logtxt = await box_cmd(a, f"EXEC {tool} log {after}")
        _, ents = vcrlog.parse_tsv(logtxt)
        lo = after
        for i, m in enumerate(modes):
            mt = tests.get(i) or {}
            if not mt and isinstance(ran, int) and i >= ran:
                results.append({"mode": m, "ok": False, "skipped": "the list stopped before it"})
                print(f"  {m:>18}: not run (the list stopped before it)")
                continue
            hi = mt.get("log_next_seq", lo)
            mine = [e for e in ents if lo <= e["seq"] < hi]
            lo = hi
            sm = {"ok": mt.get("current") == m, "current": mt.get("current"),
                  "result": mt.get("result")}
            gdi = mt.get("gdi")
            gdi = dict(gdi, ok=gdi.get("mismatches") == 0) if isinstance(gdi, dict) else None
            bad = [e for e in mine if e["level"] <= 1]
            regdiff = []
            if m in golden and sm.get("ok"):
                snap = mt.get("snapshot")
                regdiff = against_golden(snap, golden[m]) if snap else ["no snapshot"]
            ok = bool(sm.get("ok") and gdi and gdi.get("ok") and not bad and not regdiff)
            row = {"mode": m, "ok": ok, "setmode": sm, "gdi": gdi, "vs_vendor": regdiff,
                   "log_problems": [vcrlog.format_entry(e, events) for e in bad]}
            results.append(row)
            print(f"  {m:>18}: {'ok' if ok else 'FAIL'}"
                  f"{'' if ok else '  ' + json.dumps({k: row[k] for k in ('setmode', 'gdi')})}")
            for p in row["log_problems"]:
                print("      " + p)
            if regdiff:
                print("      registers (ours/vendor): " + " ".join(regdiff))
        late = [e for e in ents if e["seq"] >= lo and e["level"] <= 1]
        for e in late:
            print("  after the last mode: " + vcrlog.format_entry(e, events))
        print("modeseq:", json.dumps(seqsum) if seqsum else "NO SUMMARY - " + out.strip()[-300:])
    finally:
        await c.close()
    if a.out:
        Path(a.out).write_text(json.dumps(results, indent=1))
    failed = [r["mode"] for r in results if not r["ok"] and "skipped" not in r]
    unrun = [r["mode"] for r in results if "skipped" in r]
    print(f"{len(results) - len(failed) - len(unrun)}/{len(results)} modes passed"
          + (f"; failed: {', '.join(failed)}" if failed else "")
          + (f"; not run: {', '.join(unrun)}" if unrun else ""))
    if intr and intr.seen:
        print(f"INTERRUPTED ({', '.join(intr.seen)})")
        return 130
    return 1 if failed or restore_failed else 0


def main():
    ap = argparse.ArgumentParser(
        description="Switch through a short, paced list of modes inside the monitor's EDID "
                    "ranges and prove each one (one vcrctl modeseq process on the box).")
    ap.add_argument("host")
    ap.add_argument("--port", type=int, default=9898)
    ap.add_argument("--tool", default=r"C:\vcr\vcrctl.exe")
    ap.add_argument("--filter", help="only this bpp (8/16/32)")
    ap.add_argument("--limit", type=int)
    ap.add_argument("--out")
    ap.add_argument("--golden", help="golden capture: vendor-captured modes only + register check")
    ap.add_argument("--modes", help="comma list WxHxBPP@HZ - only these (live runs: keep it short)")
    ap.add_argument("--pace", type=float, default=5.0, help="seconds before every switch (min 3)")
    ap.add_argument("--max-live", type=int, default=12)
    ap.add_argument("--allow-many", action="store_true",
                    help="more than --max-live re-syncs (NOT on a CRT you care about)")
    ap.add_argument("--test-bed", action="store_true",
                    help="the VM test bed (loopback host only): no monitor, no EDID gate")
    a = ap.parse_args()
    try:
        sys.exit(asyncio.run(main_async(a)))
    except KeyboardInterrupt:
        # outside the switching window (before the EXECW, or reading the log
        # after it): nothing is left switched on the box
        print("\ninterrupted - no sequence was running on the box")
        sys.exit(130)


if __name__ == "__main__":
    main()
