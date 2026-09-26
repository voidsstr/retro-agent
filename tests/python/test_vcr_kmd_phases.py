"""vcr-kmd flushed phase history: what survives a power cycle.

A wedge that needs a power cycle loses the in-RAM flight recorder. The flushed
registry phases survive - but the next boot's first phase used to overwrite
them, so the history of the boot that hung was lost too. The miniport now
copies PhaseLog/PhaseCount/LastPhase* to Prev* at DriverEntry, before writing
anything (verified in the VM test bed 2026-09-26: boot #10's 842 phases read
back from Prev* after the reboot), and the SLI/AA bring-up writes its
milestones as phases (.124 deep-wedged in an AA bring-up the same night).
"""
import struct
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
KMD = REPO / "voodoo-cleanroom" / "vcr-kmd"
sys.path.insert(0, str(KMD / "tools"))

import vcrphases  # noqa: E402


def test_previous_boot_is_kept_before_this_boot_writes_a_phase():
    src = (KMD / "miniport" / "vcrmp_log.c").read_text()
    create = src[src.index("void VcrLogCreate("):]
    create = create[:create.index("\n}\n")]
    assert "keep_previous_boot();" in create
    body = src[src.index("static void keep_previous_boot(void)"):]
    body = body[:body.index("\n}\n")]
    for a, b in (("PhaseLog", "PrevPhaseLog"), ("PhaseCount", "PrevPhaseCount"),
                 ("LastPhase", "PrevLastPhase"), ("LastPhaseA", "PrevLastPhaseA"),
                 ("LastPhaseMs", "PrevLastPhaseMs"), ("BootCount", "PrevBootCount")):
        assert f'copy_value(h, L"{a}", L"{b}");' in body
    assert "ZwFlushKey(h);" in body
    # DriverEntry creates the log before its first phase
    entry = (KMD / "miniport" / "vcrmp.c").read_text()
    assert entry.index("VcrLogCreate(") < entry.index("VcrPhase(")


def test_sli_milestones_are_phases():
    src = (KMD / "miniport" / "vcrmp_multi.c").read_text()
    log = src[src.index("static void k_log("):]
    log = log[:log.index("\n}\n")]
    assert "vcr_sli_step_persists(step)" in log and "VcrPhase(VCR_EV_SLI_STEP" in log
    keep = src[src.index("static int vcr_sli_step_persists("):]
    keep = keep[:keep.index("\n}\n")]
    for s in ("step % 100 == 0", "VCR_SLI_S_SET_DONE", "VCR_SLI_S_OFF_DONE",
              "VCR_SLI_S_CLOCK_6K", "step >= 900"):
        assert s in keep
    req = src[src.index("VP_STATUS VcrSliRequest("):]
    assert "VcrPhase(VCR_EV_HWC_SLIAA" in req[:req.index("\n}\n")]


def test_the_decoder_names_the_step_the_box_stopped_at():
    events = {code: name for code, (name, *_rest) in vcrphases.vcrlog.load_events().items()}
    sli = next(c for c, n in events.items() if n == "SLI_STEP")
    rows = [(10, 300, 0, 0), (20, sli, 400, 0), (30, sli, 401, (2 << 24) | 0x04)]
    blob = b"".join(struct.pack("<4I", *r) for r in rows) + bytes(16 * 61)
    out = vcrphases.decode({"PrevPhaseLog": blob.hex(), "PrevPhaseCount": 3,
                            "PrevLastPhase": sli, "PrevLastPhaseA": 401,
                            "PrevLastPhaseMs": 30, "PrevBootCount": 7}, prev=True)
    assert out[2].endswith("PCIINIT0 chip 2 reg 0x4")
    assert "SET_BEGIN" in out[1]
    assert out[-1].startswith("PrevLastPhase SLI_STEP") and "boot #7" in out[-1]
