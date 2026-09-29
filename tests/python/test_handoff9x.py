"""handoff9x - put a Win9x box on a chosen agent build across a RESTART, with
no QUIT and no reboot (2026-09-28, .243, agent 1.86.1 -> 1.90.0).

WHY IT EXISTS. On .243 the documented swap tool, agentswap9x, waits for the
old agent PROCESS to leave the process list and then renames the exe. Paired
with RESTART it can never work, measured: the old 1.86.1 agent released its
single-instance mutex within ~3 s but stayed in the process list (3 threads;
its console DOS VM still hosted retro_chat.exe), restart.bat relaunched the
SAME exe ~8 s after RESTART, that relaunch took the free mutex - and
AGENTSWAP.TXT read "agent still running after 120 s - NOT swapping", with the
box back on 1.86.1. handoff9x watches the MUTEX instead: it takes it the moment
the old agent lets go, so RESTART's relaunch exits ("another retro_agent is
already running"), then hands it to the chosen exe. On .243 it moved the box
1.86.1 -> 1.90.0 (HANDOFF.TXT: "mutex freed after 3794 ms", "mutex TAKEN",
"RA190.EXE still running after 25 s"), and in `watch` mode guarded a 1.90.0
RESTART without touching it ("an agent holds the mutex again after 10494 ms -
nothing to do"). Evidence: .claude/evidence-icons/192.168.1.243/release/.

What is pinned, because each one failing is a box that needs a person:
  - the mutex name is the agent's own (a rename in main.c must not silently
    turn the tool into something that sees "no agent" and does nothing - or
    worse, starts a second one);
  - probing never CREATES the mutex (OpenMutexA), so it cannot make a starting
    agent see ERROR_ALREADY_EXISTS and exit; only `take` creates it, once;
  - it acts only after seeing the mutex HELD first (an agent launched it);
  - `take` releases the mutex BEFORE starting the target, and falls back to
    retro_agent.exe when the target is not running 25 s later;
  - CRT-free, i586, KERNEL32/USER32 only (the README's rule for 9x helpers).
"""
import re
import shutil
import subprocess
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
TOOL = REPO / "scripts" / "fleet" / "win9x" / "handoff9x.c"
SRC = TOOL.read_text()
CODE = re.sub(r"/\*.*?\*/", "", SRC, flags=re.S)
MAIN = (REPO / "agent" / "src" / "main.c").read_text()


def test_the_mutex_is_the_agents_own():
    agent = re.search(r'#define AGENT_INSTANCE_MUTEX\s+"([^"]+)"', MAIN)
    tool = re.search(r'#define AGENT_MUTEX\s+"([^"]+)"', CODE)
    assert agent and tool
    assert tool.group(1) == agent.group(1), (
        "handoff9x watches a mutex the agent does not create")


def test_probing_never_creates_the_mutex():
    held = CODE[CODE.index("static int mutex_held(void)"):]
    held = held[:held.index("}") + 1]
    assert "OpenMutexA(" in held and "CreateMutexA(" not in held
    assert CODE.count("CreateMutexA(") == 1, "only `take` may create it, once"
    take_at = CODE.index("CreateMutexA(")
    assert CODE.rfind("if (!take)", 0, take_at) != -1, (
        "the CreateMutexA must come after the watch branch has returned")


def test_it_acts_only_after_seeing_an_agent_hold_the_mutex():
    loop = CODE[CODE.index("t0 = GetTickCount();"):CODE.index("mutex freed after")]
    assert "else if (seen)" in loop, "free counts only once it was seen held"
    assert "never seen held" in loop and "done(6)" in loop


def test_take_hands_over_and_never_leaves_the_box_without_an_agent():
    take = CODE[CODE.index("CreateMutexA("):]
    release = take.index("CloseHandle(m);")
    start = take.index("start_and_check(target, 1)")
    assert release < start, "the target would exit on the mutex we still hold"
    assert "ERROR_ALREADY_EXISTS" in take[:release], "a lost race changes nothing"
    fallback = take[start:]
    assert 'start_and_check(DIR "retro_agent.exe", 0)' in fallback
    assert "Sleep(25000)" in CODE


def test_it_does_not_wait_on_the_process_list():
    # agentswap9x's approach - the one that cannot beat restart.bat's relaunch
    assert "CreateToolhelp32Snapshot" not in CODE
    assert "Process32" not in CODE


def test_builds_crt_free_for_a_p54c_and_imports_only_kernel32_user32(tmp_path):
    cc = shutil.which("i686-w64-mingw32-gcc")
    objdump = shutil.which("i686-w64-mingw32-objdump")
    if not cc or not objdump:
        pytest.skip("SKIPPED: no mingw i686 toolchain - handoff9x was NOT built")
    exe = tmp_path / "HANDOFF9.EXE"
    subprocess.run([cc, "-O1", "-march=i586", "-mwindows", "-nostdlib", "-e", "_start@0",
                    "-Wall", "-Wextra", "-Werror", "-o", str(exe), str(TOOL),
                    "-lkernel32", "-luser32", "-s"], check=True)
    pe = subprocess.run([objdump, "-p", str(exe)], capture_output=True, text=True,
                        check=True).stdout
    dlls = {d.upper() for d in re.findall(r"DLL Name: (\S+)", pe)}
    assert dlls == {"KERNEL32.DLL", "USER32.DLL"}, dlls
    assert re.search(r"MajorSubsystemVersion\s+4\b", pe), "Win98 loads subsystem 4.x"
    dis = subprocess.run([objdump, "-d", str(exe)], capture_output=True, text=True,
                         check=True).stdout
    for p6 in ("cmov", "fcomi", "fucomi"):
        assert not re.search(r"\s%s" % p6, dis), p6 + " is P6-only; .243 is a P54C"
