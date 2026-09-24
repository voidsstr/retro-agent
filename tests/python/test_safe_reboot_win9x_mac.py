"""safe-reboot.py must be able to read a Windows 9x box's MAC.

It used to ask only `cmd /c ipconfig /all`. Windows 9x has no cmd.exe, so no
MAC ever came back from a Win98 box and the script refused to reboot every
one of them (found on .243, 2026-09-24, while installing a Voodoo 2). It now
asks HWPROFILE first - agent-internal, so it works on 9x and spawns no child
process on a single-threaded agent - and keeps ipconfig as the fallback.

Run: pytest tests/python/test_safe_reboot_win9x_mac.py
"""
import importlib.util
import json
from pathlib import Path

SRC = Path(__file__).resolve().parents[2] / "scripts" / "fleet" / "safe-reboot.py"
spec = importlib.util.spec_from_file_location("safe_reboot_9x", SRC)
sr = importlib.util.module_from_spec(spec)
spec.loader.exec_module(sr)

# The shape .243 (Win98 SE, agent 1.78.1) actually returned.
WIN98 = json.dumps({"network": {"interfaces": [
    {"description": "ELNK3 Ethernet Adapter", "mac": "00-A0-24-B9-E9-FB",
     "ipv4": ["192.168.1.243"]}], "source": "GetAdaptersInfo"}})


def test_win98_hwprofile_mac_is_read_and_normalised():
    assert sr.macs_from_hwprofile(WIN98) == ["00:a0:24:b9:e9:fb"]


def test_zero_mac_duplicates_and_junk_are_dropped():
    doc = json.dumps({"network": {"interfaces": [
        {"mac": "00-00-00-00-00-00"}, {"mac": "AA:BB:CC:DD:EE:FF"},
        {"mac": "aa-bb-cc-dd-ee-ff"}, {"mac": "not a mac"}, {}]}})
    assert sr.macs_from_hwprofile(doc) == ["aa:bb:cc:dd:ee:ff"]


def test_unparsable_profile_falls_back_rather_than_crashing():
    assert sr.macs_from_hwprofile("ERR unknown command") == []
    assert sr.macs_from_hwprofile("{}") == []


def test_hwprofile_is_asked_before_cmd():
    src = SRC.read_text()
    assert src.index("'HWPROFILE'") < src.index("cmd /c ipconfig")
