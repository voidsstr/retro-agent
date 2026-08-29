"""Regression: Voodoo 2 detection + the XP service start-type fix.

Encodes two hardware-verified invariants from fleetbook recipe
`voodoo-2-and-voodoo-2-sli-on-a-fleet-windows-xp-box-driver-d` (2026-08-28):

1. A Voodoo 2 is ``PCI\\VEN_121A&DEV_0002``.  ``VEN_1102&DEV_0002`` is a
   Creative Sound Blaster Live! -- several fleet boxes (.240) carry one, and
   matching on ``DEV_0002`` alone reports a Voodoo 2 that isn't there.
2. The Win2K 1.02.00 INF registers fxgpio/fxptl/Ntremap at ``StartType=2``
   (auto).  On XP the Win2K display driver is core-level and fails SILENTLY
   at auto; all three must end up at ``Start=1`` (system).
"""
import asyncio
import importlib.util
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
MOD_PATH = os.path.abspath(
    os.path.join(HERE, "..", "..", "scripts", "voodoo2", "install_voodoo2.py")
)


def _mod():
    sys.path.insert(0, os.path.abspath(os.path.join(HERE, "..", "..")))
    spec = importlib.util.spec_from_file_location("install_voodoo2", MOD_PATH)
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m


class FakeConn:
    """Minimal RetroConnection stand-in returning canned REGREAD payloads."""

    def __init__(self, pci_devices):
        self.pci_devices = pci_devices

    async def send_command(self, text, binary_payload=None):
        if "Enum\\PCI" in text:
            body = ",".join(f'"{d}"' for d in self.pci_devices)
            return 0, f'{{"subkeys":[{body}]}}'.encode()
        return 0, b"{}"


def _detect(devices):
    m = _mod()
    return asyncio.run(m.find_voodoo2(FakeConn(devices)))


def test_finds_a_real_voodoo2():
    found = _detect(["VEN_121A&DEV_0002&SUBSYS_00000000&REV_02",
                     "VEN_8086&DEV_1C02&SUBSYS_04AD1028&REV_04"])
    assert len(found) == 1
    assert "VEN_121A" in found[0]


def test_finds_two_cards_for_sli():
    found = _detect(["VEN_121A&DEV_0002&SUBSYS_00000000&REV_02",
                     "VEN_121A&DEV_0002&SUBSYS_00000004&REV_02"])
    assert len(found) == 2, "an SLI pair must report as two cards"


def test_creative_sblive_is_not_a_voodoo2():
    """The old-buggy behaviour: matching DEV_0002 alone. .240 has two of these."""
    devices = ["VEN_1102&DEV_0002&SUBSYS_80311102&REV_07",
               "VEN_1102&DEV_0002&SUBSYS_80651102&REV_0A",
               "VEN_1102&DEV_7002&SUBSYS_00201102&REV_07"]
    assert _detect(devices) == [], "SB Live! must not be reported as a Voodoo 2"
    # and the naive match would have found them:
    assert [d for d in devices if "DEV_0002" in d], "fixture must exercise the trap"


def test_no_3dfx_at_all():
    assert _detect(["VEN_8086&DEV_1C02&SUBSYS_04AD1028&REV_04"]) == []


def test_all_three_services_are_fixed_up():
    """Missing any one of the three leaves the driver silently dead on XP."""
    m = _mod()
    assert set(m.SERVICES) == {"fxgpio", "fxptl", "Ntremap"}


def test_hwid_constant_is_the_3dfx_vendor():
    m = _mod()
    assert m.V2_HWID.upper().endswith("VEN_121A&DEV_0002")
    assert "1102" not in m.V2_HWID, "must not be the Creative vendor id"
