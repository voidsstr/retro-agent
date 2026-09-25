"""Auto-update on Win9x (agent 1.85.0) - the swap batch and the chat kill.

Hardware-observed on .243 (Win98 SE, 2026-09-25): agent 1.84.2 downloaded
1.84.3, shut down "for the swap", and was back on 1.84.2 four seconds later,
with 1.84.3 marked as already attempted. The batch decided by COPY's
ERRORLEVEL, which COMMAND.COM does not set: a copy that failed on the
still-locked exe read as success, the download was deleted, and the OLD build
restarted. The chat client never updated on 9x either: Toolhelp returns a full
path there, so kill_retro_chat() never matched and CopyFile hit error 32.

These tests COMPILE the real build_restart_bat() from autoupdate.c and run its
Win9x output through a small COMMAND.COM model in which a running exe cannot
be renamed - the property the new batch relies on.
"""
import re
import shutil
import subprocess
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
SRC = REPO / "agent" / "src" / "autoupdate.c"


def _extract(src, signature):
    start = src.index(signature)
    depth, i = 0, src.index("{", start)
    while True:
        if src[i] == "{":
            depth += 1
        elif src[i] == "}":
            depth -= 1
            if depth == 0:
                return src[start:i + 1]
        i += 1


@pytest.fixture(scope="module")
def batch(tmp_path_factory):
    cc = shutil.which("gcc") or shutil.which("cc")
    if not cc:
        pytest.skip("no host C compiler - the Win9x swap batch is NOT exercised")
    s = SRC.read_text(errors="replace")
    tries = re.search(r"#define UPDATE_SWAP_TRIES (\d+)", s).group(1)
    fn = _extract(s, "static int build_restart_bat(")
    d = tmp_path_factory.mktemp("swap")
    (d / "t.c").write_text(
        "#include <stdio.h>\n#include <stdlib.h>\n"
        "static unsigned long g_ver;\nstatic unsigned long GetVersion(void) { return g_ver; }\n"
        "#define UPDATE_SWAP_TRIES %s\n%s\n"
        "int main(int c, char **v) { g_ver = strtoul(v[1], 0, 16);"
        " return !build_restart_bat(v[2], \"C:\\\\RETRO_AGENT\", \"C:\\\\RETRO_AGENT\\\\retro_update.tmp\"); }\n"
        % (tries, fn))
    subprocess.run([cc, "-o", str(d / "t"), str(d / "t.c")], check=True)

    def make(win9x):
        out = d / ("9x.bat" if win9x else "nt.bat")
        subprocess.run([str(d / "t"), "80000000" if win9x else "0", str(out)], check=True)
        return out.read_bytes().decode("ascii").split("\r\n")
    return make


def run_command_com(lines, locked_for):
    """Tiny COMMAND.COM: ping/echo/goto/labels, ren, del, if [not] exist, start.
    retro_agent.exe cannot be renamed while the old agent still runs, which it
    does for the first `locked_for` pings."""
    fs = {"C:\\RETRO_AGENT\\retro_agent.exe": "old",
          "C:\\RETRO_AGENT\\retro_update.tmp": "new"}
    pings, started, pc = 0, [], 0
    labels = {l[1:].strip(): i for i, l in enumerate(lines) if l.startswith(":")}
    running = lambda: pings < locked_for
    for _ in range(10000):
        if pc >= len(lines):
            break
        ln = lines[pc].strip()
        pc += 1
        low = ln.lower()
        if not ln or low.startswith(("@echo", "echo", ":")):
            continue
        m = re.match(r"if (not )?exist (\S+) (.*)", ln, re.I)
        if m:
            if (m.group(2) in fs) != bool(m.group(1)):
                ln, low = m.group(3), m.group(3).lower()
            else:
                continue
        if low.startswith("ping"):
            pings += 1
        elif low.startswith("goto "):
            pc = labels[ln[5:].strip()]
        elif low.startswith("ren "):
            src, newname = ln.split()[1:3]
            dst = src.rsplit("\\", 1)[0] + "\\" + newname
            if src in fs and dst not in fs and not (src.endswith("retro_agent.exe") and running()):
                fs[dst] = fs.pop(src)
        elif low.startswith("del "):
            fs.pop(ln.split()[1], None)
        elif low.startswith("start "):
            what = fs.get(ln.split()[1])
            assert not (what == "new" and running()), (
                "the NEW build was started while the old agent still held its exe")
            started.append(what)
        elif low.startswith("copy"):
            raise AssertionError("the 9x batch must not decide anything by COPY")
    return fs, started


def test_9x_batch_swaps_once_the_old_agent_has_exited(batch):
    fs, started = run_command_com(batch(True), locked_for=3)
    assert started == ["new"], started
    assert fs["C:\\RETRO_AGENT\\retro_agent_old.exe"] == "old", "rollback copy kept"
    assert "C:\\RETRO_AGENT\\retro_update.tmp" not in fs


def test_9x_batch_restarts_the_old_build_if_the_exe_never_frees(batch):
    fs, started = run_command_com(batch(True), locked_for=10 ** 6)
    # it must still start SOMETHING, or the box is lost until someone walks over
    assert started == ["old"], started


def test_9x_batch_does_not_trust_copy_errorlevel(batch):
    text = "\n".join(batch(True))
    assert "errorlevel" not in text.lower()
    assert "copy" not in text.lower()


def test_nt_batch_is_unchanged_copy_with_retries(batch):
    text = "\n".join(batch(False))
    assert "copy /Y C:\\RETRO_AGENT\\retro_update.tmp C:\\RETRO_AGENT\\retro_agent.exe" in text
    assert "if not errorlevel 1 goto swapped" in text


def test_chat_kill_matches_the_basename_win9x_reports_a_full_path():
    s = SRC.read_text(errors="replace")
    body = _extract(s, "static int kill_retro_chat(")
    assert "strrchr(pe.szExeFile, '\\\\')" in body
    assert '_stricmp(base, "retro_chat.exe")' in body
    assert '_stricmp(pe.szExeFile, "retro_chat.exe")' not in body
