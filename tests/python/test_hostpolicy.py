"""The agent must not reconfigure a modern Windows box - and must be ABLE to tell.

WHY THIS TEST EXISTS
--------------------
The agent's job is to turn a machine into a retro fleet box: theme, wallpaper,
screensaver, icon layout, autologon, staged games, pinned resolutions. On a
Win9x/XP/7 fleet box that is the product. On somebody's Windows 11 PC that runs
the agent only so the fleet can reach it (the copier host .139) it is vandalism.

Both halves of this are silent when wrong, which is why they are pinned here
rather than left to review:

1. **The detection.** `GetVersionEx` reports 6.2 (Windows 8) on every 8.1/10/11
   machine unless the EXE carries a manifest naming the newer <supportedOS>
   GUIDs. retro_agent.exe has NO manifest - its only resource is the icon - so
   `dwMajorVersion >= 10` is FALSE on exactly the machines the policy protects.
   Code written that way reads as if it works, skins the box anyway, and the
   only evidence is somebody's wallpaper changing. So: assert the real check
   goes through RtlGetVersion.

2. **The coverage.** A guard that must be remembered in fifteen handlers will be
   missed in the sixteenth. The command table carries the policy as a FLAG and
   `handle_command` enforces it centrally; this test pins the flagged set so a
   newly-added host-reconfiguring command cannot quietly default to allowed.

The Win98 side of the same change (ntdll must not become a static import, or
the Win98SE box stops loading the EXE entirely) is asserted by
tests/python/test_agent_win9x_imports.py, and again on the binary below.
"""

import pathlib
import re

import pytest

REPO = pathlib.Path(__file__).resolve().parents[2]
SRC = REPO / "agent" / "src"
HOSTPOLICY_C = SRC / "hostpolicy.c"
HANDLERS_C = SRC / "handlers.c"
AGENT_EXE = REPO / "agent" / "retro_agent.exe"


def read(p):
    return p.read_text(encoding="utf-8", errors="replace")


# --------------------------------------------------------------------------
# 1. The detection must not rest on the shimmed API.
# --------------------------------------------------------------------------
def test_modern_check_uses_rtlgetversion():
    """RtlGetVersion is the only call that tells the truth without a manifest."""
    s = read(HOSTPOLICY_C)
    assert "RtlGetVersion" in s, (
        "hostpolicy.c must resolve RtlGetVersion; GetVersionEx reports 6.2 on "
        "Windows 10/11 for a manifest-less EXE and would never see 'modern'."
    )


def test_rtlgetversion_is_resolved_dynamically():
    """A static ntdll import would stop the EXE loading on Win98SE (see ntdyn.h)."""
    s = read(HOSTPOLICY_C)
    assert 'GetProcAddress' in s and 'LoadLibraryA("ntdll.dll")' in s, (
        "RtlGetVersion must come from LoadLibrary+GetProcAddress, never a direct call"
    )
    # A direct call would look like `RtlGetVersion(&x)` rather than a string or
    # a function-pointer invocation.
    assert not re.search(r'(?<![\w."])RtlGetVersion\s*\(', s), (
        "RtlGetVersion appears to be called directly - that creates an ntdll "
        "import and takes the Win98 box dark at EXE load"
    )


def test_agent_exe_does_not_import_ntdll():
    """The post-condition on the linked binary, not on the source."""
    if not AGENT_EXE.is_file():
        pytest.skip(f"{AGENT_EXE} not built")
    data = AGENT_EXE.read_bytes()
    assert b"ntdll.dll" not in data.lower() or True  # presence as a STRING is fine
    dlls = _pe_import_dlls(data)
    assert not any("ntdll" in d for d in dlls), (
        f"ntdll is in the import table ({sorted(dlls)}); Win98SE has no ntdll.dll "
        "and the loader will fail the whole process before main()"
    )


def test_win7_and_older_stay_managed():
    """The fleet HAS a Win7 box. Only 10+ is 'modern'."""
    s = read(REPO / "agent" / "src" / "hostpolicy.h")
    m = re.search(r"#define\s+HOSTPOLICY_MODERN_MAJOR\s+(\d+)", s)
    assert m, "HOSTPOLICY_MODERN_MAJOR must be defined"
    assert int(m.group(1)) == 10, (
        "Windows 7 is 6.1 and 8/8.1 are 6.2/6.3; a threshold below 10 would stop "
        "the agent managing retro fleet boxes it is supposed to manage"
    )


# --------------------------------------------------------------------------
# 2. Coverage: the automatic work and the commanded work.
# --------------------------------------------------------------------------
@pytest.mark.parametrize(
    "path,func,what",
    [
        ("retrowall.c", "retrowall_apply_startup", "theme/wallpaper/screensaver/icons"),
        ("retrowall.c", "retrowall_thread", "the wallpaper keep-loop"),
        ("sysfix.c", "sysfix_apply_startup", "autologon and the 9x fixes"),
        # Missing from this list until 1.83.1, which is exactly how it got
        # through: the startup provisioning thread copied 2.6 GB of the 54 GB
        # retro library onto WHITEBEAST (Win11) before it was stopped.
        ("gamesync.c", "gamesync_thread", "startup game provisioning + desktop shortcuts"),
        ("gamesync.c", "gs_start", "any other start of a library sync"),
    ],
)
def test_startup_appliers_are_guarded(path, func, what):
    """Each automatic host-changing entry point asks the policy first."""
    s = read(SRC / path)
    body = _function_body(s, func)
    assert body is not None, f"{func} not found in {path}"
    assert "host_policy_skip" in body or "host_manages_this_box" in body, (
        f"{func} ({what}) runs without consulting hostpolicy - it would apply "
        f"on a Windows 11 box"
    )


# Commands that RECONFIGURE THE HOST as a fleet box. Deliberately NOT the
# general remote-control primitives (EXEC, UPLOAD, REGWRITE, NETMAP, SERVICE,
# REBOOT...): those are the operator's own hands on their own machine, and
# blocking them would make the agent useless on the box it exists to reach.
EXPECTED_RECONFIGURING = {
    "GAMESYNC",     # stages games, merges install.reg
    "GAMERES",      # rewrites game configs + display registry keys
    "ICONARRANGE",  # rearranges the desktop
    "AUTOLOGIN",    # configures automatic logon
    "DRVUPDATE",    # installs drivers
    "DOSSTAGE",     # stages DOS programs onto C:\
    "WPALOAD",      # restores Windows activation state
}


def _flagged_commands():
    s = read(HANDLERS_C)
    start = s.index("static const cmd_entry_t commands[] = {")
    end = s.index("};", start)
    found = set()
    for line in s[start:end].split("\n"):
        m = re.match(r'\s*\{\s*"([A-Z_]+)"\s*,.*,\s*(\d)\s*\}\s*,?\s*$', line)
        if m and m.group(2) == "1":
            found.add(m.group(1))
    return found


def test_reconfiguring_commands_are_flagged():
    """The flagged set is exactly what we decided - no more, no less.

    Both directions matter. A MISSING flag means that command still skins a
    modern box. An EXTRA one means a command the operator needs was quietly
    taken away from them.
    """
    assert _flagged_commands() == EXPECTED_RECONFIGURING


def test_dispatch_enforces_the_flag():
    """The flag is only documentation unless handle_command acts on it."""
    body = _function_body(read(HANDLERS_C), "handle_command")
    assert body is not None
    assert "reconfigures_host" in body and "host_manages_this_box" in body, (
        "handle_command does not check the flag; every flagged command would run"
    )


@pytest.mark.parametrize(
    "path,func,readonly_mode",
    [("sysfix.c", "handle_sysfix", "check"), ("display.c", "handle_displaycfg", "get")],
)
def test_dual_mode_commands_guard_only_their_write_branch(path, func, readonly_mode):
    """SYSFIX check and DISPLAYCFG get are diagnostics and must keep working."""
    body = _function_body(read(SRC / path), func)
    assert body is not None, f"{func} not found"
    assert "host_manages_this_box" in body, f"{func} does not consult hostpolicy"
    assert readonly_mode in body, (
        f"{func} no longer has its read-only '{readonly_mode}' mode - the guard "
        f"was supposed to spare it"
    )


# --------------------------------------------------------------------------
# helpers
# --------------------------------------------------------------------------
def _function_body(src, name):
    """Text of `name`'s body, by brace matching from its definition."""
    m = re.search(r"^[A-Za-z_][\w \*]*\b" + re.escape(name) + r"\s*\([^;]*?\)\s*\{",
                  src, re.M | re.S)
    if not m:
        return None
    i = src.index("{", m.start())
    depth = 0
    for j in range(i, len(src)):
        if src[j] == "{":
            depth += 1
        elif src[j] == "}":
            depth -= 1
            if depth == 0:
                return src[i:j + 1]
    return None


def _pe_import_dlls(data):
    import struct

    e = struct.unpack_from("<I", data, 0x3C)[0]
    coff = e + 4
    n_sec = struct.unpack_from("<H", data, coff + 2)[0]
    opt_size = struct.unpack_from("<H", data, coff + 16)[0]
    opt = coff + 20
    imp_rva = struct.unpack_from("<II", data, opt + 96 + 8)[0]
    if not imp_rva:
        return set()
    secs = []
    sec = opt + opt_size
    for i in range(n_sec):
        o = sec + i * 40
        va, raw, ptr = struct.unpack_from("<III", data, o + 12)
        vs = struct.unpack_from("<I", data, o + 8)[0]
        secs.append((va, max(vs, raw), ptr))

    def off(rva):
        for va, sz, ptr in secs:
            if va <= rva < va + sz:
                return ptr + (rva - va)
        return None

    out = set()
    i = off(imp_rva)
    while True:
        ent = data[i:i + 20]
        if len(ent) < 20 or ent == b"\0" * 20:
            break
        name_rva = struct.unpack_from("<I", ent, 12)[0]
        if name_rva:
            o = off(name_rva)
            out.add(data[o:data.index(b"\0", o)].decode("ascii", "replace").lower())
        i += 20
    return out


def test_gamesync_thread_asks_the_policy_before_doing_anything():
    """The check must come FIRST - before the startup delay, the desktop
    shortcut placement, the first-boot driver work and the marker test - or a
    modern host still gets some of it."""
    body = _function_body(read(SRC / "gamesync.c"), "gamesync_thread")
    assert body is not None
    gate = body.index("host_manages_this_box")
    for later in ("Sleep(GS_FIRST_DELAY_MS)", "gs_place_tool_shortcuts()",
                  "gs_install_missing_drivers()", "gs_file_exists(GS_MARKER)"):
        assert later in body, later
        assert gate < body.index(later), f"policy check comes after {later}"


def test_gs_start_asks_the_policy_before_starting_a_worker():
    body = _function_body(read(SRC / "gamesync.c"), "gs_start")
    assert body is not None
    assert body.index("host_manages_this_box") < body.index("CreateThread(")
