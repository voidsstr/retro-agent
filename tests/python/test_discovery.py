"""
Regression tests for the LAN discovery packet parser (client/retro_discovery.py).

These lock in the wire format of the discovery protocol:
  RETRO|hostname|ip|port|os|cpu|ram_mb[|os_family][|ai=1]
  MACBUILD|hostname|ip|port|platform|arch|xcode|homebrew|status|active|done|failed

If the parsing/field-order/validation ever regresses, these fail. The dashboard
in nsc-assistant relies on this exact parser (duplicated there), so the wire
format is a contract — treat a failure here as "you changed the protocol".
"""
import os
import sys
import pytest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))
from client.retro_discovery import RetroPC, MacBuilder


# ---------------------------------------------------------------------------
# RetroPC.from_packet — the agent discovery beacon
# ---------------------------------------------------------------------------

def test_retro_minimal_valid_packet():
    pc = RetroPC.from_packet("RETRO|Q0Q1G8|10.0.0.50|9898|Win98 4.10|Pentium III|384")
    assert pc is not None
    assert pc.hostname == "Q0Q1G8"
    assert pc.ip == "10.0.0.50"
    assert pc.port == 9898
    assert pc.os == "Win98 4.10"
    assert pc.cpu == "Pentium III"
    assert pc.ram_mb == 384
    # os_family inferred when the (optional) 8th field is absent
    assert pc.os_family == "windows"
    assert pc.ai is False


def test_retro_explicit_os_family_field_wins():
    pc = RetroPC.from_packet("RETRO|H|10.0.0.51|9898|Win2K 5.0|CPU|512|linux")
    assert pc is not None
    # when parts[7] is present it is used verbatim (not inferred)
    assert pc.os_family == "linux"


def test_retro_ai_flag_detected_anywhere_after_field_8():
    pc = RetroPC.from_packet("RETRO|H|10.0.0.52|9898|Windows XP|CPU|2047|windows|ai=1")
    assert pc is not None
    assert pc.ai is True


def test_retro_ai_flag_absent_is_false():
    pc = RetroPC.from_packet("RETRO|H|10.0.0.52|9898|Windows XP|CPU|2047|windows|foo=1")
    assert pc is not None
    assert pc.ai is False


@pytest.mark.parametrize("packet", [
    "",                                              # empty
    "NOTRETRO|H|ip|9898|os|cpu|384",                 # wrong magic
    "RETRO|H|ip|9898|os|cpu",                        # too few fields (6 < 7)
    "RETRO|H|ip|notaport|os|cpu|384",                # non-int port
    "RETRO|H|ip|9898|os|cpu|notram",                 # non-int ram
])
def test_retro_invalid_packets_return_none(packet):
    assert RetroPC.from_packet(packet) is None


def test_retro_whitespace_is_stripped():
    pc = RetroPC.from_packet("  RETRO|H|10.0.0.1|9898|Win98|CPU|64\r\n")
    assert pc is not None
    assert pc.ram_mb == 64


def test_retro_to_dict_roundtrips_core_fields():
    pc = RetroPC.from_packet("RETRO|H|10.0.0.1|9898|Win98|CPU|64|windows|ai=1")
    d = pc.to_dict()
    assert d["hostname"] == "H" and d["port"] == 9898 and d["ram_mb"] == 64
    assert d["os_family"] == "windows" and d["ai"] is True


# ---------------------------------------------------------------------------
# _infer_os_family — legacy agents without the explicit field
# ---------------------------------------------------------------------------

@pytest.mark.parametrize("os_str,expected", [
    ("Win98 4.10", "windows"),
    ("Windows XP", "windows"),
    ("Win2K 5.0", "windows"),
    ("Linux 6.1", "linux"),
    ("Ubuntu 22.04", "linux"),
    ("Debian 12", "linux"),
    ("Mac OS 9.2", "mac_classic"),
    ("System 7.5", "mac_classic"),
    ("BeOS", "unknown"),
    ("", "unknown"),
])
def test_infer_os_family(os_str, expected):
    assert RetroPC._infer_os_family(os_str) == expected


# ---------------------------------------------------------------------------
# MacBuilder.from_packet — the macOS build-service beacon
# ---------------------------------------------------------------------------

def test_macbuilder_valid_packet():
    b = MacBuilder.from_packet(
        "MACBUILD|mini|10.0.0.9|9800|darwin|arm64|1|1|idle|0|42|3")
    assert b is not None
    assert b.hostname == "mini" and b.port == 9800
    assert b.arch == "arm64"
    assert b.xcode_clt is True and b.homebrew is True
    assert b.status == "idle"
    assert b.active_builds == 0 and b.builds_completed == 42 and b.builds_failed == 3


@pytest.mark.parametrize("packet", [
    "RETRO|H|ip|9898|os|cpu|384",                                   # wrong magic
    "MACBUILD|mini|ip|9800|darwin|arm64|1|1|idle|0|42",             # too few (11 < 12)
    "MACBUILD|mini|ip|notaport|darwin|arm64|1|1|idle|0|42|3",       # bad port
    "MACBUILD|mini|ip|9800|darwin|arm64|x|1|idle|0|42|3",           # bad bool
])
def test_macbuilder_invalid_returns_none(packet):
    assert MacBuilder.from_packet(packet) is None
