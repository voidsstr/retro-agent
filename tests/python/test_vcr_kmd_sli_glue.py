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
    for later in ('VcrSliOff(x, "re-enable")', "make_io(x, &io)", "vcr_sli_set(&io, r)",
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
    body = func_body(src, "int vcr_sli_set(")
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
