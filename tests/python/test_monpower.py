"""The monitor never powers off; the screensaver runs (agent 1.96.0).

tests/native/test_monpower.c proves the decisions in agent/shared/monpower.h
(against the scheme blob read off .243). This pins how agent/src/monpower.c is
wired, the things a logic test cannot see:

* powrprof.dll is LOADED, never linked. Its Vista GUID functions do not exist
  on XP and nothing guarantees Win98 resolves it at EXE load; ONE static import
  the Win98 loader cannot resolve kills the whole agent before main() (ntdyn.h,
  test_agent_win9x_imports.py). So: no -lpowrprof, no direct call, and - when
  a build is present - no POWRPROF.dll in the import table.
* it runs from retrowall_apply_startup() ABOVE that function's early returns
  (both are the NORMAL path on a fleet box - the trap the theme and the
  screensaver were caught by once), right after the screensaver is set.
* the command and the startup pass ask the host policy first (also listed in
  test_hostpolicy.py), and every write is read back.
"""
import re
import shutil
import subprocess
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
SRC = REPO / "agent" / "src"
MP = (SRC / "monpower.c").read_text()
RW = (SRC / "retrowall.c").read_text()

POWRPROF_FUNCS = [
    "GetActivePwrScheme", "ReadPwrScheme", "WritePwrScheme", "SetActivePwrScheme",
    "GetCurrentPowerPolicies", "EnumPwrSchemes", "PowerGetActiveScheme",
    "PowerReadACValueIndex", "PowerReadDCValueIndex", "PowerWriteACValueIndex",
    "PowerWriteDCValueIndex", "PowerSetActiveScheme",
]


def code_only(text):
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def body(src, name):
    m = re.search(r"^[A-Za-z_][\w \*]*\b" + re.escape(name) + r"\s*\([^;]*?\)\s*\{",
                  src, re.M | re.S)
    assert m, f"{name} not found"
    i = src.index("{", m.start())
    depth = 0
    for j in range(i, len(src)):
        if src[j] == "{":
            depth += 1
        elif src[j] == "}":
            depth -= 1
            if depth == 0:
                return src[i:j + 1]
    raise AssertionError(name)


def test_powrprof_is_loaded_not_linked():
    code = code_only(MP)
    assert 'LoadLibraryA("powrprof.dll")' in code
    no_strings = re.sub(r'"(?:[^"\\]|\\.)*"', '""', code)
    for fn in POWRPROF_FUNCS:
        # any mention outside a string literal is a direct call
        direct = re.findall(r'(?<!\w)' + fn + r'\s*\(', no_strings)
        assert not direct, f"{fn} called directly - that is a static import"
        assert f'"{fn}"' in code, f"{fn} not resolved by name"
    makefile = (REPO / "agent" / "Makefile").read_text()
    assert "powrprof" not in makefile.lower()
    assert "monpower.c" in makefile


def test_built_agent_does_not_import_powrprof():
    exe = REPO / "agent" / "retro_agent.exe"
    objdump = shutil.which("i686-w64-mingw32-objdump") or shutil.which("objdump")
    if not exe.exists() or not objdump:
        pytest.skip("no built agent / objdump - the source checks above still ran")
    out = subprocess.run([objdump, "-p", str(exe)], capture_output=True, text=True).stdout
    dlls = {m.lower() for m in re.findall(r"DLL Name:\s*(\S+)", out)}
    assert "powrprof.dll" not in dlls, dlls


def test_called_from_retrowall_above_the_early_returns():
    b = code_only(body(RW, "retrowall_apply_startup"))
    i = b.find("monpower_apply_startup()")
    assert i >= 0, "retrowall_apply_startup no longer applies the monitor power settings"
    assert b.find("set_starfield_screensaver()") < i, "set the screensaver first"
    first_return = b.find("return;", b.find("host_policy_skip"))
    # the only return before the call must be the host-policy one
    assert b.count("return", 0, i) == 1 and first_return < i


def test_guarded_by_host_policy_and_the_off_switch():
    start = code_only(body(MP, "monpower_apply_startup"))
    assert "host_policy_skip" in start
    cmd = code_only(body(MP, "handle_monpower"))
    assert "host_manages_this_box" in cmd
    run = code_only(body(MP, "mp_run"))
    assert "mp_switch_on()" in run and "do_write = 0" in run


def test_every_write_is_read_back():
    old = code_only(body(MP, "mp_run_old"))
    # a scheme is written only after a successful read, and re-read afterwards
    assert old.index("rps(id, &pp)") < old.index("wps(&id") < old.index("rps(id, &back)")
    new = code_only(body(MP, "mp_run_new"))
    assert new.index("mp_needs_write") < new.index("wr(NULL")
    assert new.count("rd(NULL") >= 2
    spi = code_only(body(MP, "mp_spi_one"))
    assert spi.count("SystemParametersInfoA(get") == 2
