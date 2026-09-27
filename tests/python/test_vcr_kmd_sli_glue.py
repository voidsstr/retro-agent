"""vcr-kmd multi-chip glue: the kernel/display wiring around the SLI port.

The SLI sequence (miniport/vcrmp_sli.c) and the clock (common/vcr_ics307.c)
are tested as pure code in tests/native/. What those tests cannot see is the
thin kernel layer that feeds them, and each assertion here is a way that layer
really failed, or would fail silently, on .124:

- METHOD_BUFFERED: the IOCTL's input and output are ONE system buffer. The
  first 4-chip run zeroed the answer before reading the request, so Glide's
  enable arrived as dwChips = sliEn = aaEn = 0 - a disable - and the game ran
  on the master alone with nothing anywhere reporting a failure (2026-09-26).
- the clock hook is a stub unless the miniport is built with
  VCR_SLI_HAVE_6K_CLOCK: without it the 6000 runs SLI on an unprogrammed clock
  and the only trace is a W_NOCLOCK bit in a log line.
- a Glide client whose context was lost skips its own SLI teardown; the next
  mode set is the only place left to turn SLI off.
"""
import re
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
KMD = REPO / "voodoo-cleanroom" / "vcr-kmd"


def func_body(text, signature):
    i = text.index(signature)
    j = text.index("\n}\n", i)
    return text[i:j]


def test_the_sli_request_is_read_before_the_answer_is_written():
    body = func_body((KMD / "miniport" / "vcrmp_multi.c").read_text(),
                     "VP_STATUS VcrSliRequest(")
    take = body.index("VideoPortMoveMemory(&rq")
    zero = body.index("VideoPortZeroMemory(out")
    assert take < zero, "the request must be copied out of the shared buffer first"
    # and nothing below reads the caller's buffer again
    assert "req)->" not in body[zero:] and "(const vcr_sli_aa_req *)req" not in body


def test_every_private_ioctl_takes_its_input_before_answering():
    """The same shape anywhere in StartIO: a handler that both reads the input
    and writes the output must copy the input to a local first."""
    text = (KMD / "miniport" / "vcrmp.c").read_text()
    for case in re.findall(r"case (IOCTL_VCR_\w+):(.*?)break;", text, re.S):
        name, body = case
        if "InputBuffer" in body and "OutputBuffer" in body:
            first_in = body.index("InputBuffer")
            first_out = body.index("OutputBuffer")
            copies = re.search(r"=\s*\*\s*\(\w+\s*\*\)\s*rp->InputBuffer", body)
            passes_both = re.search(r"\(x,\s*rp->InputBuffer,.*rp->OutputBuffer", body, re.S)
            assert copies or passes_both or first_in < first_out, name


def test_the_miniport_links_the_real_6000_clock():
    mk = (KMD / "Makefile").read_text()
    rule = mk[mk.index("$(OUT)/vcrmp.sys:"):]
    rule = rule[:rule.index("\n\n")]
    assert "-DVCR_SLI_HAVE_6K_CLOCK" in rule
    src = re.search(r"MP_SRC\s*:=(.*?)\n(?!\s)", mk, re.S).group(1)
    for f in ("vcrmp_sli.c", "vcrmp_multi.c", "vcrmp_clock.c", "vcr_ics307.c"):
        assert f in src, f
    multi = (KMD / "miniport" / "vcrmp_multi.c").read_text()
    assert "int vcr_sli_6k_clock(const vcr_sli_io *io)" in multi
    assert "VcrClock6k(" in multi


def test_a_mode_set_turns_sli_off_before_programming():
    body = func_body((KMD / "miniport" / "vcrmp_hw.c").read_text(),
                     "VP_STATUS VcrHwSetMode(")
    assert body.index("VcrSliOff(") < body.index("voodoo_program(")


def test_glide_hears_about_the_chips_the_miniport_mapped():
    esc = (KMD / "display" / "vcrdd_escape.c").read_text()
    assert "numChips = info.glide_chips" in esc
    assert "numChips = 1;" not in esc
    sli = esc[esc.index("case VCR_HWC_SLI_AA_REQUEST"):]
    sli = sli[:sli.index("break;")]
    assert "IOCTL_VCR_SLI" in sli
    mp = (KMD / "miniport" / "vcrmp.c").read_text()
    assert mp.index("VcrHwDiscover(x)") < mp.index("VcrMultiInit(x)")
    assert "m->nchips = x->glide_chips" in (KMD / "miniport" / "vcrmp_map.c").read_text()


def test_slaves_are_only_placed_inside_the_masters_own_windows():
    body = func_body((KMD / "miniport" / "vcrmp_multi.c").read_text(),
                     "void VcrMultiInit(")
    assert "x->mmio_len < n * MB32" in body
    assert "outside the master's window" in body
    assert 'VcrDiagGet(L"Sli", 1)' in body


# ---- the AA safety net (2026-09-27) --------------------------------------------
# Every AA configuration tried on .124 froze the whole PC - cfg 3 in an LFB
# read, cfg 7 and cfg 1 inside Glide's open - and a frozen box needs a person
# at the power switch. The pure decisions are native-tested
# (tests/native/test_vcr_kmd_sli.c); these pin WHERE the kernel asks them.

def test_the_aa_kill_switch_is_asked_before_anything_can_write():
    body = func_body((KMD / "miniport" / "vcrmp_multi.c").read_text(),
                     "VP_STATUS VcrSliRequest(")
    policy = body.index("vcr_sli_policy(r, sli_aa_allowed())")
    # before the live session is torn down to make room, before the accessor
    # table exists, before the sequence runs - and before the disable branch
    for later in ('VcrSliOff(x, "re-enable")', "make_io(x, &io)", "vcr_sli_set_ex(&io, r, recipe)",
                  'VcrSliOff(x, "Glide asked")'):
        assert policy < body.index(later), later
    # a refusal is a PERSISTED phase: through k_log as a 9xx step, with the
    # reason and the request's shape
    refusal = body[policy:body.index("} else if (!en)")]
    assert "k_log(x, VCR_SLI_S_REFUSED, 0, (vcr_u32)why, vcr_sli_req_tuple(r)," in refusal
    assert "VCR_SLI_EDENIED" in refusal and "VCR_SLI_EINVAL" in refusal
    # only an enable is judged; a disable always goes through
    assert "why = en ? vcr_sli_policy(" in body


def test_diag_sliaa_defaults_to_off_and_is_read_per_request():
    src = (KMD / "miniport" / "vcrmp_multi.c").read_text()
    allowed = func_body(src, "static int sli_aa_allowed(")
    assert 'VcrDiagGet(L"SliAA", 0)' in allowed          # absent = 0 = AA refused
    assert "sli_aa_allowed()" in func_body(src, "VP_STATUS VcrSliRequest(")
    # never cached at FindAdapter: a supervised run arms it for one session
    assert 'L"SliAA"' not in func_body(src, "void VcrMultiInit(")
    assert 'L"SliAA"' not in (KMD / "miniport" / "vcrmp.c").read_text()


def test_the_sequence_refuses_a_shape_with_no_mux_before_its_first_write():
    src = (KMD / "miniport" / "vcrmp_sli.c").read_text()
    # vcr_sli_set is vcr_sli_set_ex(io, r, 0) since step B; the checks live there
    assert "return vcr_sli_set_ex(io, r, 0);" in func_body(src, "int vcr_sli_set(")
    body = func_body(src, "int vcr_sli_set_ex(")
    combo = body.index("if (!vcr_sli_combo_ok(p.n, p.sli, p.aa, p.high, p.analog))")
    assert body.index("return refuse(io, VCR_SLI_R_COMBO,", combo) < body.index("sli_enable(io, &p)")
    assert combo < body.index("VCR_SLI_S_SET_MEMINFO")


def test_glide_pci_op_writes_to_sli_aa_registers_need_allow_poke():
    body = func_body((KMD / "miniport" / "vcrmp.c").read_text(), "static VP_STATUS pci_op(")
    chip = body[body.index("slot = op->target ?"):]
    guard = chip.index("if (!x->allow_poke && vcr_sli_cfg_owned(op->offset, size))")
    write = chip.index("VcrPciWrite(x, slot, op->offset, op->value, size)")
    assert guard < write
    refused = chip[guard:write]
    assert "return ERROR_ACCESS_DENIED;" in refused
    assert "VLOG(VCR_LV_WARN, VCR_EV_SLI_POKE_REFUSED" in refused      # every one, in the ring
    assert "vcr_sli_poke_first(&x->poke_memo," in refused              # each new one, flushed
    assert "VcrPhase(VCR_EV_SLI_POKE_REFUSED" in refused
    # reads are never refused
    assert "op->value = VcrPciRead(x, slot, op->offset, size);" in chip


def test_the_escape_hands_glide_a_failure_for_every_refusal():
    esc = (KMD / "display" / "vcrdd_escape.c").read_text()
    sli = esc[esc.index("case VCR_HWC_SLI_AA_REQUEST"):]
    sli = sli[:sli.index("break;")]
    # < 0 (EINVAL for no mux, EDENIED for the kill switch) -> VCR_HWC_FAIL
    assert "rs->resStatus = (!rc && (LONG)sr.result >= 0) ? VCR_HWC_OK : VCR_HWC_FAIL;" in sli
    pci = esc[esc.index("case VCR_HWC_PCI_OP"):]
    pci = pci[:pci.index("break;")]
    assert "rs->resStatus = rc ? VCR_HWC_FAIL : VCR_HWC_OK;" in pci
    hdr = (KMD / "include" / "vcr_sli.h").read_text()
    assert re.search(r"#define VCR_SLI_EDENIED\s+\(-4\)", hdr)
    assert re.search(r"#define VCR_SLI_R_COMBO\s+9\b", hdr)
    assert re.search(r"#define VCR_SLI_R_AA_OFF\s+10\b", hdr)


# ---- step B (2026-09-27): the vendor-style AA recipe, Diag\SliAAState, vcrctl sliaa --
# The register values are native-tested (tests/native/test_vcr_kmd_sli.c: the
# expected cfg 3/7/1 tables for both recipes, the read-back by config cycles
# only, the CLI gate). These pin where the kernel and the tool use them.

def test_the_vendor_recipe_is_off_by_default_and_read_per_request():
    src = (KMD / "miniport" / "vcrmp_multi.c").read_text()
    recipe = func_body(src, "static vcr_u32 sli_recipe(")
    assert 'VcrDiagGet(L"SliAAVendorRecipe", 0)' in recipe      # absent = 0 = dos_mode.c
    assert "VCR_SLI_F_VENDOR_AA : 0" in recipe
    req = func_body(src, "VP_STATUS VcrSliRequest(")
    # read in the enable branch, after the policy - never cached at FindAdapter
    assert req.index("vcr_sli_policy(r, sli_aa_allowed())") < req.index("sli_recipe()")
    assert req.index("sli_recipe()") < req.index("vcr_sli_set_ex(&io, r, recipe)")
    assert "SliAAVendorRecipe" not in func_body(src, "void VcrMultiInit(")
    assert "SliAAVendorRecipe" not in (KMD / "miniport" / "vcrmp.c").read_text()
    # the only caller of the sequence with a flag; the disable keeps the default
    assert "return vcr_sli_set(&io, &r);" in func_body(src, "static int sli_disable(")


def test_the_aa_state_is_recorded_after_the_enable_by_config_cycles_only():
    multi = (KMD / "miniport" / "vcrmp_multi.c").read_text()
    req = func_body(multi, "VP_STATUS VcrSliRequest(")
    # only THIS enable's pciInit0 writes count, then the sequence, then the record
    reset = req.index("x->sli_pci0_mask = 0;")
    run = req.index("vcr_sli_set_ex(&io, r, recipe)")
    gate = req.index("if (vcr_sli_aa_state_wanted(r, rc))")
    assert reset < run < gate < req.index("sli_aa_state(x, &io, r, rc, recipe);")
    st = func_body(multi, "static void sli_aa_state(")
    assert "vcr_sli_aa_readback(io, r, rc, recipe, x->sli_pci0, x->sli_pci0_mask, &st)" in st
    assert 'VcrDiagSetBinary(L"SliAAState", &st, sizeof st, TRUE);' in st     # flushed
    assert 'st.boot = VcrDiagGet(L"BootCount", 0);' in st
    # pciInit0 has no config alias: k_log takes the value the sequence writes
    log = func_body(multi, "static void k_log(")
    assert "step == VCR_SLI_S_PCIINIT0" in log and "x->sli_pci0[chip] = val;" in log
    # the read-back itself: config reads and log lines, nothing else
    sli = (KMD / "miniport" / "vcrmp_sli.c").read_text()
    rb = func_body(sli, "int vcr_sli_aa_readback(")
    for banned in ("reg_r(", "reg_w(", "vga_r(", "vga_w(", "vga_iw(", "cfg_w(", "write_3d(",
                   "io->io_rd", "io->io_wr", "io->vga_rd", "io->vga_wr", "io->cfg_wr"):
        assert banned not in rb, banned
    assert "cfg_r(io, c, k_state_cfg[i])" in rb
    # the binary writer flushes like a phase
    lg = func_body((KMD / "miniport" / "vcrmp_log.c").read_text(), "void VcrDiagSetBinary(")
    assert "3 /* REG_BINARY */" in lg and "ZwFlushKey(h)" in lg


def test_the_state_record_layout_is_the_one_the_decoder_reads():
    hdr = (KMD / "include" / "vcr_sli.h").read_text()
    assert re.search(r"#define VCR_SLI_STATE_BYTES\s+224\b", hdr)
    assert "VCR_STATIC_ASSERT(sli_aa_state_size, sizeof(vcr_sli_aa_state) == VCR_SLI_STATE_BYTES);" in hdr
    sli = (KMD / "miniport" / "vcrmp_sli.c").read_text()
    table = re.search(r"k_state_cfg\[VCR_SLI_STATE_NCFG\] = \{(.*?)\};", sli, re.S).group(1)
    names = re.findall(r"VCR_CFG_\w+", table)
    regs = (KMD / "include" / "vcr_regs.h").read_text()
    offs = [int(re.search(rf"#define {n}\s+(0x[0-9a-f]+)", regs).group(1), 16) for n in names]
    assert offs == [0x40, 0x48, 0x80, 0x84, 0x88, 0x8c, 0x90, 0x94, 0xac]


def test_vcrctl_sliaa_gates_before_it_sends_and_sends_like_glide():
    tool = (KMD / "tools" / "vcrctl.c").read_text()
    body = func_body(tool, "static int cmd_sliaa(")
    parse = body.index("vcr_sliaa_parse(argc, argv, &c)")
    gate = body.index("why = vcr_sliaa_gate(&c);")
    refuse = body.index('return fail("sliaa", why);')
    first_escape = min(body.index("hwc("), body.index("esc("))
    assert parse < gate < refuse < first_escape, "the at-the-box gate must run before any escape"
    # Glide's route: GETDEVICECONFIG, then the request through the HWCEXT probe
    assert body.index("hwc(VCR_HWC_GETDEVICECONFIG") < body.index("hwc(VCR_HWC_SLI_AA_REQUEST")
    assert "vcr_sliaa_fill(&c, chips, fb, r);" in body
    assert "vcr_sli_aa_req *r = &rq.opt.sliAA;" in body
    # the kernel's own answer, from our driver's info
    assert "esc(VCR_ESC_INFO" in body and "sli_result" in body
    main = func_body(tool, "int main(")
    assert 'rc = cmd_sliaa(argc - 2, argv + 2);' in main
    assert "--i-am-at-the-box" in tool[:tool.index("#define WIN32_LEAN_AND_MEAN")]   # usage says so
    hdr = (KMD / "tools" / "vcr_sliaa.h").read_text()
    assert '#define VCR_SLIAA_AT_BOX_FLAG   "--i-am-at-the-box"' in hdr
    # the gate keys on AA, not on the request's other fields
    gate_fn = func_body(hdr, "static const char *vcr_sliaa_gate(")
    assert "c->aa && !c->at_box" in gate_fn
    # an edit to the header alone rebuilds the tool
    mk = (KMD / "Makefile").read_text()
    rule = mk[mk.index("$(OUT)/vcrctl.exe:"):]
    assert "tools/vcr_sliaa.h" in rule[:rule.index("\n")]


def test_the_escape_still_carries_a_refusal_back_as_fail():
    """vcrctl sliaa and Glide share the escape: a policy refusal (EDENIED),
    a missing mux (EINVAL) or a vendor-recipe MEMINFO refusal (EINVAL) all
    reach the caller as VCR_HWC_FAIL."""
    esc = (KMD / "display" / "vcrdd_escape.c").read_text()
    sli = esc[esc.index("case VCR_HWC_SLI_AA_REQUEST"):]
    sli = sli[:sli.index("break;")]
    assert "rs->resStatus = (!rc && (LONG)sr.result >= 0) ? VCR_HWC_OK : VCR_HWC_FAIL;" in sli
    hdr = (KMD / "include" / "vcr_sli.h").read_text()
    assert re.search(r"#define VCR_SLI_R_MEMINFO\s+11\b", hdr)
    body = func_body((KMD / "miniport" / "vcrmp_sli.c").read_text(), "static int refuse(")
    assert "VCR_SLI_R_AA_OFF ? VCR_SLI_EDENIED : VCR_SLI_EINVAL" in body
