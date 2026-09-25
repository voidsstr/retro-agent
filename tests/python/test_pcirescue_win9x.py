"""Win9x PCI rescue, and Win9x accelerators[] - both found on .243 (2026-09-24).

.243 (Win98 SE, Compaq-class 430HX) has a Voodoo 2 that its boot never sees:
the BIOS leaves it unconfigured (command 0000, BAR0 0) and Win98's boot-time
PCI enumeration creates no devnode, yet the card answers config cycles later
and CM_Reenumerate_DevNode on the PCI bus found it, installed its driver and
assigned BAR0 0x09000000. Without that, Glide - which scans config space
itself - enables the card at address 0, over RAM, and takes the machine down.

agent/src/pcirescue.c re-enumerates the PCI bus at startup when an INSTALLED
PCI device has no devnode. hwextra.c's accelerators[] read only the NT
registry path, so every Win9x box reported no 3dfx silicon.

Run: pytest tests/python/test_pcirescue_win9x.py
"""
import re
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
SRC = REPO / "agent" / "src"


def _read(name):
    return (SRC / name).read_text(errors="replace")


def test_cfgmgr32_is_loaded_dynamically_never_imported():
    s = _read("pcirescue.c")
    assert 'LoadLibraryA("cfgmgr32.dll")' in s
    assert 'GetProcAddress(h, "CM_Locate_DevNodeA")' in s
    assert 'GetProcAddress(h, "CM_Reenumerate_DevNode")' in s
    # a direct call would create a static import Win98 may not resolve
    assert not re.search(r"(?<![\w_])CM_Reenumerate_DevNode\s*\(", s)
    assert not re.search(r"(?<![\w_\"])CM_Locate_DevNodeA\s*\(", s)


def test_runs_only_on_win9x_and_only_for_installed_devices():
    s = _read("pcirescue.c")
    assert "VER_PLATFORM_WIN32_WINDOWS" in s
    assert '"Driver"' in s, "must only rescue devices that have a driver installed"
    assert "if (!r->n_missing_before && !force)" in s, "must not re-enumerate when nothing is missing"


def test_spawned_early_at_startup_through_spawn_helper():
    m = _read("main.c")
    assert 'spawn_helper(pcirescue_thread, "pcirescue")' in m
    assert m.index("spawn_helper(pcirescue_thread") < m.index("spawn_helper(automap_thread_proc"), \
        "the rescue must run before the other startup work, i.e. before anyone can start a game"


def test_pcirescan_command_is_registered_and_not_a_host_reconfigure():
    h = _read("handlers.c")
    row = re.search(r'\{\s*"PCIRESCAN",\s*1,\s*NULL,\s*handle_pcirescan,\s*(\d)\s*\}', h)
    assert row, "PCIRESCAN row missing from the command table"
    assert row.group(1) == "0"


def test_accelerators_read_the_win9x_enum_path_too():
    s = _read("hwextra.c")
    body = s[s.index("void hwextra_emit_accelerators"):]
    body = body[:body.index("\n}\n")]
    assert '"Enum\\\\PCI"' in body, "Win9x keeps PCI under HKLM\\Enum\\PCI"
    assert '"SYSTEM\\\\CurrentControlSet\\\\Enum\\\\PCI\\\\%s"' not in body, \
        "the per-device path must follow whichever root was opened"


def test_startup_result_is_persisted_not_only_logged():
    """1.84.3: the log cannot hold the boot on a 9x box running retro_chat.

    On .243 the chat long-polls rotated agent.log + agent.log.1 within two
    hours, so whether the rescue had run at boot - or Windows had found the
    Voodoo by itself - could not be answered after the fact. The startup pass
    now records its outcome in HKLM\\Software\\RetroAgent\\PciRescueBoot and
    PCIRESCAN reports it as last_boot.
    """
    s = _read("pcirescue.c")
    thread = s[s.index("DWORD WINAPI pcirescue_thread"):s.index("static void pcir_emit_list")]
    assert "pcir_store_boot(summary)" in thread, "the startup pass must record what it did"
    assert 'pcir_store_boot("disabled by PciRescue=0")' in thread, (
        "a disabled pass must say so, or an old RESCUED line reads as this boot's")
    assert '"PciRescueBoot"' in s
    handler = s[s.index("void handle_pcirescan"):]
    assert 'json_kv_str(&j, "last_boot", last_boot)' in handler
    # both outcomes are spelled out, so a failure cannot read as a success
    assert '"RESCUED" : "NOT rescued"' in s
