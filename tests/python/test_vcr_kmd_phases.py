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
              "VCR_SLI_S_CLOCK_6K", "VCR_SLI_S_NOMUX", "step >= 900",
              "VCR_SLI_S_AA_STATE", "VCR_SLI_S_AAONLY_SLICTRL"):
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
                     (115696, "SLI_STEP", 401, (3 << 24) | 0x04),
                     (115697, "SLI_POKE_REFUSED", (4 << 24) | (2 << 16) | 0x80, 0x1009),
                     (115698, "SLI_POKE_REFUSED", (5 << 24) | (1 << 16) | 0x94, 0x1cc00000),
                     (115699, "SLI_POKE_REFUSED", (3 << 24) | (1 << 16) | 0x40, 0))
    assert out[0].endswith("request {4,0,1,0,1}")
    assert out[1].endswith("REFUSED COMBO shape {4,0,1,0,1}")
    assert out[2].endswith("request {4,1,1,0,1}")
    assert out[3].endswith("REFUSED AA_OFF shape {4,1,1,0,1}")
    assert out[4].endswith("CLOCK_6K result -3 (NOT programmed)")
    assert out[5].endswith("CLOCK_6K result 0 (programmed)")
    assert out[6].endswith("NOMUX chip 2 sli 0 aa 1 analog 1 sampleHigh 0")
    assert out[7].endswith("REFUSED reason NLINES (value not recorded: kernel before 2026-09-27)")
    # a record from the 412b03c kernel (no reason byte) says so ...
    assert out[8].endswith("chip 2 cfg 0x80 <- 0x00001009 refused: "
                           "AllowPoke=0 (kernel before 2026-09-27)")
    assert out[9].endswith("PCIINIT0 chip 3 reg 0x4")       # every other step: as before
    # ... and a current one names the reason (vcr_sli.h VCR_POKE_R_*)
    assert out[10].endswith("chip 2 cfg 0x80 <- 0x00001009 refused: AA_OFF")
    assert out[11].endswith("chip 1 cfg 0x94 <- 0x1cc00000 refused: SLAVE")
    assert out[12].endswith("chip 1 cfg 0x40 <- 0x00000000 refused: SNOOP")


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
    # the poke-refusal phase: a = reason << 24 | chip << 16 | offset, b = value
    mp = body((KMD / "miniport" / "vcrmp.c").read_text(), "static VP_STATUS pci_op(")
    assert "((ULONG)why << 24) | (op->target << 16) | (op->offset & 0xffff)," in mp
    assert re.search(r"VcrPhase\(VCR_EV_SLI_POKE_REFUSED,\s*\n\s*\(\(ULONG\)why << 24\)", mp)
    reasons = vcrphases.poke_defines()
    assert reasons == {1: "BOUNDS", 2: "HEADER", 3: "SNOOP", 4: "AA_OFF", 5: "SLAVE"}


# ---- step B (2026-09-27): the vendor AA recipe and Diag\SliAAState ---------------

def test_the_decoder_names_the_recipe_the_read_back_and_a_memory_refusal():
    out = phase_rows((1000, "SLI_STEP", 400, (4 << 24) | 0x116),   # SET_BEGIN, vendor recipe
                     (1001, "SLI_STEP", 400, (4 << 24) | 0x016),   # SET_BEGIN, dos_mode.c
                     (1002, "SLI_STEP", 423, 0x00000000 | (2 << 24) | 0x20c),
                     (1003, "SLI_STEP", 422, (1 << 24)),            # AA_STATE reading chip 1
                     (1004, "SLI_STEP", 422, (4 << 24) | 1),        # AA_STATE done, 4 chips
                     (1005, "SLI_STEP", 901, 0x0b800000 | 0x1b7e))  # REFUSED MEMINFO
    assert out[0].endswith("(vendor AA recipe)")
    assert "vendor" not in out[1] and out[1].endswith("SET_BEGIN chip 4 reg 0x16")
    assert "AAONLY_SLICTRL chip 2" in out[2]
    assert out[3].endswith("AA_STATE reading chip 1 (config cycles)")
    assert out[4].endswith("AA_STATE read back (4 chips)")
    assert out[5].endswith("REFUSED MEMINFO tileMark 0x1b7e000 (vendor AA recipe)")


def state_blob(tuple_, flags, result, chips, boot=19, ms=116500):
    """A Diag\\SliAAState record laid out as vcr_sli.h's vcr_sli_aa_state."""
    hdr = struct.pack("<16I", vcrphases.STATE_MAGIC, vcrphases.STATE_BYTES, boot, ms, tuple_,
                      flags, result & 0xffffffff, len(chips), 8, 16, 0x01b7e000, 32 << 20,
                      0, 0x01000000, 0x01180000, 0)
    body = b"".join(struct.pack("<10I", *c) for c in chips)
    body += bytes(40 * (4 - len(chips)))
    return (hdr + body).hex()


# cfg 7 {4,0,1,1,1}, vendor recipe - tests/native/test_vcr_kmd_sli.c k_aa_tables
CFG7_VENDOR = [
    (0x06000b01, 0x45, 0x2811, 0, 0, 0, 0x20001b7e, 0x9db7e000, 0x800, 0x303),
    (0x4ba07b01, 0x0c011445, 0x02000803, 0xff000000, 0, 0, 0x20001b7e, 0x9db7e000, 0x82f, 0x303),
    (0x4ba07b01, 0x0c011445, 0x02002843, 0, 0xff00, 0, 0x20001b7e, 0x8db7e000, 0x827, 0x303),
    (0x4ba07b01, 0x0c011445, 0x02000803, 0xff000000, 0, 0, 0x20001b7e, 0x8db7e000, 0x82f, 0x303),
]


def test_the_state_record_decodes_register_by_register():
    out = vcrphases.decode_state(state_blob(0x40111, 0x1 | 0xf00, 2, CFG7_VENDOR))
    assert out[0] == ("SliAAState: boot #19 at 116.500s, request {4,0,1,1,1} nlines 8 bpp 16, "
                      "recipe vendor, result 0x2 (NOCLOCK)")
    assert "tileMark 0x01b7e000 total 0x02000000" in out[1] and "0x01000000-0x01180000" in out[1]
    assert "aaLfb" in out[2] and "pciInit0" in out[2]
    assert "9db7e000" in out[3] and "20001b7e" in out[3] and out[3].endswith("00000303 (written)")
    assert "8db7e000" in out[5]
    # the one register the recipe is about, spelled out: chips 0/1 read, 2/3 do not
    assert out[7] == "  chip 0 aaLfbCtrl: base 0x1b7e000 cpuWr dispWr READ_EN div4 16bpp"
    assert out[9] == "  chip 2 aaLfbCtrl: base 0x1b7e000 cpuWr dispWr div4 16bpp"
    assert len(out) == 3 + 4 + 4


def test_the_state_record_says_what_it_does_not_know():
    # dos_mode.c recipe, one chip, pciInit0 not recorded, a refused-looking result
    one = [(0x301, 0x45, 0x1009, 0, 0xff00, 0, 0x11801000, 0x0c000000, 0x800, 0)]
    out = vcrphases.decode_state(state_blob(0x10100, 0, 0, one))
    assert "recipe dos_mode" in out[0] and "result 0 (clean)" in out[0]
    assert out[3].endswith("not recorded")
    assert out[-1] == "  chip 0 aaLfbCtrl: base 0x0 cpuWr dispWr 16bpp"
    assert "result -4 (refused)" in vcrphases.decode_state(state_blob(0x40111, 0, -4, one))[0]
    # not a record at all: said, not guessed - a wrong magic, a short blob
    good = state_blob(0x40111, 0, 0, one)
    assert good[:8] == "53414131"                               # "SAA1"
    wrong = "00000000" + good[8:]
    assert vcrphases.decode_state(wrong)[0].startswith("SliAAState: not a vcr-kmd state record")
    assert vcrphases.decode_state(good[:200])[0].startswith("SliAAState: not a vcr-kmd")   # truncated
    assert vcrphases.decode_state("00" * 10)[0].startswith("SliAAState: 10 bytes")


def test_the_decoder_and_the_kernel_agree_on_the_state_layout():
    hdr = (KMD / "include" / "vcr_sli.h").read_text()
    m = re.search(r"#define VCR_SLI_STATE_MAGIC\s+(0x[0-9a-fA-F]+)u", hdr)
    assert m and int(m.group(1), 16) == vcrphases.STATE_MAGIC
    assert struct.pack("<I", vcrphases.STATE_MAGIC) == b"SAA1"
    m = re.search(r"#define VCR_SLI_STATE_BYTES\s+(\d+)", hdr)
    assert int(m.group(1)) == vcrphases.STATE_BYTES == 16 * 4 + 4 * 10 * 4
    m = re.search(r"#define VCR_SLI_F_VENDOR_AA\s+(0x[0-9a-fA-F]+)u", hdr)
    assert int(m.group(1), 16) == vcrphases.F_VENDOR_AA
    # the header fields, in order, as the struct declares them
    struct_src = hdr[hdr.index("typedef struct vcr_sli_aa_state {"):hdr.index("} vcr_sli_aa_state;")]
    head = struct_src[:struct_src.index("struct {")]
    fields = []
    for decl in re.findall(r"vcr_[iu]32 ([^;]+);", head):
        fields += [f.strip() for f in decl.split(",")]
    assert tuple(fields) == vcrphases.STATE_HDR
    # the per-chip registers: the kernel's read order
    sli = (KMD / "miniport" / "vcrmp_sli.c").read_text()
    table = re.search(r"k_state_cfg\[VCR_SLI_STATE_NCFG\] = \{(.*?)\};", sli, re.S).group(1)
    names = re.findall(r"VCR_CFG_\w+", table)
    regs = (KMD / "include" / "vcr_regs.h").read_text()
    offs = tuple(int(re.search(rf"#define {n}\s+(0x[0-9a-f]+)", regs).group(1), 16) for n in names)
    assert offs == vcrphases.STATE_CFG
    assert len(vcrphases.STATE_CFG_NAMES) == len(vcrphases.STATE_CFG) == 9
    # and the tool prints it after the phases
    tool = (KMD / "tools" / "vcrphases.py").read_text()
    main = tool[tool.index("def main("):]
    assert 'decode_state(values["SliAAState"])' in main
