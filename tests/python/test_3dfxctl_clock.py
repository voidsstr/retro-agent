"""The 3dfx Control Panel's Clock tab (3dfxctl 2.1.0, clean-room lane, 2026-09-29).

The user asked for 3dfx "clock settings [that] can be set in real time". The
panel sets the V5's graphics clock through vcr-kmd's VCR_ESC_CLOCK (the driver
moves the master chip at once, in <= 5 MHz steps, and every game start copies
it to the slave chips; tests/python/test_vcr_kmd_core_clock.py pins that half).
What this file pins is the panel's half - each one a way a clock tool hurts a
box:

- it asks the DRIVER (a request struct through the escape), never pokes a
  register itself, and never writes the driver's kill switch;
- it offers only 133-200 MHz and nothing while a 3D game holds the card;
- an overclock goes back by itself unless a person keeps it (a garbled screen
  cannot be read to find the button), and is saved for restarts only once kept;
- "use this clock again after Windows restarts" re-applies it at logon ONLY
  after a clean shutdown: Windows' ShutdownTime stamp must have moved since the
  clock was set, and the stamp is consumed BEFORE the clock moves - so a crash
  in the session that runs the clock leaves the next start at stock.

The decisions themselves are in ctl_logic.h (tests/native/test_3dfxctl_logic.c).
"""
import re
from pathlib import Path

import test_3dfxctl_panel as panel

REPO = Path(__file__).resolve().parents[2]
SRC = panel.SRC
LOGIC = panel.LOGIC
_body = panel._body


def test_the_clock_is_asked_of_the_driver_never_poked():
    c = _body("clock_call", strings=False)
    assert re.search(r"ExtEscape\(dc, \(int\)VCR_ESC_CLOCK, \(int\)sizeof rq, \(LPCSTR\)&rq,", c)
    assert "rq.size = sizeof rq;" in c
    # a short or missing answer is "no answer", never a half-read struct
    assert "n >= (int)sizeof *res && res->size >= sizeof *res" in c
    # no register pokes and no Diag writes for the clock anywhere in the panel
    assert "VCR_ESC_REG" not in panel.CODE
    lits = set(re.findall(r'"((?:[^"\\]|\\.)*)"', panel.CODE_S))
    assert "CoreClock" not in lits, "the panel must never write the driver's kill switch"


def test_only_the_panels_range_and_never_under_a_game():
    assert re.search(r"#define CTL_CLK_MIN_MHZ\s+133\b", LOGIC)
    assert re.search(r"#define CTL_CLK_MAX_MHZ\s+200\b", LOGIC)
    p = _body("PageProc", strings=False)
    i = p.index("case IDC_CLK_SET:")
    seg = p[i:p.index("case IDC_CLK_PERSIST:", i)]
    assert seg.index("ctl_clock_mhz_ok(") < seg.index("clock_start(VCR_CLOCK_OP_SET")
    assert "if (g_busy || !G.have_clock)" in seg
    # the trackbar's own range is the panel's
    b = _body("page_build", strings=False)
    assert "MAKELPARAM(CTL_CLK_MIN_MHZ, CTL_CLK_MAX_MHZ)" in b
    # a 3D game holding the card disables Set (the driver refuses as well)
    u = _body("clk_update_ui", strings=False)
    assert "ex = G.clk.exclusive_pid != 0;" in u
    assert "EnableWindow(g_clk_set, !g_busy && !ex && !same);" in u


def test_an_overclock_goes_back_unless_kept_and_is_saved_only_once_kept():
    d = _body("on_clock_done", strings=False)
    confirm = d.index("ctl_clock_confirm(j.res.cur_khz, j.res.boot_khz)")
    dialog = d.index("KeepProc", confirm)
    back = d.index("if (keep != IDOK) {", dialog)
    putback = d.index("clock_start(j.prev_stock ? VCR_CLOCK_OP_RESTORE : VCR_CLOCK_OP_SET, "
                      "j.prev_khz, 1);", back)
    assert confirm < dialog < back < putback
    assert d.index("return;", putback) < d.index("clk_persist_write(")
    # a put-back is not asked about again, and a failed set is not "kept"
    assert d.index("} else if (j.revert) {") < confirm
    assert d.index("if (!j.answered) {") < confirm
    # the countdown's default button is the way back (KeepProc, shared with the refresh)
    k = _body("KeepProc", strings=False)
    assert "g_countdown = 15;" in k and "BS_DEFPUSHBUTTON" in k and "IDCANCEL" in k
    # the post-condition is read back from the card before anything is said
    assert d.index("stack_live();") < d.index("clk_msg(")


def test_the_logon_reapply_consumes_the_stamp_before_the_clock_moves():
    s = _body("clock_startup", strings=False)
    decide = s.index("switch (ctl_clock_startup(saved, has_it, now, nnow, seen, nseen)) {")
    sw_end = s.index("\n    }\n", decide)
    cases = s[decide:sw_end]
    # every refusal forgets the saved clock and returns before any clock request
    for case in ("CTL_CLK_START_NONE", "CTL_CLK_START_UNCLEAN", "CTL_CLK_START_UNKNOWN",
                 "CTL_CLK_START_RANGE"):
        c = cases[cases.index(f"case {case}:"):]
        c = c[:c.index("return 0;")]
        assert "clk_persist_clear(why, sizeof why);" in c, case
    assert "clock_call(" not in cases
    # A LOGON IS NOT A BOOT: the card still running the saved clock (a logoff
    # and logon) keeps everything - no clear, no stamp, no note of a crash
    run = cases[cases.index("case CTL_CLK_START_RUNNING:"):]
    run = run[:run.index("return 0;")]
    assert "clk_persist_clear" not in run and "clk_note_last" not in run
    pre = s[:decide]
    assert "clock_call(VCR_CLOCK_OP_GET, 0, &r)" in pre and "r.cur_pll[0] == want" in pre
    after = s[sw_end:]
    stamp = after.index('reg_set_bin(HKEY_LOCAL_MACHINE, CTL_KEY_PANEL, "StartupSeen", now, 8)')
    readback = after.index("memcmp(now, back, 8) != 0", stamp)
    move = after.index("clock_call(VCR_CLOCK_OP_SET, saved, &r)")
    assert stamp < readback < move, "the stamp must be consumed (and read back) first"
    # and the logon mode is dispatched before any window exists
    w = _body("WinMain", strings=False)
    assert w.index('ci_strstr(cmd, "/startup")') < w.index("DialogBoxIndirectParamA")


def test_persistence_needs_windows_own_stamp_and_reads_everything_back():
    assert re.search(r'#define CTL_KEY_WINDOWS\s+"SYSTEM\\\\CurrentControlSet\\\\Control\\\\Windows"',
                     LOGIC)
    assert '"ShutdownTime"' in _body("shutdown_stamp", strings=False)
    w = _body("clk_persist_write", strings=False)
    assert w.index("if (!shutdown_stamp(st)) {") < w.index("reg_set_bin(")
    assert "memcmp(st, back, 8) != 0" in w
    assert 'write_value(HKEY_LOCAL_MACHINE, CTL_KEY_PANEL, "StartupClock", v, 1, why, n)' in w
    assert "strcmp(got, line) != 0" in w                      # the Run value, read back
    assert '"\\"%s\\" /startup"' in w
    # off: the logon entry goes first - from then on nothing re-applies anything
    c = _body("clk_persist_clear", strings=False)
    assert c.index("CTL_RUN_CLOCK") < c.index('"StartupClock"')
    # "on" means both halves exist
    o = _body("clk_persist_on", strings=False)
    assert '"StartupClock"' in o and "CTL_RUN_CLOCK" in o


def test_the_clock_tab_is_for_our_stack_with_a_driver_that_answers():
    m = _body("MainProc", strings=False)
    assert "(t == CTL_TAB_CLOCK && G.lane == CTL_LANE_VCR && G.have_clock)" in m
    l = _body("stack_live", strings=False)
    assert "clock_call(VCR_CLOCK_OP_GET, 0, &G.clk)" in l
    assert "G.info.backend == VCR_HW_VOODOO" in l
    deps = re.search(r"^DEPS\s*=\s*((?:.*\\\n)*.*)$", panel.MAKEFILE, re.M).group(1)
    assert "vcr_clock.h" in deps


def test_the_version_says_the_clock_is_in():
    assert '#define CTL_VERSION     "2.1.0"' in SRC
