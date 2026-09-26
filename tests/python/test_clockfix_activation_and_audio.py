"""Agent 1.85.2: two fresh-image defects found on a Dell Dimension 4600 (2026-09-26).

1. clockfix spent an unactivated XP box's whole activation grace. The CMOS clock
   read 2004 during setup, so the install (and the 30-day grace) started in
   2004; clockfix then set 2026, and WMI read RemainingGracePeriod=0. The next
   reboot would have stopped at the logon-blocking activation screen.
   Decision: agent/shared/clockguard.h (tests/native/test_clockguard.c).

2. The SoundMAX driver installed "working" and there was no wave device: the
   RunOnce entries wdmaudio.inf writes to register sysaudio/kmixer/wdmaud had
   been consumed without running. Decision: agent/shared/audiofix.h
   (tests/native/test_audiofix.c).

These pin the WIRING the native tests cannot see.
"""
import os
import re

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SRC = os.path.join(REPO, "agent", "src")


def _read(name):
    with open(os.path.join(SRC, name), encoding="utf-8", errors="replace") as f:
        return f.read()


def _code(src):
    """C source with comments removed - a comment may NAME a forbidden call to
    explain why it is not made."""
    src = re.sub(r"/\*.*?\*/", "", src, flags=re.S)
    return re.sub(r"//[^\n]*", "", src)


def _body(src, signature):
    i = src.index(signature)
    j = src.index("\n}\n", i)
    return src[i:j]


def test_clockfix_asks_about_activation_before_it_sets_the_clock():
    body = _body(_read("clockfix.c"), "DWORD WINAPI clockfix_thread")
    assert "clk_activation_allows(" in body, "the activation guard is gone"
    assert body.index("clk_activation_allows(") < body.index("SetSystemTime("), (
        "the guard runs after the clock is already moved")


def test_a_refusal_is_recorded_not_silent():
    src = _read("clockfix.c")
    guard = _body(src, "static int clk_activation_allows")
    assert "REFUSED" in guard and "clk_note(" in guard, (
        "a clock left wrong on purpose must say so in ClockFixed, or it reads as "
        "a clock nobody tried to fix")
    assert "clockguard_decide(" in guard, "the decision must come from the shared header"


def test_the_wmi_reader_is_read_only_and_dynamically_loaded():
    src = _code(_read("wpawmi.c"))
    for forbidden in ("ExecMethod", "ActivateOffline", "SetProductKey", "SetConfirmationID"):
        assert forbidden not in src, (
            "%s changes activation - the agent only reads it" % forbidden)
    assert 'LoadLibraryA("ole32.dll")' in src and 'LoadLibraryA("oleaut32.dll")' in src
    assert "VariantInit(" not in src, (
        "VariantInit is an oleaut32 export: calling it creates a static import")
    makefile = open(os.path.join(REPO, "agent", "Makefile")).read()
    assert "-lole32" not in makefile and "-loleaut32" not in makefile, (
        "ole32/oleaut32 must stay dynamic (see ntdyn.h - a static import Win9x "
        "cannot resolve kills the exe at load)")
    assert "$(SRCDIR)/wpawmi.c" in makefile


def test_licstatus_reports_windows_own_verdict():
    src = _read("licstatus.c")
    assert "report_wpa_wmi(&j)" in src
    assert '"grace_days"' in src


def test_the_audio_check_runs_every_startup_behind_the_host_policy():
    src = _read("gamesync.c")
    thread = _body(src, "DWORD WINAPI gamesync_thread")
    assert "gs_audio_stack_check();" in thread
    assert thread.index("host_manages_this_box()") < thread.index("gs_audio_stack_check();"), (
        "the audio repair must never run on a modern Windows host")
    assert thread.index("gs_audio_stack_check();") < thread.index("if (gs_file_exists(GS_MARKER))"), (
        "placed after the marker check it would never run on a provisioned box")


def test_the_audio_repair_writes_registry_only_and_runs_only_streamci():
    src = _read("gamesync.c")
    rewrite = _body(src, "static int gs_audio_rewrite_registration")
    assert "SPINST_REGISTRY" in rewrite
    assert not re.search(r"SPINST_(FILES|ALL)", rewrite), (
        "CopyFiles can prompt for media on a box with nobody at it")
    assert "(void *)GetProcAddress" in rewrite, "setupapi INF calls must stay dynamic"
    run = _body(src, "static int gs_audio_run_streamci")
    assert "audiofix_is_streamci_cmd(" in run, (
        "the repair may run wdmaudio's registrations and NOTHING else queued in RunOnce")
    check = _body(src, "static void gs_audio_stack_check")
    assert "audiofix_decide(" in check
    assert "STILL NO SOUND" in check and "NO SOUND on this box" in check, (
        "a repair that did not work must say so")
