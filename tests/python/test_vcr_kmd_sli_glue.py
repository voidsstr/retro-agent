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
