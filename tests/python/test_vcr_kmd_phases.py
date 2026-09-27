"""vcr-kmd flushed phase history: what survives a power cycle.

A wedge that needs a power cycle loses the in-RAM flight recorder. The flushed
registry phases survive - but the next boot's first phase used to overwrite
them, so the history of the boot that hung was lost too. The miniport now
copies PhaseLog/PhaseCount/LastPhase* to Prev* at DriverEntry, before writing
anything (verified in the VM test bed 2026-09-26: boot #10's 842 phases read
back from Prev* after the reboot), and the SLI/AA bring-up writes its
milestones as phases (.124 deep-wedged in an AA bring-up the same night).
"""
import re
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


def body(src, signature):
    i = src.index(signature)
    return src[i:src.index("\n}\n", i)]


def test_sli_milestones_are_phases():
    src = (KMD / "miniport" / "vcrmp_multi.c").read_text()
    log = body(src, "static void k_log(")
    # which steps (the pure predicate, native-tested) and what of them (b)
    assert "vcr_sli_step_persists(step, x ? x->sli_persist_all : 0)" in log
    assert "VcrPhase(VCR_EV_SLI_STEP, step, vcr_sli_phase_b(step, chip, reg, val)" in log
    keep = body((KMD / "miniport" / "vcrmp_sli.c").read_text(), "int vcr_sli_step_persists(")
    for s in ("persist_all ||", "step % 100 == 0", "VCR_SLI_S_SET_DONE", "VCR_SLI_S_OFF_DONE",
              "VCR_SLI_S_CLOCK_6K", "VCR_SLI_S_NOMUX", "step >= 900"):
        assert s in keep
    req = body(src, "VP_STATUS VcrSliRequest(")
    assert "VcrPhase(VCR_EV_HWC_SLIAA" in req
    # Diag\SliPersistAll: read before the boot-time slave placement logs a step,
    # and again at every request; absent = 0 (milestones only)
    init = body(src, "void VcrMultiInit(")
    assert init.index('x->sli_persist_all = VcrDiagGet(L"SliPersistAll", 0);') < init.index("make_io(")
    assert 'x->sli_persist_all = VcrDiagGet(L"SliPersistAll", 0);' in req


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


# ---- the value a step exists to report survives the power cycle (2026-09-27) ----
# Boot #18 (cfg 1, the third AA wedge) read back SET_DONE as b=0x04000000: chip 4,
# "reg 0" - the warn mask, the one thing that could say whether the kernel had
# found no video mux, was dropped (evidence/glidelab/postmortem_20260927).

def phase_rows(*rows):
    events = {name: code for code, (name, *_r) in vcrphases.vcrlog.load_events().items()}
    blob = b"".join(struct.pack("<4I", ms, events[name], a, b) for ms, name, a, b in rows)
    blob += bytes(16 * (64 - len(rows)))
    return vcrphases.decode({"PhaseLog": blob.hex(), "PhaseCount": len(rows),
                             "LastPhase": events[rows[-1][1]], "LastPhaseA": rows[-1][2],
                             "LastPhaseMs": rows[-1][0]}, prev=False)


def test_the_decoder_shows_the_warn_mask_and_says_when_an_old_kernel_dropped_it():
    out = phase_rows((116328, "SLI_STEP", 420, 0x04000000),     # boot #18 as recorded
                     (116400, "SLI_STEP", 420, 0x04800006),     # the same, from a new kernel
                     (116500, "SLI_STEP", 420, 0x04800000),
                     (116600, "SLI_STEP", 507, 0x04800001))
    assert out[0].endswith("SET_DONE chip 4 (value not recorded: kernel before 2026-09-27)")
    assert out[1].endswith("SET_DONE chips 4 warn 0x6 (NOCLOCK|NOMUX)")
    assert out[2].endswith("SET_DONE chips 4 warn 0 (clean)")
    assert out[3].endswith("OFF_DONE chips 4 warn 0x1 (TIMEOUT)")


def test_the_decoder_names_refusals_clock_results_and_the_request_shape():
    out = phase_rows((115687, "HWC_SLIAA", 4, 0x102),            # boot #18: cfg 1 as sent
                     (115688, "SLI_STEP", 901, 0x09840101),     # REFUSED COMBO {4,0,1,0,1}
                     (115689, "HWC_SLIAA", 4, 0x103),            # cfg 3
                     (115690, "SLI_STEP", 901, 0x0a841101),     # REFUSED AA_OFF
                     (115691, "SLI_STEP", 418, 0x00fffffd),     # CLOCK_6K -3
                     (115692, "SLI_STEP", 418, 0x00800000),
                     (115693, "SLI_STEP", 419, 0x02800006),     # NOMUX
                     (115694, "SLI_STEP", 901, 0x00000003),     # an old-kernel refusal
                     (115695, "SLI_POKE_REFUSED", (2 << 16) | 0x80, 0x1009),
                     (115696, "SLI_STEP", 401, (3 << 24) | 0x04))
    assert out[0].endswith("request {4,0,1,0,1}")
    assert out[1].endswith("REFUSED COMBO shape {4,0,1,0,1}")
    assert out[2].endswith("request {4,1,1,0,1}")
    assert out[3].endswith("REFUSED AA_OFF shape {4,1,1,0,1}")
    assert out[4].endswith("CLOCK_6K result -3 (NOT programmed)")
    assert out[5].endswith("CLOCK_6K result 0 (programmed)")
    assert out[6].endswith("NOMUX chip 2 sli 0 aa 1 analog 1 sampleHigh 0")
    assert out[7].endswith("REFUSED reason NLINES (value not recorded: kernel before 2026-09-27)")
    assert out[8].endswith("chip 2 cfg 0x80 <- 0x00001009 refused")
    assert out[9].endswith("PCIINIT0 chip 3 reg 0x4")       # every other step: as before


def test_the_decoder_and_the_kernel_agree_on_the_encoding():
    """vcrphases.py reads what vcr_sli_phase_b writes: the same flag bit, the
    same value steps, the same tuple packing."""
    hdr = (KMD / "include" / "vcr_sli.h").read_text()
    m = re.search(r"#define VCR_SLI_PB_VALUE\s+(0x[0-9a-fA-F]+)u", hdr)
    assert m and int(m.group(1), 16) == vcrphases.PB_VALUE
    pb = body((KMD / "miniport" / "vcrmp_sli.c").read_text(), "vcr_u32 vcr_sli_phase_b(")
    value_cases = set(re.findall(r"case VCR_SLI_S_(\w+):", pb))
    assert value_cases == set(vcrphases.VALUE_STEPS)
    tup = re.search(r"#define VCR_SLI_TUPLE\(n, sli, aa, high, analog\)(.*?)\n\n", hdr, re.S).group(1)
    for field, shift in (("n", 16), ("sli", 12), ("aa", 8), ("high", 4)):
        assert f"VCR_SLI_NIB({field}) << {shift}" in tup
    assert vcrphases.shape(0x40101) == "{4,0,1,0,1}"
    # the poke-refusal phase: a = chip << 16 | offset, b = value
    mp = body((KMD / "miniport" / "vcrmp.c").read_text(), "static VP_STATUS pci_op(")
    assert "VcrPhase(VCR_EV_SLI_POKE_REFUSED, (op->target << 16) | (op->offset & 0xffff)," in mp
