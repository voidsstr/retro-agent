"""vcr-kmd live testing must be gentle with the monitor.

Every mode switch is a re-sync of the monitor, and on a CRT a change of
horizontal-frequency band clicks its mode relays and steps the high voltage.
On 2026-09-26 the first silicon battery on .124 (a 1998 Sony CPD-G200) ran
mode_sweep over all 123 vendor modes with a return to the desktop after each:
~250 re-syncs at two a second. The user heard it and it was stopped at ~126.

The register check of every vendor mode needs no monitor at all -
tools/golden_compare.py runs the driver's own mode math against the capture on
the dev host (123/123 identical). So:

- ONE gate, tools/vcr_pace.h: at least 3 s between any two switches on the
  box (the stamp is a file, so it holds across processes) and a temporary mode
  HELD 3 s before it is given back, by the tool or by XP at its exit or
  crash. Every switch site in vcrctl / ddlab / d3dprobe / glidelab goes
  through it, and every tool that includes it is rebuilt when it changes. Its
  first two versions FAILED OPEN, so now it fails CLOSED: only a stamp that
  is not there means "never switched" (one that cannot be read waits a whole
  floor), the stamp is written in place - BEFORE the switch as well as after
  it - every switch is remembered in-process too, a named mutex is held from
  the wait to the stamp and a tool that cannot have it within 20 s does NOT
  switch ("pace lock busy"), the stamp is read again after every sleep of at
  most a second and a floor that never passes is given up after 120 s - with
  no switch - a crash filter holds the mode, an exit hold stamps XP's revert
  AHEAD (it lands after the process is gone) and a stamp ahead is waited out
  to its time, and a --pace outside 0..30000 ms is refused, never wrapped. A
  switch the gate refuses is NOT made, by any route: no Release, no
  RestoreDisplayMode, no grSstWinClose that would give a held mode back
  unpaced - XP's revert at exit is left to the exit hold. A lab whose window
  loses the foreground while it holds a mode records that unpaced switch and
  ends the run ("focus_lost"). What no process can pace is a KILL, so the host
  kills through the gate, `vcrctl pace-kill <pid>` (only our own switching
  tools), or runs `vcrctl pace-mark` after any other death (the gate's own
  behaviour: tests/native/test_vcr_kmd_pace.c);
- `vcrctl modeseq` switches modes in ONE process (no bounce back to the
  desktop between modes), at most 16 a run, one run at a time (a mutex), and
  stops civilly on a host-created stop file; glidelab caps its open/close
  cycles at 3, hands Glide its refresh as FX_GLIDE_REFRESH and fails a
  session that opened any other ("opened_hz");
- the driver: RESET_DEVICE resets to VGA (as at HEAD) - a shortcut that left
  the old mode scanning was REJECTED, because RESET_DEVICE is also XP's
  hand-off to VgaSave - so a mode change is TWO timing changes on the cable,
  and the host plans count both. A bugcheck under SLI/AA first gives the
  master its own video clock back (vcr_sli_reset_video: the video half of the
  disable, no 3D writes). The mode list is never unfiltered for want of an
  EDID: a range-less EDID gets only its OWN persisted range, no EDID gets the
  envelope of every monitor the box has read BOUNDED BY the conservative
  default (H 30-48 kHz, V 50-75 Hz, 80 MHz) unless Diag\\MonTrustEnvelope=1,
  and whose limits are in force is `vcr_info.mon_src`, appended at the end of
  the struct (the selection itself: tests/native/test_vcr_kmd_edid.c). A
  desktop mode the list no longer holds falls back to a listed one instead
  of failing the PDEV;
- the host scripts refuse to switch unless the driver read the monitor's EDID
  and filters by THIS monitor's own range (mon_src 1 or 2), under another
  driver only on a saved `vcrctl info` whose monitor the box's registry
  confirms, check every mode against its ranges on the host with the
  driver's own mode math (a run at refresh 0 at the highest refresh the
  driver lists), cap and pace every live run, give each EXECW the gate's
  worst case per switch (the lock wait included) and never plain EXEC for a
  switch, and turn SIGINT/SIGTERM into a stop file instead of abandoning a
  sweep mid-list. A run that did not end civilly is not killed first: the
  stop file, a bounded wait for the tool to leave on its own, only then a
  pace-kill BY PID (never `/im`, never PROCKILL, never a process that
  predates the run, nothing at all when the pre-launch list is unknown), the
  stamp, the pace, and at most ONE restore, never retried; a timed-out,
  wedged, focus-losing or lock-busy session ends the whole sweep or battery's
  switching.
Source-level, host-compiled and fake-agent checks only: nothing here touches
a box (an autouse guard fails any test that tries to open a socket).
"""
import argparse
import ast
import asyncio
import json
import re
import shutil
import socket
import subprocess
import sys
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
KMD = REPO / "voodoo-cleanroom" / "vcr-kmd"
TOOLS = KMD / "tools"
MINIPORT = KMD / "miniport"
NATIVE = REPO / "tests" / "native"
sys.path.insert(0, str(TOOLS))

import d3dprobe_run  # noqa: E402
import ddlab_run  # noqa: E402
import glidelab_run  # noqa: E402
import glidelab_sweep  # noqa: E402
import golden_capture  # noqa: E402
import lab_run  # noqa: E402
import mode_sweep  # noqa: E402
import silicon_battery  # noqa: E402
import sli_golden  # noqa: E402
import sli_golden_sweep  # noqa: E402
import sli_shot  # noqa: E402

# the real one: the fakes below replace asyncio.sleep (the scripts' pace
# waits) with a recorder, and a fake agent that must yield to the event loop
# still needs a sleep that does
_REAL_SLEEP = asyncio.sleep

PACE_H = (TOOLS / "vcr_pace.h").read_text()
VCRCTL = (TOOLS / "vcrctl.c").read_text()
DDLAB = (TOOLS / "ddlab.c").read_text()
D3DPROBE = (TOOLS / "d3dprobe.c").read_text()
GLIDELAB = (TOOLS / "glidelab.c").read_text()
LABS_C = (("ddlab.c", DDLAB), ("d3dprobe.c", D3DPROBE), ("glidelab.c", GLIDELAB))


def _labs():
    """LABS_C as the module holds them NOW (a test body reads this, so a
    source swapped in by a mutation check is the one checked)."""
    return (("ddlab.c", DDLAB), ("d3dprobe.c", D3DPROBE), ("glidelab.c", GLIDELAB))
SWEEP = (TOOLS / "mode_sweep.py").read_text()
BATTERY = (TOOLS / "silicon_battery.py").read_text()
EDID_H = (KMD / "include" / "vcr_edid.h").read_text()
EDID_C = (KMD / "common" / "vcr_edid.c").read_text()
IOCTL_H = (KMD / "include" / "vcr_ioctl.h").read_text()
DDC = (MINIPORT / "vcrmp_ddc.c").read_text()
VCRMP = (MINIPORT / "vcrmp.c").read_text()
HW = (MINIPORT / "vcrmp_hw.c").read_text()
SLI = (MINIPORT / "vcrmp_sli.c").read_text()
VCRDD = (KMD / "display" / "vcrdd.c").read_text()
EDID_NATIVE = NATIVE / "test_vcr_kmd_edid.c"


# ---- nothing here may reach a box ----------------------------------------------


@pytest.fixture(autouse=True)
def _no_network(monkeypatch):
    """Every agent call in this file is a fake. A script that slips past its
    fake (an unpatched snapshot(), a new helper opening its own connection)
    would try the network - and most of them swallow the error and carry on,
    so the attempt is recorded and FAILS the test rather than being blocked
    quietly."""
    tried = []

    def refuse(what):
        def boom(*a, **k):
            tried.append((what, a[1:2] or a[:1]))
            raise OSError(f"network blocked in the monitor-safety tests ({what})")
        return boom

    async def aboom(*a, **k):
        tried.append(("open_connection", a[:2]))
        raise OSError("network blocked in the monitor-safety tests (open_connection)")
    monkeypatch.setattr(socket.socket, "connect", refuse("connect"))
    monkeypatch.setattr(socket.socket, "connect_ex", refuse("connect_ex"))
    monkeypatch.setattr(socket, "create_connection", refuse("create_connection"))
    monkeypatch.setattr(socket, "getaddrinfo", refuse("getaddrinfo"))
    monkeypatch.setattr(asyncio, "open_connection", aboom)
    yield
    assert not tried, f"a test tried to reach the network: {tried}"


# ---- C source helpers --------------------------------------------------------


def _blank(src, strings=True):
    """src with comments (and string/char literals) blanked, offsets and
    newlines kept: an API named in a comment or a printf is not a call."""
    out, i, n = list(src), 0, len(src)
    while i < n:
        if src.startswith("/*", i):
            j = src.find("*/", i + 2)
            j = n if j < 0 else j + 2
        elif src.startswith("//", i):
            j = src.find("\n", i)
            j = n if j < 0 else j
        elif strings and src[i] in "\"'":
            j = i + 1
            while j < n and src[j] != src[i]:
                j += 2 if src[j] == "\\" else 1
            j += 1
        else:
            i += 1
            continue
        for k in range(i, min(j, n)):
            if out[k] != "\n":
                out[k] = " "
        i = j
    return "".join(out)


def _functions(code):
    """{name: (start, end)} of every top-level brace block of blanked C that
    is a function body (the name is the first `ident(` of its header)."""
    out, depth, start = {}, 0, 0
    for i, ch in enumerate(code):
        if ch == "{":
            if depth == 0:
                start = i
            depth += 1
        elif ch == "}":
            depth -= 1
            if depth == 0:
                head = re.split(r"\n\s*\n|[;}]", code[max(0, start - 400):start])[-1]
                m = re.search(r"(\w+)\s*\(", head)
                if m:
                    out[m.group(1)] = (start, i + 1)
    return out


def _body(src, name, strings=True):
    """The body of function `name` in src, comments blanked (and string
    literals, unless strings=False). Found on the fully blanked text: a brace
    inside a printf format is not a block."""
    s, e = _functions(_blank(src))[name]
    return _blank(src, strings)[s:e]


def _block_after(code, at):
    """The { ... } block that opens at or after offset `at`."""
    s = code.index("{", at)
    depth = 0
    for i in range(s, len(code)):
        depth += {"{": 1, "}": -1}.get(code[i], 0)
        if depth == 0:
            return code[s:i + 1]
    raise AssertionError("unbalanced braces")


def _c_func(src, name):
    """The whole definition of C function `name` - header from the start of
    its line through the closing brace - as SOURCE: what a host harness
    compiles, so the test runs the code the driver runs, not a copy."""
    code = _blank(src)
    m = re.search(rf"^[A-Za-z_][\w \t*]*\b{name}\s*\([^;{{}}]*\)\s*\{{", code, re.M)
    assert m, name
    body = _block_after(code, m.end() - 1)
    return src[m.start():m.end() - 1 + len(body)]


def _define(src, name):
    m = re.search(rf"#define\s+{name}\s+\(?(-?\d+)", src)
    assert m, name
    return int(m.group(1))


def _c_string(src, name):
    """The value of `#define name "..."`, C escapes decoded."""
    m = re.search(rf'#define\s+{name}\s+"((?:[^"\\]|\\.)*)"', src)
    assert m, name
    return m.group(1).encode().decode("unicode_escape")


def _c_array(src, name):
    """The bytes of `... name[] = { 0x.., ... };` in C source."""
    m = re.search(rf"\b{name}\s*\[\s*\d*\s*\]\s*=\s*\{{(.*?)\}};", src, re.S)
    assert m, name
    return bytes(int(x, 16) for x in re.findall(r"0x([0-9a-fA-F]{2})", m.group(1)))


def _native_runs(path):
    """(the TEST()s a native suite defines, the ones its MUNIT_MAIN RUNs).
    A behaviour pinned only by a test nobody runs is not pinned."""
    src = path.read_text()
    defined = set(re.findall(r"^TEST\((\w+)\)", src, re.M))
    main = src[src.index("MUNIT_MAIN("):]
    return defined, set(re.findall(r"\bRUN\((\w+)\)", main))


def _host_cc():
    """A host C compiler, or skip SAYING the harness did not run: a pin that
    silently does not run is not a pin."""
    cc = shutil.which("gcc") or shutil.which("cc") or shutil.which("clang")
    if not cc:
        pytest.skip("no host C compiler: this harness compiles the driver's own code and did "
                    "NOT run")
    return cc


PACE_BEFORE = r"\bvcr_pace_before_switch\s*\("
PACE_AFTER = (r"\bvcr_pace_after_switch\s*\(|\bvcr_pace_after_switch_ex\s*\(|"
              r"\bvcr_pace_after_restore\s*\(|\bpace_gave_back\s*\(")

# Every call that changes the display mode, per tool, and the tool's own
# helpers that are paced inside (a call to one of those ends the previous
# switch's stretch of code, so each switch must be wrapped on its own).
SWITCHES = {
    "vcrctl.c": (VCRCTL, r"ChangeDisplaySettings(?:Ex)?A|IDirectDraw7?_SetDisplayMode|"
                         r"IDirectDraw7?_RestoreDisplayMode", ("set_mode", "modetest_one")),
    "ddlab.c": (DDLAB, r"ChangeDisplaySettings(?:Ex)?A|IDirectDraw7?_SetDisplayMode|"
                       r"IDirectDraw7?_RestoreDisplayMode", ()),
    "d3dprobe.c": (D3DPROBE, r"IDirect3D8_CreateDevice|IDirect3DDevice8_Release|"
                             r"IDirect3DDevice8_Reset|ChangeDisplaySettings(?:Ex)?A",
                   ("release_device",)),
    "glidelab.c": (GLIDELAB, r"p_grSstWinOpen|p_grSstWinClose|p_grGlideShutdown|"
                             r"ChangeDisplaySettings(?:Ex)?A",
                   ("open_board", "close_board", "shutdown_glide")),
}


def _switch_sites(src, apis, wrappers):
    """[(function, api, before-stretch, after-stretch)] for every switch call:
    the code since the previous switch or paced helper call in the same
    function, and the code up to the next one."""
    code = _blank(src)
    funcs = _functions(code)
    out = []
    for fname, (s, e) in funcs.items():
        body = code[s:e]
        sites = [(m.start(), m.end(), m.group(1))
                 for m in re.finditer(rf"\b({apis})\s*\(", body)]
        marks = sorted(sites + [(m.start(), m.end(), None) for w in wrappers
                                for m in re.finditer(rf"\b{w}\s*\(", body)
                                if w != fname])
        for k, (a, b, api) in enumerate(marks):
            if api is None:
                continue
            prev = marks[k - 1][1] if k else 0
            nxt = marks[k + 1][0] if k + 1 < len(marks) else len(body)
            out.append((fname, api, body[prev:a], body[b:nxt]))
    return out


# ---- the gate itself: tools/vcr_pace.h -----------------------------------------


def test_the_pace_floor_is_three_seconds_on_a_file_on_the_box():
    # the stamp is a FILE on the box, not a static: a script running one tool
    # after another (setmode, golden, ddlab ...) is paced as if one process
    assert _define(PACE_H, "VCR_PACE_MIN_MS") >= 3000
    assert _c_string(PACE_H, "VCR_PACE_FILE").lower().startswith("c:\\vcr\\")
    wait = _body(PACE_H, "vcr_pace_wait")
    assert "vcr_pace_last_ms()" in wait and "Sleep(need)" in wait
    # a caller's --pace raises the floor, never lowers it - and never past
    # VCR_PACE_MAX_MS (it was atoi() into a DWORD: "-1" was a seven-week floor)
    set_min = _body(PACE_H, "vcr_pace_set_min")
    assert re.search(r"if \(ms > g_vcr_pace_ms\)\s*g_vcr_pace_ms = ms;", set_min)
    clamp = re.search(r"if \(ms > VCR_PACE_MAX_MS\)\s*ms = VCR_PACE_MAX_MS;", set_min)
    assert clamp and clamp.start() < set_min.index("g_vcr_pace_ms = ms;")
    assert _define(PACE_H, "VCR_PACE_MIN_MS") <= _define(PACE_H, "VCR_PACE_MAX_MS") <= 60000
    # the exit hold is armed by the switch itself, so every return path of a
    # program holding a temporary mode is covered
    after = _body(PACE_H, "vcr_pace_after_switch_ex")
    assert "atexit(vcr_pace_at_exit)" in after and "vcr_pace_mark()" in after
    assert re.search(r"#define vcr_pace_after_switch\(\)\s+vcr_pace_after_switch_ex\(1\)", PACE_H)
    assert re.search(r"#define vcr_pace_after_restore\(\)\s+vcr_pace_after_switch_ex\(0\)", PACE_H)
    # and its behaviour runs in the native suite
    assert (REPO / "tests" / "native" / "test_vcr_kmd_pace.c").exists()


def test_every_tool_that_switches_modes_includes_the_gate():
    """A new tool that switches must come through vcr_pace.h too: the
    2026-09-26 burst on .124 came from a sweep that had no pace of its own."""
    api = re.compile(r"\b(ChangeDisplaySettings(?:Ex)?[AW]?|\w*SetDisplayMode|\w*RestoreDisplayMode|"
                     r"IDirect3D[89]_CreateDevice|\w*grSstWinOpen)\s*\(")
    switching = [p.name for p in sorted(TOOLS.glob("*.c")) if api.search(_blank(p.read_text()))]
    assert set(switching) >= {"vcrctl.c", "ddlab.c", "d3dprobe.c", "glidelab.c"}
    for name in switching:
        assert '#include "vcr_pace.h"' in (TOOLS / name).read_text(), name


@pytest.mark.parametrize("tool", sorted(SWITCHES))
def test_every_switch_site_goes_through_the_gate(tool):
    src, apis, wrappers = SWITCHES[tool]
    sites = _switch_sites(src, apis, wrappers)
    assert sites, f"{tool}: no switch site found - the pattern no longer matches the code"
    for fname, api, before, after in sites:
        assert re.search(PACE_BEFORE, before) or (
            tool == "glidelab.c" and re.search(r"\bpace_before\s*\(", before)), \
            f"{tool}:{fname}: {api} is not preceded by vcr_pace_before_switch()"
        assert re.search(PACE_AFTER, after), \
            f"{tool}:{fname}: {api} is not followed by vcr_pace_after_switch/_restore"


def test_each_switch_api_is_called_where_the_gate_wraps_it():
    """One call site per open/close/release, inside its paced helper: a second,
    bare call is how a tool grows an unpaced switch."""
    def calls(src, api):
        return [f for f, a, _, _ in _switch_sites(src, api, ()) if a == api]
    assert calls(GLIDELAB, "p_grSstWinOpen") == ["open_board"]
    assert calls(GLIDELAB, "p_grSstWinClose") == ["close_board"]
    assert calls(GLIDELAB, "p_grGlideShutdown") == ["shutdown_glide"]
    assert re.search(PACE_BEFORE, _body(GLIDELAB, "pace_before"))
    assert calls(D3DPROBE, "IDirect3DDevice8_Release") == ["release_device"]
    assert calls(D3DPROBE, "IDirect3D8_CreateDevice") == ["main"]
    assert not calls(D3DPROBE, "IDirect3DDevice8_Reset")
    assert calls(VCRCTL, "ChangeDisplaySettingsA").count("set_mode") == 1
    for f in ("cmd_setmode", "cmd_golden", "modetest_one"):
        assert re.search(r"\bset_mode\s*\(", _body(VCRCTL, f)), f


def test_a_switch_in_is_recorded_even_when_it_failed():
    """A driver can program the CRTC and then fail the switch; the monitor
    re-synced all the same. Recording only successes is how a retry loop
    becomes a burst: nothing between the call and its record may branch on
    the result."""
    cases = ((VCRCTL, "set_mode", "ChangeDisplaySettingsA"),
             (VCRCTL, "cmd_ddraw", "IDirectDraw7_SetDisplayMode"),
             (DDLAB, "main", "IDirectDraw7_SetDisplayMode"),
             (D3DPROBE, "main", "IDirect3D8_CreateDevice"),
             (GLIDELAB, "open_board", "p_grSstWinOpen"))
    for src, fname, api in cases:
        body = _body(src, fname)
        for m in re.finditer(rf"\b{api}\s*\(", body):
            rest = body[body.index(";", m.end()) + 1:]
            rec = re.search(r"\bvcr_pace_after_switch\s*\(", rest)
            assert rec, f"{fname}: {api} never recorded"
            gap = rest[:rec.start()]
            assert not re.search(r"\breturn\b|FAILED|SUCCEEDED|DISP_CHANGE|\bctx\b", gap), \
                f"{fname}: {api} is recorded only on one outcome"


def test_no_exit_path_skips_the_hold():
    """vcr_pace.h holds a temporary mode from atexit(); anything that ends the
    process without running atexit must hold and stamp by itself."""
    for name, src in (("vcrctl.c", VCRCTL),) + _labs():
        code = _blank(src)
        assert not re.search(r"\b(abort|_exit|_Exit|TerminateProcess|quick_exit)\s*\(", code), name
        for m in re.finditer(r"\bExitProcess\s*\(", code):
            assert name == "glidelab.c", f"{name}: an ExitProcess skips the exit hold"
            before = code[max(0, m.start() - 600):m.start()]
            # glidelab `abandon`: the exit hold run by hand - it holds the
            # mode, stamps the revert AHEAD and gives the lock back
            assert re.search(r"\bvcr_pace_at_exit\s*\(\s*\)\s*;\s*$", before), \
                "glidelab abandon: ExitProcess without the exit hold"
    # the one TerminateProcess the tools may make is the gate's own paced
    # kill - in the header, and compiled only where it is asked for
    code = _blank(PACE_H)
    kills = [m.start() for m in re.finditer(r"\bTerminateProcess\s*\(", code)]
    s, e = _functions(code)["vcr_pace_kill"]
    assert kills and all(s <= k < e for k in kills), "a TerminateProcess outside vcr_pace_kill"
    guard = PACE_H.index("#ifdef VCR_PACE_WANT_KILL")
    assert guard < s and PACE_H.index("#endif /* VCR_PACE_WANT_KILL */") > e
    # only vcrctl asks for it, before the include; no lab can kill
    assert re.search(r'#define VCR_PACE_WANT_KILL\s*\n#include "vcr_pace.h"', VCRCTL)
    for name, src in _labs():
        assert "VCR_PACE_WANT_KILL" not in _blank(src), name


@pytest.mark.parametrize("name,src", (("vcrctl.c", VCRCTL),) + LABS_C)
def test_a_crash_dies_at_once_instead_of_holding_a_mode_behind_a_dialog(name, src):
    """The tools run hidden under EXEC on a box nobody sits at: a Watson box
    would keep the process - and a fullscreen mode - alive until someone
    clicked it. SetErrorMode comes before anything that can switch."""
    body = _body(src, "main")
    m = re.search(r"SetErrorMode\(SEM_FAILCRITICALERRORS \| SEM_NOGPFAULTERRORBOX\)", body)
    assert m, name
    first = re.search(r"\bfor\s*\(|\bGetDC\s*\(|\bset_mode\s*\(|\bopen_board\s*\(", body)
    assert first and m.start() < first.start(), f"{name}: SetErrorMode is not first"


@pytest.mark.parametrize("name,src", LABS_C)
def test_a_labs_pace_option_can_only_raise_the_floor_and_is_refused_not_wrapped(name, src):
    """`--pace` was atoi() into a DWORD: "-1" became a floor of seven weeks,
    "5s" silently 5 ms. It goes through vcr_pace_parse_ms (plain decimal,
    0..VCR_PACE_MAX_MS), raises the floor through vcr_pace_set_min, and a
    value it refuses ends the run with a RESULT error before anything is
    created or switched."""
    code = _blank(src, strings=False)
    branch = re.search(r'"--pace"\) && v\) \{\s*if \(vcr_pace_parse_ms\(v, &pace\)\)\s*'
                       r"vcr_pace_set_min\(pace\);\s*else\s*bad_pace = v;\s*i\+\+;\s*\}", code)
    assert branch, name
    assert code.count('"--pace"') == 1                    # no second, older branch
    assert not re.search(r"vcr_pace_set_min\s*\(\s*\(DWORD\)\s*(atoi|strtoul)", code), name
    main = _body(src, "main")
    refuse = main.index("if (bad_pace) {")
    block = _block_after(main, refuse)
    assert re.search(r"\breturn\b", block) and "RESULT" in _block_after(
        _body(src, "main", strings=False), refuse), name
    first = re.search(r"\b(DirectDrawCreateEx|Direct3DCreate8|LoadLibraryA|vcr_pace_before_switch|"
                      r"pace_before|open_board)\s*\(", main)
    assert first and refuse < first.start(), f"{name}: a bad --pace is refused too late"


def test_vcrctl_modeseq_refuses_a_pace_it_cannot_parse_before_anything_switches():
    body = _body(VCRCTL, "cmd_modeseq")
    m = re.search(r"if \(!vcr_pace_parse_ms\(argv\[2\], &pace\)\)\s*return fail\(", body)
    assert m
    assert m.start() < body.index("CreateMutexA(") < body.index("vcr_pace_set_min(pace);")
    assert not re.search(r"\bstrtoul\s*\(\s*argv\[2\]|\batoi\s*\(\s*argv\[2\]", body)


def test_the_pace_argument_parser_refuses_rather_than_wraps():
    parse = _body(PACE_H, "vcr_pace_parse_ms")
    # a first character that is not a digit (a sign, a space, "0x" starts
    # with a digit but stops at 'x' - trailing junk) is refused ...
    assert re.search(r"if \(!s \|\| \*s < '0' \|\| \*s > '9'\)\s*return 0;", _body(
        PACE_H, "vcr_pace_parse_ms", strings=False))
    # ... so is anything past VCR_PACE_MAX_MS, checked digit by digit (no
    # overflow can wrap it back into range) ...
    assert re.search(r"if \(v > VCR_PACE_MAX_MS\)\s*return 0;", parse)
    # ... and any trailing character
    assert re.search(r"if \(\*s\)\s*return 0;", parse)
    # the host refuses the same range before it asks
    assert mode_sweep.PACE_MAX_S * 1000 == _define(PACE_H, "VCR_PACE_MAX_MS")


# The gate's first versions failed OPEN (adversarial reviews, 2026-09-26):
# each hole let a switch follow another at once. The behaviour is the native
# suite's; these pin that the header still has the mechanism and that the
# native suite still RUNS a test for each hole.
PACE_NATIVE = NATIVE / "test_vcr_kmd_pace.c"
PACE_NATIVE_TESTS = {
    # the stamp fails closed
    "a_stamp_that_exists_but_cannot_be_read_waits_the_whole_floor",
    "a_short_torn_or_zeroed_stamp_waits_the_whole_floor",
    "c_vcr_as_a_file_waits_the_whole_floor_and_says_the_stamp_is_lost",
    "only_a_missing_stamp_means_never_switched",
    "the_stamp_is_written_in_place_never_truncated_first",
    # the in-process copy
    "a_failed_stamp_write_paces_this_process_says_so_and_retries_at_exit",
    # the re-read loop, in chunks, to a hard cap
    "the_wait_reads_the_stamp_again_after_every_sleep",
    "a_stamp_that_never_stops_moving_cannot_stall_a_tool_forever",
    "the_wait_sleeps_in_chunks_and_reads_the_stamp_after_each",
    # the mutex - fail closed: a stuck holder is a refusal, not a go
    "the_switch_lock_is_held_from_the_wait_to_the_stamp",
    "the_switch_lock_is_balanced_however_the_calls_nest",
    "a_stuck_holder_cannot_deadlock_a_queued_tool_or_let_it_beat_the_floor",
    "a_lock_abandoned_by_a_killed_tool_is_acquired_and_waits_a_floor",
    "a_tool_refused_on_the_way_out_still_gives_its_mode_back_through_the_exit_hold",
    # round 3: the pre-stamp, nesting, the future stamp, the argument
    "the_stamp_is_written_before_the_switch_as_well_as_after_it",
    "a_before_call_nested_in_one_that_waited_does_not_wait_again",
    "a_stamp_ahead_is_waited_out_to_its_time_and_a_floor_past_it",
    "pace_arguments_are_refused_or_clamped_never_wrapped",
    # the kill, and the host's remedies
    "pace_mark_after_a_kill_measures_the_next_switch_from_the_revert",
    "pace_kill_refuses_anything_but_our_own_tools",
    "pace_kill_is_made_as_a_paced_switch",
    "pace_kill_kills_nothing_when_the_lock_is_busy",
    "pace_kill_says_when_the_victim_is_not_gone_or_cannot_be_killed",
    # the exception filter
    "a_crash_holding_a_temporary_mode_holds_it_stamps_and_ends_the_process",
    "a_crash_in_the_middle_of_a_switch_is_held_as_one",
    "a_crash_with_no_mode_held_is_left_to_the_filter_before",
    "a_filter_that_repaired_the_fault_keeps_the_pacing_state",
    # round 1's
    "the_floor_holds_across_processes", "a_clock_that_went_backwards_waits_the_whole_floor",
    "set_min_raises_the_floor_and_never_lowers_it",
    "a_temporary_mode_is_held_at_exit_and_the_revert_stamped",
    "a_mode_given_back_is_not_held_again_at_exit",
}


def test_the_native_suite_runs_the_real_gate_and_a_test_for_every_hole():
    src = PACE_NATIVE.read_text()
    # the true header, not a copy of it - with the paced kill compiled in
    assert re.search(r'#define VCR_PACE_WANT_KILL\s*\n'
                     r'#include "\.\./\.\./voodoo-cleanroom/vcr-kmd/tools/vcr_pace\.h"', src)
    defined, run = _native_runs(PACE_NATIVE)
    assert PACE_NATIVE_TESTS <= defined, sorted(PACE_NATIVE_TESTS - defined)
    assert PACE_NATIVE_TESTS <= run, sorted(PACE_NATIVE_TESTS - run)


def test_the_stamp_fails_closed():
    """Only a stamp that is NOT THERE means "never switched". The first cut
    read any failure as "long ago": a DOWNLOAD or a virus scanner holding the
    file, C:\\vcr being a file, a 0-byte stamp caught mid-truncate."""
    last = _body(PACE_H, "vcr_pace_last_ms")
    assert re.search(r"if \(err == ERROR_FILE_NOT_FOUND\)\s*return VCR_PACE_NEVER;", last)
    assert re.search(r"if \(err != ERROR_PATH_NOT_FOUND\)\s*return VCR_PACE_UNKNOWN;", last)
    # no C:\vcr is "never" only when there is no FILE called C:\vcr either
    assert re.search(r"if \(GetFileAttributesA\(VCR_PACE_DIR\) != INVALID_FILE_ATTRIBUTES\)"
                     r"\s*return VCR_PACE_UNKNOWN;", last)
    # there, but short, torn or zero: unknown, not never
    assert re.search(r"got != sizeof t \|\| t == VCR_PACE_NEVER", last)
    assert re.search(r"t = VCR_PACE_UNKNOWN;", last)
    # the wait: unknown is a whole floor - a DEADLINE from when it was first
    # seen, so sleeping in chunks neither ends it early nor restarts it
    wait = _body(PACE_H, "vcr_pace_wait")
    assert re.search(r"if \(last == VCR_PACE_UNKNOWN\) \{\s*key = VCR_PACE_UNKNOWN;", wait)
    assert re.search(r"if \(key != blind\) \{\s*blind = key;\s*blind_at = now;\s*\}", wait)
    assert re.search(r"if \(blind && \(n = vcr_pace_rest\(blind_at, now, full\)\) > need\)"
                     r"\s*need = n;", wait)
    # written in place: CREATE_ALWAYS truncated first, and a reader in that
    # gap saw 0 bytes; nothing anywhere in the gate may truncate
    store = _body(PACE_H, "vcr_pace_store")
    assert "OPEN_ALWAYS" in store
    assert not re.search(r"\b(CREATE_ALWAYS|TRUNCATE_EXISTING|SetEndOfFile|DeleteFileA)\b",
                         _blank(PACE_H))
    # a failed write is SAID (stderr: the tools answer on stdout) and remembered
    assert "g_vcr_pace_unsaved = !ok;" in store and re.search(r"if \(!ok\)\s*VCR_PACE_REPORT\(",
                                                             store)
    assert "fprintf(stderr" in _blank(PACE_H, strings=False)


def test_every_switch_is_remembered_in_process_too():
    """A stamp write that fails must still pace THIS process, and the next
    tool gets one more try at exit."""
    mark = _body(PACE_H, "vcr_pace_mark")
    # the copy first, whatever becomes of the file
    assert re.search(r"g_vcr_pace_mine = vcr_pace_now_ms\(\);\s*return vcr_pace_store\("
                     r"g_vcr_pace_mine\);", mark)
    wait = _body(PACE_H, "vcr_pace_wait")
    # the wait is measured from the later of the box's stamp and this copy
    assert "vcr_pace_fold(last, now, full, &need, &key);" in wait
    assert "vcr_pace_fold(g_vcr_pace_mine, now, full, &need, &key);" in wait
    at_exit = _body(PACE_H, "vcr_pace_at_exit")
    assert re.search(r"else if \(g_vcr_pace_unsaved\) \{\s*vcr_pace_store\(g_vcr_pace_mine\);",
                     at_exit)


def test_one_switch_at_a_time_on_the_box_and_no_switch_without_the_lock():
    """Two tools read the same stamp, slept once, woke together and both
    switched: a named mutex is held from the wait to the stamp. Round 2 went
    on WITHOUT the lock after a floor when it could not have it - but a
    holder stuck that long is hung in a driver call and may switch at any
    moment. Now the lock is not optional: no lock, no switch."""
    assert _c_string(PACE_H, "VCR_PACE_MUTEX")
    lock = _body(PACE_H, "vcr_pace_lock")
    assert "CreateMutexA(NULL, FALSE, VCR_PACE_MUTEX)" in lock
    # bounded: a stuck holder must not stall every tool on the box ...
    assert "WaitForSingleObject(g_vcr_pace_lock, VCR_PACE_LOCK_MS)" in lock
    assert _define(PACE_H, "VCR_PACE_LOCK_MS") >= _define(PACE_H, "VCR_PACE_MIN_MS")
    # ... an abandoned lock (its owner was killed) is acquired, and counted ...
    assert re.search(r"if \(got == WAIT_OBJECT_0 \|\| got == WAIT_ABANDONED\)\s*"
                     r"g_vcr_pace_depth\+\+;", lock)
    assert re.search(r"if \(got == WAIT_OBJECT_0\)\s*return 1;", lock)
    # ... anything but a clean acquire is a switch just now (a whole floor),
    # and only the abandoned lock counts as HELD: a timeout, a failed
    # CreateMutex or WAIT_FAILED answer 0
    assert re.search(r"g_vcr_pace_mine = vcr_pace_now_ms\(\);\s*return got == WAIT_ABANDONED;\s*\}$",
                     lock)
    assert re.search(r"DWORD got = WAIT_FAILED;", lock)       # no mutex: not a WAIT_OBJECT_0
    before = _body(PACE_H, "vcr_pace_before_switch")
    # the refusal: said, and nothing held, nothing waited, nothing stamped
    refused = re.search(r"if \(!vcr_pace_lock\(\)\) \{\s*g_vcr_pace_why = VCR_PACE_WHY_LOCK;\s*"
                        r"return 0;\s*\}", before)
    assert refused and refused.end() < before.index("vcr_pace_wait()")
    assert before.index("vcr_pace_lock()") < before.index("vcr_pace_wait()")
    after = _body(PACE_H, "vcr_pace_after_switch_ex")
    # given back only once the stamp is written: the next holder reads it
    assert after.index("vcr_pace_mark()") < after.index("vcr_pace_unlock_to(0)")
    # never left to the next tool to find abandoned
    assert "vcr_pace_unlock_to(0);" in _body(PACE_H, "vcr_pace_at_exit")
    # a caller that waited and will not switch gives it back unstamped
    assert re.search(r"#define vcr_pace_cancel\(\)\s+vcr_pace_unlock_to\(0\)", PACE_H)
    # the words the host keys on are the header's
    assert _c_string(PACE_H, "VCR_PACE_WHY_LOCK") == "pace lock busy"
    assert _c_string(PACE_H, "VCR_PACE_WHY_WAIT") == "pace floor never passed"
    for why in ("pace lock busy", "pace floor never passed"):
        assert why in _blank(PACE_H, strings=False)
    assert mode_sweep.lab_halt({"error": _c_string(PACE_H, "VCR_PACE_WHY_LOCK")})
    # and the host's budgets queue behind that lock for exactly this long
    assert mode_sweep.PACE_LOCK_S * 1000 == _define(PACE_H, "VCR_PACE_LOCK_MS")


def test_the_switch_is_stamped_before_it_is_made():
    """The wait is over and the switch is imminent - and a switch call can
    take seconds. A reader measuring from the PREVIOUS stamp meanwhile could
    overtake it: the before-call stamps "now" under the lock, the after-call
    stamps again. A before-call nested in one that already waited (vcrctl
    modeseq waits, then set_mode paces the same switch) is the same switch
    and waits no second floor from that pre-stamp."""
    before = _body(PACE_H, "vcr_pace_before_switch")
    nested = re.search(r"if \(depth && g_vcr_pace_armed\) \{\s*g_vcr_pace_why = NULL;\s*"
                       r"return 1;\s*\}", before)
    waited = re.search(r"if \(!vcr_pace_wait\(\)\) \{\s*vcr_pace_unlock_to\(depth\);\s*"
                       r"g_vcr_pace_why = VCR_PACE_WHY_WAIT;\s*return 0;\s*\}", before)
    pre = re.search(r"vcr_pace_mark\(\);\s*g_vcr_pace_armed = 1;\s*g_vcr_pace_why = NULL;\s*"
                    r"return 1;\s*\}$", before)
    assert nested and waited and pre
    assert before.index("vcr_pace_lock()") < nested.start() < waited.start() < pre.start()
    # "depth" is what this process held BEFORE this call: only a call nested
    # inside an armed one skips the wait
    assert re.search(r"unsigned depth = g_vcr_pace_depth;", before)
    # armed is cleared when the lock is given back entirely
    assert re.search(r"if \(!g_vcr_pace_depth\)\s*g_vcr_pace_armed = 0;",
                     _body(PACE_H, "vcr_pace_unlock_to"))


def test_the_wait_reads_the_stamp_again_after_every_sleep_to_a_hard_cap():
    """A tool that died holding the lock, the host's pace-mark after a kill,
    an exit hold: each moves the stamp while this one sleeps. The wait sleeps
    at most VCR_PACE_CHUNK_MS, reads the stamp after every sleep and ends a
    floor after the LATEST switch; a floor that never passes (a stamp that
    keeps moving) is given up at VCR_PACE_WAIT_CAP_MS - and then NO switch
    is made. Round 2 settled for the last stamp it read and switched."""
    wait = _body(PACE_H, "vcr_pace_wait")
    loop = _block_after(wait, wait.index("for (;;)"))
    assert loop.index("vcr_pace_last_ms()") < loop.index("Sleep(need)")
    assert re.search(r"if \(!need\)\s*return 1;", loop)
    cap = re.search(r"if \(slept >= VCR_PACE_WAIT_CAP_MS\) \{[^}]*return 0;\s*\}", loop)
    assert cap and cap.start() < loop.index("Sleep(need)")
    assert re.search(r"if \(need > VCR_PACE_CHUNK_MS\)\s*need = VCR_PACE_CHUNK_MS;\s*"
                     r"Sleep\(need\);\s*slept \+= need;\s*\}$", loop)
    chunk, floor = _define(PACE_H, "VCR_PACE_CHUNK_MS"), _define(PACE_H, "VCR_PACE_MIN_MS")
    assert 0 < chunk <= floor
    # a legitimate wait never reaches the cap: the furthest ahead a gate
    # stamp may be, plus the longest floor a caller may ask for
    assert _define(PACE_H, "VCR_PACE_WAIT_CAP_MS") >= (_define(PACE_H, "VCR_PACE_AHEAD_MAX_MS")
                                                       + _define(PACE_H, "VCR_PACE_MAX_MS"))
    assert _define(PACE_H, "VCR_PACE_WAIT_CAP_MS") > _define(PACE_H, "VCR_PACE_WAIT_MAX_MS")


def test_a_revert_that_lands_after_the_process_is_stamped_ahead():
    """atexit runs before the DLLs detach - glide3x's DLL_PROCESS_DETACH shuts
    Glide down (seconds of idle waits) before its RestoreDisplayMode - and XP
    drops a dead process's mode later still. A stamp made when the exit hold
    ran was seconds EARLY. So the hold, the crash filter's hold and a paced
    kill stamp now + VCR_PACE_EXIT_LAG_MS, and a stamp in the future (up to
    VCR_PACE_AHEAD_MAX_MS) is waited out to its time plus a floor; further
    ahead than any gate writes, the clock went back: a whole floor from when
    it was first seen."""
    ahead = _body(PACE_H, "vcr_pace_mark_ahead")
    assert re.search(r"g_vcr_pace_mine = vcr_pace_now_ms\(\) \+ ms;\s*"
                     r"return vcr_pace_store\(g_vcr_pace_mine\);", ahead)
    hold = _body(PACE_H, "vcr_pace_hold_to_exit")
    assert re.search(r"vcr_pace_lock\(\);\s*vcr_pace_wait\(\);\s*"
                     r"vcr_pace_mark_ahead\(VCR_PACE_EXIT_LAG_MS\);", hold)
    fold = _body(PACE_H, "vcr_pace_fold")
    assert re.search(r"if \(t > now \+ VCR_PACE_AHEAD_MAX_MS\) \{", fold)
    assert re.search(r"n = t > now \? \(DWORD\)\(t - now\) \+ full : vcr_pace_rest\(t, now, full\);",
                     fold)
    # a blind key is judged once, when first seen (it would slide inside the
    # horizon as the clock caught up)
    assert re.search(r"if \(t == VCR_PACE_NEVER \|\| t == \*key\)\s*return;", fold)
    lag, horizon = _define(PACE_H, "VCR_PACE_EXIT_LAG_MS"), _define(PACE_H, "VCR_PACE_AHEAD_MAX_MS")
    assert 0 < lag < horizon          # the gate's own forward stamps are inside the horizon
    # the host budgets for it
    assert mode_sweep.PACE_EXIT_LAG_S * 1000 == lag


def test_a_crash_holds_the_mode_like_an_exit():
    """A crash skips atexit(): a top-level exception filter holds the mode,
    stamps the revert, and ends the process - installed at the first
    before-call, because a driver faulting inside the first switch is the
    likeliest crash."""
    arm = _body(PACE_H, "vcr_pace_arm_filter")
    assert "g_vcr_pace_prev_filter = SetUnhandledExceptionFilter(vcr_pace_crash_filter);" in arm
    assert re.search(r"if \(g_vcr_pace_filter_armed\)\s*return;", arm)      # once, chained
    before = _body(PACE_H, "vcr_pace_before_switch")
    assert before.index("vcr_pace_arm_filter()") < before.index("vcr_pace_lock()")
    assert "vcr_pace_arm_filter()" in _body(PACE_H, "vcr_pace_after_switch_ex")
    crash = _body(PACE_H, "vcr_pace_crash_filter")
    # held: a temporary mode set, or mid-switch (the lock held)
    assert "held = temp || g_vcr_pace_depth" in crash
    # the hold comes before the previous filter gets its say: mingw's may end
    # the process
    assert crash.index("vcr_pace_hold_to_exit()") < crash.index("g_vcr_pace_prev_filter(ep)")
    # the atexit that may still run must not hold a second time
    assert crash.index("g_vcr_pace_temp_mode = 0;") < crash.index("vcr_pace_hold_to_exit()")
    assert re.search(r"if \(r == EXCEPTION_CONTINUE_EXECUTION\) \{", crash)
    assert "return held ? EXCEPTION_EXECUTE_HANDLER : r;" in crash
    # the hold it runs stamps ahead, as at exit
    hold = _body(PACE_H, "vcr_pace_hold_to_exit")
    assert re.search(r"vcr_pace_lock\(\);\s*vcr_pace_wait\(\);\s*"
                     r"vcr_pace_mark_ahead\(VCR_PACE_EXIT_LAG_MS\);", hold)


def test_every_tool_that_includes_the_gate_is_rebuilt_when_it_changes():
    """An edit to the header alone left every tool built from the OLD gate:
    make saw nothing newer than the .exe."""
    mk = (KMD / "Makefile").read_text()
    assert re.search(r"^PACE_H\s*:=\s*tools/vcr_pace\.h\s*$", mk, re.M)
    users = sorted(p.stem for p in TOOLS.glob("*.c") if '#include "vcr_pace.h"' in p.read_text())
    assert set(users) >= {"vcrctl", "ddlab", "d3dprobe", "glidelab"}
    for stem in users:
        m = re.search(rf"^\$\(OUT\)/{stem}\.exe:([^\n|]*)", mk, re.M)
        assert m and "$(PACE_H)" in m.group(1).split(), f"{stem}.exe does not depend on $(PACE_H)"
    # and make, which expands the variables, agrees (-q -p: runs nothing,
    # prints its rule database)
    if not shutil.which("make"):
        pytest.skip("no make on this host: the Makefile text was checked, make's own reading "
                    "of it was NOT")
    db = subprocess.run(["make", "-qp", "-C", str(KMD)], capture_output=True, text=True).stdout
    for stem in users:
        m = re.search(rf"^out/{stem}\.exe:([^\n|]*)", db, re.M)
        assert m and "tools/vcr_pace.h" in m.group(1).split(), f"make: {stem}.exe"


# ---- a switch the gate refuses is not made, by ANY route -------------------------


def _checked_calls(name, src, func):
    """Every call of `func()` in src must have its answer looked at: `if
    (!f())`, `if (f())`, `a && !f()`, `x = f();` (then tested), `return f()`.
    A bare `f();` switches whatever the gate said. -> the number of calls."""
    code = _blank(src)
    n = 0
    for m in re.finditer(rf"\b{func}\s*\(\s*\)", code):
        n += 1
        lead = code[:m.start()].rstrip()
        assert lead.endswith(("(", "!", "=")) or lead.endswith("return"), \
            f"{name}: `{func}()` at line {code.count(chr(10), 0, m.start()) + 1} ignores its answer"
    return n


def _if_body(code, at):
    """The statement (or { block }) an `if (...)` whose condition holds
    offset `at` runs."""
    s = code.rfind("if (", 0, at) + 3
    depth = 0
    for i in range(s, len(code)):
        depth += {"(": 1, ")": -1}.get(code[i], 0)
        if depth == 0:
            break
    rest = code[i + 1:]
    if rest.lstrip().startswith("{"):
        return _block_after(rest, 0)
    return rest[:rest.index(";") + 1]


@pytest.mark.parametrize("name,src", (("vcrctl.c", VCRCTL),) + LABS_C)
def test_every_before_call_honours_a_refusal(name, src):
    assert _checked_calls(name, src, "vcr_pace_before_switch")
    if name == "glidelab.c":
        assert _checked_calls(name, src, "pace_before")


def test_vcrctl_switches_nothing_the_gate_refused():
    """set_mode answers CDS_NOT_MADE (not a DISP_CHANGE_* value) and switches
    nothing; every give-back is made only when its own before-call said go;
    modeseq stops its list at a refusal and says why; ddraw leaves a mode it
    may not give back for the exit hold - no RestoreDisplayMode, no leaving
    exclusive mode, no Release, each of which would give it back unpaced."""
    assert _define(VCRCTL, "CDS_NOT_MADE") <= -100            # DISP_CHANGE_* are 1 .. -6
    sm = _body(VCRCTL, "set_mode")
    go = re.search(r"if \(!vcr_pace_before_switch\(\)\)\s*return CDS_NOT_MADE;", sm)
    assert go and go.end() < sm.index("ChangeDisplaySettingsA(")
    # every ChangeDisplaySettingsA(NULL) (the give-backs) sits in the block of
    # a before-call that said go
    for fname in ("cmd_modetest", "cmd_modeseq", "main"):
        body = _body(VCRCTL, fname)
        for m in re.finditer(r"ChangeDisplaySettingsA\(NULL", body):
            opener = body.rfind("if (vcr_pace_before_switch()) {", 0, m.start())
            assert opener >= 0 and m.start() < opener + len(_block_after(body, opener)), fname
    # the refusals are said, with the gate's words, and fail the command
    assert '\\"error\\":\\"%s\\"' in _body(VCRCTL, "print_not_made", strings=False)
    assert "g_vcr_pace_why" in _body(VCRCTL, "print_not_made")
    gold = _body(VCRCTL, "cmd_golden")
    assert re.search(r"if \(r == CDS_NOT_MADE\) \{[^}]*g_vcr_pace_why[^}]*return 1;", gold)
    # modetest: nothing switched, nothing given back
    mt = _body(VCRCTL, "cmd_modetest")
    assert re.search(r"if \(rc == MODETEST_NOT_MADE\)\s*return 1;", mt)
    assert mt.index("MODETEST_NOT_MADE") < mt.index("vcr_pace_before_switch()")
    # modeseq: a refused before-call ends the list, after the stop check
    seq = _body(VCRCTL, "cmd_modeseq")
    loop = _block_after(seq, seq.index("for (i = 0; i < n; i++)"))
    gone = re.search(r"if \(!go\) \{\s*refused = g_vcr_pace_why;\s*break;\s*\}", loop)
    assert gone and loop.index("modeseq_stop_asked()") < gone.start() < loop.index("modetest_one(")
    assert re.search(r"go = vcr_pace_before_switch\(\);", loop)
    assert re.search(r"else \{\s*r = CDS_NOT_MADE;", seq)       # a refused restore is none
    assert re.search(r'if \(refused\)\s*printf\(",\\"error\\":\\"%s\\"", refused\);',
                     _body(VCRCTL, "cmd_modeseq", strings=False))
    # ddraw: refused on the way in - no SetDisplayMode; on the way out - the
    # exclusive session is left for the exit hold, untouched
    dd = _body(VCRCTL, "cmd_ddraw")
    assert re.search(r"if \(!vcr_pace_before_switch\(\)\) \{\s*why = g_vcr_pace_why;\s*"
                     r"left = 1;\s*\}", dd)
    keep = dd.index("if (!left) {")
    blk = _block_after(dd, keep)
    for api in ("IDirectDraw7_SetCooperativeLevel(dd, wnd, DDSCL_NORMAL)", "IDirectDraw7_Release(dd)"):
        assert dd.count(api) == 1 and api in blk, api
    assert dd.count("IDirectDraw7_RestoreDisplayMode(") == 1
    rs = dd.index("IDirectDraw7_RestoreDisplayMode(")
    assert re.search(r"if \(!vcr_pace_before_switch\(\)\) \{", dd[dd.rfind("if (switched) {", 0, rs):rs])
    assert '\\"left_for_exit\\":%s' in _body(VCRCTL, "cmd_ddraw", strings=False)
    # it pumps no messages while it holds a mode (the hold is a Sleep in the
    # gate), so a lost foreground cannot make it switch behind the gate: a
    # pump added here needs the labs' WM_ACTIVATEAPP handling too
    assert not re.search(r"\b(PeekMessage|GetMessage|DispatchMessage)\w*\s*\(", dd)


@pytest.mark.parametrize("name,src,out_fn,give_back", (
    ("ddlab.c", DDLAB, "restore_mode", "IDirectDraw7_RestoreDisplayMode"),
    ("d3dprobe.c", D3DPROBE, "release_device", "IDirect3DDevice8_Release"),
    ("glidelab.c", GLIDELAB, "close_board", "p_grSstWinClose"),
    ("glidelab.c", GLIDELAB, "shutdown_glide", "p_grGlideShutdown")))
def test_a_lab_refused_on_the_way_out_leaves_its_mode_for_the_exit_hold(name, src, out_fn, give_back):
    """Refused, the switch out is not made - and the give-back API is not
    called by another road either (a Release, a close, a shutdown). The lab
    says so in a last RESULT and returns, so the exit hold paces XP's revert."""
    body = _body(src, out_fn)
    refuse = re.search(r"if \((?:held && |g_full && )?!(?:vcr_pace_before_switch|pace_before)\(\)\)",
                       body)
    assert refuse and refuse.start() < body.index(give_back + "(")
    assert re.search(r"\breturn 0;", body[refuse.end():body.index(give_back + "(")])
    # every caller looks at the answer, and a refusal returns from main
    main = _body(src, "main")
    calls = list(re.finditer(rf"\b{out_fn}\s*\(", main))
    assert calls, (name, out_fn)
    for m in calls:
        lead = main[:m.start()].rstrip()
        assert lead.endswith("!") and lead[:-1].rstrip().endswith("if ("), \
            f"{name}: {out_fn}() is called without looking at the answer"
        assert re.search(r"\breturn\b", _if_body(main, m.start())), \
            f"{name}: a refused {out_fn}() does not end the run"


# ---- the labs: a lost foreground is a switch nobody paced ------------------------


@pytest.mark.parametrize("name,src", LABS_C)
def test_a_lab_that_loses_the_foreground_while_it_holds_a_mode_ends_the_run(name, src):
    """DirectDraw / D3D / Glide's DirectDraw hook give the desktop back the
    moment an exclusive window is deactivated - a switch the gate never saw -
    and put the mode back when it is re-activated. So: WM_ACTIVATEAPP(FALSE)
    while the mode is held stamps that switch and ends the run; nothing more
    is dispatched (a queued re-activation would switch back in); the frame
    loops stop; the RESULT says "focus_lost"; the run still leaves through
    its paced switch out."""
    wp = _body(src, "wndproc")
    assert re.search(r"if \(m == WM_ACTIVATEAPP && !w && g_held\) \{\s*vcr_pace_mark\(\);\s*"
                     r"g_focus_lost = 1;\s*\}", wp), name
    assert "DefWindowProcA(h, m, w, l)" in wp
    assert re.search(r"while \(!g_focus_lost && PeekMessageA\(", _body(src, "pump")), name
    # every window it takes the screen with is that window procedure's
    assert _blank(src).count("wc.lpfnWndProc = wndproc;") >= 1
    assert not re.search(r"lpfnWndProc\s*=\s*(?!wndproc\b)\w+", _blank(src)), name
    # held from the switch in ...
    code = _blank(src)
    assert re.search(r"vcr_pace_after_switch\(\);\s*g_held = 1;", code) or \
        re.search(r"g_held = g_full;", code), name
    # ... and "ours" again only once the paced switch out has said go
    for fn, api in (("restore_mode", "IDirectDraw7_RestoreDisplayMode"),
                    ("release_device", "IDirect3DDevice8_Release"),
                    ("close_board", "p_grSstWinClose"), ("shutdown_glide", "p_grGlideShutdown")):
        if fn in _functions(code):
            b = _body(src, fn)
            go = re.search(r"(vcr_pace_before_switch|pace_before)\(\)", b)
            assert go and go.start() < b.index("g_held = 0;") < b.index(api + "("), (name, fn)
    # the frame (f) and open/close (c) loops stop, and the RESULT says so (an
    # error: not a pass)
    frame_loops = re.findall(r"for \([fc] = 0;[^)]*\)", code)
    assert frame_loops and all("!g_focus_lost" in lp for lp in frame_loops), (name, frame_loops)
    raw = _blank(src, strings=False)
    assert '\\"focus_lost\\":true,\\"error\\":' in raw and '\\"focus_lost\\":false' in raw, name


def test_the_host_stops_on_a_lab_that_lost_the_focus_or_the_lock():
    halt = mode_sweep.lab_halt
    assert halt({"mode": "flip", "focus_lost": True})
    assert halt({"mode": "flip", "error": "SetDisplayMode not made: pace lock busy"})
    assert halt({}, "RESULT ... pace lock busy ...")
    assert halt({"mode": "fill", "opened_hz": 60}, want_hz=85)
    assert not halt({"mode": "fill", "opened_hz": 85}, want_hz=85)
    assert not halt({"mode": "flip", "focus_lost": False, "mismatch": 0})
    assert not halt({"mode": "fill", "opened_hz": 60})       # nothing asked: nothing to compare
    assert silicon_battery.halt_reason([{"focus_lost": True}])


# ---- glidelab: the refresh it opens is the refresh the host checked ---------------


def test_glidelab_opens_the_refresh_asked_or_fails_the_run():
    """glidelab asked grSstWinOpen for a GR_REFRESH code, and Glide may still
    open another (a registry override, a rate the driver moved) - a mode the
    host gate never checked. The refresh goes to Glide as FX_GLIDE_REFRESH
    too, before the DLL loads; the rate the mode really opened at is read
    back; any other closes the session (paced) and fails it with opened_hz."""
    main = _body(GLIDELAB, "main")
    raw = _body(GLIDELAB, "main", strings=False)
    no_code = main.index("if (hzcode < 0) {")
    assert re.search(r"\breturn 2;", _block_after(main, no_code))
    env = raw.index('SetEnvironmentVariableA("FX_GLIDE_REFRESH", hz)')
    assert re.search(r'_snprintf\(hz, sizeof hz, "%d", O\.hz\);', raw)
    assert no_code < env < main.index("LoadLibraryA(O.dll)") < main.index("p_grGlideInit()")
    ob = _body(GLIDELAB, "open_board")
    assert re.search(r"vcr_pace_after_switch\(\);\s*g_held = 1;\s*pump\(\);\s*"
                     r"g_opened_hz = ctx \? current_hz\(\) : 0;", ob)
    assert "EnumDisplaySettingsA(NULL, ENUM_CURRENT_SETTINGS, &dm)" in _body(GLIDELAB, "current_hz")
    # the asked rate, or XP's 0 / 1 "hardware default" (unknown is not a
    # mismatch - lab_halt reads it the same way); anything else closes
    assert re.search(r"if \(g_opened_hz == O\.hz \|\| g_opened_hz == 0 \|\| g_opened_hz == 1\)"
                     r"\s*return 1;", _body(GLIDELAB, "refresh_ok"))
    # every session checks it before it draws a frame, and a wrong one ends
    # through the paced close
    for m in re.finditer(r"ctx = open_board\(", main):
        tail = main[m.end():]
        chk = tail.index("refresh_ok()")
        draw = re.search(r"\b(flat_state|do_fill|do_bands|p_grBufferClear)\s*\(", tail)
        assert draw and chk < draw.start()
        assert "close_board(ctx)" in tail[chk:draw.start()]
    assert '\\"opened_hz\\":%d' in _body(GLIDELAB, "tail_json", strings=False)
    # the host fails a session whose RESULT says it opened another rate
    assert "want_hz=a.refresh" in (TOOLS / "glidelab_run.py").read_text()
    assert "want_hz=want_hz" in (TOOLS / "lab_run.py").read_text()


# ---- vcrctl --------------------------------------------------------------------


def test_modeseq_enforces_a_floor_between_switches():
    assert _define(VCRCTL, "MODESEQ_MIN_PACE_MS") >= 3000
    # the usage text's 3 s can never undercut the shared gate
    assert re.search(r"#if MODESEQ_MIN_PACE_MS < VCR_PACE_MIN_MS\s*\n#error", VCRCTL)
    body = _body(VCRCTL, "cmd_modeseq")
    # the caller's pace is raised to the floor, never trusted below it ...
    assert re.search(r"if \(pace < MODESEQ_MIN_PACE_MS\)\s*pace = MODESEQ_MIN_PACE_MS;", body)
    loop_at = body.index("for (i = 0; i < n; i++)")
    # ... and governs every wait in the process, the FIRST switch's included:
    # the old `if (i) Sleep(pace)` let the first switch follow the previous
    # tool's last one at once
    assert 0 <= body.index("vcr_pace_set_min(pace);") < loop_at
    loop = _block_after(body, loop_at)
    wait = re.search(PACE_BEFORE, loop)
    assert wait and not re.search(r"\bif\b", loop[:wait.start()]), "unconditional, every mode"
    assert wait.start() < loop.index("modetest_one(")
    # and the restore after the list waits too
    tail = body[loop_at + len(loop):]
    assert re.search(PACE_BEFORE, tail[:tail.index("ChangeDisplaySettingsA(")])


def test_modeseq_caps_the_modes_per_run():
    assert _define(VCRCTL, "MODESEQ_MAX_MODES") <= 16
    body = _body(VCRCTL, "cmd_modeseq")
    # refused before it can take the mutex, let alone switch
    assert 0 <= body.index("n > MODESEQ_MAX_MODES") < body.index("CreateMutexA(")


def test_modeseq_does_not_bounce_to_the_desktop_between_modes():
    one = _body(VCRCTL, "modetest_one")
    # the single-mode helper must not restore - that doubled the re-syncs
    assert "ChangeDisplaySettingsA(" not in one and "set_mode(" in one
    seq = _body(VCRCTL, "cmd_modeseq")
    # ONE restore, after the loop: never inside it, never retried
    assert seq.count("ChangeDisplaySettingsA(NULL") == 1
    loop_at = seq.index("for (i = 0; i < n; i++)")
    loop = _block_after(seq, loop_at)
    assert "ChangeDisplaySettingsA(" not in loop
    assert seq.index("ChangeDisplaySettingsA(NULL") > loop_at + len(loop)


def test_modeseq_runs_one_at_a_time():
    """Two sweeps would race between reading the stamp and switching, and a
    host that times out and re-issues one would double the re-syncs."""
    assert _c_string(VCRCTL, "MODESEQ_MUTEX")
    body = _body(VCRCTL, "cmd_modeseq", strings=False)
    blank = _body(VCRCTL, "cmd_modeseq")                  # same offsets, strings blanked
    assert "CreateMutexA(NULL, FALSE, MODESEQ_MUTEX)" in body
    assert "WaitForSingleObject(one, 0)" in body          # never waits: busy is an answer
    assert "WAIT_ABANDONED" in body                       # a crashed run is not a running one
    busy = body.index('\\"busy\\":true')
    assert busy < blank.index("vcr_pace_set_min(") < blank.index("for (i = 0; i < n; i++)")
    # the busy branch returns before anything switches
    assert re.search(r"\breturn 1;", body[busy:blank.index("vcr_pace_set_min(")])
    # held until the process exits (after the exit hold), never released early
    assert "ReleaseMutex" not in blank
    assert blank.count("CloseHandle(one)") == 1 and blank.index("CloseHandle(one)") < busy


def test_modeseq_can_be_stopped_from_the_host():
    """Killing vcrctl drops the test mode the instant it dies - possibly right
    after a switch. The stop file ends the run the civil way: seen after the
    wait and before the next switch, consumed, one paced restore."""
    stop = _c_string(VCRCTL, "MODESEQ_STOP_FILE")
    assert stop == mode_sweep.STOP_FILE == r"C:\vcr\modeseq.stop"
    asked = _body(VCRCTL, "modeseq_stop_asked")
    assert "GetFileAttributesA(MODESEQ_STOP_FILE)" in asked
    assert "DeleteFileA(MODESEQ_STOP_FILE)" in asked     # consumed: it stops ONE run
    body = _body(VCRCTL, "cmd_modeseq")
    loop_at = body.index("for (i = 0; i < n; i++)")
    loop = _block_after(body, loop_at)
    w, st, br, mt = (re.search(PACE_BEFORE, loop).start(), loop.index("modeseq_stop_asked()"),
                     loop.index("break;"), loop.index("modetest_one("))
    assert w < st < br < mt
    # it waited under the pace lock for a switch it will not make: the lock
    # goes back unstamped, or the next tool finds it abandoned and waits a
    # floor for a switch nobody made
    assert st < loop.index("vcr_pace_cancel();") < br
    tail = body[loop_at + len(loop):]
    # stopped before the first mode: nothing to give back, no restore
    assert re.search(r"if \(i\)\s*\{", tail)
    # a stop that came in late is consumed, not left to stop the next run
    assert tail.index("stopped |= modeseq_stop_asked();") < tail.index("ChangeDisplaySettingsA(")


def test_vcrctl_pace_mark_stamps_the_box_after_a_kill():
    """A kill runs no exit hold and XP reverts the mode unrecorded - the one
    exit nothing inside a process can pace. `vcrctl pace-mark` is the host's
    half for a death it did not make through the gate: it stamps the box so
    the next switch is measured from the revert."""
    blank = _body(VCRCTL, "cmd_pace_mark")
    body = _body(VCRCTL, "cmd_pace_mark", strings=False)
    # a stamp that did not land is a failure (exit 1, ok:false), not an ok
    assert re.search(r"if \(!vcr_pace_mark\(\)\)\s*return fail\(", blank)
    assert body.index('\\"cmd\\":\\"pace-mark\\",\\"ok\\":true') > body.index("return fail(")
    # no lock, no wait, no switch: the stamp carries the time of the kill,
    # not the moment a lock came free
    assert not re.search(r"\bvcr_pace_(before_switch|lock|wait|after_switch\w*)\s*\(", blank)
    assert not re.search(SWITCHES["vcrctl.c"][1], blank)
    main = _body(VCRCTL, "main", strings=False)
    assert re.search(r'!strcmp\(cmd, "pace-mark"\)\)\s*rc = cmd_pace_mark\(\);', main)
    assert re.search(r"^ \*   pace-mark ", VCRCTL, re.M)           # in the usage
    # every host cleanup calls it by that name, and reads its "ok"
    reap = SWEEP[SWEEP.index("async def reap("):SWEEP.index("async def restore_once(")]
    assert 'f"EXEC {tool} pace-mark"' in reap and 'j.get("ok")' in reap


def _kill_images(src):
    """The images `vcrctl pace-kill` will end, as vcr_pace.h lists them."""
    return frozenset(re.findall(r'"([\w.]+)"', re.search(
        r"vcr_pace_kill_images\[\]\s*=\s*\{(.*?)\};", src, re.S).group(1)))


PACE_KILL_IMAGES = _kill_images(PACE_H)


def test_vcrctl_pace_kill_is_a_kill_made_through_the_gate():
    """A PROCKILL or EXECW's tree-kill drops the victim's mode the instant it
    dies - maybe a moment after a switch, two re-syncs back to back - and
    records nothing. `vcrctl pace-kill <pid>` makes the kill a paced switch:
    under the lock, a floor after the latest stamp, TerminateProcess, a
    bounded wait for the victim to be gone, the revert stamped ahead, the
    lock given back. Only our own switching tools; never pid 0/4 or itself."""
    kill = _body(PACE_H, "vcr_pace_kill")
    order = [kill.index(s) for s in ("vcr_pace_before_switch()", "TerminateProcess(proc, 1)",
                                     "WaitForSingleObject(proc, VCR_PACE_KILL_WAIT_MS)",
                                     "vcr_pace_mark_ahead(VCR_PACE_EXIT_LAG_MS)",
                                     "vcr_pace_unlock_to(0)")]
    assert order == sorted(order)
    assert re.search(r"if \(!vcr_pace_before_switch\(\)\)\s*return 0;", kill)   # refused: none
    # TerminateProcess refused: nothing died - the lock back, no stamp beyond
    # the pre-stamp
    assert re.search(r"if \(!TerminateProcess\(proc, 1\)\) \{\s*\*err = GetLastError\(\);\s*"
                     r"vcr_pace_cancel\(\);\s*return -1;\s*\}", kill)
    assert _define(PACE_H, "VCR_PACE_KILL_WAIT_MS") <= 10000
    # the refusal list: our four switching tools, compared on the base name
    # without case
    images = _kill_images(PACE_H)
    # gdilab switches nothing, but lab_run's cleanup kills only through
    # pace-kill: without it a hung gdilab stayed on the box
    assert images == {"vcrctl.exe", "ddlab.exe", "d3dprobe.exe", "glidelab.exe", "gdilab.exe"}
    ref = _body(PACE_H, "vcr_pace_kill_refusal")
    assert "vcr_pace_kill_pid_refusal(pid, self)" in ref and "strrchr(image, '\\\\')" in \
        _body(PACE_H, "vcr_pace_kill_refusal", strings=False)
    assert re.search(r"if \(pid == 0 \|\| pid == 4\)", _body(PACE_H, "vcr_pace_kill_pid_refusal"))
    assert re.search(r"if \(pid == self\)", _body(PACE_H, "vcr_pace_kill_pid_refusal"))
    # vcrctl: a strict decimal pid; pid 0/4/self refused before anything is
    # opened; the process OPENED before its name is checked (the handle pins
    # the pid - it cannot be recycled between the check and the kill); the
    # name checked before the kill; the answer the host reads
    pk = _body(VCRCTL, "cmd_pace_kill")
    assert re.search(r"if \(p == arg \|\| \*p\)\s*return pace_kill_answer\(", pk)
    seq = [pk.index(s) for s in ("vcr_pace_kill_pid_refusal(", "OpenProcess(PROCESS_TERMINATE",
                                 "image_of(pid", "vcr_pace_kill_refusal(", "vcr_pace_kill(proc")]
    assert seq == sorted(seq)
    assert "TerminateProcess" not in _blank(VCRCTL)            # only through the gate
    ans = _body(VCRCTL, "pace_kill_answer", strings=False)
    assert '{\\"cmd\\":\\"pace-kill\\",\\"ok\\":%s,\\"pid\\":%lu,\\"exited\\":%s' in ans
    # killed but not gone is NOT ok: the host must not switch until it is
    assert re.search(r"return pace_kill_answer\(pid, exited, exited, image,",
                     _body(VCRCTL, "cmd_pace_kill", strings=False))
    main = _body(VCRCTL, "main", strings=False)
    assert re.search(r'!strcmp\(cmd, "pace-kill"\) && argc > 2\)\s*rc = cmd_pace_kill\(argv\[2\]\);',
                     main)
    assert re.search(r"^ \*   pace-kill PID ", VCRCTL, re.M)       # in the usage
    # the host's images are all ones it will kill
    for images in (mode_sweep.VCRCTL_IMAGES, silicon_battery.ORPHANS, glidelab_run.IMAGES,
                   ("ddlab.exe",), ("d3dprobe.exe",)):
        assert {i.lower() for i in images} <= _kill_images(PACE_H), images


def test_modeseq_answers_what_mode_sweep_reads():
    body = _body(VCRCTL, "cmd_modeseq", strings=False)
    for key in ("modes", "ran", "failed", "stopped", "restore", "busy"):
        assert f'\\"{key}\\":' in body, key
    for key in ('"restore"', '"busy"', '"stopped"', '"ran"'):
        assert key in SWEEP, key


def test_a_failed_restore_is_held_not_retried():
    """A restore that failed leaves the test mode for XP to revert at exit -
    one more re-sync, so the exit hold covers it; retrying it in a loop is a
    burst of them."""
    gave = _body(VCRCTL, "pace_gave_back")
    assert re.search(r"vcr_pace_after_switch_ex\(!ok\)", gave)
    for fname in ("cmd_modetest", "cmd_modeseq"):
        body = _body(VCRCTL, fname)
        assert body.count("ChangeDisplaySettingsA(NULL") == 1, fname
        assert re.search(r"pace_gave_back\(r == DISP_CHANGE_SUCCESSFUL\)", body), fname
    assert not re.search(r"\b(for|while)\s*\(", _body(VCRCTL, "cmd_modetest"))
    main = _body(VCRCTL, "main")
    assert main.count("ChangeDisplaySettingsA(NULL") == 1
    # and a failure says so: modetest adds a restore line, the command exits 1
    mt = _body(VCRCTL, "cmd_modetest", strings=False)
    assert '\\"cmd\\":\\"restore\\",\\"ok\\":false' in mt and "rc = 1;" in mt


def test_vcrctl_ddraw_holds_the_mode_instead_of_flashing_it():
    body = _body(VCRCTL, "cmd_ddraw")
    # it was in and out of a mode inside 500 ms: two re-syncs a tube cannot finish
    assert "Sleep(500)" not in body
    # a failed RestoreDisplayMode leaves the state unknown: held again at exit
    assert re.search(r"if \(FAILED\(hr_restore\)\)\s*vcr_pace_after_switch\(\);", body)


def test_vcrctl_output_is_out_before_a_host_can_kill_it():
    """A host timeout tree-kills a modeseq, and the exit hold keeps a process
    alive after main returns: a result still in the pipe buffer is a mode
    nobody hears about - above all a FAILED one."""
    one = _body(VCRCTL, "modetest_one")
    fail_at = one.index("if (r != DISP_CHANGE_SUCCESSFUL)")
    fail = _block_after(one, fail_at)
    assert "print_log_next_seq();" in fail and "fflush(stdout);" in fail
    rest = one[fail_at + len(fail):]
    assert "print_log_next_seq();" in rest and "fflush(stdout);" in rest
    main = _body(VCRCTL, "main")
    assert re.search(r"fflush\(stdout\);\s*return rc;\s*\}$", main)


def test_vcrctl_info_says_whose_limits_are_in_force():
    """`"mon_src":N` after everything else - or null when the driver is older
    than the field (its vcr_info.size stops short of it): a missing source is
    NOT 0, which would read as "filter off", and not a guess either."""
    info = _body(VCRCTL, "cmd_info", strings=False)
    assert re.search(r"if \(v\.size >= FIELD_OFFSET\(vcr_info, mon_src\) \+ sizeof v\.mon_src\)\s*"
                     r'printf\("\\",\\"mon_src\\":%u}\\n", v\.mon_src\);\s*else\s*'
                     r'printf\("\\",\\"mon_src\\":null}\\n"\);', info)
    assert info.index('\\"edid\\":\\"') < info.index("mon_src")
    # the EDID bytes go out too: --monitor-info is matched against the box's
    # registry by them
    assert re.search(r'printf\("%02x", v\.edid\[i\]\);', info)


# ---- glidelab ------------------------------------------------------------------


def test_glidelab_caps_its_open_close_cycles():
    """`cycle` at its old default of 10 was 20 re-syncs ~0.3 s apart."""
    cap = _define(GLIDELAB, "GLIDELAB_MAX_CYCLES")
    assert 1 <= cap <= 3
    # the default in the options initializer (the 11th field is `cycles`)
    m = re.search(r"\} O = \{([^}]*)\};", GLIDELAB)
    fields = [f.strip() for f in m.group(1).split(",")]
    assert int(fields[10]) <= cap
    main = _body(GLIDELAB, "main")
    clamp = re.search(r"if \(O\.cycles > GLIDELAB_MAX_CYCLES\) \{[^}]*O\.cycles = GLIDELAB_MAX_CYCLES;",
                      main)
    assert clamp and clamp.start() < main.index("p_grGlideInit()")
    assert '\\"cycles_asked\\":' in GLIDELAB           # the RESULT says what was cut
    # the host runner agrees on the number and refuses more itself
    assert glidelab_run.MAX_CYCLES == cap


def _parsed(mod, argv):
    """The argparse Namespace mod.main() builds for argv - without running it.
    asyncio.run is intercepted for the one call main() makes."""
    got = {}

    def fake_run(coro):
        got["a"] = coro.cr_frame.f_locals["a"]
        coro.close()
        return 0

    with pytest.MonkeyPatch.context() as m:
        m.setattr(sys, "argv", [mod.__name__ + ".py"] + argv)
        m.setattr(mod.asyncio, "run", fake_run)
        with pytest.raises(SystemExit) as ex:
            mod.main()
    return got.get("a"), ex.value.code


def test_glidelab_run_refuses_too_many_cycles_before_connecting():
    a, code = _parsed(glidelab_run, ["no-such-host", "cycle", "--cycles", str(glidelab_run.MAX_CYCLES + 1)])
    assert a is None and code == 2                     # ap.error: never reached a box
    a, code = _parsed(glidelab_run, ["no-such-host", "cycle"])
    assert code == 0 and a.cycles <= glidelab_run.MAX_CYCLES


def test_a_refresh_glidelab_cannot_open_is_refused_before_connecting():
    """glidelab.c turns --refresh into a GR_REFRESH code from its HZ[] table;
    it used to fall through to 60 Hz for anything else, without a word: the
    session would open a mode the host gate never checked."""
    m = re.search(r"\} HZ\[\] = \{(.*?)\};", _blank(GLIDELAB), re.S)
    table = {int(hz) for hz in re.findall(r"\{\s*(\d+)\s*,\s*GR_REFRESH_\w+\s*\}", m.group(1))}
    assert table and set(glidelab_run.GLIDE_HZ) == table
    for hz in sorted(table):
        a, code = _parsed(glidelab_run, ["no-such-host", "fill", "--refresh", str(hz)])
        assert code == 0 and a.refresh == hz
    for hz in (50, 90, 110):
        assert hz not in table
        a, code = _parsed(glidelab_run, ["no-such-host", "fill", "--refresh", str(hz)])
        assert a is None and code == 2, hz
        a, code = _parsed(glidelab_sweep, ["no-such-host", "--label", "t", "--refresh", str(hz)])
        assert a is None and code == 2, hz
    # the gate checks the mode Glide opens: 16 bpp at that refresh
    assert glidelab_run.glide_modes(["1024x768", "1024x768"], 85) == ["1024x768x16@85"]


# ---- the driver: RESET_DEVICE goes back to VGA, and says why ---------------------


def test_reset_device_goes_all_the_way_back_to_vga_and_says_why():
    """A 2026-09-26 shortcut released the display on RESET_DEVICE instead,
    leaving the old mode scanning for the mode set it assumed would follow -
    one re-sync per mode change, not two. REJECTED: RESET_DEVICE is also XP's
    hand-off to VgaSave (a full-screen console or DOS box), where no mode set
    of ours follows, and the desktop's leftover extension state combines with
    a VGA text-mode writer into ~9 kHz / ~21 Hz, far under a CRT's floor. The
    reason must stay where the shortcut would be tried again."""
    raw = VCRMP
    code = _blank(raw)
    at = code.index("case IOCTL_VIDEO_RESET_DEVICE:")
    end = code.index("break;", at)
    case = code[at:end]
    assert 0 <= case.index("VcrSliOff(") < case.index("VcrHwResetToVga(x);")
    # every backend, every caller: no condition in front of it
    assert not re.search(r"\bif\b|\bbackend\b|\breturn\b", case)
    why = raw[at:end]                                   # the comment, kept
    for word in ("REJECTED", "VgaSave", "vcr_pace.h"):
        assert word in why, f"RESET_DEVICE no longer records the reason ({word})"
    # the release path is gone - code and prototype
    for p in sorted(MINIPORT.glob("*.[ch]")):
        assert not re.search(r"\bVcrHwReleaseDisplay\b", _blank(p.read_text())), p.name


def test_only_the_bugcheck_path_and_reset_device_reset_to_vga():
    """No mode set detours through VGA text of its own accord: the two callers
    are HwResetHw (bugcheck / shutdown - the HAL and bootvid draw their text
    there) and RESET_DEVICE."""
    callers = []
    for p in sorted(MINIPORT.glob("*.c")):
        code = _blank(p.read_text())
        for fname, (s, e) in _functions(code).items():
            if fname != "VcrHwResetToVga" and re.search(r"\bVcrHwResetToVga\s*\(", code[s:e]):
                callers.append(fname)
    assert sorted(callers) == ["VcrResetHw", "VcrStartIO"]
    assert "VcrHwResetToVga(x)" in _body(VCRMP, "VcrResetHw")
    sio = _body(VCRMP, "VcrStartIO")
    calls = [m.start() for m in re.finditer(r"\bVcrHwResetToVga\s*\(", sio)]
    assert len(calls) == 1
    assert sio.startswith("case IOCTL_VIDEO_RESET_DEVICE:", sio.rindex("case ", 0, calls[0]))


def test_the_host_counts_both_timing_changes_a_switch_costs():
    """So a mode change is old -> 31.5 kHz VGA -> new: two timing changes on
    the cable per switch. A plan that counted switches alone under-stated
    what the tube gets by half; the number follows the driver, and changes
    with it."""
    assert mode_sweep.TIMING_CHANGES_PER_SWITCH == 2
    for name, src in (("mode_sweep.py", SWEEP), ("silicon_battery.py", BATTERY)):
        assert "TIMING_CHANGES_PER_SWITCH * switches" in src, name


# ---- the driver: a bugcheck under SLI/AA gives the master its own clock back -------


def test_a_bugcheck_under_sli_gives_the_master_its_own_video_clock_first():
    """Under 4-way SLI the master's cfgVideoCtrl0 (0x801 on .124) takes its
    video clock from the slave side / the 6000's external synthesizer, not
    from its pllCtrl0: restoring pllCtrl0 alone left the blue screen's text
    mode scanning at whatever clock that was. HwResetHw runs at HIGH_LEVEL,
    so the video half of the disable runs on its own - no 3D sliCtrl writes
    (they wait for FIFO room a wedged engine never frees), config cycles
    raw (HalGetBusDataByOffset is for IRQL <= DISPATCH), nothing from the
    registry, nothing allocated - and before pllCtrl0 is put back."""
    reset = _body(HW, "VcrHwResetToVga")
    saved = reset.index("if (!b->saved || !x->chip[0].regs)")
    assert saved < reset.index("sli_video_reset(x);") < reset.index("VCR_R_PLLCTRL0")
    assert reset.count("sli_video_reset(") == 1
    svr = _body(HW, "sli_video_reset")
    # the HARDWARE is asked, not only x->sli_chips (a bugcheck inside an
    # enable, a config the driver did not start): outside SLI/AA the master's
    # cfgVideoCtrl0 is 0
    assert re.search(r"if \(!x->sli_chips && \(vc0 == 0 \|\| vc0 == 0xffffffffu\)\)\s*return;", svr)
    assert re.search(r"if \(!VCR_IS_NAPALM\(x->device\) \|\| !x->nchips\)\s*return;", svr)
    assert "rc = vcr_sli_reset_video(&io, x->nchips);" in svr
    # every accessor it hands over is legal at any IRQL
    code = _blank(HW)
    rst = [f for f in _functions(code) if f.startswith("rst_")] + ["sli_video_reset"]
    assert {"rst_cfg_rd", "rst_cfg_wr", "rst_io_rd", "rst_io_wr", "rst_log"} <= set(rst)
    for f in rst:
        b = _body(HW, f)
        assert not re.search(r"\b(HalGetBusData\w*|HalSetBusData\w*|VcrPciRead|VcrPciWrite|"
                             r"VcrDiag\w+|VcrPhase\w*|\w*AllocatePool\w*|ExAllocate\w*|"
                             r"VideoPortGetRegistry\w*|VideoPortSetRegistry\w*|KeWait\w*)\s*\(",
                             b), f
    assert "raw_read(x, rst_slot(x, chip), off, 4)" in _body(HW, "rst_cfg_rd")
    assert "vcr_pci_raw_write32(" in _body(HW, "rst_cfg_wr")
    # the reset half is the disable's own per-chip half - shared, so the two
    # cannot drift - and it makes no 3D write
    video = _body(SLI, "vcr_sli_reset_video")
    assert "sli_disable_video(io, n)" in video and "write_3d(" not in video
    assert not re.search(r"\b(write_3d|wait_idle|fifo\w*)\s*\(", _body(SLI, "sli_disable_video"))
    dis = _body(SLI, "sli_disable")
    assert dis.index("write_3d(") < dis.index("sli_disable_video(io, n)")
    assert re.search(r"n < 1 \|\|\s*n > VCR_SLI_MAX_CHIPS\)\s*return VCR_SLI_EINVAL;", video)


SLI_RESET_HARNESS = r"""
/* A host harness around tests/native/test_vcr_kmd_sli.c's mock of four
 * VSA-100s (the suite's own main renamed away): vcr_sli_reset_video, the
 * bugcheck path's SLI/AA teardown, against the 4-chip state vcr_sli_set
 * leaves on .124. */
#define main vcr_kmd_sli_suite_main
#include "@SLI_TEST@"
#undef main

TEST(reset_takes_the_video_path_back_to_the_master_without_3d_writes) {
    mock *m = &M;
    vcr_sli_io io;
    vcr_sli_aa_req on = req(4, 1, 0, 0, 1, 32, 16);
    vcr_u32 c;
    unsigned before[4];
    unsigned long r0;
    mapped(m, &io, 4);
    CHECK(vcr_sli_set(&io, &on) >= 0, "enable failed");
    CHECK_EQ_U(CFG(m, 0, VCR_CFG_VIDEOCTRL0), 0x801u);   /* EN | VIDPLL_SEL, as on .124 */
    for (c = 0; c < 4; c++)
        before[c] = m->slictrl_direct[c];
    m->unlogged = 0;
    CHECK_EQ_I(vcr_sli_reset_video(&io, 4), 0);
    for (c = 0; c < 4; c++) {
        CHECK_EQ_U(m->slictrl_direct[c], before[c]);    /* no 3D write at all */
        CHECK_EQ_U(CFG(m, c, VCR_CFG_VIDEOCTRL0), c ? 0x03000000u : 0);
        CHECK_EQ_U(CFG(m, c, VCR_CFG_VIDEOCTRL1), 0);
        CHECK_EQ_U(CFG(m, c, VCR_CFG_SLIAAMISC), 0x800u);
        if (c) {                                         /* slaves: syncs off, DAC off */
            CHECK_EQ_U(IOR(m, c, VCR_R_DACMODE), 0xa);
            CHECK_EQ_U(IOR(m, c, VCR_R_VIDPROCCFG) & 1, 0);
        }
    }
    CHECK(has_step(m, VCR_SLI_S_OFF_VIDEOCTRL0), "logged");
    /* a status that never frees costs it nothing: it polls no FIFO */
    mapped(m, &io, 4);
    CHECK(vcr_sli_set(&io, &on) >= 0, "enable failed");
    for (c = 0; c < 4; c++)
        m->status_stuck[c] = 1;
    r0 = m->status_reads[0];
    CHECK_EQ_I(vcr_sli_reset_video(&io, 4), 0);
    CHECK_EQ_U(m->status_reads[0], r0);
    CHECK_EQ_U(CFG(m, 0, VCR_CFG_VIDEOCTRL0), 0);
    /* one chip: the master alone; nonsense refused */
    mapped(m, &io, 1);
    CHECK_EQ_I(vcr_sli_reset_video(&io, 1), 0);
    CHECK_EQ_U(CFG(m, 0, VCR_CFG_VIDEOCTRL0), 0);
    CHECK_EQ_I(vcr_sli_reset_video(&io, 0), VCR_SLI_EINVAL);
    CHECK_EQ_I(vcr_sli_reset_video(&io, 5), VCR_SLI_EINVAL);
    CHECK_EQ_I(vcr_sli_reset_video(NULL, 4), VCR_SLI_EINVAL);
    no_bus_faults(m);
}

int main(void)
{
    RUN(reset_takes_the_video_path_back_to_the_master_without_3d_writes);
    return munit_total_fails ? 1 : 0;
}
"""


def test_the_sli_reset_really_takes_the_master_off_the_sli_clock(tmp_path):
    """The behaviour, on the native SLI suite's own mock of the four chips:
    after a 4-chip SLI enable, the reset half leaves the master's
    cfgVideoCtrl0 at 0, the slaves' syncs tristated and their DACs and video
    processors off - with no 3D write and no FIFO poll."""
    cc = _host_cc()
    src = tmp_path / "sli_reset.c"
    src.write_text(SLI_RESET_HARNESS.replace("@SLI_TEST@", str(NATIVE / "test_vcr_kmd_sli.c")))
    exe = tmp_path / "sli_reset"
    b = subprocess.run([cc, "-std=c11", "-O0", "-g", "-Wall", f"-I{NATIVE}", f"-I{NATIVE}/stubs",
                        str(src), "-lm", "-o", str(exe)], capture_output=True, text=True)
    assert b.returncode == 0, b.stderr[-3000:]
    r = subprocess.run([str(exe)], capture_output=True, text=True, timeout=60)
    assert r.returncode == 0, r.stdout + r.stderr
    assert "[ ok ] reset_takes_the_video_path_back_to_the_master_without_3d_writes" in r.stdout


# ---- the driver: the mode list is never unfiltered for want of an EDID ----------


EDID_NATIVE_TESTS = {
    "the_monitor_id_is_the_edid_s_own_identity",
    "a_good_edid_puts_its_own_range_in_force",
    # a range-less EDID: only its OWN persisted range - round 1 handed it
    # whichever monitor answered last
    "an_edid_without_ranges_gets_its_own_persisted_range",
    "an_edid_without_ranges_never_gets_another_monitors_range",
    # no EDID: the envelope of every monitor seen, BOUNDED BY the default -
    # round 2 used the bare envelope, which after a Sony-only history listed
    # the Sony's rows for a tube that answers no DDC
    "no_edid_uses_the_envelope",
    "a_no_edid_boot_after_a_sony_only_history_gets_no_sony_rows",
    "the_envelope_is_the_intersection_of_every_monitor_seen",
    "a_zero_dot_clock_is_no_limit_not_the_smallest",
    "monitors_that_share_no_range_leave_the_default",
    "an_envelope_without_vga_is_not_trusted",
    # else the (conservative) default
    "no_edid_and_nothing_persisted_uses_the_safe_default",
    "mon_reset_forgets_the_envelope",
    "an_absurd_persisted_range_is_not_trusted",
    # whose limits: appended to vcr_info, numbered as the host reads them
    "vcr_info_reports_the_source_at_its_end",
}


def test_the_native_suite_runs_the_real_selection_for_every_source():
    src = EDID_NATIVE.read_text()
    assert '#include "../../voodoo-cleanroom/vcr-kmd/common/vcr_edid.c"' in src
    assert '#include "../../voodoo-cleanroom/vcr-kmd/include/vcr_ioctl.h"' in src
    # the functions the miniport calls, not only their parts
    assert "vcr_mon_boot(" in src and "vcr_mon_trust_envelope(" in src
    defined, run = _native_runs(EDID_NATIVE)
    assert EDID_NATIVE_TESTS <= defined, sorted(EDID_NATIVE_TESTS - defined)
    assert EDID_NATIVE_TESTS <= run, sorted(EDID_NATIVE_TESTS - run)


def test_the_mode_list_is_limited_even_without_an_edid():
    """No EDID used to mean no filter: 1600x1200@85 (106 kHz) was settable on
    .124's 96 kHz Sony whenever the DDC read failed. Round 1's fallback was the
    LAST monitor's range, whatever is plugged in now. The selection is
    common/vcr_edid.c vcr_mon_boot, run by tests/native/test_vcr_kmd_edid.c;
    this pins the kernel wiring."""
    body = _body(DDC, "VcrMonitorInit", strings=False)
    assert "vcr_hwcaps_set_monitor(" not in body         # the EDID-or-nothing path
    # the whole decision is the host-tested one, called once
    boot = body.index("src = vcr_mon_boot(e, reset, &env, &env_id, &r);")
    assert body.count("vcr_mon_boot(") == 1 and "vcr_mon_select(" not in body
    # its inputs are read before it: the persisted envelope and whose it is,
    # and the operator's reset request
    assert 0 <= body.index("mon_load(&env, &env_id);") < boot
    assert 0 <= body.index('reset = VcrDiagGet(L"MonReset", 0);') < boot
    # the envelope is saved whatever the source (vcr_mon_boot only ever
    # narrows it with an EDID's own range), straight after the decision
    assert re.search(r"src = vcr_mon_boot\(e, reset, &env, &env_id, &r\);\s*"
                     r"if \(mon_save\(&env, env_id\)\) \{", body)
    # MonReset is set back to 0 only AFTER the cleared envelope is saved, and
    # flushed: a power loss between the two repeats the reset rather than
    # keeping the envelope and losing the request
    rs = re.search(r'if \(reset\) \{[^}]*VcrDiagSet\(L"MonReset", 0, TRUE\);', body)
    assert rs and rs.start() > body.index("mon_save(&env, env_id)")
    assert body.count('VcrDiagSet(L"MonReset"') == 1                 # and nowhere earlier
    assert "VLOG(VCR_LV_WARN" in _block_after(body, rs.start())      # and says what it forgot
    assert re.search(r"vcr_hwcaps_set_range\(&x->caps, &r\);\s*\}$", body)
    # the only ways out before the range is set: a virtual display, and the
    # explicit EdidFilter=0 override - which says so at WARN
    rets = [m.start() for m in re.finditer(r"\breturn;", body)]
    assert len(rets) == 2
    assert body.rfind("x->backend != VCR_HW_VOODOO", 0, rets[0]) >= 0
    over = body[body.index('if (!VcrDiagGet(L"EdidFilter", 1))'):rets[1]]
    assert "VCR_LV_WARN" in over
    # a fallback is never silent: every boot the limits are not this EDID's
    # own, a WARN names the source - and for the default, why
    assert re.search(r"if \(src == VCR_MON_SRC_DEFAULT\)\s*VLOG\(VCR_LV_WARN", body)
    assert re.search(r"else if \(src != VCR_MON_SRC_EDID\)\s*VLOG\(VCR_LV_WARN", body)
    assert "default_why(e, &env, env_id)" in body and "vcr_mon_src_name(src)" in body
    why = _body(DDC, "default_why", strings=False)
    for reason in ('"none saved"', '"saved is other monitor\'s"', '"saved envelope unusable"'):
        assert reason in why, reason


def test_no_edid_narrows_the_default_with_the_envelope_never_widens_it():
    """Round 2 handed a no-EDID boot the bare envelope. But a tube that
    cannot answer DDC - or sits behind a KVM that eats it - is exactly the
    no-EDID case and was never narrowed into the envelope: after a Sony-only
    history the envelope WAS the Sony's range, and 1280x1024@85 (91 kHz) was
    listed for whatever was on the cable. So no EDID gets the envelope
    INTERSECTED with the conservative default; only Diag\\MonTrustEnvelope=1
    (default 0) lifts that, loudly, every boot."""
    sel = _body(EDID_C, "vcr_mon_select")
    no_edid = sel[sel.index("} else if (vcr_mon_range_usable(env)) {"):]
    assert re.search(r"\*out = \*env;\s*vcr_mon_envelope_add\(out, &def\);\s*"
                     r"if \(vcr_mon_range_usable\(out\)\)\s*return VCR_MON_SRC_ENVELOPE;", no_edid)
    # a range-less EDID of the SAME monitor keeps its own (bare) range: the
    # envelope is inside it
    assert re.search(r"if \(env_id && env_id == vcr_mon_id\(e\) && vcr_mon_range_usable\(env\)\) \{"
                     r"\s*\*out = \*env;\s*return VCR_MON_SRC_SAME;", sel)
    trust = _body(EDID_C, "vcr_mon_trust_envelope")
    assert re.search(r"if \(src != VCR_MON_SRC_ENVELOPE \|\| !vcr_mon_range_usable\(env\)\)\s*"
                     r"return 0;", trust)
    # the kernel: the trust applied only to an ENVELOPE decision, off by
    # default, after the envelope is saved and before the limits go in force
    body = _body(DDC, "VcrMonitorInit", strings=False)
    t = re.search(r'if \(src == VCR_MON_SRC_ENVELOPE && VcrDiagGet\(L"MonTrustEnvelope", 0\)\) \{'
                  r'\s*trusted = vcr_mon_trust_envelope\(src, &env, &r\);\s*if \(trusted\)\s*'
                  r'VLOG\(VCR_LV_WARN', body)
    assert t
    assert body.index("mon_save(&env, env_id)") < t.start() < body.index("x->mon_src = src;") \
        < body.rindex("vcr_hwcaps_set_range(&x->caps, &r);")
    assert body.count("MonTrustEnvelope") == 2           # read once, named in its WARN
    assert re.search(r'" bounded by default"', body)      # the source line says which it was


def test_the_default_is_conservative():
    """The first default (H 30-70 kHz, V 50-85 Hz, 135 MHz) was documented as
    what "any CRT of the era accepts" - false: 14" and 15" tubes of 1995-98
    stop between 38 and 60 kHz. Too narrow costs a mode the operator gets
    back with an EDID; too wide costs the tube. It may not widen again."""
    d = {k: _define(EDID_H, f"VCR_MON_DEF_{k}") for k in
         ("HMIN_KHZ", "HMAX_KHZ", "VMIN_HZ", "VMAX_HZ", "PIXCLK_KHZ")}
    assert d["HMIN_KHZ"] >= 30 and d["HMAX_KHZ"] <= 48
    assert d["VMIN_HZ"] >= 50 and d["VMAX_HZ"] <= 75
    assert 0 < d["PIXCLK_KHZ"] <= 80000
    # and it is what the selection uses, for the default and as the bound on
    # the envelope
    md = _body(EDID_C, "mon_default")
    for k in d:
        assert f"VCR_MON_DEF_{k}" in md, k
    assert "mon_default(&def);" in _body(EDID_C, "vcr_mon_select")


def test_whose_limits_are_in_force_is_appended_to_vcr_info_and_filled():
    """edid_ok alone cannot tell a same-monitor range from the envelope or the
    default, and the host gate must. mon_src is APPENDED - every older field
    keeps its offset - and an IOCTL_VCR_INFO from a tool built before it (its
    buffer ends before mon_src) is answered with the struct as it was, not
    refused."""
    info = _blank(IOCTL_H)
    s = info.index("typedef struct vcr_info {")
    blk = _block_after(info, s)
    fields = [ln.strip() for ln in blk.strip("{}").split(";") if ln.strip()]
    assert fields[-1] == "vcr_u32 mon_src" and re.match(r"vcr_u8\s+edid\[128\]", fields[-2])
    for name, val in (("NONE", 0), ("EDID", 1), ("SAME", 2), ("ENVELOPE", 3), ("DEFAULT", 4)):
        assert _define(EDID_H, f"VCR_MON_SRC_{name}") == val, name
    # the host reads it by those numbers
    assert mode_sweep.MON_SRC_OWN == (1, 2)
    assert {k: v.split()[0] for k, v in mode_sweep.MON_SRC_NAMES.items()} == \
        {0: "none", 1: "edid", 2: "same-monitor", 3: "envelope", 4: "default"}
    # filled from the decision; NONE before it, and again when EdidFilter=0
    # turns the filter off
    body = _body(DDC, "VcrMonitorInit", strings=False)
    first = body.index("x->mon_src = VCR_MON_SRC_NONE;")
    assert first < body.index("x->backend != VCR_HW_VOODOO") < body.index("x->mon_src = src;")
    ef = _block_after(body, body.index('if (!VcrDiagGet(L"EdidFilter", 1))'))
    assert "x->mon_src = VCR_MON_SRC_NONE;" in ef
    assert "ULONG     mon_src;" in (MINIPORT / "vcrmp.h").read_text()
    assert "v->mon_src = x->mon_src;" in _body(VCRMP, "fill_info")
    sio = _body(VCRMP, "VcrStartIO")
    case = _block_after(sio, sio.index("case IOCTL_VCR_INFO:"))
    assert "NEED_OUT(FIELD_OFFSET(vcr_info, mon_src));" in case
    assert re.search(r"info = rp->OutputBufferLength < sizeof v \? rp->OutputBufferLength : sizeof v;"
                     r"\s*VideoPortMoveMemory\(rp->OutputBuffer, &v, info\);", case)
    assert "v->size = sizeof *v;" in _body(VCRMP, "fill_info")


def test_vcrctl_info_reports_the_limits_in_force():
    """The host gate reads these: the EDID's raw fields read as zero without an
    EDID, which made a filtered list look unfiltered."""
    body = _body(VCRMP, "fill_info")
    for f in ("mon_hmin_khz", "mon_hmax_khz", "mon_vmin_hz", "mon_vmax_hz", "mon_max_pixclk_khz"):
        assert f"v->{f} = x->caps.{f};" in body, f
    assert "v->mon_filter = x->caps.mon_hmax_khz != 0;" in body
    assert "v->edid_ok = x->edid_ok;" in body


def test_the_persisted_range_is_read_back_under_the_names_it_was_saved_under():
    load = _body(DDC, "mon_load", strings=False)
    save = _body(DDC, "mon_save", strings=False)
    names = set(re.findall(r'L"(Mon\w+)"', load))
    assert len(names) == 6 and names == set(re.findall(r'L"(Mon\w+)"', save))
    # the identity the same-monitor rule compares against travels with it
    assert "MonId" in names and "MonReset" not in names and "MonTrustEnvelope" not in names
    # flushed: the boot that needs it is the one after a power-off
    sets = re.findall(r"VcrDiagSet\(L\"Mon\w+\", [^,]+, (TRUE|FALSE)\)", save)
    assert sets and sets[-1] == "TRUE"
    # written only when it changed: a registry write per boot buys nothing
    assert save.index("return 0;") < save.index("VcrDiagSet(")


def test_both_native_suites_run():
    run_all = (REPO / "tests" / "run_all.sh").read_text()
    assert 'for src in "$NAT"/test_*.c' in run_all
    for t in ("test_vcr_kmd_edid.c", "test_vcr_kmd_pace.c", "test_vcr_kmd_sli.c"):
        assert (REPO / "tests" / "native" / t).exists(), t


# ---- the display DLL: a desktop mode the list no longer holds falls back ----------


PICK_HARNESS = r"""
#include <stdio.h>
#include <string.h>
typedef unsigned long ULONG;
typedef long LONG;
typedef struct { ULONG VisScreenWidth, VisScreenHeight, BitsPerPlane, NumberOfPlanes,
                 Frequency; } VIDEO_MODE_INFORMATION;
typedef struct { ULONG dmPelsWidth, dmPelsHeight, dmBitsPerPel, dmDisplayFrequency; } DEVMODEW;
@FUNCS@
static VIDEO_MODE_INFORMATION L[512];
static ULONG N;
/* stdin, one per line: "C" clears the list, "A w h bpp hz" adds a mode,
 * "P w h bpp hz" picks (0 0 0 0 = the DEVMODE's fields zero), "Z" picks with
 * no DEVMODE at all. Each pick prints "w h bpp hz fell_back" or "-1". */
int main(void)
{
    char op[4];
    ULONG w, h, b, f, fb;
    LONG i;
    while (scanf("%3s", op) == 1) {
        if (!strcmp(op, "C")) {
            N = 0;
        } else if (!strcmp(op, "A") && scanf("%lu %lu %lu %lu", &w, &h, &b, &f) == 4) {
            L[N].VisScreenWidth = w; L[N].VisScreenHeight = h;
            L[N].BitsPerPlane = b; L[N].NumberOfPlanes = 1; L[N].Frequency = f;
            N++;
        } else if (!strcmp(op, "P") && scanf("%lu %lu %lu %lu", &w, &h, &b, &f) == 4) {
            DEVMODEW d;
            d.dmPelsWidth = w; d.dmPelsHeight = h; d.dmBitsPerPel = b; d.dmDisplayFrequency = f;
            i = pick_mode(L, N, &d, &fb);
            if (i < 0) printf("-1\n");
            else printf("%lu %lu %lu %lu %lu\n", L[i].VisScreenWidth, L[i].VisScreenHeight,
                        L[i].BitsPerPlane, L[i].Frequency, fb);
        } else if (!strcmp(op, "Z")) {
            i = pick_mode(L, N, NULL, &fb);
            if (i < 0) printf("-1\n");
            else printf("%lu %lu %lu %lu %lu\n", L[i].VisScreenWidth, L[i].VisScreenHeight,
                        L[i].BitsPerPlane, L[i].Frequency, fb);
        } else {
            return 2;
        }
    }
    return 0;
}
"""
PICK_FUNCS = ("mode_bpp", "pick_refresh", "shown_area", "largest_within", "nearest_bpp",
              "pick_mode")


@pytest.fixture(scope="module")
def pick(tmp_path_factory):
    """pick_mode and its helpers compiled on the host from display/vcrdd.c
    ITSELF: -> run(list of modes, requests) -> [answer per request]."""
    cc = _host_cc()
    d = tmp_path_factory.mktemp("pick")
    src = d / "pick.c"
    src.write_text(PICK_HARNESS.replace("@FUNCS@", "\n".join(_c_func(VCRDD, f)
                                                             for f in PICK_FUNCS)))
    exe = d / "pick"
    b = subprocess.run([cc, "-std=c11", "-O0", "-Wall", str(src), "-o", str(exe)],
                       capture_output=True, text=True)
    assert b.returncode == 0, b.stderr[-3000:]

    def run(listed, asks):
        lines = ["C"] + [f"A {m.replace('x', ' ').replace('@', ' ')}" for m in listed]
        for q in asks:
            lines.append("Z" if q is None else f"P {q.replace('x', ' ').replace('@', ' ')}")
        r = subprocess.run([str(exe)], input="\n".join(lines) + "\n", capture_output=True,
                           text=True, timeout=30)
        assert r.returncode == 0, r.stderr
        out = []
        for ln in r.stdout.split("\n")[:len(asks)]:
            p = ln.split()
            out.append(None if p == ["-1"] else (f"{p[0]}x{p[1]}x{p[2]}@{p[3]}", p[4] == "1"))
        return out
    return run


# what the conservative default leaves on a CRT: every depth
DEFAULT_LIST = [f"{m}x{b}@{hz}" for b in (8, 16, 32) for m, hz in
                (("640x480", 60), ("640x480", 75), ("800x600", 60), ("800x600", 72),
                 ("800x600", 75), ("1024x768", 60), ("1280x720", 60))]


def test_a_desktop_mode_the_list_no_longer_holds_falls_back_to_a_listed_one(pick):
    """The list honours the monitor, so a 1280x1024@85 desktop persisted under
    a good EDID is missing on the boot the monitor was off. Failing the PDEV
    for it handed the desktop to the VGA driver: the fallback is the largest
    listed size inside the request (by how much of the requested picture it
    shows - 1024x768, not the CVT 1280x720, for 1280x1024), at the depth asked
    or the nearest (deeper on a tie), at the highest refresh NOT above the
    one asked; else 640x480; else the smallest listed mode - never a failure
    while anything is listed, and every candidate is inside the limits."""
    asks = ["1024x768x16@60", "1024x768x32@85", "800x600x32@0", "800x600x32@70",
            "1280x1024x32@85", "1600x1200x24@75", "1920x1080x32@60", "512x384x16@60",
            "1280x960x16@60", "0x0x0@0", None]
    got = pick(DEFAULT_LIST, asks)
    assert got == [
        ("1024x768x16@60", False),       # listed: exact
        ("1024x768x32@60", False),       # listed size: the highest refresh below 85
        ("800x600x32@60", False),        # refresh 0: the lowest
        ("800x600x32@60", False),        # the highest BELOW 70, never 72 above it
        ("1024x768x32@60", True),        # not listed: 1024x768 shows more of a 5:4 picture
        ("1024x768x32@60", True),        # no 24 bpp: the nearest depth, the deeper on a tie
        ("1280x720x32@60", True),        # a 16:9 request keeps its shape
        ("640x480x16@60", True),         # nothing fits inside: 640x480
        ("1024x768x16@60", True),
        ("800x600x16@60", False),        # nothing asked: 800x600x16, the lowest refresh
        ("800x600x16@60", False),        # no DEVMODE at all: the same
    ]
    # nothing inside the request and no 640x480: the smallest listed mode at
    # its lowest refresh - not a failed PDEV
    assert pick(["1024x768x32@75", "1024x768x32@60", "1280x1024x32@60"],
                ["640x480x16@60"]) == [("1024x768x32@60", True)]
    # a fallback never picks a refresh above the one asked
    assert pick(["1024x768x32@60", "1024x768x32@70", "1024x768x32@85"],
                ["1280x1024x32@75"]) == [("1024x768x32@70", True)]
    # an empty list is the one failure left
    assert pick([], ["1024x768x16@60"]) == [None]
    # anything asked, from a list with anything in it, lands on that list
    many = [f"{w}x{h}x{b}@{hz}" for w, h in ((320, 200), (640, 480), (1600, 1200), (2048, 1536))
            for b in (8, 16, 24, 32) for hz in (0, 1, 50, 60, 75, 160)]
    for listed in (["1600x1200x16@60"], ["640x480x8@60", "2048x1536x32@60"], DEFAULT_LIST):
        for (m, _), q in zip(pick(listed, many), many):
            assert m in listed, (listed, q, m)


def test_the_display_dll_warns_when_it_fell_back():
    en = _body(VCRDD, "DrvEnablePDEV")
    call = en.index("i = pick_mode(m, n, pdm, &fell_back);")
    fail = en.index("if (i < 0) {")
    warn = re.search(r"if \(fell_back\)\s*VcrDd\(VCR_LV_WARN", en)
    assert call < fail < warn.start()
    assert "not listed" in _body(VCRDD, "DrvEnablePDEV", strings=False)[warn.start():][:400]


# ---- the host scripts: the monitor gate -----------------------------------------

# the Sony CPD-G200's EDID as XP stored it on .124 (the native suite's fixture)
SONY_EDID = _c_array(EDID_NATIVE.read_text(), "k_sony_cpd_g200")
SONY = {"cmd": "info", "ok": True, "edid_ok": 1, "mon_filter": 1, "mon_src": 1,
        "monitor": "SNY1270 CPD-G200", "mon_h_khz": [30, 96], "mon_v_hz": [48, 120],
        "mon_max_pixclk_khz": 260000, "edid": SONY_EDID.hex(), "log_next_seq": 7}


def _edid_as(pnp, product):
    """The Sony's EDID wearing another model's identity (bytes 8-11)."""
    i = ((ord(pnp[0]) - 64) << 10) | ((ord(pnp[1]) - 64) << 5) | (ord(pnp[2]) - 64)
    return SONY_EDID[:8] + bytes([i >> 8, i & 0xff, product & 0xff, product >> 8]) + SONY_EDID[12:]


# tools/modecalc.c's scan rates (hkhz_x1000, vhz_x1000, pixel kHz), as the
# driver programs them; pinned against the real mode math below
RATES = {
    "640x480x8@60": (31470, 59942, 25176),
    "640x480x16@85": (43262, 84994, 35994),
    "800x600x16@85": (53672, 85058, 56249),
    "1024x768x16@85": (68676, 84995, 94499),
    "1024x768x32@100": (81431, 100038, 113352),
    "1600x1200x16@70": (87431, 69944, 182556),
    "1600x1200x32@70": (87431, 69944, 182556),
    "1600x1200x16@75": (93716, 74972, 195681),
    "1600x1200x16@85": (106288, 85030, 221931),
    "1920x1440x16@75": (112538, 75025, 297102),
    "640x480x16@120": (61808, 120015, 52414),
    "1024x768x16@60": (48383, 60028, 65028),
}
CALC = {m: {"mode": m, "hkhz_x1000": h, "vhz_x1000": v, "khz": k} for m, (h, v, k) in RATES.items()}


def test_the_gate_needs_the_drivers_own_edid_filter_and_this_monitors_own_range():
    rng, why = mode_sweep.monitor_gate(SONY)
    assert why is None and rng["h_khz"] == (30, 96) and rng["pixclk_khz"] == 260000
    # a range-less EDID's same-monitor persisted range is its own too
    assert mode_sweep.monitor_gate(dict(SONY, mon_src=2))[1] is None
    refused = (None, {"ok": False, "error": "not the vcr-kmd driver?"},
               dict(SONY, edid_ok=0),        # a persisted / default range: not THIS monitor's
               dict(SONY, mon_filter=0),     # Diag\EdidFilter=0: every mode listed
               {k: v for k, v in SONY.items() if k != "mon_h_khz"},
               dict(SONY, mon_h_khz=[96, 30]), dict(SONY, mon_v_hz=[0, 120]),
               dict(SONY, mon_max_pixclk_khz=0), dict(SONY, mon_h_khz="30-96"))
    for info in refused:
        rng, why = mode_sweep.monitor_gate(info)
        assert rng is None and why, info
    # WHOSE range: the envelope (3), the default (4), none (0), a driver too
    # old to say (null), anything not a number - a host check against those
    # numbers proves nothing about the tube on the cable
    for src in (0, 3, 4, None, "1", 1.5, 5):
        rng, why = mode_sweep.monitor_gate(dict(SONY, mon_src=src))
        assert rng is None and "mon_src" in why, src
    rng, why = mode_sweep.monitor_gate({k: v for k, v in SONY.items() if k != "mon_src"})
    assert rng is None and "older" in why


def test_the_host_range_check_is_the_drivers_rule_on_the_programmed_timing():
    """Half a unit of slack at BOTH ends, the dot clock exact - exactly
    common/vcr_modes.c vcr_mode_check. Round 3 had no slack at the top, which
    refused the Sony's own 640x480@120 (120.015 Hz) that the driver lists:
    every refresh-0 fullscreen run at 640x480 was refused, and the battery
    latched the refusal as a driver failure (2026-09-26)."""
    rng, _ = mode_sweep.monitor_gate(SONY)

    def bad(h, v, k):
        return "m" in mode_sweep.out_of_range(["m"], rng, {"m": {"hkhz_x1000": h,
                                                                "vhz_x1000": v, "khz": k}})
    assert not bad(96500, 120500, 260000)       # half a unit over H and V: in, as the driver
    assert not bad(60000, 120015, 100000)       # the Sony's 640x480@120 - was refused
    assert bad(96501, 60000, 100000)            # past the half unit on H
    assert bad(60000, 120501, 100000)           # past the half unit on V
    assert bad(60000, 60000, 260001)            # a hair over the dot clock
    assert not bad(29500, 48000, 25000)         # the bottom keeps the driver's half unit
    assert bad(29499, 60000, 25000) and bad(60000, 47499, 25000)
    assert "x" in mode_sweep.out_of_range(["x"], rng, {})          # unknown timing: refused
    assert "m" in mode_sweep.out_of_range(["m"], rng, {"m": {"khz": 1}})
    # the real rows that decided it: 1600x1200@85 and 1920x1440@75 on the Sony
    out = mode_sweep.out_of_range(list(RATES), rng, CALC)
    assert set(out) == {"1600x1200x16@85", "1920x1440x16@75"}


@pytest.fixture(scope="module")
def real_calc():
    if not (shutil.which("gcc") or shutil.which("cc")):
        pytest.skip("no host C compiler: tools/modecalc.c not built - the real-mode-math "
                    "check did NOT run")
    import golden_compare
    return golden_compare.ours()


def test_the_pinned_rates_are_the_drivers_mode_math(real_calc):
    for m, (h, v, k) in RATES.items():
        o = real_calc[m]
        assert (o["hkhz_x1000"], o["vhz_x1000"], o["khz"]) == (h, v, k), m


def _default_range():
    d = {k: _define(EDID_H, f"VCR_MON_DEF_{k}") for k in
         ("HMIN_KHZ", "HMAX_KHZ", "VMIN_HZ", "VMAX_HZ", "PIXCLK_KHZ")}
    return {"h_khz": (d["HMIN_KHZ"], d["HMAX_KHZ"]), "v_hz": (d["VMIN_HZ"], d["VMAX_HZ"]),
            "pixclk_khz": d["PIXCLK_KHZ"], "monitor": "default"}


def test_the_battery_list_is_inside_the_sony_and_the_dangerous_rows_are_not(real_calc):
    rng, _ = mode_sweep.monitor_gate(SONY)
    assert not mode_sweep.out_of_range(list(silicon_battery.LIVE_MODES), rng, real_calc)
    danger = [f"{wh}x{b}@{hz}" for wh, hz in (("1600x1200", 85), ("1920x1440", 75))
              for b in (8, 16, 32)]
    assert set(mode_sweep.out_of_range(danger, rng, real_calc)) == set(danger)
    # and the driver's safe default (no EDID ever read) refuses them too
    assert set(mode_sweep.out_of_range(danger, _default_range(), real_calc)) == set(danger)


def test_the_default_keeps_vga_and_svga_and_refuses_what_a_small_tube_cannot_take(real_calc):
    """By the driver's own mode math and rule (half a unit of slack at both
    ends: 1024x768@60 at 48.4 kHz and 800x600@75 at 75.03 Hz stay, as the
    native suite pins): 640x480 and 800x600 stay, 1024x768@70 and up, every
    1280x1024 and 1280x960 and the 85 Hz rows go."""
    default = _default_range()
    kept = ["640x480x8@60", "640x480x16@75", "800x600x16@56", "800x600x16@60",
            "800x600x16@72", "1280x720x16@60"]
    assert not mode_sweep.out_of_range(kept, default, real_calc)
    refused = ["1024x768x16@70", "1024x768x16@75", "1024x768x16@85", "1280x1024x16@60",
               "1280x1024x16@75", "1280x1024x16@85", "1280x960x16@60", "640x480x16@85",
               "800x600x16@85"]
    assert set(mode_sweep.out_of_range(refused, default, real_calc)) == set(refused)


# every host script that switches the monitor, and the gate it goes through
GATED = {
    "mode_sweep.py": ("gate_modes(",),
    "silicon_battery.py": ("monitor_gate(",),
    "golden_capture.py": ("gate_on_box(",),
    "glidelab_run.py": ("gate_on_box(",),
    "glidelab_sweep.py": ("gate_on_box(",),
    "ddlab_run.py": ("fullscreen_gate(",),
    "d3dprobe_run.py": ("fullscreen_gate(",),
    "lab_run.py": ("fullscreen_gate(", "gate_on_box("),
    "sli_golden.py": ("fullscreen_gate(",),
    "sli_shot.py": ("fullscreen_gate(",),
    "sli_golden_sweep.py": ("sg.gate(",),
}


def test_every_switching_script_gates_on_the_same_check():
    """Any host script that starts a switching tool (modeseq, a golden, a lab,
    Quake II through our ICD) goes through mode_sweep's gate - the round-2
    battery had it, the lab runners and the SLI scripts did not."""
    starts = re.compile(r"(ddlab|d3dprobe|glidelab)\.exe|\bmodeseq\b|\} golden |Quake2|"
                        r"\{a\.lab\}\.exe")
    switching = {p.name for p in TOOLS.glob("*.py") if starts.search(p.read_text())}
    assert switching >= {"mode_sweep.py", "golden_capture.py", "ddlab_run.py", "d3dprobe_run.py",
                         "lab_run.py", "glidelab_run.py", "sli_golden.py", "sli_shot.py"}
    assert switching <= set(GATED), sorted(switching - set(GATED))
    for name, gates in GATED.items():
        src = (TOOLS / name).read_text()
        assert name == "mode_sweep.py" or re.search(r"^import (mode_sweep|sli_golden)\b", src,
                                                    re.M), name
        for g in gates:
            assert g in src, (name, g)
    # the helpers all end in the one monitor gate
    for helper in ("gate_on_box", "fullscreen_gate"):
        h = SWEEP[SWEEP.index(f"async def {helper}("):]
        h = h[:h.index("\n\n\n")]
        assert "gate_modes(" in h and "choose_monitor_info(" in h, helper
    assert "monitor_gate(info, where)" in SWEEP[SWEEP.index("def gate_modes("):]


# ---- the host scripts: --monitor-info is checked against the box -------------------


def _registry(monitors, broken=False):
    """FakeAgent answers for REGREAD of Enum\\DISPLAY: monitors is
    [(model id, instance, present now, EDID bytes or None)]."""
    root = mode_sweep.DISPLAY_ENUM
    if broken:
        def dropped(cmd):
            raise ConnectionResetError("REGREAD dropped")
        return [(f'REGREAD HKLM "{root}"', dropped)]
    ids = sorted({m[0] for m in monitors})
    out = [(f'REGREAD HKLM "{root}"', json.dumps({"values": [], "subkeys": ids}))]
    for i in ids:
        insts = [m[1] for m in monitors if m[0] == i]
        out.append((f'REGREAD HKLM "{root}\\{i}"', json.dumps({"values": [], "subkeys": insts})))
    for i, inst, present, e in monitors:
        p = f"{root}\\{i}\\{inst}"
        sub = ["Device Parameters"] + (["Control"] if present else [])
        out.append((f'REGREAD HKLM "{p}\\Device Parameters" EDID',
                    json.dumps({"value": {"name": "EDID", "type": "REG_BINARY",
                                          "data": " ".join(f"{x:02X}" for x in e)}})
                    if e else '{"root":"HKLM","path":"x","value":}'))
        out.append((f'REGREAD HKLM "{p}"', json.dumps({"values": [], "subkeys": sub})))
    return out


def _fresh(monitors, saved=SONY, checked=False, broken=False):
    box = FakeAgent(_registry(monitors, broken))

    async def call(cmd, t):
        return await box("h").send_command(cmd, timeout=t)
    return asyncio.run(mode_sweep.monitor_fresh(call, saved, "--monitor-info x", checked)), box


SONY_PRESENT = ("SNY1270", "5&1", True, SONY_EDID)
OTHER_EDID = _edid_as("GSM", 0x5678)


def test_a_saved_monitor_info_stands_in_only_for_the_monitor_it_describes(capsys):
    """Under the vendor driver nothing on the box answers for the EDID, so the
    gate reads a `vcrctl info` saved from OUR driver. A saved file says
    nothing about NOW: the fleet's hardware moves between boxes. XP keeps
    each monitor devnode's EDID under Enum\\DISPLAY whatever the driver, and
    the present one has a Control subkey: that must name the saved model."""
    # the present Sony: stands in; an old devnode's EDID is never read
    why, box = _fresh([SONY_PRESENT, ("OLD1234", "5&2", False, _edid_as("OLD", 0x1234))])
    assert why is None
    assert not box.cmds("OLD1234\\5&2\\Device Parameters")
    # a DIFFERENT model on the box: refused, and the flag cannot override it
    for checked in (False, True):
        why, _ = _fresh([("GSM5678", "5&1", True, OTHER_EDID)], checked=checked)
        assert why and "GSM5678" in why and "SNY1270" in why
    # a registry that cannot say is refused (fail closed) ...
    why, _ = _fresh([], broken=True)
    assert why and mode_sweep.CHECKED_FLAG in why
    why, _ = _fresh([("Default_Monitor", "5&1", True, None)])
    assert why and "no present monitor carries an EDID" in why
    why, _ = _fresh([SONY_PRESENT, ("Default_Monitor", "5&2", True, None)])
    assert why and "without an EDID" in why
    why, _ = _fresh([("SNY1270", "5&1", False, SONY_EDID)])       # only an old devnode
    assert why and "none is present" in why
    # ... unless the operator looked at the box and SAYS so - loudly
    capsys.readouterr()
    why, _ = _fresh([], broken=True, checked=True)
    assert why is None and "MONITOR NOT CONFIRMED" in capsys.readouterr().out
    # the saved file must carry its own EDID, and agree with it
    why, _ = _fresh([SONY_PRESENT], saved=dict(SONY, edid=""))
    assert why and "no EDID" in why
    why, _ = _fresh([SONY_PRESENT], saved=dict(SONY, monitor="GSM5678 x"))
    assert why and "disagree" in why


def test_the_live_driver_wins_over_a_saved_info():
    """With OUR driver installed the live `vcrctl info` is the gate - a saved
    file never overrides it (the live one may be on the envelope, and a
    file must not turn that into a pass)."""
    async def call(cmd, t):
        raise AssertionError(f"no registry read with our driver live: {cmd}")
    info, where, why = asyncio.run(mode_sweep.choose_monitor_info(
        call, dict(SONY, mon_src=3), "/nonexistent.json"))
    assert info["mon_src"] == 3 and where == "vcrctl info" and why is None
    assert mode_sweep.monitor_gate(info, where)[0] is None


# ---- the host scripts: mode_sweep ------------------------------------------------


def test_mode_sweep_is_short_and_paced_by_default():
    assert re.search(r'"--pace", type=float, default=5\.0', SWEEP)
    assert re.search(r'"--max-live", type=int, default=12', SWEEP)
    assert "max(a.pace, 3.0)" in SWEEP
    assert "modeseq" in SWEEP
    # the old per-mode `modetest` loop (a desktop bounce per mode) is gone
    assert 'run(f"modetest' not in SWEEP
    assert "REFUSED" in SWEEP and "a.allow_many" in SWEEP
    # the agent clamps EXECW at 900 s and kills the tree - which can leave
    # vcrctl holding a mode - so a list that could run that long is refused
    assert mode_sweep.EXECW_CEILING < 900


def test_restore_is_needed_only_when_the_run_did_not_give_the_mode_back():
    need = mode_sweep.restore_needed
    assert need(None)                                     # died, timed out, never heard
    assert need({"cmd": "modeseq", "ok": True})           # a summary without a restore
    assert need({"cmd": "modeseq", "restore": -1})        # the restore failed
    assert need({"cmd": "modeseq", "restore": -1000, "error": "pace lock busy"})  # not made
    assert not need({"cmd": "modeseq", "ok": True, "restore": 0})
    assert not need({"cmd": "modeseq", "ok": False, "stopped": True, "restore": 0})
    assert not need({"cmd": "modeseq", "ok": False, "busy": True, "error": "busy"})
    assert not need({"cmd": "modeseq", "ok": False, "error": "too many modes"})


def test_the_timeout_marker_is_the_agents_own_words():
    """Every cleanup keys on the line the agent appends when EXECW killed the
    tree at its timeout: a marker that drifted from the agent's would make a
    timed-out run read as a clean one."""
    exec_c = (REPO / "agent" / "src" / "exec.c").read_text()
    m = re.search(r'"\\n(\[EXECW: timed out[^"\\]*)\\n"', exec_c)
    assert m and m.group(1).startswith(mode_sweep.EXECW_TIMED_OUT)
    assert TIMED_OUT.strip() == m.group(1)


class FakeAgent:
    """Stands in for RetroConnection: answers what a script asks the agent and
    records it. Nothing here touches the network. An answer is a string, or a
    callable(cmd) returning one - or raising, which is how a host timeout or a
    dropped connection is played - or returning a coroutine, which is awaited
    (a reply that takes its time). The first answer whose prefix the command
    starts with is the one given."""

    def __init__(self, answers):
        self.answers = answers           # [(command prefix, str | callable(cmd) -> str)]
        self.sent = []                   # [(command, timeout)]
        self.timeline = []               # [("cmd", command) | ("sleep", s)], in order

    def __call__(self, host, port=9898):
        fake = self

        class Conn:
            async def connect(self, secret, timeout=None):
                pass

            async def send_command(self, cmd, timeout=None, binary_payload=None):
                fake.sent.append((cmd, timeout))
                fake.timeline.append(("cmd", cmd))
                for prefix, ans in fake.answers:
                    if cmd.startswith(prefix):
                        out = ans(cmd) if callable(ans) else ans
                        if asyncio.iscoroutine(out):
                            out = await out
                        return 0, out.encode() if isinstance(out, str) else out
                return 0, b""

            async def close(self):
                pass
        return Conn()

    def cmds(self, needle=""):
        return [c for c, _ in self.sent if needle in c]


VCRCTL_BOX = r"C:\vcr\vcrctl.exe"
VCRCTL_IMAGE = "VCRCTL.EXE"
# the only kill a host script may make, and the only restore
PACE_KILL_CMD = f"EXECW {mode_sweep.PACE_KILL_EXECW_S} {VCRCTL_BOX} pace-kill "
RESTORE_CMD = f"EXECW {mode_sweep.RESTORE_EXECW_S} {VCRCTL_BOX} restore"


class Procs:
    """The box's process table, for the cleanup paths. PROCLIST answers
    `before` ({pid: image}) until the tool is launched - the runner's snapshot
    of what was running already, i.e. not its own - and what is still
    `running` after that. `vcrctl pace-kill <pid>` answers as vcrctl does:
    refused for an image vcr_pace.h's list does not hold, "pace lock busy"
    for a pid in `busy`, killed-but-not-gone for one in `unkillable`, else
    killed. PROCKILL and taskkill still answer, so a script that goes back to
    them is SEEN doing it (every test here asserts they never are)."""

    def __init__(self, before=None, after=None, unkillable=(), busy=(), closes=()):
        self.before = dict(before or {})
        self.running = dict(after or {})
        self.unkillable, self.busy, self.closes = set(unkillable), set(busy), set(closes)
        self.launched = False
        self.pace_killed = []

    def launch(self, ans):
        """`ans`, answered as the launch of the tool (the EXECW)."""
        def go(cmd):
            self.launched = True
            return ans(cmd) if callable(ans) else ans
        return go

    def table(self):
        return self.running if self.launched else self.before

    def proclist(self, cmd):
        return json.dumps([{"name": "explorer.exe", "pid": 1}, {"name": "retro_agent.exe", "pid": 2}]
                          + [{"name": n, "pid": p} for p, n in self.table().items()])

    def pace_kill(self, cmd):
        pid = int(cmd.split()[-1])
        self.pace_killed.append(pid)
        image = self.table().get(pid)

        def say(ok, exited=False, **kw):
            return json.dumps(dict({"cmd": "pace-kill", "ok": ok, "pid": pid, "exited": exited},
                                   **kw)) + "\n"
        if pid in (0, 4) or image is None:
            return say(False, error="cannot open the process (gone already?)", win32_error=87)
        if image.lower() not in PACE_KILL_IMAGES:
            return say(False, image=image, error="not a vcr-kmd tool - only vcrctl, ddlab, "
                       "d3dprobe and glidelab are killed")
        if pid in self.busy:
            return say(False, image=image, error="pace lock busy")
        if pid in self.unkillable:
            return say(False, image=image, error="terminated but not gone after 10 s")
        self.table().pop(pid, None)
        return say(True, exited=True, image=image)

    def kill(self, cmd):
        pid = int(cmd.split()[1])
        if pid not in self.unkillable:
            self.table().pop(pid, None)
        return "OK"

    def close(self, cmd):
        pid = int(cmd.split()[-1])
        if pid in self.closes:
            self.table().pop(pid, None)
        return ""

    def answers(self):
        return [("PROCLIST", self.proclist), (PACE_KILL_CMD, self.pace_kill),
                ("PROCKILL ", self.kill), ("EXEC taskkill", self.close)]


def _at(timeline, needle, start=0):
    """Where in the timeline the first command containing `needle` (at or
    after `start`) was sent."""
    for i in range(start, len(timeline)):
        if timeline[i][0] == "cmd" and needle in timeline[i][1]:
            return i
    raise AssertionError(f"never sent (from {start}): {needle!r}")


def _slept(timeline, lo, hi):
    return sum(v for k, v in timeline[lo:hi] if k == "sleep")


def _launches(box):
    """The EXECWs that started a tool - not the pace-kills and restores."""
    return [(c, t) for c, t in box.sent if c.startswith("EXECW")
            and " pace-kill " not in c and not c.endswith(" restore")]


def _killed_only_through_pace_kill(box):
    """A PROCKILL, EXECW's tree kill or `taskkill` drops the victim's mode the
    instant it dies, unpaced and unrecorded; `taskkill /im` killed EVERY
    vcrctl on the box, another session's too. The only kill is `vcrctl
    pace-kill <pid>`, under EXECW with a budget the host outwaits."""
    assert not box.cmds("PROCKILL"), box.cmds("PROCKILL")
    assert not box.cmds("taskkill"), box.cmds("taskkill")
    for c, t in box.sent:
        if " pace-kill " in c:
            assert re.fullmatch(rf"EXECW {mode_sweep.PACE_KILL_EXECW_S} \S+ pace-kill \d+", c), c
            assert t > mode_sweep.PACE_KILL_EXECW_S, (c, t)


def _record_sleeps(monkeypatch, box, hook=None):
    """asyncio.sleep -> a note in the box's timeline (and `hook`: a signal
    arriving during a wait). The scripts share one asyncio module, so this
    covers mode_sweep.reap()'s waits in every tool that calls it."""
    async def no_wait(s, *args, **kw):
        box.timeline.append(("sleep", s))
        if hook:
            hook()
    monkeypatch.setattr(mode_sweep.asyncio, "sleep", no_wait)


DESKTOP = "1280x1024x32@85"
RESTORE_OK = '{"cmd":"restore","ok":true,"result":0}'
RESTORE_FAILED = '{"cmd":"restore","ok":false,"result":-1}'
PACE_MARK_OK = '{"cmd":"pace-mark","ok":true}'
TIMED_OUT = "[EXECW: timed out, process tree killed]\n"
STOP_FILE = mode_sweep.STOP_FILE


def _box(modes, info=SONY, execw=None, restore=RESTORE_OK, procs=None, left_in="1024x768x16@85"):
    """A box running our driver. `vcrctl modes` says DESKTOP is current until
    the tool is launched and `left_in` after it: a run that did not end
    civilly may have left a test mode up. The restore is answered only as
    the paced EXECW it must be; a plain `EXEC ... restore` gets nothing."""
    procs = procs or Procs()

    def listed(cmd):
        return json.dumps({"cmd": "modes", "ok": True, "modes": modes,
                           "current": left_in if procs.launched else DESKTOP})
    box = FakeAgent([
        (f"EXEC {VCRCTL_BOX} info", json.dumps(info)),
        (f"EXEC {VCRCTL_BOX} modes", listed),
        (f"EXEC {VCRCTL_BOX} log", "#vcrlog boot_count=1 version=1 entries=1024 next_seq=7\n"),
        (RESTORE_CMD, restore),
        (f"EXEC {VCRCTL_BOX} pace-mark", PACE_MARK_OK),
    ] + procs.answers() + [("EXECW", procs.launch(execw or ""))])
    box.procs = procs
    return box


def _modeseq_out(done, **summary):
    """What `vcrctl modeseq` prints: a line per mode it ran (`done`), then its
    summary (`summary` overrides a clean run's)."""
    lines = [json.dumps({"cmd": "modetest", "ok": True, "index": i, "current": m,
                         "gdi": {"mismatches": 0}, "log_next_seq": 7}) for i, m in enumerate(done)]
    s = {"cmd": "modeseq", "ok": True, "modes": len(done), "ran": len(done), "failed": 0,
         "stopped": False, "pace_ms": 5000, "restore": 0}
    s.update(summary)
    return "\n".join(lines + [json.dumps(s)]) + "\n"


def _sweep(monkeypatch, box, argv, hook=None, calc=CALC):
    a, code = _parsed(mode_sweep, argv)
    assert code == 0
    monkeypatch.setattr(mode_sweep, "RetroConnection", box)
    monkeypatch.setattr(mode_sweep, "modecalc", lambda: calc)
    _record_sleeps(monkeypatch, box, hook)
    rc = asyncio.run(mode_sweep.main_async(a))
    return rc, [v for k, v in box.timeline if k == "sleep"]


IN_RANGE = ["640x480x8@60", "640x480x16@85", "800x600x16@85", "1024x768x16@85"]
# the fallback's bounded wait for a modeseq told to stop, at --pace 5: the
# wait before its next switch and before its restore - each queued behind
# the switch lock first - a mode under test, a margin
CIVIL_WAIT = 2 * mode_sweep.switch_s(5.0) + mode_sweep.MODESEQ_STEP_S + mode_sweep.CIVIL_MARGIN_S
# ten more modes the Sony takes, for a plan that is long by its budget
LONG_LIST = [f"{640 + 16 * k}x480x16@60" for k in range(10)]
LONG_CALC = dict(CALC, **{m: {"mode": m, "hkhz_x1000": 31470, "vhz_x1000": 60000, "khz": 25176}
                          for m in LONG_LIST})


@pytest.mark.parametrize("why,modes,info,extra", (
    ("more than --max-live", list(RATES) + [f"{w}x{w}x16@60" for w in range(100, 110)], SONY, []),
    ("no EDID", IN_RANGE, dict(SONY, edid_ok=0), []),
    ("filter off", IN_RANGE, dict(SONY, mon_filter=0), []),
    ("the envelope", IN_RANGE, dict(SONY, mon_src=3), []),
    ("the default", IN_RANGE, dict(SONY, mon_src=4), []),
    ("a driver too old to say", IN_RANGE, dict(SONY, mon_src=None), []),
    ("out of the Sony's range", ["1024x768x16@85", "1600x1200x16@85"], SONY, []),
    ("a pace past vcr_pace.h's maximum", IN_RANGE, SONY, ["--pace", "200"]),
    ("longer than the agent's EXECW clamp", LONG_LIST, SONY, ["--pace", "25"]),
))
def test_mode_sweep_refuses_before_anything_is_switched(monkeypatch, why, modes, info, extra):
    box = _box(modes, info)
    argv = ["192.168.1.124", "--modes", ",".join(modes)] + extra
    if why == "more than --max-live":
        argv = ["192.168.1.124"] + extra
    rc, _ = _sweep(monkeypatch, box, argv, calc=LONG_CALC)
    assert rc == 2, why
    assert not box.cmds("EXECW") and not box.cmds("modeseq") and not box.cmds("restore"), why
    assert not box.cmds("PROCKILL") and not box.cmds("pace-mark"), why
    if why == "longer than the agent's EXECW clamp":
        # it WOULD fit a --pace 5 plan: the budget counts the pace and the lock
        assert mode_sweep.modeseq_budget(len(modes), 25) > mode_sweep.EXECW_CEILING
        assert mode_sweep.modeseq_budget(len(modes), 5) <= mode_sweep.EXECW_CEILING


def test_the_test_bed_bypass_is_refused_on_a_real_box(monkeypatch):
    box = _box(IN_RANGE)
    rc, _ = _sweep(monkeypatch, box, ["192.168.1.124", "--test-bed"])
    assert rc == 2 and not box.sent                     # refused before connecting
    assert mode_sweep.is_loopback("127.0.0.1") and not mode_sweep.is_loopback("192.168.1.124")


def test_mode_sweep_runs_one_paced_modeseq_and_leaves_a_good_restore_alone(monkeypatch):
    box = _box(IN_RANGE, execw=_modeseq_out(IN_RANGE), left_in=DESKTOP)
    rc, _ = _sweep(monkeypatch, box, ["192.168.1.124", "--modes", ",".join(IN_RANGE)])
    assert rc == 0
    execw = _launches(box)
    assert len(execw) == 1
    cmd, timeout = execw[0]
    budget = int(cmd.split()[1])
    assert "modeseq 5000 " in cmd and budget <= mode_sweep.EXECW_CEILING
    # a wait before every switch - each mode, the restore, a failed restore's
    # exit hold - each able to queue behind another tool's switch lock
    assert budget == mode_sweep.modeseq_budget(len(IN_RANGE), 5.0)
    assert budget >= (len(IN_RANGE) + 2) * (mode_sweep.PACE_LOCK_S + 5)
    assert timeout > budget                           # the host outwaits the agent's kill
    names = [c for c, _ in box.sent]
    # a stop file left by an interrupted run is removed BEFORE, or it ends this one
    assert names.index(f"DELETE {STOP_FILE}") < names.index(cmd)
    # what runs already is listed BEFORE the launch: a cleanup kill spares it
    assert names.index("PROCLIST") < names.index(cmd)
    # it ended civilly: nothing stopped, killed, stamped or restored from here
    assert not box.cmds("restore") and not box.cmds("pace-kill")
    _killed_only_through_pace_kill(box)
    assert not box.cmds("pace-mark") and not box.cmds("UPLOAD")


def test_the_plan_counts_switches_and_the_timing_changes_they_cost(monkeypatch, capsys):
    box = _box(IN_RANGE, execw=_modeseq_out(IN_RANGE), left_in=DESKTOP)
    rc, _ = _sweep(monkeypatch, box, ["192.168.1.124", "--modes", ",".join(IN_RANGE)])
    assert rc == 0
    m = re.search(r"(\d+) mode switches incl\. the restore = (\d+) timing changes",
                  capsys.readouterr().out)
    switches = len(IN_RANGE) + 1
    assert m and int(m.group(1)) == switches
    assert int(m.group(2)) == mode_sweep.TIMING_CHANGES_PER_SWITCH * switches


@pytest.mark.parametrize("out", ("", TIMED_OUT, _modeseq_out(IN_RANGE, ok=False, restore=-1),
                                 _modeseq_out(IN_RANGE[:2], ok=False, ran=2, restore=-1000,
                                              error="pace lock busy")),
                         ids=("no-reply", "execw-timed-out", "restore-failed", "restore-refused"))
def test_mode_sweep_restores_once_when_the_run_did_not(monkeypatch, out):
    """No summary, the agent's EXECW timeout, a restore that failed or that
    the gate refused: vcrctl may be still running and pacing, and killing it
    would skip its hold. So it is not killed first: the stop file, vcrctl
    leaves on its own, the pace is stamped and waited, and the desktop is
    restored ONCE - under EXECW, as the paced switch it is - and a failed
    restore is not retried."""
    box = _box(IN_RANGE, execw=out, restore=RESTORE_FAILED)
    rc, _ = _sweep(monkeypatch, box, ["192.168.1.124", "--modes", ",".join(IN_RANGE)])
    assert rc != 0
    assert box.cmds(" restore") == [RESTORE_CMD]          # once, paced, NOT retried
    (c, t), = [(c, t) for c, t in box.sent if c.endswith(" restore")]
    assert t > mode_sweep.RESTORE_EXECW_S
    tl = box.timeline
    ex = _at(tl, "EXECW")
    up = _at(tl, f"UPLOAD {STOP_FILE}", ex)
    gone = _at(tl, "PROCLIST", up)                        # has vcrctl left?
    pm = _at(tl, "pace-mark", gone)
    rs = _at(tl, " restore", pm)
    # it had left on its own: nothing was killed
    assert not box.cmds("pace-kill")
    _killed_only_through_pace_kill(box)
    # the stop file nothing consumed goes before the restore: it would end
    # the next run before its first switch
    assert pm < _at(tl, f"DELETE {STOP_FILE}", pm) < rs
    assert _slept(tl, pm, rs) >= 5                        # the pace (--pace 5)


def test_a_timed_out_run_takes_the_fallback_even_with_a_summary(monkeypatch):
    """A vcrctl that outlived the agent's tree kill may still hold the mode,
    so the fallback runs - but a restore is one more switch: none while the
    desktop mode is the current one."""
    box = _box(IN_RANGE, execw=_modeseq_out(IN_RANGE) + TIMED_OUT, left_in=DESKTOP)
    _sweep(monkeypatch, box, ["192.168.1.124", "--modes", ",".join(IN_RANGE)])
    assert box.cmds(f"UPLOAD {STOP_FILE}") and box.cmds("pace-mark")
    assert not box.cmds(" restore")


def _fallback(monkeypatch, procs, spare=(), stop_sent=False):
    box = _box(IN_RANGE, procs=procs)
    procs.launched = True
    monkeypatch.setattr(mode_sweep, "RetroConnection", box)
    _record_sleeps(monkeypatch, box)
    a = argparse.Namespace(host="192.168.1.124", port=9898, tool=VCRCTL_BOX)
    r = asyncio.run(mode_sweep.fallback_restore(a, 5.0, desktop=DESKTOP,
                                                spare=None if spare is None else list(spare),
                                                stop_sent=stop_sent))
    return box, r


def test_the_fallback_waits_for_vcrctl_before_it_pace_kills_it_by_pid(monkeypatch):
    box, r = _fallback(monkeypatch, Procs(after={9: VCRCTL_IMAGE}))
    assert r and r.get("ok")
    tl = box.timeline
    up, kill = _at(tl, f"UPLOAD {STOP_FILE}"), _at(tl, "pace-kill")
    # the stop file first, then the WHOLE bounded wait (the lock time in it),
    # then the kill - through the gate, by PID
    assert up < kill and _slept(tl, up, kill) >= CIVIL_WAIT
    assert CIVIL_WAIT >= 2 * (mode_sweep.PACE_LOCK_S + 5)
    assert box.cmds("pace-kill") == [PACE_KILL_CMD + "9"]
    _killed_only_through_pace_kill(box)
    # pace-kill stamped the revert: no second stamp, and the pace before the
    # one restore
    assert not box.cmds("pace-mark")
    rs = _at(tl, " restore", kill)
    assert _slept(tl, kill, rs) >= 5
    assert box.cmds(" restore") == [RESTORE_CMD]
    # bounded: a look per poll, then the kill's own confirm
    assert len(box.cmds("PROCLIST")) <= int(CIVIL_WAIT // mode_sweep.POLL_S) + 1 + 6


@pytest.mark.parametrize("why", ("unkillable", "busy"))
def test_the_fallback_never_restores_under_a_vcrctl_that_is_still_running(monkeypatch, why):
    """Killed but not gone, or the pace lock busy (it hung holding it): the
    survivor stays, is SAID to be there, is not killed by any other route,
    and no restore is made under it - XP gives the mode back when it goes."""
    procs = Procs(after={9: VCRCTL_IMAGE}, **{why: {9}})
    box, r = _fallback(monkeypatch, procs)
    assert r is None and not box.cmds(" restore")
    assert box.cmds("pace-kill") == [PACE_KILL_CMD + "9"]      # one kill, never a loop of them
    _killed_only_through_pace_kill(box)
    assert box.cmds("pace-mark")                               # not stamped by a kill: stamp now
    assert len(box.cmds("PROCLIST")) <= int(CIVIL_WAIT // mode_sweep.POLL_S) + 1 + 6


def test_the_fallback_spares_a_vcrctl_that_predates_the_sweep(monkeypatch):
    """Another session's vcrctl is left alone - and no restore is made under
    it, which could fight it. The stop file, already up, is not sent twice."""
    box, r = _fallback(monkeypatch, Procs(after={4: VCRCTL_IMAGE}), spare=[4], stop_sent=True)
    assert r is None
    assert not box.cmds("pace-kill") and not box.cmds(" restore") and not box.cmds("UPLOAD")
    _killed_only_through_pace_kill(box)
    assert box.cmds("pace-mark")                          # a stamp switches nothing


def test_the_fallback_kills_nothing_when_it_cannot_tell_whose_a_vcrctl_is(monkeypatch):
    """The PROCLIST before the launch did not answer: a vcrctl running now may
    be another session's. Nothing is killed, nothing restored, and it says
    so."""
    box, r = _fallback(monkeypatch, Procs(after={9: VCRCTL_IMAGE}), spare=None)
    assert r is None
    assert not box.cmds("pace-kill") and not box.cmds(" restore")
    _killed_only_through_pace_kill(box)
    assert box.cmds("pace-mark")


def test_a_signal_stops_the_list_on_the_box_instead_of_abandoning_it():
    """The 2026-09-26 sweep was stopped part-way; a host that simply dies
    mid-EXECW leaves vcrctl to run its whole list. A signal escalates one
    step at a time, and nothing is killed before the THIRD."""
    enter = SWEEP[SWEEP.index("def __enter__"):SWEEP.index("def __exit__")]
    assert "signal.SIGINT" in enter and "signal.SIGTERM" in enter
    assert re.search(r'box_cmd\(a, f"UPLOAD \{STOP_FILE\}"', SWEEP)
    assert mode_sweep.KILL_AFTER_SIGNALS == 3

    async def go():
        loop = asyncio.get_running_loop()
        it = mode_sweep.Interrupts()
        it.execw = loop.create_future()
        it.on_signal("SIGINT")
        assert it.stop.is_set() and not it.execw.cancelled()   # first: ask vcrctl to stop
        it.on_signal("SIGTERM")
        assert it.execw.cancelled() and not it.force.is_set()   # second: stop waiting, kill nothing
        it.on_signal("SIGINT")
        assert it.force.is_set()                                # third: may kill
        # in the fallback's wait for vcrctl to leave, only the third counts
        w = mode_sweep.Interrupts()
        w.restoring = w.waiting = True
        w.on_signal("SIGINT")
        w.on_signal("SIGTERM")
        assert not w.force.is_set() and not w.stop.is_set()
        w.on_signal("SIGINT")
        assert w.force.is_set()
        # after the wait, the kill / restore decision finishes first
        busy = mode_sweep.Interrupts()
        busy.restoring = True
        for _ in range(3):
            busy.on_signal("SIGINT")
        assert not busy.stop.is_set() and not busy.force.is_set()
    asyncio.run(go())
    # the restore decision is taken in a finally, whatever happened to the EXECW
    fin = SWEEP[SWEEP.index("            finally:"):]
    assert fin.index("intr.restoring = True") < fin.index("if restore_needed(seqsum) or timed_out:")


def _signalled_sweep(monkeypatch, n, procs):
    """A sweep that gets `n` signals while vcrctl runs its list: the first as
    modeseq starts, the second once the stop file is up, the third in the
    fallback's wait for vcrctl to leave."""
    holder, box = {}, None

    class Caught(mode_sweep.Interrupts):
        def __init__(self):
            super().__init__()
            holder["it"] = self
    monkeypatch.setattr(mode_sweep, "Interrupts", Caught)

    async def execw(cmd):
        it = holder["it"]
        it.on_signal("SIGINT")
        while not box.cmds(f"UPLOAD {STOP_FILE}"):
            await _REAL_SLEEP(0)
        if n == 1:
            # vcrctl saw the file before its third switch: it stops, restores once
            return _modeseq_out(IN_RANGE[:2], ok=False, modes=len(IN_RANGE), ran=2, stopped=True)
        it.on_signal("SIGTERM")
        await asyncio.Event().wait()                  # the reply never comes
    box = _box(IN_RANGE, execw=execw, procs=procs)

    def third():
        it = holder.get("it")
        if n >= 3 and it and it.waiting and len(it.seen) == 2:
            it.on_signal("SIGINT")
    rc, _ = _sweep(monkeypatch, box, ["192.168.1.124", "--modes", ",".join(IN_RANGE)], hook=third)
    return rc, box


def test_one_signal_ends_the_list_civilly(monkeypatch):
    rc, box = _signalled_sweep(monkeypatch, 1, Procs())
    assert rc == 130
    assert len(box.cmds(f"UPLOAD {STOP_FILE}")) == 1
    # vcrctl stopped and restored by itself: nothing killed, stamped or restored from here
    assert not box.cmds("pace-kill")
    _killed_only_through_pace_kill(box)
    assert not box.cmds("pace-mark") and not box.cmds(" restore")


def test_a_second_signal_stops_waiting_but_kills_nothing(monkeypatch):
    rc, box = _signalled_sweep(monkeypatch, 2, Procs())      # vcrctl left: the stop file
    assert rc == 130
    assert len(box.cmds(f"UPLOAD {STOP_FILE}")) == 1          # not sent twice
    assert not box.cmds("pace-kill")
    _killed_only_through_pace_kill(box)
    tl = box.timeline
    assert _at(tl, "pace-mark") < _at(tl, " restore")
    assert box.cmds(" restore") == [RESTORE_CMD]


def test_a_third_signal_cuts_the_wait_short_and_pace_kills_by_pid(monkeypatch):
    rc, box = _signalled_sweep(monkeypatch, 3, Procs(after={9: VCRCTL_IMAGE}))
    assert rc == 130
    tl = box.timeline
    kill = _at(tl, "pace-kill")
    assert _slept(tl, _at(tl, f"UPLOAD {STOP_FILE}"), kill) < CIVIL_WAIT
    assert box.cmds("pace-kill") == [PACE_KILL_CMD + "9"]
    _killed_only_through_pace_kill(box)
    assert not box.cmds("pace-mark")                          # pace-kill stamped it
    assert _slept(tl, kill, _at(tl, " restore", kill)) >= 5
    assert box.cmds(" restore") == [RESTORE_CMD]


# ---- the host scripts: every budget counts the gate, no switch under plain EXEC --------


def test_every_budget_counts_the_gates_worst_case_per_switch():
    """The agent's EXECW tree-kill reverts a mode unpaced, so it must never
    land on a tool that is only waiting its turn: queued behind another
    tool's switch lock for VCR_PACE_LOCK_MS, then its floor - or a longer
    pace - from a stamp an exit hold or a paced kill wrote EXIT_LAG ahead.
    The numbers are read from vcr_pace.h, never copied."""
    lock, lag = _define(PACE_H, "VCR_PACE_LOCK_MS") / 1000, _define(PACE_H, "VCR_PACE_EXIT_LAG_MS") / 1000
    floor, mx = _define(PACE_H, "VCR_PACE_MIN_MS") / 1000, _define(PACE_H, "VCR_PACE_MAX_MS") / 1000
    assert (mode_sweep.PACE_LOCK_S, mode_sweep.PACE_EXIT_LAG_S, mode_sweep.PACE_FLOOR_S,
            mode_sweep.PACE_MAX_S) == (lock, lag, floor, mx)
    assert "_pace_h(" in SWEEP and 'HERE / "vcr_pace.h"' in SWEEP
    sw = mode_sweep.switch_s
    for p in (0, 3, 5, 30):
        assert sw(p) == lock + lag + max(p, floor)
    # a restore: the switch, and - failed - the exit hold before the process goes
    assert mode_sweep.RESTORE_EXECW_S >= 2 * sw()
    # a paced kill: its switch and the wait for the victim to be gone
    assert mode_sweep.PACE_KILL_EXECW_S >= sw() + _define(PACE_H, "VCR_PACE_KILL_WAIT_MS") / 1000
    # a golden capture: the switch in, and the exit hold out
    assert golden_capture.GOLDEN_EXECW_S >= 2 * sw()
    # modeseq: every mode, the restore and a failed restore's hold, each a switch
    for n, p in ((1, 3), (4, 5), (12, 5)):
        assert mode_sweep.modeseq_budget(n, p) >= (n + 2) * sw(p) + n * mode_sweep.MODETEST_S
    assert mode_sweep.modeseq_budget(12, 5) <= mode_sweep.EXECW_CEILING      # the default cap fits
    # the fallback's civil wait for a stopped modeseq: the two paced switches
    # it may still make (the lock wait in each), a mode under test, a margin
    assert "bound = 2 * switch_s(pace) + MODESEQ_STEP_S + CIVIL_MARGIN_S" in SWEEP
    assert mode_sweep.MODESEQ_STEP_S >= mode_sweep.MODETEST_S and mode_sweep.CIVIL_MARGIN_S > 0
    # a lab: two switches (in, out) on top of its work; none for one that
    # does not switch
    assert mode_sweep.execw_budget(2, 120) == int(-(-(2 * sw() + 120) // 1))
    assert mode_sweep.execw_budget(0, 120) == 120
    # glidelab: every open and every close
    assert glidelab_run.session_budget("cycle", 3, 180) == mode_sweep.execw_budget(6, 180)
    assert glidelab_run.session_budget("fill", 1, 180) == mode_sweep.execw_budget(2, 180)
    assert glidelab_run.session_budget("abandon", 1, 180) == mode_sweep.execw_budget(2, 180)
    # anything past the agent's clamp is refused, not cut off
    assert mode_sweep.budget_refusal(mode_sweep.EXECW_CEILING + 1, "x")
    assert not mode_sweep.budget_refusal(mode_sweep.EXECW_CEILING, "x")


SWITCHING_SUBCOMMANDS = ("setmode", "golden", "modetest", "modeseq", "ddraw", "restore",
                         "pace-kill")


def test_no_host_script_runs_a_switch_under_plain_exec():
    """EXEC's fixed 60 s kills a tool that is only waiting its turn at the
    gate - and the kill reverts its mode unpaced. Every command that switches
    (or kills a switching tool) goes under EXECW with a budget."""
    for p in sorted(TOOLS.glob("*.py")):
        src = p.read_text()
        for m in re.finditer(r"EXEC \{[^}]+\} ([\w-]+)", src):
            assert m.group(1) not in SWITCHING_SUBCOMMANDS, (p.name, m.group(0))
        for m in re.finditer(r"\b(?:run|vcr|vcrctl)\(\s*f?[\"']([\w-]+)", src):
            assert m.group(1) not in SWITCHING_SUBCOMMANDS, (p.name, m.group(0))
    # the two host-side switches are EXECW, with the host outwaiting them
    assert re.search(r'call\(f"EXECW \{RESTORE_EXECW_S\} \{tool\} restore", RESTORE_EXECW_S \+ '
                     r"HOST_SLACK_S\)", SWEEP)
    assert re.search(r'call\(f"EXECW \{PACE_KILL_EXECW_S\} \{tool\} pace-kill \{pid\}",\s*'
                     r"PACE_KILL_EXECW_S \+ HOST_SLACK_S\)", SWEEP)
    # and bounded on the host even if the transport ignores its own timeout
    for fn in ("pace_kill", "restore_once"):
        body = SWEEP[SWEEP.index(f"async def {fn}("):]
        assert "asyncio.wait_for(" in body[:body.index("\n\n\n")], fn


def test_reap_kills_only_through_pace_kill_and_only_what_it_launched(monkeypatch):
    """mode_sweep.reap, the one cleanup every runner uses: a survivor that was
    NOT running before the launch is pace-killed by PID; one that was is
    another session's and is left; with no pre-launch list nothing is
    killed and the report says whose is unknown. A pace-kill that stamped
    every target needs no pace-mark; anything less gets one. Then the pace."""
    def reap(procs, spare, images=("ddlab.exe",)):
        procs.launched = True
        box = FakeAgent([(f"EXEC {VCRCTL_BOX} pace-mark", PACE_MARK_OK)] + procs.answers())
        _record_sleeps(monkeypatch, box)

        async def call(cmd, t):
            return await box("h").send_command(cmd, timeout=t)
        return asyncio.run(mode_sweep.reap(call, list(images), VCRCTL_BOX, 5.0, spare=spare)), box

    rep, box = reap(Procs(after={3: "DDLAB.EXE", 7: "ddlab.exe"}), spare=[3])
    assert rep["killed"] == [7] and rep["spared"] == [3] and rep["gone"]
    assert box.cmds("pace-kill") == [PACE_KILL_CMD + "7"]
    _killed_only_through_pace_kill(box)
    assert rep["pace_mark"] and rep["stamped_by"] == "pace-kill" and not box.cmds("pace-mark")
    assert box.timeline[-1] == ("sleep", 5.0)
    # no list from before the launch: nothing killed, said, stamped
    rep, box = reap(Procs(after={7: "ddlab.exe"}), spare=None)
    assert not box.cmds("pace-kill") and not rep["gone"] and rep["left"] == [7]
    assert "unknown" in rep["whose"] and box.cmds("pace-mark")
    # a pace-kill refused (lock busy) or not gone: not gone, stamped by hand,
    # and no other kill
    for kw in ({"busy": {7}}, {"unkillable": {7}}):
        rep, box = reap(Procs(after={7: "ddlab.exe"}, **kw), spare=[])
        assert not rep["gone"] and rep["kill_errors"] and box.cmds("pace-mark")
        _killed_only_through_pace_kill(box)
    # nothing left: only the stamp and the pace (it may have died unstamped)
    rep, box = reap(Procs(after={}), spare=[])
    assert rep["gone"] and box.cmds("pace-mark") and not box.cmds("pace-kill")
    assert box.timeline[-1][0] == "sleep" and box.timeline[-1][1] >= mode_sweep.PACE_FLOOR_S
    # a pace-kill that answered for another pid is not a kill of this one
    procs = Procs(after={7: "ddlab.exe"})
    procs.pace_kill = lambda cmd: '{"cmd":"pace-kill","ok":true,"pid":8,"exited":true}\n'
    rep, box = reap(procs, spare=[])
    assert rep["killed"] == [] and not rep["gone"]


# ---- the host scripts: silicon_battery -------------------------------------------


def test_battery_visits_a_short_mode_list_and_rests():
    tree = ast.parse(BATTERY)
    vals = {t.id: n.value for n in tree.body if isinstance(n, ast.Assign)
            for t in n.targets if isinstance(t, ast.Name)}
    live = ast.literal_eval(vals["LIVE_MODES"])
    assert 1 <= len(live) <= 12
    assert ast.literal_eval(vals["PACE"]) >= 5
    a = argparse.Namespace(golden=None, label="t", vendor_cursor="v")
    subs = silicon_battery.schedule(a, "h", ["--port", "9898"])
    modes = [s for s in subs if s["step"] == "modes"]
    assert len(modes) == 1 and "--allow-many" not in modes[0]["args"]
    args = modes[0]["args"]
    assert args[args.index("--modes") + 1] == ",".join(live)
    # the count the operator is shown is the count the monitor gets: small
    assert modes[0]["switches"] == len(live) + 1
    assert sum(s["switches"] for s in subs) <= 24
    assert all(s["switches"] == 2 for s in subs if s["fullscreen"])


def test_the_battery_never_cuts_a_runner_off_on_its_own_way_out():
    """Each sub-run's timeout covers the runner's EXECW (the gate's worst case
    per switch included), the host's slack on it, and a whole cleanup after
    it - a battery SIGTERM in the middle of a runner's cleanup is exactly
    when a lab may still hold a mode. The runners get the work budget the
    battery counted with."""
    ms = mode_sweep
    cleanup = silicon_battery.CLEANUP_S
    assert cleanup >= ms.PACE_KILL_EXECW_S + ms.HOST_SLACK_S + silicon_battery.PACE
    subs = silicon_battery.schedule(argparse.Namespace(golden=None, label="t", vendor_cursor="v"),
                                    "h", ["--port", "9898"])
    for s in subs:
        runner = Path(s["args"][0]).stem
        if runner == "mode_sweep":
            need = (ms.modeseq_budget(len(silicon_battery.LIVE_MODES), silicon_battery.PACE)
                    + 2 * ms.switch_s(silicon_battery.PACE) + ms.RESTORE_EXECW_S)
        elif runner in silicon_battery.LAB_WORK_S:
            work = silicon_battery.LAB_WORK_S[runner]
            assert s["args"][s["args"].index("--timeout") + 1] == str(work), s["args"]
            need = ms.execw_budget(s["switches"], work)
        else:
            continue
        assert s["timeout"] >= need + ms.HOST_SLACK_S + cleanup, (runner, s["timeout"], need)


def test_the_battery_cannot_skip_its_monitor_gate():
    assert re.search(r'if "info" in skip:\s*\n\s*log\([^)]*\)\s*\n\s*skip\.discard\("info"\)', BATTERY)


def _battery_env(monkeypatch, tmp_path, exe=b"MZ-this-build", back=None, info=SONY):
    (tmp_path / "out").mkdir(parents=True)
    (tmp_path / "out" / "vcrctl.exe").write_bytes(exe)
    monkeypatch.setattr(silicon_battery, "KMD", tmp_path)
    sent = []

    async def agent_raw(host, port, cmd, timeout=90, payload=None):
        sent.append(cmd)
        if cmd.startswith("DOWNLOAD"):
            return 0, exe if back is None else back
        if cmd.startswith("EXEC") and cmd.endswith(" info"):
            return 0, json.dumps(info).encode()
        return 0, b"OK"
    monkeypatch.setattr(silicon_battery, "agent_raw", agent_raw)
    return sent, tmp_path / "info.jsonl"


def test_the_battery_stops_before_any_switch_unless_the_gate_passes(monkeypatch, tmp_path):
    sent, jl = _battery_env(monkeypatch, tmp_path)
    assert silicon_battery.info_step("h", 9898, jl) == 0
    # an older vcrctl left on the box (the UPLOAD over a running one failed)
    sent, jl = _battery_env(monkeypatch, tmp_path / "b", back=b"MZ-older-build")
    assert silicon_battery.info_step("h", 9898, jl) == 3
    assert not any(c.endswith(" info") for c in sent)
    bad = (dict(SONY, edid_ok=0), dict(SONY, mon_filter=0), {"ok": False},
           dict(SONY, mon_src=3), dict(SONY, mon_src=4),
           {k: v for k, v in SONY.items() if k != "mon_src"})
    for k, info in enumerate(bad):
        sent, jl = _battery_env(monkeypatch, tmp_path / f"gate{k}", info=info)
        assert silicon_battery.info_step("h", 9898, jl) == 3, info
        assert json.loads(jl.read_text().splitlines()[-1])["rc"] == 3


def _run_name(args):
    """A battery sub-run as `step mode bpp/res`, from its argv."""
    stem = Path(args[0]).stem
    if stem == "ddlab_run":
        return f"ddraw {args[4]} {args[args.index('--bpp') + 1]}"
    if stem in ("d3dprobe_run",):
        return f"d3d {args[4]}" + (f" {args[args.index('--res') + 1]}" if "--res" in args else "")
    return stem


def _battery(monkeypatch, tmp_path, skip, rc_of, res_of=None):
    """silicon_battery.main() with every sub-run faked: -> (rc, the runs made,
    the rests taken, when a cleanup ran (as (the number of runs made by then,
    the pre-launch list it was handed)), the evidence rows)."""
    monkeypatch.setattr(silicon_battery, "KMD", tmp_path)
    ran, rests, cleaned = [], [], []

    async def alive(host, port):
        return True

    def fake_run(args, timeout):
        ran.append(args)
        name = _run_name(args)
        return rc_of(name), (res_of(name) if res_of else ["r"]), 0.0

    def snapshot(h, p):
        return [1000 + len(ran)]                  # a distinct list before each sub-run
    monkeypatch.setattr(silicon_battery, "ping", alive)
    monkeypatch.setattr(silicon_battery, "info_step", lambda h, p, jl: 0)
    monkeypatch.setattr(silicon_battery, "run", fake_run)
    monkeypatch.setattr(silicon_battery, "snapshot", snapshot)
    monkeypatch.setattr(silicon_battery, "clean_up",
                        lambda h, p, spare: cleaned.append((len(ran), spare))
                        or {"gone": True, "pace_mark": True})
    monkeypatch.setattr(silicon_battery.time, "sleep", rests.append)
    monkeypatch.setattr(sys, "argv", ["silicon_battery.py", "h", "--label", "t", "--skip", skip])
    rc = silicon_battery.main()
    rows = [json.loads(ln) for ln in (tmp_path / "evidence" / "silicon" / "t.jsonl")
            .read_text().splitlines()]
    return rc, [_run_name(a) for a in ran], rests, cleaned, rows


def test_a_failed_fullscreen_run_ends_its_steps_fullscreen_runs(monkeypatch, tmp_path):
    """A driver that just failed an exclusive-mode set is not driven through
    four more of them (each two re-syncs)."""
    rc, ran, rests, _, rows = _battery(monkeypatch, tmp_path, "info,modes,cursor,gdi,d3d,d3dperf",
                                       lambda name: 1 if name == "ddraw flip 16" else 0)
    assert rc == 1
    assert ran == ["ddraw caps 16", "ddraw flip 16", "ddraw caps 32"]
    assert rests == [silicon_battery.PACE]                    # a rest before the one fullscreen run
    assert sum("skipped" in r for r in rows) == 3


def test_a_fullscreen_failure_ends_every_later_switching_run_of_the_battery(monkeypatch, tmp_path):
    """Round 1 latched per STEP: a ddraw failure still let d3d and d3dperf
    drive the same driver through four more exclusive-mode sets."""
    rc, ran, rests, cleaned, rows = _battery(monkeypatch, tmp_path, "modes,cursor,gdi",
                                             lambda name: 1 if name == "ddraw flip 16" else 0)
    assert rc == 1
    assert ran == ["ddraw caps 16", "ddraw flip 16", "ddraw caps 32", "d3d caps"]
    skipped = [r for r in rows if "skipped" in r]
    assert len(skipped) == 6 and {r["step"] for r in skipped} == {"ddraw", "d3d", "d3dperf"}
    assert not cleaned                                         # a failure is not a timeout


@pytest.mark.parametrize("res", ({"mode": "caps", "focus_lost": True},
                                 {"mode": "caps", "error": "CreateDevice not made: pace lock busy"}),
                         ids=("focus-lost", "pace-lock-busy"))
def test_a_lab_that_lost_the_screen_or_the_lock_ends_every_later_switch(monkeypatch, tmp_path, res):
    """In ANY step - its runner's exit code notwithstanding: the screen or the
    pacing was not the battery's alone, and whatever took it may switch too."""
    rc, ran, rests, cleaned, rows = _battery(
        monkeypatch, tmp_path, "modes,cursor,gdi,ddraw", lambda name: 0,
        res_of=lambda name: [res] if name == "d3d caps" else ["r"])
    assert rc == 1 and ran == ["d3d caps"]
    row = next(r for r in rows if r.get("step") == "d3d" and "halt" in r)
    assert row["halt"] and sum("skipped" in r for r in rows) == 3
    assert not cleaned and rests == []


def test_a_timed_out_run_is_cleaned_up_and_ends_every_later_switch(monkeypatch, tmp_path):
    rc, ran, rests, cleaned, rows = _battery(monkeypatch, tmp_path, "",
                                             lambda name: -9 if name == "mode_sweep" else 0)
    # the runs that switch nothing still run; nothing after it switches
    assert ran == ["mode_sweep", "cursor_golden", "lab_run", "ddraw caps 16", "ddraw caps 32",
                   "d3d caps"]
    # at once, before anything else ran - handed the list taken before THAT run
    assert cleaned == [(1, [1000])]
    assert next(r for r in rows if r.get("step") == "modes")["cleanup"]
    assert sum("skipped" in r for r in rows) == 7
    assert rests == []                                         # no fullscreen run was left to rest for


def test_the_battery_plan_counts_switches_and_the_timing_changes_they_cost(monkeypatch, tmp_path,
                                                                          capsys):
    rc, ran, rests, _, _ = _battery(monkeypatch, tmp_path, "", lambda name: 0)
    assert rc == 0
    m = re.search(r"at most (\d+) mode switches = (\d+) timing changes", capsys.readouterr().out)
    subs = silicon_battery.schedule(argparse.Namespace(golden=None, label="t",
                                                       vendor_cursor="amigamerlin-3.1-r11"),
                                    "h", ["--port", "9898"])
    switches = sum(s["switches"] for s in subs)
    assert m and int(m.group(1)) == switches
    assert int(m.group(2)) == mode_sweep.TIMING_CHANGES_PER_SWITCH * switches
    # the plan's schedule is the schedule that ran
    assert len(ran) == len(subs)
    assert rests == [silicon_battery.PACE] * sum(s["fullscreen"] for s in subs)


def test_the_battery_knows_a_timed_out_run_by_any_of_its_signs():
    to = silicon_battery.timed_out
    assert to(-9, ["r"])                                        # the battery's own timeout
    assert to(1, [{"mode": "flip", "execw": TIMED_OUT.strip()}])  # a lab runner kept the marker
    assert to(1, ["...", TIMED_OUT.strip()])                     # ... or a tool printed it
    assert to(1, [{"mode": "fill", "timed_out": True}])          # a runner's EXECW never answered
    assert not to(1, [{"mode": "flip", "error": "bad_fill 3"}])  # a failure is not a timeout
    assert not to(0, [{"ok": 1}])
    # the marker survives into the result from a line that is not JSON
    rc, res, _ = silicon_battery.run(["-c", "print('{\"step\": 1}'); print("
                                      + repr(TIMED_OUT.strip()) + ")"], 60)
    assert rc == 0 and to(rc, res)


def test_the_battery_reaps_orphans_through_pace_kill_sparing_its_snapshot(monkeypatch):
    """It launched the labs through a runner, so a survivor is its own only if
    it was not running before that sub-run: those are pace-killed by PID,
    and it says so; what predates it is left. With no snapshot nothing is
    killed. Then the stamp and the pace."""
    procs = Procs(after={5: "ddlab.exe", 7: "DDLAB.EXE", 8: "d3dprobe.exe", 9: "vcrctl.exe"})
    procs.launched = True
    box = FakeAgent([(f"EXEC {VCRCTL_BOX} pace-mark", PACE_MARK_OK)] + procs.answers())

    async def agent_raw(host, port, cmd, timeout=90, payload=None):
        return await box(host, port).send_command(cmd, timeout=timeout)
    monkeypatch.setattr(silicon_battery, "agent_raw", agent_raw)
    _record_sleeps(monkeypatch, box)
    rep = silicon_battery.clean_up("h", 9898, [5])
    assert rep["gone"] and rep["pace_mark"] and rep["spared"] == [5]
    assert sorted(rep["killed"]) == [7, 8, 9]
    assert sorted(procs.pace_killed) == [7, 8, 9]
    _killed_only_through_pace_kill(box)
    assert not [c for c in box.cmds() if c.endswith((" 1", " 2", " 5"))]   # not the shell, agent, spared
    assert not box.cmds("pace-mark")                            # every target stamped by its kill
    tl = box.timeline
    assert tl[-1][0] == "sleep" and tl[-1][1] >= silicon_battery.PACE
    # the snapshot of the sub-run is what is handed over; no snapshot: no kill
    procs = Procs(after={7: "ddlab.exe"})
    procs.launched = True
    box = FakeAgent([(f"EXEC {VCRCTL_BOX} pace-mark", PACE_MARK_OK)] + procs.answers())
    _record_sleeps(monkeypatch, box)
    rep = silicon_battery.clean_up("h", 9898, None)
    assert not rep["gone"] and not rep["killed"] and not box.cmds("pace-kill")
    assert box.cmds("pace-mark")
    # and the snapshot really is a list from BEFORE the sub-run
    assert re.search(r"before = snapshot\(h, a\.port\)\s*\n\s*rc, res, dt = run\(", BATTERY)
    assert "clean_up(h, a.port, before)" in BATTERY


def test_a_timed_out_step_is_asked_to_stop_before_it_is_killed(tmp_path):
    """SIGTERM first: mode_sweep traps it and stops its list on the box with
    one paced restore; a SIGKILL would leave vcrctl switching."""
    mark = tmp_path / "got-sigterm"
    child = ("import signal, sys, time\n"
             f"def term(*a):\n    open({str(mark)!r}, 'w').write('x'); sys.exit(0)\n"
             "signal.signal(signal.SIGTERM, term)\n"
             "print('armed', flush=True)\n"
             "time.sleep(60)\n")
    rc, res, dt = silicon_battery.run(["-c", child], 1.5)
    assert mark.exists(), "the step was not sent SIGTERM"
    assert rc == -9 and dt < 30
    assert any("armed" in str(r) for r in res)                # what it printed is kept


# ---- the host scripts: the lab runners -------------------------------------------

LAB_RUNNERS = {
    "ddlab_run": (ddlab_run, "ddlab.exe", lambda: argparse.Namespace(
        host="192.168.1.124", port=9898, mode="flip", res="800x600", bpp=16, frames=60,
        timeout=120, tool=VCRCTL_BOX, verbose=False, monitor_info=None,
        i_have_checked_the_monitor=False)),
    "d3dprobe_run": (d3dprobe_run, "d3dprobe.exe", lambda: argparse.Namespace(
        host="192.168.1.124", port=9898, mode="render", res="640x480", bpp=16, frames=200,
        full=True, novsync=False, tests="", timeout=180, tool=VCRCTL_BOX, verbose=False,
        monitor_info=None, i_have_checked_the_monitor=False)),
    "lab_run": (lab_run, "ddlab.exe", lambda: argparse.Namespace(
        lab="ddlab", host="192.168.1.124", port=9898, timeout=240, tool=VCRCTL_BOX,
        verbose=False, monitor_info=None, i_have_checked_the_monitor=False)),
}
# what lab_run's lab is told (a fullscreen ddlab blt, gated like its own runner)
LAB_RUN_ARGS = ["blt", "--res", "800x600", "--bpp", "16"]
LAB_OK = 'RESULT {"ok": 1}\n'
# what `vcrctl modes` lists for the labs' sizes: each gated at its highest
LAB_LISTED = ["640x480x16@60", "640x480x16@85", "800x600x16@60", "800x600x16@85",
              "1024x768x16@85", "1024x768x32@100"]


def _lab(monkeypatch, tmp_path, capsys, name, execw, log, procs=None, info=SONY, listed=LAB_LISTED,
         ns=None, rest=None):
    mod, image, mk = LAB_RUNNERS[name]
    (tmp_path / "out").mkdir(parents=True, exist_ok=True)
    for img in ("ddlab.exe", "d3dprobe.exe", "gdilab.exe", "glidelab.exe"):
        (tmp_path / "out" / img).write_bytes(b"MZ")
    monkeypatch.setattr(mod, "KMD", tmp_path)
    procs = procs or Procs()
    box = FakeAgent([(f"EXEC {VCRCTL_BOX} info", json.dumps(info)),
                     (f"EXEC {VCRCTL_BOX} modes", json.dumps({"cmd": "modes", "ok": True,
                                                              "modes": listed,
                                                              "current": DESKTOP})),
                     (f"EXEC {VCRCTL_BOX} pace-mark", PACE_MARK_OK)] + procs.answers()
                    + [("EXECW", procs.launch(execw)), ("DOWNLOAD", log)])
    monkeypatch.setattr(mod, "RetroConnection", box)
    monkeypatch.setattr(mode_sweep, "modecalc", lambda: CALC)
    _record_sleeps(monkeypatch, box)
    a = ns or mk()
    rc = asyncio.run(mod.main_async(a, list(LAB_RUN_ARGS if rest is None else rest))
                     if mod is lab_run else mod.main_async(a))
    return rc, box, json.loads(capsys.readouterr().out.strip().splitlines()[-1])


def _dropped(cmd):
    raise ConnectionResetError("the agent dropped the connection")


@pytest.mark.parametrize("execw", (TIMED_OUT, _dropped), ids=("execw-timed-out", "no-reply"))
@pytest.mark.parametrize("name", sorted(LAB_RUNNERS))
def test_a_lab_that_did_not_end_civilly_is_pace_killed_by_pid(monkeypatch, tmp_path, capsys, name,
                                                              execw):
    image = LAB_RUNNERS[name][1].upper()
    procs = Procs(before={3: image}, after={3: image, 7: image})
    rc, box, res = _lab(monkeypatch, tmp_path, capsys, name, execw, LAB_OK, procs)
    assert rc == 1 and res["timed_out"] and "error" in res
    if execw is TIMED_OUT:
        # the agent's own words, kept: the battery keys on them
        assert res["execw"].startswith(mode_sweep.EXECW_TIMED_OUT)
    assert silicon_battery.timed_out(rc, [res])
    tl = box.timeline
    kill = _at(tl, "pace-kill 7")
    assert _at(tl, "PROCLIST") < kill
    assert box.cmds("pace-kill") == [PACE_KILL_CMD + "7"]
    _killed_only_through_pace_kill(box)
    assert res["cleanup"]["killed"] == [7] and res["cleanup"]["stamped_by"] == "pace-kill"
    assert _slept(tl, kill, len(tl)) >= mode_sweep.PACE_FLOOR_S
    # the one running before this launch is not this run's
    assert res["cleanup"]["spared"] == [3] and 3 not in procs.pace_killed
    (cmd, t), = _launches(box)
    assert t > int(cmd.split()[1])                             # the host hears the agent's marker


def test_a_timed_out_gdilab_does_not_stay_on_the_box(monkeypatch, tmp_path, capsys):
    procs = Procs(after={7: "GDILAB.EXE"})
    ns = argparse.Namespace(lab="gdilab", host="192.168.1.124", port=9898, timeout=240,
                            tool=VCRCTL_BOX, verbose=False)
    rc, box, res = _lab(monkeypatch, tmp_path, capsys, "lab_run", TIMED_OUT, LAB_OK, procs, ns=ns,
                        rest=[])
    assert rc == 1
    assert not box.cmds("taskkill") or all("/im" not in c for c in box.cmds("taskkill"))
    assert res["cleanup"]["gone"] and 7 not in procs.running


def test_a_lab_pace_kill_will_not_end_is_left_and_said(monkeypatch, tmp_path, capsys):
    """Whatever becomes of the gdilab gap above: a survivor the gate will not
    kill is never killed by another road, and the report says it is there."""
    procs = Procs(after={7: "GDILAB.EXE"})
    ns = argparse.Namespace(lab="gdilab", host="192.168.1.124", port=9898, timeout=240,
                            tool=VCRCTL_BOX, verbose=False)
    rc, box, res = _lab(monkeypatch, tmp_path, capsys, "lab_run", TIMED_OUT, LAB_OK, procs, ns=ns,
                        rest=[])
    _killed_only_through_pace_kill(box)
    assert rc == 1 and (res["cleanup"]["gone"] or res["cleanup"].get("kill_errors"))


@pytest.mark.parametrize("name", sorted(LAB_RUNNERS))
def test_a_lab_that_ended_civilly_is_left_alone(monkeypatch, tmp_path, capsys, name):
    rc, box, res = _lab(monkeypatch, tmp_path, capsys, name, "", LAB_OK)
    assert rc == 0 and "cleanup" not in res
    assert not box.cmds("pace-kill") and not box.cmds("pace-mark")
    _killed_only_through_pace_kill(box)


@pytest.mark.parametrize("log", ("", 'RESULT {"ok": 1,\n'), ids=("crashed", "garbled"))
@pytest.mark.parametrize("name", sorted(LAB_RUNNERS))
def test_a_lab_without_a_usable_result_fails_and_is_cleaned_up(monkeypatch, tmp_path, capsys,
                                                                name, log):
    """A crash skips the lab's exit hold - XP reverted its mode unstamped -
    and a garbled RESULT is not a pass."""
    rc, box, res = _lab(monkeypatch, tmp_path, capsys, name, "", log)
    assert rc == 1 and "error" in res and box.cmds("pace-mark")


@pytest.mark.parametrize("log,why", (('RESULT {"mode":"flip","focus_lost":true,"error":"x"}\n',
                                      "focus"),
                                     ('RESULT {"mode":"flip","error":"SetDisplayMode not made: '
                                      'pace lock busy"}\n', "pace lock")),
                         ids=("focus-lost", "pace-lock-busy"))
@pytest.mark.parametrize("name", sorted(LAB_RUNNERS))
def test_a_lab_that_lost_the_screen_or_the_lock_fails(monkeypatch, tmp_path, capsys, name, log, why):
    rc, box, res = _lab(monkeypatch, tmp_path, capsys, name, "", log)
    assert rc == 1 and why in res["halt"] and silicon_battery.halt_reason([res])
    _killed_only_through_pace_kill(box)


def test_a_fullscreen_lab_is_gated_at_the_highest_refresh_before_it_is_uploaded(monkeypatch,
                                                                                tmp_path, capsys):
    """ddlab flip/blt and d3dprobe --full switch at refresh 0: the driver / XP
    picks the rate, so the mode checked is the HIGHEST the driver lists at
    that size - the worst the pick can land on. Refused: nothing uploaded,
    nothing started, rc 2 and a RESULT that says why."""
    # the Sony takes 800x600x16@85: runs, with the two switches in its budget
    rc, box, res = _lab(monkeypatch, tmp_path, capsys, "ddlab_run", "", LAB_OK)
    assert rc == 0
    (cmd, _), = _launches(box)
    assert int(cmd.split()[1]) == mode_sweep.execw_budget(2, 120)
    assert box.cmds(" modes") and box.cmds(" info")
    # a tube whose V max is 80 refuses the 85 Hz the driver could pick
    for name in ("ddlab_run", "lab_run"):
        rc, box, res = _lab(monkeypatch, tmp_path / f"v80{name}", capsys, name, "", LAB_OK,
                            info=dict(SONY, mon_v_hz=[48, 80]))
        assert rc == 2 and res["refused"] and "800x600x16@85" in res["error"], name
        assert not _launches(box) and not box.cmds("UPLOAD"), name
    # d3dprobe --full at 640x480: the driver also lists 120 Hz there - 120.015
    # Hz is inside the Sony's 120 by the driver's own half unit, so it runs
    rc, box, res = _lab(monkeypatch, tmp_path / "d120", capsys, "d3dprobe_run", "", LAB_OK,
                        listed=LAB_LISTED + ["640x480x16@120"])
    assert rc == 0 and _launches(box)
    # nothing listed at that size, or no list at all: refused
    rc, box, res = _lab(monkeypatch, tmp_path / "none", capsys, "ddlab_run", "", LAB_OK,
                        listed=["640x480x16@60"])
    assert rc == 2 and "lists no 800x600x16" in res["error"]
    # under another driver: --monitor-info, or refused with the hint
    rc, box, res = _lab(monkeypatch, tmp_path / "vendor", capsys, "ddlab_run", "", LAB_OK,
                        info={"cmd": "info", "ok": False})
    assert rc == 2 and "--monitor-info" in res["error"] and not box.cmds("UPLOAD")
    # the envelope is not this monitor's range: refused like no EDID
    rc, box, res = _lab(monkeypatch, tmp_path / "env", capsys, "d3dprobe_run", "", LAB_OK,
                        info=dict(SONY, mon_src=3))
    assert rc == 2 and "mon_src" in res["error"] and not box.cmds("UPLOAD")


def test_a_lab_that_switches_nothing_is_not_gated(monkeypatch, tmp_path, capsys):
    ns = argparse.Namespace(host="192.168.1.124", port=9898, mode="caps", res="800x600", bpp=16,
                            frames=60, timeout=120, tool=VCRCTL_BOX, verbose=False)
    rc, box, res = _lab(monkeypatch, tmp_path, capsys, "ddlab_run", "", LAB_OK,
                        info={"ok": False}, ns=ns)
    assert rc == 0 and not box.cmds(" info") and not box.cmds(" modes")
    (cmd, _), = _launches(box)
    assert int(cmd.split()[1]) == 120                           # no switch, no gate time
    ns = argparse.Namespace(host="h", port=9898, mode="render", res="800x600", bpp=16, frames=10,
                            full=False, novsync=False, tests="", timeout=180, tool=VCRCTL_BOX,
                            verbose=False)
    rc, box, res = _lab(monkeypatch, tmp_path / "w", capsys, "d3dprobe_run", "", LAB_OK,
                        info={"ok": False}, ns=ns)
    assert rc == 0 and not box.cmds(" modes")                   # windowed: no switch


def test_lab_run_knows_which_labs_switch(monkeypatch, tmp_path, capsys):
    base = dict(host="h", port=9898, timeout=240, tool=VCRCTL_BOX, verbose=False)
    # gdilab switches nothing: not gated
    rc, box, res = _lab(monkeypatch, tmp_path, capsys, "lab_run", "", LAB_OK, info={"ok": False},
                        ns=argparse.Namespace(lab="gdilab", **base), rest=[])
    assert rc == 0 and not box.cmds(" info")
    # a lab nobody told it about is not run blind
    rc, box, res = _lab(monkeypatch, tmp_path / "m", capsys, "lab_run", "", LAB_OK,
                        ns=argparse.Namespace(lab="mystery", **base), rest=[])
    assert rc == 2 and res["refused"] and not box.sent
    # glidelab: --res at --refresh, gated as named; a refresh glidelab has no
    # code for is refused before anything; one it opened otherwise fails
    rc, box, res = _lab(monkeypatch, tmp_path / "g", capsys, "lab_run", "",
                        'RESULT {"mode":"fill","opened_hz":60}\n',
                        ns=argparse.Namespace(lab="glidelab", **base),
                        rest=["fill", "--res", "1024x768", "--refresh", "85"])
    assert rc == 1 and "opened 60 Hz" in res["halt"]
    (cmd, _), = _launches(box)
    assert int(cmd.split()[1]) == mode_sweep.execw_budget(2, 240)
    rc, box, res = _lab(monkeypatch, tmp_path / "g77", capsys, "lab_run", "", LAB_OK,
                        ns=argparse.Namespace(lab="glidelab", **base),
                        rest=["fill", "--res", "1024x768", "--refresh", "77"])
    assert rc == 2 and not box.sent
    rc, box, res = _lab(monkeypatch, tmp_path / "g16", capsys, "lab_run", "", LAB_OK,
                        ns=argparse.Namespace(lab="glidelab", **base),
                        rest=["fill", "--res", "1600x1200", "--refresh", "85"])
    assert rc == 2 and "1600x1200x16@85" in res["error"] and not box.cmds("UPLOAD")


# ---- the host scripts: golden_capture -----------------------------------------------


def _golden(monkeypatch, tmp_path, modes, info, extra=(), golden=None, procs=None,
            restore=RESTORE_OK, left_in="1024x768x16@85", registry=()):
    exe = tmp_path / "vcrctl.exe"
    exe.write_bytes(b"MZ" * 50)
    box = _box(modes, info, procs=procs, restore=restore, left_in=left_in)
    box.answers[:0] = list(registry)
    box.answers.insert(0, ("DIRLIST", json.dumps({"entries": [{"name": "vcrctl.exe",
                                                               "size": 100}]})))
    box.answers.insert(0, (f"EXECW {golden_capture.GOLDEN_EXECW_S} {VCRCTL_BOX} golden",
                           box.procs.launch('{"cmd":"golden","ok":true}' if golden is None
                                            else golden)))
    a, code = _parsed(golden_capture, ["192.168.1.124", "--label", "t", "--exe", str(exe),
                                       "--out", str(tmp_path / "g.json")] + list(extra))
    assert code == 0
    monkeypatch.setattr(mode_sweep, "modecalc", lambda: CALC)
    monkeypatch.setattr(golden_capture, "RetroConnection", box)
    _record_sleeps(monkeypatch, box)
    return asyncio.run(golden_capture.main_async(a)), box, box.timeline


def test_golden_capture_refuses_the_old_all_mode_sweep(monkeypatch, tmp_path):
    many = [f"{w}x{h}x16@{hz}" for w, h in ((640, 480), (800, 600), (1024, 768))
            for hz in (60, 70, 72, 75, 85)]
    rc, box, _ = _golden(monkeypatch, tmp_path, many, SONY)
    assert rc == 2 and not box.cmds(" golden ") and not box.cmds(" info")


@pytest.mark.parametrize("info,extra", (
    (dict(SONY, edid_ok=0), ["--modes", "1024x768x16@85"]),
    (dict(SONY, mon_src=3), ["--modes", "1024x768x16@85"]),
    (SONY, ["--modes", "1024x768x16@85,1600x1200x16@85"]),
    ({"cmd": "info", "ok": False, "error": "VCR_ESC_INFO refused"}, ["--modes", "1024x768x16@85"]),
))
def test_golden_capture_gates_every_mode_before_loading_or_switching(monkeypatch, tmp_path,
                                                                     info, extra):
    rc, box, _ = _golden(monkeypatch, tmp_path, ["1024x768x16@85", "1600x1200x16@85"], info, extra)
    assert rc == 2
    assert not box.cmds(" golden ") and not box.cmds("sc ") and not box.cmds(" restore")


def test_golden_capture_under_the_vendor_driver_needs_the_monitor_on_the_box(monkeypatch, tmp_path):
    """A capture is taken on the vendor driver, which cannot be asked for the
    EDID: --monitor-info, used only once the registry says its monitor is
    the one there. Another model is refused before anything switches."""
    saved = tmp_path / "ours.json"
    saved.write_text(json.dumps(SONY))
    not_ours = {"cmd": "info", "ok": False}
    for k, (mons, want) in enumerate(((([("GSM5678", "1", True, OTHER_EDID)]), 2),
                                      ([SONY_PRESENT], 0))):
        (tmp_path / str(k)).mkdir()
        rc, box, _ = _golden(monkeypatch, tmp_path / str(k), ["1024x768x16@85"], not_ours,
                             ["--modes", "1024x768x16@85", "--monitor-info", str(saved)],
                             left_in=DESKTOP, registry=_registry(mons))
        assert rc == want and bool(box.cmds(" golden ")) == (want == 0)


def test_golden_capture_paces_captures_and_the_restore(monkeypatch, tmp_path):
    modes = ["640x480x16@85", "1024x768x16@85"]
    rc, box, order = _golden(monkeypatch, tmp_path, modes, SONY,
                             ["--modes", ",".join(modes), "--pace", "1"])
    assert rc == 0
    golden = [i for i, (k, v) in enumerate(order) if k == "cmd" and " golden " in v]
    restore = [i for i, (k, v) in enumerate(order) if k == "cmd" and v.endswith(" restore")]
    assert len(golden) == 2 and len(restore) == 1
    for lo, hi in ((golden[0], golden[1]), (golden[1], restore[0])):
        waits = [v for k, v in order[lo:hi] if k == "sleep"]
        assert waits and min(waits) >= 3                  # --pace 1 is raised to the floor
    assert json.loads((tmp_path / "g.json").read_text())["monitor_gate"]["pace_s"] >= 3
    # each capture under EXECW with its own budget - the gate's worst case
    # for its switch in and its exit hold - and the host outwaits it: a hang
    # comes back as the agent's answer, not as a host exception
    assert golden_capture.GOLDEN_EXECW_S >= 2 * mode_sweep.switch_s()
    for c, t in box.sent:
        if " golden " in c:
            assert c.startswith(f"EXECW {golden_capture.GOLDEN_EXECW_S} ")
            assert t > golden_capture.GOLDEN_EXECW_S
        if c.endswith(" restore"):
            assert c == RESTORE_CMD and t > mode_sweep.RESTORE_EXECW_S
    assert not box.cmds("pace-kill") and not box.cmds("pace-mark")   # a clean run: nothing reaped
    _killed_only_through_pace_kill(box)


def test_golden_capture_skips_the_restore_when_the_desktop_is_back(monkeypatch, tmp_path):
    rc, box, _ = _golden(monkeypatch, tmp_path, ["640x480x16@85"], SONY,
                         ["--modes", "640x480x16@85"], left_in=DESKTOP)
    assert rc == 0 and not box.cmds(" restore")


def test_golden_capture_stops_at_a_timed_out_capture_and_cleans_up(monkeypatch, tmp_path):
    modes = ["640x480x16@85", "1024x768x16@85"]
    rc, box, tl = _golden(monkeypatch, tmp_path, modes, SONY, ["--modes", ",".join(modes)],
                          golden=TIMED_OUT, procs=Procs(after={9: VCRCTL_IMAGE}))
    assert rc == 1
    assert len(box.cmds(" golden ")) == 1                 # no further capture
    g = _at(tl, " golden ")
    # its vcrctl may still be finishing its hold: a bounded wait first ...
    kill = _at(tl, "pace-kill 9", _at(tl, "PROCLIST", g))
    assert _slept(tl, g, kill) >= golden_capture.GOLDEN_EXECW_S
    # ... then a pace-kill by PID, the pace, ONE restore
    rs = _at(tl, " restore", kill)
    assert _slept(tl, kill, rs) >= 5
    assert box.cmds(" restore") == [RESTORE_CMD] and box.cmds("pace-kill") == [PACE_KILL_CMD + "9"]
    _killed_only_through_pace_kill(box)
    out = json.loads((tmp_path / "g.json").read_text())
    assert "timed out" in out["error"] and out["cleanup"]["killed"] == [9]
    assert out["cleanup"]["pace_mark"] and out["restore"]["ok"]


@pytest.mark.parametrize("exc,code", ((ConnectionResetError("dropped"), 1),
                                      (KeyboardInterrupt(), 130)), ids=("dropped", "signal"))
def test_golden_capture_cleans_up_after_any_exception_then_unloads_the_probe(monkeypatch, tmp_path,
                                                                             exc, code):
    modes = ["640x480x16@85", "1024x768x16@85"]
    n = {"k": 0}

    def golden(cmd):
        n["k"] += 1
        if n["k"] == 2:
            raise exc
        return '{"cmd":"golden","ok":true}'
    probe = tmp_path / "vcrprobe.sys"
    probe.write_bytes(b"MZ")
    rc, box, tl = _golden(monkeypatch, tmp_path, modes, SONY,
                          ["--modes", ",".join(modes), "--probe", str(probe)],
                          golden=golden, procs=Procs(after={9: VCRCTL_IMAGE}))
    assert rc == code
    assert len(box.cmds(" golden ")) == 2
    rs = _at(tl, " restore")
    # the answer was lost, the capture may not be: a bounded wait for its
    # vcrctl, then the pace-kill, then the one restore
    g2 = [i for i, (k, v) in enumerate(tl) if k == "cmd" and " golden " in v][1]
    kill = _at(tl, "pace-kill 9", g2)
    assert _slept(tl, g2, kill) >= golden_capture.GOLDEN_EXECW_S
    assert kill < rs and box.cmds(" restore") == [RESTORE_CMD]
    _killed_only_through_pace_kill(box)
    # the probe after the cleanup: a vcrctl still holding it keeps it from stopping
    assert rs < _at(tl, "sc stop vcrprobe") < _at(tl, "sc delete vcrprobe")


def _golden_signalled(monkeypatch, tmp_path, golden, procs):
    holder = {}

    class Caught(golden_capture.Interrupts):
        def __init__(self):
            super().__init__()
            holder["it"] = self
    monkeypatch.setattr(golden_capture, "Interrupts", Caught)
    modes = ["640x480x16@85", "1024x768x16@85"]
    rc, box, tl = _golden(monkeypatch, tmp_path, modes, SONY, ["--modes", ",".join(modes)],
                          golden=lambda cmd: golden(holder["it"]), procs=procs)
    return rc, box, json.loads((tmp_path / "g.json").read_text())


def test_golden_capture_lets_the_capture_in_flight_finish_on_a_first_signal(monkeypatch, tmp_path):
    """A running `vcrctl golden` gives its mode back by itself within seconds
    - its exit hold, then XP's revert, paced and stamped - and a kill would
    skip exactly that. The first signal ends the loop, and the capture in
    flight is WAITED for."""
    async def golden(it):
        it.on_signal("SIGINT")                     # arrives while vcrctl golden runs
        await _REAL_SLEEP(0)
        return '{"cmd":"golden","ok":true}'
    rc, box, out = _golden_signalled(monkeypatch, tmp_path, golden, Procs())
    assert rc == 130
    assert len(box.cmds(" golden ")) == 1                 # no further capture
    assert out["captures"][0]["ok"]                        # the running one finished
    assert not box.cmds("pace-kill")
    _killed_only_through_pace_kill(box)
    assert box.cmds(" restore") == [RESTORE_CMD]           # one restore (not at the desktop)


def test_golden_capture_pace_kills_on_a_second_signal(monkeypatch, tmp_path):
    async def golden(it):
        it.on_signal("SIGINT")
        await _REAL_SLEEP(0)
        it.on_signal("SIGTERM")
        await asyncio.Event().wait()                      # never answers
    rc, box, out = _golden_signalled(monkeypatch, tmp_path, golden, Procs(after={9: VCRCTL_IMAGE}))
    assert rc == 130
    assert box.cmds("pace-kill") == [PACE_KILL_CMD + "9"]
    _killed_only_through_pace_kill(box)
    assert out["cleanup"]["killed"] == [9] and out["restore"]["ok"]
    assert box.cmds(" restore") == [RESTORE_CMD]


def test_golden_capture_ends_the_loop_when_the_pace_lock_is_busy(monkeypatch, tmp_path):
    """vcrctl refused the switch: something else is switching the box. No
    further capture asks it to, and nothing is killed for it."""
    modes = ["640x480x16@85", "1024x768x16@85"]
    rc, box, _ = _golden(monkeypatch, tmp_path, modes, SONY, ["--modes", ",".join(modes)],
                         golden='{"cmd":"golden","ok":false,"error":"pace lock busy"}')
    out = json.loads((tmp_path / "g.json").read_text())
    assert rc == 1 and len(box.cmds(" golden ")) == 1 and "pace lock" in out["error"]
    assert not box.cmds("pace-kill")


def test_golden_capture_never_restores_under_someone_elses_vcrctl(monkeypatch, tmp_path):
    procs = Procs(before={4: VCRCTL_IMAGE}, after={4: VCRCTL_IMAGE})
    rc, box, _ = _golden(monkeypatch, tmp_path, ["1024x768x16@85"], SONY,
                         ["--modes", "1024x768x16@85"], golden=TIMED_OUT, procs=procs)
    assert rc == 1 and not box.cmds(" restore") and not box.cmds("pace-kill")
    _killed_only_through_pace_kill(box)
    out = json.loads((tmp_path / "g.json").read_text())
    assert out["cleanup"]["spared"] == [4] and not out["restore"]["ok"]


def test_a_failed_final_restore_is_rc_1_and_not_retried(monkeypatch, tmp_path):
    rc, box, _ = _golden(monkeypatch, tmp_path, ["1024x768x16@85"], SONY,
                         ["--modes", "1024x768x16@85"], restore=RESTORE_FAILED)
    assert rc == 1 and box.cmds(" restore") == [RESTORE_CMD]


# ---- the host scripts: glidelab_run ----------------------------------------------


class AgentBox:
    """v56k_bench.Box's interface (cmd / text / exec_ / upload / download),
    answered by a FakeAgent."""

    def __init__(self, fake):
        self.fake = fake

    async def cmd(self, command, timeout=60.0):
        st, d = await self.fake("h").send_command(command, timeout=timeout)
        return st, d.decode("latin1", "replace")

    async def text(self, command, timeout=60.0):
        return (await self.cmd(command, timeout))[1]

    async def exec_(self, cmdline, timeout=90.0):
        return (await self.cmd(f"EXEC {cmdline}", timeout))[1]

    async def upload(self, remote, data):
        await self.fake("h").send_command(f"UPLOAD {remote}", binary_payload=data)

    async def download(self, remote):
        return (await self.fake("h").send_command(f"DOWNLOAD {remote}"))[1]


GLIDE_OK = 'RESULT {"mode": "fill", "ok": 1, "opened_hz": 85}\n'
GLIDELAB_IMAGE = "GLIDELAB.EXE"


def _glide_box(execw, log=GLIDE_OK, procs=None, info=SONY, registry=()):
    procs = procs or Procs()
    return FakeAgent(list(registry) + [(f"EXEC {VCRCTL_BOX} info", json.dumps(info)),
                                       (f"EXEC {VCRCTL_BOX} pace-mark", PACE_MARK_OK)]
                     + procs.answers() + [("EXECW", procs.launch(execw)), ("DOWNLOAD", log)])


def _glide_run(monkeypatch, tmp_path, fake, argv):
    a, code = _parsed(glidelab_run, ["192.168.1.124"] + argv)
    assert code == 0
    (tmp_path / "out").mkdir(parents=True, exist_ok=True)
    (tmp_path / "out" / "glidelab.exe").write_bytes(b"MZ")
    monkeypatch.setattr(glidelab_run, "KMD", tmp_path)
    monkeypatch.setattr(glidelab_run.vb, "Box", lambda ip: AgentBox(fake))
    monkeypatch.setattr(mode_sweep, "modecalc", lambda: CALC)
    _record_sleeps(monkeypatch, fake)
    return asyncio.run(glidelab_run.main_async(a)), a


def test_glidelab_run_gates_the_mode_before_anything_is_uploaded_or_opened(monkeypatch, tmp_path):
    fake = _glide_box("")
    rc, _ = _glide_run(monkeypatch, tmp_path, fake, ["fill", "--res", "1600x1200", "--refresh", "85"])
    assert rc == 2 and fake.cmds() == [f"EXEC {VCRCTL_BOX} info"]   # asked, refused, nothing else
    # --then opens the same mode: covered by the same check
    fake = _glide_box("")
    rc, _ = _glide_run(monkeypatch, tmp_path, fake,
                       ["fill", "--res", "1600x1200", "--refresh", "85", "--then", "bands"])
    assert rc == 2 and not fake.cmds("EXECW") and not fake.cmds("UPLOAD")
    # the envelope or the default is not this monitor's range
    fake = _glide_box("", info=dict(SONY, mon_src=4))
    rc, _ = _glide_run(monkeypatch, tmp_path, fake, ["fill", "--res", "1024x768", "--refresh", "85"])
    assert rc == 2 and not fake.cmds("EXECW")
    fake = _glide_box("")
    rc, _ = _glide_run(monkeypatch, tmp_path, fake, ["fill", "--res", "1024x768", "--refresh", "85"])
    assert rc == 0 and len(_launches(fake)) == 1
    (cmd, t), = _launches(fake)
    assert int(cmd.split()[1]) == glidelab_run.session_budget("fill", 2, 180) and t > int(cmd.split()[1])


def test_glidelab_run_under_another_driver_needs_a_saved_monitor_info(monkeypatch, tmp_path):
    """The vendor driver cannot be asked for the EDID: the gate reads a
    `vcrctl info` saved from OUR driver - only once the box's registry says
    its monitor is the one there - and still refuses what the monitor cannot
    take."""
    not_ours = {"cmd": "info", "ok": False, "error": "VCR_ESC_INFO refused"}
    fake = _glide_box("", info=not_ours)
    rc, _ = _glide_run(monkeypatch, tmp_path, fake, ["fill", "--res", "1024x768", "--refresh", "85"])
    assert rc == 2 and not fake.cmds("EXECW")
    saved = tmp_path / "ours-info.json"
    saved.write_text(json.dumps(SONY))
    mi = ["--monitor-info", str(saved)]
    for res, want in (("1024x768", 0), ("1600x1200", 2)):
        fake = _glide_box("", info=not_ours, registry=_registry([SONY_PRESENT]))
        rc, _ = _glide_run(monkeypatch, tmp_path, fake, ["fill", "--res", res, "--refresh", "85"]
                           + mi)
        assert rc == want and len(_launches(fake)) == (want == 0), res
    # another monitor on the box now: refused, the flag or not
    for extra in ([], [mode_sweep.CHECKED_FLAG]):
        fake = _glide_box("", info=not_ours,
                          registry=_registry([("GSM5678", "1", True, OTHER_EDID)]))
        rc, _ = _glide_run(monkeypatch, tmp_path, fake,
                           ["fill", "--res", "1024x768", "--refresh", "85"] + mi + extra)
        assert rc == 2 and not fake.cmds("EXECW"), extra
    # a registry that cannot say: refused, unless the operator looked
    for extra, want in (([], 2), ([mode_sweep.CHECKED_FLAG], 0)):
        fake = _glide_box("", info=not_ours, registry=_registry([], broken=True))
        rc, _ = _glide_run(monkeypatch, tmp_path, fake,
                           ["fill", "--res", "1024x768", "--refresh", "85"] + mi + extra)
        assert rc == want, extra


def _host_gave_up(cmd):
    raise asyncio.TimeoutError("the host gave up on the EXECW")


@pytest.mark.parametrize("execw", (TIMED_OUT, _host_gave_up), ids=("execw-timed-out", "no-reply"))
def test_a_glide_session_that_timed_out_is_reaped_before_anything_else(monkeypatch, tmp_path,
                                                                       capsys, execw):
    procs = Procs(before={3: GLIDELAB_IMAGE}, after={3: GLIDELAB_IMAGE, 7: GLIDELAB_IMAGE})
    fake = _glide_box(execw, log="", procs=procs)
    rc, a = _glide_run(monkeypatch, tmp_path, fake, ["fill", "--res", "1024x768", "--refresh", "85"])
    res = json.loads(capsys.readouterr().out.strip().splitlines()[-1])
    assert rc == 1 and res["timed_out"] and not res.get("wedged")
    tl = fake.timeline
    kill = _at(tl, "pace-kill 7", _at(tl, "PROCLIST", _at(tl, "EXECW")))
    assert fake.cmds("pace-kill") == [PACE_KILL_CMD + "7"]
    _killed_only_through_pace_kill(fake)
    assert _slept(tl, kill, len(tl)) >= a.pace
    assert res["cleanup"]["spared"] == [3] and 3 not in procs.pace_killed
    (cmd, t), = _launches(fake)
    assert t > int(cmd.split()[1])


def test_a_wedged_glide_session_ends_the_run_and_then_waits_the_pace(monkeypatch, tmp_path):
    argv = ["abandon", "--res", "1024x768", "--refresh", "85", "--then", "fill"]
    # a survivor that cannot be confirmed gone: --then never runs
    fake = _glide_box(TIMED_OUT, log="", procs=Procs(after={7: GLIDELAB_IMAGE}, unkillable={7}))
    rc, _ = _glide_run(monkeypatch, tmp_path, fake, argv)
    assert rc == 1 and len(_launches(fake)) == 1
    _killed_only_through_pace_kill(fake)
    # killed and gone: --then runs, a pace after the cleanup's own
    n = {"k": 0}

    def execw(cmd):
        n["k"] += 1
        return TIMED_OUT if n["k"] == 1 else ""
    fake = _glide_box(execw, log=lambda cmd: GLIDE_OK if cmd.endswith("fill.log") else "",
                      procs=Procs(after={7: GLIDELAB_IMAGE}))
    rc, a = _glide_run(monkeypatch, tmp_path, fake, argv)
    assert rc == 1 and len(_launches(fake)) == 2
    tl = fake.timeline
    second = _at(tl, "fill.log")                             # the second session clears its log
    assert tl[second - 1][0] == "sleep" and tl[second - 1][1] >= a.pace
    assert _slept(tl, _at(tl, "pace-kill 7"), second) >= 2 * a.pace


@pytest.mark.parametrize("log,why", (('RESULT {"mode": "fill", "ok": 1, "opened_hz": 60}\n', "60 Hz"),
                                     ('RESULT {"mode": "fill", "focus_lost": true, "error": "x"}\n',
                                      "focus"),
                                     ('RESULT {"mode": "fill", "error": "grSstWinOpen not made: '
                                      'pace lock busy"}\n', "pace lock")),
                         ids=("opened-another-refresh", "focus-lost", "pace-lock-busy"))
def test_a_glide_session_that_opened_another_refresh_or_lost_the_screen_ends_the_run(
        monkeypatch, tmp_path, capsys, log, why):
    fake = _glide_box("", log=log)
    rc, _ = _glide_run(monkeypatch, tmp_path, fake, ["fill", "--res", "1024x768", "--refresh", "85",
                                                     "--then", "bands"])
    rows = [json.loads(ln) for ln in capsys.readouterr().out.splitlines() if ln.startswith("{")]
    assert rc == 1 and len(_launches(fake)) == 1                # --then does not follow
    assert why in rows[0]["halt"] and "ended the run" in rows[1]["error"]
    _killed_only_through_pace_kill(fake)


def test_a_glide_session_that_ended_civilly_is_left_alone(monkeypatch, tmp_path):
    fake = _glide_box("")
    rc, _ = _glide_run(monkeypatch, tmp_path, fake, ["fill", "--res", "1024x768", "--refresh", "85"])
    assert rc == 0
    assert not fake.cmds("pace-kill") and not fake.cmds("pace-mark")
    _killed_only_through_pace_kill(fake)
    # a garbled RESULT is not a pass - and the lab may have died without its hold
    fake = _glide_box("", log='RESULT {"mode": "fill", \n')
    rc, _ = _glide_run(monkeypatch, tmp_path, fake, ["fill", "--res", "1024x768", "--refresh", "85"])
    assert rc == 1 and fake.cmds("pace-mark")


def test_glidelab_run_refuses_a_session_budget_past_the_clamp(monkeypatch, tmp_path):
    fake = _glide_box("")
    rc, _ = _glide_run(monkeypatch, tmp_path, fake, ["fill", "--res", "1024x768", "--refresh", "85",
                                                     "--timeout", "790"])
    assert rc == 2 and not fake.sent
    fake = _glide_box("")
    rc, _ = _glide_run(monkeypatch, tmp_path, fake, ["cycle", "--res", "1024x768", "--refresh", "85",
                                                     "--cycles", "3"])
    assert rc == 0
    (cmd, _), = _launches(fake)
    assert int(cmd.split()[1]) == mode_sweep.execw_budget(6, 180)


# ---- the host scripts: glidelab_sweep --------------------------------------------


class FakeGlideAgent:
    def __init__(self, infos):
        self.infos = list(infos)
        self.asked = 0

    async def vcrctl(self, args, tool, timeout=60):
        assert args == "info"
        self.asked += 1
        info = self.infos.pop(0) if len(self.infos) > 1 else self.infos[0]
        return info, json.dumps(info)

    async def alive(self):
        return True


def _glide_sweep(monkeypatch, tmp_path, argv, infos=(SONY,), run=None, procs=None):
    (tmp_path / "out").mkdir(parents=True, exist_ok=True)
    (tmp_path / "out" / "glidelab.exe").write_bytes(b"MZ")
    a, code = _parsed(glidelab_sweep, ["192.168.1.124", "--label", "t"] + argv)
    assert code == 0
    agent, events = FakeGlideAgent(infos), []
    procs = procs or Procs()

    class Box:
        def __init__(self, ip):
            pass

        async def cmd(self, command, timeout=60):
            events.append(("cmd", command))
            if command == "PROCLIST":
                return 0, procs.proclist(command)
            if command.startswith(PACE_KILL_CMD):
                return 0, procs.pace_kill(command)
            if command.endswith(" pace-mark"):
                return 0, PACE_MARK_OK
            return 0, ""

        async def text(self, cmd, timeout=60):
            return ""

        async def upload(self, path, data):
            events.append("upload")

    async def run_mode(box, r_args, mode):
        events.append(("run", mode, r_args.cycles))
        procs.launched = True
        return run(mode, r_args) if run else {"mode": mode}

    async def no_wait(s, *args, **kw):
        events.append(("sleep", s))

    async def set_cfg(ip, cfg):
        pass

    async def wait_back(ag, limit=900):
        return ag
    monkeypatch.setattr(glidelab_sweep, "KMD", tmp_path)
    monkeypatch.setattr(glidelab_sweep.db, "Agent", lambda ip: agent)
    monkeypatch.setattr(glidelab_sweep.db, "safe_reboot", lambda ip: True)
    monkeypatch.setattr(glidelab_sweep.db, "wait_back", wait_back)
    monkeypatch.setattr(glidelab_sweep.gr.vb, "Box", Box)
    monkeypatch.setattr(glidelab_sweep.gr, "run_mode", run_mode)
    monkeypatch.setattr(glidelab_sweep.sw, "set_cfg", set_cfg)
    monkeypatch.setattr(glidelab_sweep.asyncio, "sleep", no_wait)
    monkeypatch.setattr(mode_sweep, "modecalc", lambda: CALC)
    return asyncio.run(glidelab_sweep.main_async(a)), agent, events


def test_glidelab_sweep_refuses_its_old_default_burst(monkeypatch, tmp_path):
    # 4 configs x 2 res x 3 sessions = 48 re-syncs: refused before connecting
    rc, agent, events = _glide_sweep(monkeypatch, tmp_path, [])
    assert rc == 2 and agent.asked == 0 and not events


def test_glidelab_sweep_gates_again_after_every_reboot(monkeypatch, tmp_path):
    """The driver reads the EDID at boot; a DDC read that failed there leaves
    it on the envelope or the default, so the gate is asked again before the
    next session."""
    for k, later in enumerate((dict(SONY, edid_ok=0), dict(SONY, mon_src=3))):
        rc, agent, events = _glide_sweep(monkeypatch, tmp_path / str(k),
                                         ["--cfgs", "0", "--res", "1024x768"],
                                         infos=(SONY, later))
        assert rc == 2 and agent.asked == 2
        assert not [e for e in events if e[0] == "run"]


def test_glidelab_sweep_paces_every_session(monkeypatch, tmp_path):
    rc, agent, events = _glide_sweep(monkeypatch, tmp_path,
                                     ["--cfgs", "0,5", "--res", "1024x768", "--pace", "1"])
    assert rc == 0 and agent.asked == 1 + 2              # the start, then after each reboot
    runs = [i for i, e in enumerate(events) if e[0] == "run"]
    assert len(runs) == 2 * len(glidelab_sweep.RUNS)
    for i in runs:
        assert events[i - 1][0] == "sleep" and events[i - 1][1] >= 3
        assert events[i][2] == 1                          # one open/close per session
    # the 1600x1200@85 a vendor sweep could ask for is out of the Sony's range
    rc, _, events = _glide_sweep(monkeypatch, tmp_path / "x",
                                 ["--cfgs", "0", "--res", "1600x1200", "--refresh", "85",
                                  "--no-reboot"])
    assert rc == 2 and not events


@pytest.mark.parametrize("outcome", ("timed_out", "wedged", "halt", "raised"))
def test_a_timed_out_or_wedged_session_ends_the_whole_sweep(monkeypatch, tmp_path, outcome):
    """A board that just hung a session is not handed the next one - not the
    next cell, not the next config. Nor is one whose session lost the screen,
    found the pace lock busy or opened another refresh."""
    def run(mode, r_args):
        if outcome == "raised":
            raise ConnectionResetError("the agent dropped the connection")
        return {"mode": mode, "error": "x",
                outcome: "the pace lock was busy" if outcome == "halt" else True}
    rc, _, events = _glide_sweep(monkeypatch, tmp_path,
                                 ["--cfgs", "0,5", "--res", "1024x768", "--no-reboot"], run=run)
    assert rc == 3
    assert len([e for e in events if e[0] == "run"]) == 1
    rows = [json.loads(ln) for ln in (tmp_path / "evidence" / "glidelab" / "t.jsonl")
            .read_text().splitlines()]
    assert len(rows) == 1
    if outcome == "raised":
        # run_mode's own cleanup never ran: the sweep runs it before stopping
        cmds = [e[1] for e in events if e[0] == "cmd"]
        assert "PROCLIST" in cmds and any(c.endswith(" pace-mark") for c in cmds)
        assert rows[0]["wedged"] and rows[0]["cleanup"]["pace_mark"]


def test_a_raising_session_is_reaped_with_the_list_it_took_before_its_launch(monkeypatch, tmp_path):
    """run_mode keeps its pre-launch PROCLIST on its args before the EXECW, so
    when it raises after that, the sweep's own cleanup still knows whose a
    glidelab is: the new one is pace-killed, the old one spared."""
    procs = Procs(before={3: GLIDELAB_IMAGE}, after={3: GLIDELAB_IMAGE, 7: GLIDELAB_IMAGE})

    def run(mode, r_args):
        r_args.spare = [3]
        raise ConnectionResetError("the agent dropped the connection")
    rc, _, events = _glide_sweep(monkeypatch, tmp_path,
                                 ["--cfgs", "0", "--res", "1024x768", "--no-reboot"], run=run,
                                 procs=procs)
    assert rc == 3 and procs.pace_killed == [7]
    rows = [json.loads(ln) for ln in (tmp_path / "evidence" / "glidelab" / "t.jsonl")
            .read_text().splitlines()]
    assert rows[0]["cleanup"]["killed"] == [7] and rows[0]["cleanup"]["spared"] == [3]
    assert not [e for e in events if e[0] == "cmd" and ("PROCKILL" in e[1] or "taskkill" in e[1])]


# ---- the host scripts: the SLI scripts (Quake II switches the monitor too) ----------


def _sli_fake(info, listed):
    return FakeAgent([(f"EXEC {VCRCTL_BOX} info", json.dumps(info)),
                      (f"EXEC {VCRCTL_BOX} modes", json.dumps({"cmd": "modes", "ok": True,
                                                               "modes": listed,
                                                               "current": DESKTOP}))])


def _sli_env(monkeypatch, tmp_path, mod, fake):
    (tmp_path / "tools").mkdir(parents=True, exist_ok=True)
    (tmp_path / "out").mkdir(parents=True, exist_ok=True)
    for f in ("vcrctl.exe", "vcrprobe.sys"):
        (tmp_path / "out" / f).write_bytes(b"MZ")
    monkeypatch.setattr(mod, "HERE", tmp_path / "tools")
    monkeypatch.setattr(mod.vb, "Box", lambda ip: AgentBox(fake))
    monkeypatch.setattr(mode_sweep, "modecalc", lambda: CALC)


def test_sli_golden_gates_the_games_mode_before_the_probe_or_the_config(monkeypatch, tmp_path):
    """Quake II through our ICD goes fullscreen at WxH and picks the highest
    refresh the driver lists at that size, at any depth: that is the mode
    checked, before vcrprobe is loaded or the SLI config written."""
    fake = _sli_fake(dict(SONY, mon_v_hz=[48, 70]), ["640x480x16@60", "640x480x32@85"])
    _sli_env(monkeypatch, tmp_path, sli_golden, fake)
    a = argparse.Namespace(host="h", label="t", cfg=5, w=640, h=480, depth=16, settle=1,
                           samples=1, out=None, monitor_info=None,
                           i_have_checked_the_monitor=False)
    assert asyncio.run(sli_golden.main_async(a)) == 2
    assert not fake.cmds("vcrprobe") and not fake.cmds("EXEC sc ") and not fake.cmds("REG")
    # the envelope is not this monitor's range either
    fake = _sli_fake(dict(SONY, mon_src=3), ["640x480x16@60"])
    _sli_env(monkeypatch, tmp_path, sli_golden, fake)
    assert asyncio.run(sli_golden.main_async(a)) == 2 and not fake.cmds("vcrprobe")


def test_sli_shot_gates_the_games_mode_before_the_config(monkeypatch, tmp_path):
    fake = _sli_fake({"cmd": "info", "ok": False}, ["1024x768x16@85"])
    _sli_env(monkeypatch, tmp_path, sli_shot, fake)
    a = argparse.Namespace(host="h", cfg=5, res="1024x768", depth=16, frames=1, label="t",
                           start="map base1", chips=4, nlines=8, timeout=10, monitor_info=None,
                           i_have_checked_the_monitor=False)
    assert asyncio.run(sli_shot.main_async(a)) == 2
    assert not fake.cmds("REGWRITE") and not fake.cmds("REGREAD")


def test_sli_golden_sweep_gates_before_its_first_config_and_stops_on_a_later_refusal(monkeypatch,
                                                                                      tmp_path):
    cfgs, runs = [], []
    monkeypatch.setattr(sli_golden_sweep, "KMD", tmp_path)
    (tmp_path / "out").mkdir()
    (tmp_path / "out" / "vcrctl.exe").write_bytes(b"MZ")

    async def set_cfg(ip, cfg):
        cfgs.append(cfg)
    monkeypatch.setattr(sli_golden_sweep, "set_cfg", set_cfg)
    monkeypatch.setattr(sli_golden_sweep.db, "safe_reboot", lambda ip: True)

    async def wait_back(ag, limit=900):
        return 1.0
    monkeypatch.setattr(sli_golden_sweep.db, "wait_back", wait_back)

    class Alive:
        def __init__(self, ip):
            pass

        async def alive(self):
            return True
    monkeypatch.setattr(sli_golden_sweep.db, "Agent", Alive)
    monkeypatch.setattr(mode_sweep, "modecalc", lambda: CALC)
    base = dict(host="h", label="t", cfgs=[5, 7], w=1024, h=768, depth=16, settle=1,
                reboot=True, recover_wait=1, compare=None, monitor_info=None,
                i_have_checked_the_monitor=False)
    # refused up front: no config written, no reboot, no capture
    fake = _sli_fake(dict(SONY, mon_filter=0), ["1024x768x16@85"])
    monkeypatch.setattr(sli_golden_sweep.vb, "Box", lambda ip: AgentBox(fake))
    monkeypatch.setattr(sli_golden_sweep.subprocess, "run",
                        lambda *a, **k: runs.append(a) or pytest.fail("a capture was started"))
    assert asyncio.run(sli_golden_sweep.main_async(argparse.Namespace(**base))) == 2
    assert not cfgs and not runs
    # passes up front; sli_golden.py's own gate refuses after the reboot (rc 2):
    # the sweep stops there - no further config
    fake = _sli_fake(SONY, ["1024x768x16@85"])
    monkeypatch.setattr(sli_golden_sweep.vb, "Box", lambda ip: AgentBox(fake))

    def capture(argv, **kw):
        runs.append(argv)
        assert "--monitor-info" not in argv                   # none asked, none passed
        return subprocess.CompletedProcess(argv, 2, "REFUSED", "")
    monkeypatch.setattr(sli_golden_sweep.subprocess, "run", capture)
    assert asyncio.run(sli_golden_sweep.main_async(argparse.Namespace(**base))) == 1
    assert cfgs == [5] and len(runs) == 1
    # the flags are handed on, for the gate after each reboot
    a = argparse.Namespace(**dict(base, monitor_info="m.json", i_have_checked_the_monitor=True))
    assert sli_golden_sweep.gate_args(a) == ["--monitor-info", "m.json", mode_sweep.CHECKED_FLAG]


def test_the_battery_keeps_the_monitor_model_but_never_its_serial():
    """The info step records `vcrctl info`, which carries the whole EDID; its
    0xFF display descriptor is the unit's serial number. Evidence committed
    to the repo keeps the model (0xFC name) and blanks the serial, as the
    EDID fixtures do (2026-09-26: the first .124 battery wrote it out)."""
    sony = ("00ffffffffffff004dd970123de66a00330901020e211896eb0cc9a057479b2712484cffff80"
            "31594559615971598199814fa94f0101ea240060410028303060130038ea1000001e000000fd00"
            "30781e601a000a202020202020000000fc004350442d473230300a20202020000000ff00"
            + "30" * 7 + "0a202020202000c5")
    out = json.loads(silicon_battery.blank_edid_serial(json.dumps({"cmd": "info", "edid": sony})))
    b = bytes.fromhex(out["edid"])
    assert len(b) == 128
    assert b[108:113] == b"\0\0\0\xff\0" and b[113:126] == b"\n" + b" " * 12   # serial gone
    assert b"CPD-G200" in b                                                       # model kept
    # a block with no serial descriptor is returned unchanged
    assert silicon_battery.blank_edid_serial('{"edid":"%s"}' % ("00" * 128)) == \
        '{"edid":"%s"}' % ("00" * 128)
