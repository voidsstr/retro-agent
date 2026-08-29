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


def test_regwrite_uses_the_five_token_form(monkeypatch):
    """REGWRITE is <root> <path> <name> <type> <data> (agent/src/registry.c:284).

    Folding the value name into the path -- `...\\fxgpio\\Start 1 REG_DWORD` --
    makes the agent RegCreateKeyExA a subkey literally named `Start`, write a
    value named `1` into it, and answer OK while the real Start is untouched.
    Verified the hard way on .171 (2026-08-28): three OK writes, all three
    services still Start=2.
    """
    m = _mod()
    sent = []

    class RecordingConn(FakeConn):
        async def send_command(self, text, binary_payload=None):
            sent.append(text)
            return await super().send_command(text, binary_payload)

    async def drive():
        conn = RecordingConn([])
        for svc in m.SERVICES:
            await m.cmd(
                conn,
                rf"REGWRITE HKLM SYSTEM\CurrentControlSet\Services\{svc} Start REG_DWORD 1",
            )
    asyncio.run(drive())

    for line in sent:
        head, _, rest = line.partition("REGWRITE ")
        tokens = rest.split()
        assert len(tokens) == 5, f"REGWRITE needs 5 tokens, got {len(tokens)}: {line}"
        root, path, name, rtype, data = tokens
        assert root == "HKLM"
        assert not path.endswith("\\Start"), \
            "value name must be its own token, not glued onto the path"
        assert name == "Start"
        assert rtype == "REG_DWORD", "type comes BEFORE the data"
        assert data == "1"


def test_install_reads_back_instead_of_trusting_ok():
    """A misparsed REGWRITE answers OK, so the script must verify by reading."""
    src = open(MOD_PATH).read()
    fix = src.split("THE FIX")[1]
    assert "service_start_types(conn)" in fix, "must re-read the values after writing"
    assert "WARNING" in fix, "must warn when a service did not reach Start=1"


def _doc(name):
    root = os.path.abspath(os.path.join(HERE, "..", ".."))
    return open(os.path.join(root, name)).read()


def test_sli_doc_does_not_claim_ram_size_blocks_sli():
    """3dfx's own SLI check does NOT compare memory size.

    glide3x/cvg/init/sli.c:87-96 compares numberTmus, fbiBoardID and
    fbiVideoStruct; the fbiMemSize/tmuMemSize comparisons are commented out in
    the 1999 initial checkin, with a note that init normalises to the smaller
    board. So an 8MB + 12MB pair DOES SLI, as 2x8MB. An earlier revision of this
    README asserted the opposite and would have sent someone shopping for the
    wrong thing.
    """
    d = _doc("scripts/voodoo2/README.md")
    assert "fbiBoardID" in d, "the SLI section must name the field actually compared"
    assert "8 MB + 12 MB pair does SLI" in d or "does SLI" in d, \
        "the README must state that mismatched RAM still SLIs"
    assert "different manufacturers and/or different RAM size" not in d, \
        "the retracted claim must not reappear"


def test_subsystem_id_is_documented_as_useless():
    """SUBSYS_00000000 is universal to every Voodoo 2 — the chip has no
    subsystem registers. Inferring a brand from it is the expensive mistake."""
    d = _doc("scripts/voodoo2/README.md")
    assert "SUBSYS_00000000" in d
    assert "not determinable in software" in d or "identifies nothing" in d, \
        "the README must say the brand cannot be read in software"
