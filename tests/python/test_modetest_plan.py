"""The per-box modetest plan must stay inside what modetest.exe itself allows.

tools/modetest/run_modetest.py writes down, per box, which modes modetest.exe
switches through and how often. modetest refuses a run whose plan exceeds its
own limits (tools/modetest/modetest_logic.h), and a plan that disagrees with
the tool would only be discovered on a box - mid-run, on a CRT. So the plan is
checked here against the same numbers, offline. Nothing here touches a box.
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(REPO, "tools", "modetest"))

import run_modetest as rm  # noqa: E402

LOGIC = open(os.path.join(REPO, "tools", "modetest", "modetest_logic.h")).read()


def _define(name):
    return int(re.search(r"#define %s\s+(\d+)" % name, LOGIC).group(1))


# switch counts per test, as modetest_logic.h mt_plan_switches() counts them
def _plan_switches(args):
    body = LOGIC[LOGIC.index("static int mt_plan_switches"):]
    body = body[:body.index("\n}\n")]
    cost = {m.group(1): int(m.group(2)) for m in
            re.finditer(r"if \(tests & MT_T_(\w+)\)\s+n \+= (\d+);", body)}
    toks = args.split()
    n = 0
    for flag, key in (("-a", "A"), ("-b", "B"), ("-c", "C"), ("-cb", "CB"), ("-d", "D")):
        if flag in toks:
            n += cost[key]
    return n


def test_the_switch_costs_were_parsed():
    assert _plan_switches("1 1 1 -a -b 75 -c -cb 75 -d") == 15


def test_every_step_declares_the_switches_the_tool_will_make():
    for ip, b in rm.PLAN.items():
        for i, s in enumerate(b["steps"]):
            assert _plan_switches(s["args"]) == s["switches"], (ip, i, s["args"])
            assert s["switches"] <= _define("MT_MAX_SWITCHES"), (ip, i)


def test_no_step_asks_for_more_than_the_persisted_desktop():
    for ip, b in rm.PLAN.items():
        pw, ph, pbpp, _ = b["persisted"]
        for s in b["steps"]:
            nums = [int(t) for t in s["args"].split()[:3] if t.isdigit()]
            if len(nums) == 3:
                w, h, bpp = nums
                assert w <= pw and h <= ph, (ip, s["args"])


def test_explicit_rates_are_inside_the_monitors_stated_ceiling():
    for ip, b in rm.PLAN.items():
        m = re.search(r"vmax (\d+)", b["monitor"])
        for s in b["steps"]:
            for flag in ("-b", "-cb"):
                toks = s["args"].split()
                if flag in toks:
                    rate = toks[toks.index(flag) + 1]
                    assert rate != "max" or m, (ip, "max needs an EDID ceiling")
                    if rate.isdigit():
                        assert m, (ip, "an explicit rate on a box with no EDID")
                        assert int(rate) <= int(m.group(1)), (ip, s["args"])


def test_win98_steps_name_no_rate_and_open_no_directx():
    """Win9x lists every mode at 0 Hz (.243), so there is no rate to name, and
    the first measurements there are the plain no-rate request only."""
    for ip, b in rm.PLAN.items():
        if b["os"] != "win98":
            continue
        for s in b["steps"]:
            toks = s["args"].split()
            assert not set(toks) & {"-b", "-cb", "-c", "-d"}, (ip, s["args"])


def test_directx_steps_are_launched_visible():
    for ip, b in rm.PLAN.items():
        for s in b["steps"]:
            if set(s["args"].split()) & {"-c", "-cb", "-d"}:
                assert s["fg"], (ip, s["args"], "exclusive mode needs the foreground")


def test_the_v5_box_is_gated_and_never_gets_direct3d9():
    b = rm.PLAN["192.168.1.124"]
    assert b.get("v5") is True
    assert all("-d" not in s["args"].split() for s in b["steps"])
    assert b.get("pace", 0) >= 8, "the P1120 is a CRT: pace it harder than 6 s"


def test_the_host_outlasts_the_tools_own_watchdog():
    """If the agent's EXECW timed out first it would tree-kill modetest in the
    middle of a restore: XP then reverts the mode at once, unpaced, on a CRT.

    Review 2026-09-29: 'outlasts the watchdog' is not enough - after the
    watchdog fires, modetest still waits for a switch in flight
    (MT_TAKEOVER_MS), may retry a busy pace lock three times and runs the exit
    hold. The old margin (60 s) was shorter than that path."""
    src = open(os.path.join(REPO, "tools", "modetest", "modetest.c")).read()
    assert "ms = 60000u + (DWORD)(mt_plan_switches(g_tests) + 2) *" in src
    takeover_s = int(re.search(r"#define MT_TAKEOVER_MS (\d+)u", src).group(1)) // 1000
    pace_h = open(os.path.join(REPO, "voodoo-cleanroom", "vcr-kmd", "tools",
                               "vcr_pace.h")).read()
    lock_s = int(re.search(r"#define VCR_PACE_LOCK_MS\s+(\d+)u", pace_h).group(1)) // 1000
    worst_after = takeover_s + 3 * (lock_s + 5) + 30 + (lock_s + 8)
    assert rm.GIVEBACK_WORST_S >= worst_after, (rm.GIVEBACK_WORST_S, worst_after)
    for ip, b in rm.PLAN.items():
        pace = b.get("pace", 6)
        for s in b["steps"]:
            tool_cap = 60 + (s["switches"] + 2) * (pace + rm.HOLD_S + 4)
            assert rm.tool_cap(b, s) == tool_cap, (ip, s["args"])
            assert rm.step_budget(b, s) >= tool_cap + worst_after, (ip, s["args"])
            assert rm.step_budget(b, s) <= 900, (ip, s["args"], "EXECW clamps at 15 min")


def test_the_execw_client_waits_as_long_as_the_agent():
    """The agent answers EXECW when modetest exits; send_command's default
    timeout is 60 s. The client must be told the budget, or a step over a
    minute raises, closes the connection and loses its evidence."""
    src = open(os.path.join(REPO, "tools", "modetest", "run_modetest.py")).read()
    assert re.search(r'send_command\("EXECW %d %s" % \(budget, line\),\s*'
                     r'timeout=budget \+ 60\)', src)


def test_the_runner_checks_the_screen_itself_after_every_step():
    ok = '{"width":1920,"height":1080,"bpp":32,"refresh":60}'
    assert rm.screen_is_persisted(ok, (1920, 1080, 32, 60))
    assert not rm.screen_is_persisted(ok.replace("1920", "1280").replace("1080", "960"),
                                      (1920, 1080, 32, 60))
    assert not rm.screen_is_persisted(ok.replace(":60", ":75"), (1920, 1080, 32, 60))
    # Win9x: the live rate reads 0 = unknown, not different
    w98 = '{"width":1024,"height":768,"bpp":8,"refresh":0,"registry_refresh":75}'
    assert rm.screen_is_persisted(w98, (1024, 768, 8, 75))
    assert not rm.screen_is_persisted("ERR something", (1024, 768, 8, 75))
    src = open(os.path.join(REPO, "tools", "modetest", "run_modetest.py")).read()
    assert 'res["screen_after_ok"] = screen_is_persisted(dc, b["persisted"])' in src


def test_every_createthread_passes_a_thread_id_for_win9x():
    """Win95/98 FAIL CreateThread with lpThreadId = NULL (error 87) - the agent's
    helper threads once silently never started on .243 that way. modetest 0.1.0
    started its WATCHDOG like that, so on the one Win98 box in the plan the run
    would have had no watchdog at all (review 2026-09-29)."""
    src = open(os.path.join(REPO, "tools", "modetest", "modetest.c")).read()
    calls = re.findall(r"CreateThread\(([^;]*?)\);", src, flags=re.S)
    assert calls, "CreateThread calls not found - the parser is broken"
    for args in calls:
        last = args.split(",")[-1].strip()
        assert last not in ("NULL", "0"), "CreateThread(..., %s): NULL thread id" % last
    # and a switching run refuses to start without its watchdog
    assert "if (!mt_start_watchdog())" in src


def test_the_shipped_build_has_no_test_hooks():
    """MT_TEST_CAP_MS shortens the watchdog budget for the build-VM test of the
    take-over; build.sh must never define it."""
    build = open(os.path.join(REPO, "tools", "modetest", "build.sh")).read()
    assert "MT_TEST_CAP_MS" not in build


def test_windows_11_is_never_planned():
    assert "192.168.1.249" in rm.NEVER and "192.168.1.249" not in rm.PLAN


def test_parse_finds_the_tagged_run_and_its_verdict():
    log = (
        "[      0 ms] ==== modetest 0.1.0  (x.exe -list -tag aaa)\n"
        "[     20 ms] exit 0 (switches made: 0)\n"
        "[      0 ms] ==== modetest 0.1.0  (x.exe 1280 960 32 -a -b 75 -tag bbb)\n"
        "[   6000 ms] RESULT test=A step=norate req=1280x960x32@0 now=1280x960x32 "
        "eds_hz=60 caps_hz=60 dd_freq=60 vblank_mhz=59990 vblank_hz=60 agree=2 "
        "via=\"WaitForVerticalBlank\" rc=SUCCESSFUL\n"
        "[  37500 ms] B VERDICT: the no-rate request DID NOT KEEP the pre-switched 75 Hz\n"
        "[  48000 ms] exit 0 (switches made: 5)\n")
    r = rm.parse_log(log, "bbb")
    assert r["exit"] == 0 and r["switches"] == 5
    assert r["results"][0]["eds_hz"] == "60" and r["results"][0]["via"] == "WaitForVerticalBlank"
    assert "DID NOT KEEP" in r["verdicts"][0]
    assert rm.parse_log(log, "zzz").get("error")
    bad = log.replace("B VERDICT", "## RESTORE FAILED: x\n[ 1 ms] B VERDICT")
    assert rm.parse_log(bad, "bbb")["restore_failed"] is True
