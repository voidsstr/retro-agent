"""Our h5 Glide's SLI/AA guards (fork voidsstr/retro3dfx-glide, SLIAA-GUARD, 2026-09-27).

The V5 6000 in .124 froze hard in three AA configurations after the kernel's
SLI/AA setup: cfg 1 and cfg 7 inside grSstWinOpen, cfg 3 in grLfbReadRegion.
cfg 1 is a malformed request - Glide laid 2-sample AA out for four chips, only
then applied "single chip", and still named all four chips: {4, no SLI,
2-sample, analog}, a combination no kernel has a video-mux branch for. These
guards only REFUSE or NARROW; none adds a hardware access:

  (a) cfg 1 (single chip with AA) on a board of more than two chips fails
      grSstWinOpen before any buffer, mode set or escape; single-chip AA's own
      PCI_OP loop writes only the chips Glide renders on;
  (b) a request no kernel can program fails the open at the top of
      hwcInitVideo (before the DirectDraw mode set) and again on the very bytes
      just before the escape - hwcSliAaTupleSupported() in minihwc/h5sliaa.h,
      pinned to vcr-kmd's video_mux() by tests/native/test_h5_sliaa_tuple.c;
  (c) the escape's own answer (ExtEscape, resStatus) is honoured - and a
      refusal after HWCSETEXCLUSIVE gives the display back (HWCRLSEXCLUSIVE +
      hwcResetVideo, through the helper hwcRestoreVideo's release uses), so
      the kernel is not left with an exclusive owner until the process exits;
  (d) a READ lock in a multi-chip AA mode is refused before the SLI read
      toggle and the first FIFO write, unless RETRO_GLIDE_AA_LFB_READ=1;
  (e) the idle wait never resets the master of a multi-chip SLI/AA board (the
      slaves snoop its init registers), and says which branch it took first;
      a close that finds such a board busy (cfg 5 included) skips the SLI/AA
      disable escape and SAYS so - vcr-kmd's release turns SLI/AA off itself.

The pure decisions are compiled and run by the native test; this file pins
WHERE the Glide sources call them. Reads the fork clone under
voodoo-cleanroom/build/ (gitignored; the main tree's when a worktree has none)
and SKIPS loudly when it or the i686 cross compiler is absent.
"""
import re
import shutil
import subprocess
from pathlib import Path

import pytest

CR = Path(__file__).resolve().parents[2] / "voodoo-cleanroom"
MAIN_CR = Path("/home/voidsstr/development/retro-agent/voodoo-cleanroom")
TREE = next((p / "build/retro3dfx-glide/glide3x/h5" for p in (CR, MAIN_CR)
             if (p / "build/retro3dfx-glide/glide3x/h5").is_dir()), None)
XCC = shutil.which("i686-w64-mingw32-gcc")
HCC = shutil.which("gcc") or shutil.which("cc")

# glide3x/h5/glide3/src/Makefile.mingw CDEFS for a release h5 build, plus
# build-stack.sh's OPTFLAGS (as tests/python/test_h5_glide_trace.py)
CDEFS = ("-D__WIN32__ -DFX_DLL_ENABLE -DHWC_ACCESS_DDRAW=1 -DHWC_EXT_INIT=1 -DGLIDE_ALT_TAB=1 "
         "-DBETA=1 -DHWC_MINIVDD_HACK=1 -DWIN40COMPAT=1 -DWINXP_ALT_TAB_FIX=1 "
         "-DWINXP_SAFER_ALT_TAB_FIX=1 -DNEED_MSGFILE_ASSIGN -UWINNT -DGLIDE3 -DGLIDE3_ALPHA "
         "-DGLIDE_HW_TRI_SETUP=1 -DGLIDE_INIT_HWC -DGLIDE_PACKED_RGB=0 "
         "-DGLIDE_PACKET3_TRI_SETUP=1 -DGLIDE_TRI_CULLING=1 -DUSE_PACKET_FIFO=1 "
         "-DGLIDE_CHECK_CONTEXT -DH3 -DFX_GLIDE_H5_CSIM=1 -DFX_GLIDE_NAPALM=1 "
         "-DGLIDE_PLUG -DGLIDE_SPLASH").split()
OPT = "-O2 -ffast-math -march=pentium3 -mtune=pentium3 -mfpmath=sse".split()
VARIANTS = {"c": ["-DGLIDE_USE_C_TRISETUP"],
            "x86": ["-DGL_AMD3D", "-DGL_MMX", "-DGL_SSE", "-DGL_X86"]}
EDITED = ("glide3/src/gpci.c", "glide3/src/gsst.c", "glide3/src/glfb.c", "minihwc/minihwc.c")


def src(rel):
    if TREE is None:
        pytest.skip("retro3dfx-glide clone absent - the SLI/AA guards are NOT checked "
                    "(run build-stack.sh)")
    return (TREE / rel).read_text(encoding="latin-1")


def _blank(text):
    """text with comments and string/char literals blanked (offsets kept)."""
    out, i, n = list(text), 0, len(text)
    while i < n:
        if text.startswith("/*", i):
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
        elif text.startswith("//", i):
            j = text.find("\n", i)
            j = n if j < 0 else j
        elif text[i] in "\"'":
            j = i + 1
            while j < n and text[j] != text[i]:
                j += 2 if text[j] == "\\" else 1
            j += 1
        else:
            i += 1
            continue
        for k in range(i, min(j, n)):
            if out[k] != "\n":
                out[k] = " "
        i = j
    return "".join(out)


def _func(text, header):
    """The body of the C function whose definition contains `header`."""
    s = text.index(header)
    b = _blank(text)
    o = b.index("{", s)
    depth = 0
    for i in range(o, len(b)):
        depth += {"{": 1, "}": -1}.get(b[i], 0)
        if depth == 0:
            return text[o:i + 1]
    raise AssertionError(f"unbalanced braces after {header!r}")


def _ws(text):
    return re.sub(r"\s+", " ", text)


def _order(text, *needles):
    """Assert every needle occurs, each after the previous; return positions."""
    pos, at = [], 0
    for n in needles:
        i = text.find(n, at)
        assert i >= 0, f"{n!r} missing after offset {at} (order {needles})"
        pos.append(i)
        at = i + len(n)
    return pos


# ---- the tree compiles, and the header is pure --------------------------------


@pytest.mark.parametrize("variant", sorted(VARIANTS))
@pytest.mark.parametrize("rel", EDITED)
def test_the_guard_edited_sources_compile_for_i686(rel, variant):
    if TREE is None or XCC is None:
        pytest.skip("fork clone or i686-w64-mingw32-gcc absent - the h5 sources were NOT compiled")
    inc = [TREE / "glide3/src", TREE / "incsrc", TREE / "minihwc",
           TREE.parent / "swlibs/fxmisc", TREE.parent / "swlibs/newpci/pcilib",
           TREE.parent / "swlibs/fxmemmap", TREE.parent / "swlibs/texus2/lib"]
    r = subprocess.run([XCC, "-m32", "-Wall"] + [f"-I{p}" for p in inc] + CDEFS + VARIANTS[variant]
                       + OPT + ["-fsyntax-only", str(TREE / rel)],
                       cwd=TREE / "glide3/src", capture_output=True, text=True, errors="replace")
    assert r.returncode == 0, r.stderr[-3000:]
    # nothing new to warn about in the guard code itself
    assert "h5sliaa.h" not in r.stderr, r.stderr[-3000:]


def test_the_decision_header_is_pure_c_and_warning_free():
    """The header the native test includes must stay Win32-free and hardware-
    free, or the test stops being able to run the DLL's code on the host."""
    h = src("minihwc/h5sliaa.h")
    code = _blank(h)
    for banned in ("#include", "HWC_", "ExtEscape", "GETENV", "getenv", "Fx", "gc->", "bInfo"):
        assert banned not in code, banned
    if HCC is None:
        pytest.skip("no host C compiler - h5sliaa.h NOT compiled")
    for std in ("c89", "c11"):
        r = subprocess.run([HCC, f"-std={std}", "-Wall", "-Wextra", "-Werror", "-pedantic",
                            "-fsyntax-only", "-x", "c", str(TREE / "minihwc/h5sliaa.h")],
                           capture_output=True, text=True)
        assert r.returncode == 0, r.stderr


def test_every_sli_aa_decision_goes_through_the_header():
    """One copy of each decision: the one the native test runs."""
    assert '#include "h5sliaa.h"' in src("minihwc/minihwc.h")
    g = src("glide3/src/gpci.c")
    assert 'h5SliAaConfigEnv(GLIDE_GETENV("SSTH3_SLI_AA_CONFIGURATION", 2L)' in g
    assert 'switch(GLIDE_GETENV("SSTH3_SLI_AA_CONFIGURATION"' not in g
    s = _blank(src("glide3/src/gsst.c"))
    assert s.count("h5SliAaForcedSample(") == 3            # 32, 15 and 16 bpp
    assert "(_GlideRoot.environment.aaSample == 8)" not in s
    assert s.count("h5SliAaLayout(") == 1
    raw = src("glide3/src/gsst.c")
    live = raw[raw.index("#if 0 /* Old Way */"):raw.index("/* Yeesh. */")].split("#else", 1)[1]
    assert "h5SliAaLayout(" in live and "switch( gc->chipCount )" not in live
    m = _blank(src("minihwc/minihwc.c"))
    for fn in ("h5SliAaRequestNeeded(", "h5SliAaRequestTuple(", "h5SliAaAnalog(",
               "h5SliAaChipsDrivenOk(", "hwcSliAaTupleSupported(", "h5SliAaPciOpChips(",
               "h5SliAaIdleResetOk("):
        assert fn in m, fn
    assert "h5SliAaLfbReadOk(" in _blank(src("glide3/src/glfb.c"))


# ---- (a) cfg 1 on more than two chips -------------------------------------------


def test_single_chip_aa_on_a_multi_chip_board_fails_the_open_before_any_work():
    s = src("glide3/src/gsst.c")
    # its braces are split across #if branches, so slice it by its markers
    body = s[s.index("GR_EXT_ENTRY(grSstWinOpenExt"):s.index("} /* grSstWinOpenExt */")]
    b = _blank(body)
    fs, lay, ok, late = _order(b, "h5SliAaForcedSample(", "h5SliAaLayout(",
                               "h5SliAaSingleChipOk(", "gc->chipCount = 1 ;")
    # the refusal sits inside the forceSingleChip block, before the late drop
    blk = body[ok:late]
    # the exact condition: a `0 &&` or a swapped argument is a disabled guard
    assert ("if (!h5SliAaSingleChipOk(gc->bInfo->pciInfo.numChips, "
            "_GlideRoot.environment.forceSingleChip, gc->grPixelSample)) {") in _ws(body)
    assert "GrErrorCallback(" in blk and "return 0;" in blk
    assert "SSTH3_SLI_AA_CONFIGURATION=1" in blk
    assert "hwcTrace(" in blk
    # ...and before every buffer, mode set and escape of the open
    _order(b, "h5SliAaSingleChipOk(", "hwcAllocBuffers(", "hwcInitVideo(")
    # what minihwc checks the chip count against
    assert "bInfo->h3chipsDriven     = gc->chipCount;" in body
    assert "h3chipsDriven;" in src("minihwc/minihwc.h")


def test_single_chip_aa_loop_writes_only_the_driven_chips_the_close_loop_is_untouched():
    """The open's single-chip AA PCI_OP loop is bounded by h5SliAaPciOpChips;
    the CLOSE loop also runs for cfg 0 on four chips (proven), so it keeps
    upstream's bound and cfg 0's close stays byte-identical."""
    m = src("minihwc/minihwc.c")
    iv = _func(m, "hwcInitVideo(hwcBoardInfo *bInfo, FxBool tiled")
    loop = iv[iv.index("const FxU32 opChips = h5SliAaPciOpChips("):]
    assert "for (function_number = 0; function_number < opChips; function_number++)" in loop
    assert "function_number < bInfo->pciInfo.numChips" not in _blank(iv)
    rv = _func(m, "hwcRestoreVideo(hwcBoardInfo *bInfo)")
    assert "for (function_number = 0; function_number < bInfo->pciInfo.numChips; function_number++)" in rv
    assert "h5SliAaPciOpChips" not in rv


# ---- (b) a request no kernel can program ----------------------------------------


def test_the_open_is_checked_before_the_directdraw_mode_set():
    m = src("minihwc/minihwc.c")
    iv = _func(m, "hwcInitVideo(hwcBoardInfo *bInfo, FxBool tiled")
    b = _blank(iv)
    chk = b.index("hwcSliAaOpenOk(bInfo)")
    assert chk < b.index("setVideoMode(")
    for hw in ("ExtEscape", "HWC_IO_", "HWC_CAGP_"):
        assert hw not in b[:chk], hw
    assert "if (!hwcSliAaOpenOk(bInfo)) return FXFALSE;" in _ws(_blank(iv))
    ok = _func(m, "hwcSliAaOpenOk(hwcBoardInfo *bInfo)")
    _order(_blank(ok), "h5SliAaRequestNeeded(", "h5SliAaChipsDrivenOk(",
           "h5SliAaRequestTuple(", "h5SliAaAnalog(", "hwcSliAaTupleOk(")
    t = _func(m, "hwcSliAaTupleOk(const char *before")
    assert "hwcSliAaTupleSupported(chips, sli, aa, high, analog)" in t
    assert "errorString" in t and "hwcTrace(" in t and "REFUSED" in t


def test_the_bytes_are_checked_just_before_the_escape_and_the_answer_is_honoured():
    m = src("minihwc/minihwc.c")
    blk = m.split("HWCEXT_SLI_AA_REQUEST ;", 1)[1]
    blk = blk[:blk.index("OS is 9x")]
    assert "if (!hwcSliAaReqOk(&ctxReq)) return hwcFailOpenAfterExclusive(bInfo);" in _ws(_blank(blk))
    tr, chk, ret, esc, ans, fail, store = _order(
        blk, "hwcTraceSliAAReq(&ctxReq);", "if (!hwcSliAaReqOk(&ctxReq))",
        "return hwcFailOpenAfterExclusive(bInfo);",
        "retVal = ExtEscape", "if ((FxI32) retVal <= 0 || ctxRes.resStatus != 1) {",
        "return hwcFailOpenAfterExclusive(bInfo);", "HWC_IO_STORE(bInfo->regInfo, vidScreenSize")
    # nothing reaches the hardware between the check and the escape
    assert "ExtEscape" not in blk[chk:esc] and "HWC_IO_" not in blk[chk:esc]
    # the refusal says why
    assert "sprintf(errorString" in blk[ans:fail] and "hwcTrace(" in blk[ans:fail]
    rq = _func(m, "hwcSliAaReqOk(const hwcExtRequest_t *q)")
    for f in ("dwChips", "dwsliEn", "dwaaEn", "dwaaSampleHigh", "dwsliAaAnalog"):
        assert f"q->optData.sliAAReq.ChipInfo.{f}" in rq, f


def test_the_request_is_filled_as_the_tuple_the_check_computes():
    """hwcSliAaOpenOk decides from h5SliAaRequestTuple(); the escape sends what
    hwcInitVideo fills in. The two must be the same function of bInfo."""
    m = src("minihwc/minihwc.c")
    iv = _func(m, "hwcInitVideo(hwcBoardInfo *bInfo, FxBool tiled")
    b = _blank(iv)
    _order(b, "bInfo->h3analogSli = h5SliAaAnalog(bInfo->pciInfo.numChips, bInfo->h3analogSli);",
           "h5SliAaRequestNeeded(bInfo->pciInfo.numChips, bInfo->h3nwaySli,",
           "HWCEXT_SLI_AA_REQUEST ;")
    fill = iv[iv.index("ctxReq.which = HWCEXT_SLI_AA_REQUEST ;"):iv.index("hwcTraceSliAAReq(&ctxReq);")]
    fill = re.sub(r"\s+", " ", fill)
    for line in ("ChipInfo.dwaaEn = (bInfo->h3pixelSample > 1) ;",
                 "ChipInfo.dwsliEn = (bInfo->h3nwaySli > 1) ;",
                 "ChipInfo.dwsliAaAnalog = bInfo->h3analogSli ;",
                 "ChipInfo.dwChips = bInfo->pciInfo.numChips ;",
                 "ChipInfo.dwaaSampleHigh = 0; if(bInfo->h3pixelSample == 4) "
                 "ctxReq.optData.sliAAReq.ChipInfo.dwaaSampleHigh = 1; "
                 "else if(bInfo->h3pixelSample == 8) "
                 "ctxReq.optData.sliAAReq.ChipInfo.dwaaSampleHigh = 2;"):
        assert line in fill, line
    h = src("minihwc/h5sliaa.h")
    tup = re.sub(r"\s+", " ", _func(h, "h5SliAaRequestTuple(unsigned long numChips"))
    for line in ("t.chips = numChips;", "t.sli = nwaySli > 1;", "t.aa = pixelSample > 1;",
                 "t.high = pixelSample == 4 ? 1 : pixelSample == 8 ? 2 : 0;",
                 "t.analog = analog;"):
        assert line in tup, line


# ---- (d) the READ lock ------------------------------------------------------------


def test_a_multi_chip_aa_read_lock_is_refused_before_the_sli_toggle_and_the_fifo():
    g = src("glide3/src/glfb.c")
    lk = _func(g, "static FxBool _grLfbLock (GrLock_t type")
    b = _blank(lk)
    guard, toggle, fifo = _order(b, "h5SliAaLfbReadOk(", "hwcSLIReadDisable(", "_grValidateState()")
    # nothing that writes the FIFO or touches the chips before the guard
    for hw in ("GR_SET", "REG_GROUP_BEGIN", "hwcSLIRead", "_grValidateState", "HWC_", "GR_CAGP"):
        assert hw not in b[:guard], hw
    blk = lk[guard:toggle]
    assert ("if ((type & ~GR_LFB_NOIDLE) == GR_LFB_READ_ONLY && "
            "!h5SliAaLfbReadOk(gc->grPixelSample, gc->chipCount, _grAaLfbReadOptIn())) {") in _ws(_blank(lk))
    assert "GR_RETURN(FXFALSE);" in blk and "hwcTrace(" in blk
    # the opt-in: exactly "1", from the process environment only
    opt = _func(g, "_grAaLfbReadOptIn(void)")
    assert 'getenv("RETRO_GLIDE_AA_LFB_READ")' in opt and 'strcmp(e, "1") == 0' in opt
    assert "GETENV(" not in opt
    # every read path is a _grLfbLock (grLfbLock and grLfbReadRegion both)
    assert "_grLfbLock(GR_LFB_READ_ONLY" in g
    assert "return _grLfbLock(_type, buffer, writeMode, origin, pixelPipeline, info);" in g


# ---- (e) the idle wait's master reset ---------------------------------------------


def test_the_idle_wait_skips_the_master_reset_on_multi_chip_sliaa_and_traces_first():
    m = src("minihwc/minihwc.c")
    fn = _func(m, "FxBool hwcIdleHardwareWithTimeout(hwcBoardInfo *bInfo)")
    b = _blank(fn)
    wait, tr, dec, ret, reset = _order(
        b, "hwcWaitIdleBounded(bInfo)", "hwcTrace(", "h5SliAaIdleResetOk(", "return FXFALSE;",
        "HWC_IO_LOAD(bInfo->regInfo, miscInit0, miscInit0);")
    assert "idle: NOT idle - master reset?" in fn[tr:dec]
    assert "SKIPPED" in fn[dec:ret]
    assert "HWC_IO_" not in b[:dec]
    assert ("if (!h5SliAaIdleResetOk(bInfo->pciInfo.numChips, bInfo->h3nwaySli, "
            "bInfo->h3pixelSample)) {") in _ws(_blank(fn))
    # the callers treat FXFALSE as "not idle": the open fails, the close skips
    # its register restore and still releases exclusive mode
    assert "hwcInitFifo: NOT idle - open fails" in m
    rv = _func(m, "hwcRestoreVideo(hwcBoardInfo *bInfo)")
    assert "goto hwcRestoreVideo_release;" in rv


# ---- (c') a refusal after HWCSETEXCLUSIVE gives the display back (review 2026-09-27) -------


def test_a_refusal_after_exclusive_mode_gives_the_display_back():
    """By the SLI/AA escape hwcInitVideo has run setVideoMode, HWCSETEXCLUSIVE
    and the master's video registers; grSstWinOpen returns 0 on its failure
    WITHOUT hwcRestoreVideo. The first SLIAA-GUARD returned FXFALSE there, so
    the kernel kept its exclusive owner (vcr-kmd: no 2D acceleration, no
    hardware pointer, no D3D) and the CRT a mode nobody used, until the
    process exited - and with vcr-kmd's Diag\\SliAA=0 default that is every
    AA open. Every return after the request is built goes through
    hwcFailOpenAfterExclusive, which releases through hwcReleaseDisplay."""
    m = src("minihwc/minihwc.c")
    iv = _func(m, "hwcInitVideo(hwcBoardInfo *bInfo, FxBool tiled")
    b = _blank(iv)
    excl = b.index("ctxReq.which = HWCEXT_HWCSETEXCLUSIVE;")
    # the pre-check before the mode set needs no release: nothing is taken yet
    assert b.index("if (!hwcSliAaOpenOk(bInfo))") < b.index("setVideoMode(") < excl
    req = b.index("ctxReq.which = HWCEXT_SLI_AA_REQUEST ;")
    # to the Win9x branch (its first declaration: code, not a comment or string)
    end = b.index("DIOC_DATA DIOC_Data;", req)
    returns = re.findall(r"return[^;]*;", b[req:end])
    assert returns == ["return hwcFailOpenAfterExclusive(bInfo);"] * 2, returns
    # the helper: the refusal kept for the app, the display released and reset
    f = _func(m, "hwcFailOpenAfterExclusive(hwcBoardInfo *bInfo)")
    fb = _blank(f)
    _order(fb, "hwc_errncpy(why, errorString);", "hwcTrace(", "hwcReleaseDisplay(bInfo, FXTRUE)",
           "hwc_errncpy(errorString, why);", "return FXFALSE;")
    # ...and no SLI/AA disable: the kernel enabled nothing
    assert "HWCEXT_SLI_AA_REQUEST" not in fb and "ExtEscape" not in fb
    r = _func(m, "hwcReleaseDisplay(hwcBoardInfo *bInfo, FxBool resetAnyway)")
    rb = _blank(r)
    _order(rb, "ctxReq.which = HWCEXT_HWCRLSEXCLUSIVE;", "memset(&ctxRes, 0, sizeof(ctxRes));",
           "ExtEscape(", "if (!released) {", "if (!resetAnyway)", "return FXFALSE;",
           "hwcResetVideo(bInfo);", "return released;")
    assert rb.count("ExtEscape(") == 1 and "SLI_AA" not in rb
    # both are defined before hwcInitVideo (static, no prototype needed)
    assert m.index("\nhwcReleaseDisplay(") < m.index("\nhwcFailOpenAfterExclusive(") < \
        m.index("\nhwcInitVideo(hwcBoardInfo")


def test_a_refused_open_holds_its_mode_for_the_pace_floor_before_giving_it_back():
    """The release restores the desktop INSIDE grSstWinOpen, milliseconds after
    setVideoMode - two re-syncs of .124's 1998 CRT back to back, invisible to
    the tools' pace gate, which wraps the whole call. Before this fix the
    refused mode stayed up until the process exited (paced by accident). The
    helper now holds it until vcr_pace.h's floor has passed since the mode
    set; the tick is taken when setVideoMode returns, before HWCSETEXCLUSIVE."""
    m = src("minihwc/minihwc.c")
    hold = int(re.search(r"#define HWC_REFUSAL_HOLD_MS (\d+)UL", m).group(1))
    pace = (Path(__file__).resolve().parents[2] / "voodoo-cleanroom" / "vcr-kmd" / "tools" /
            "vcr_pace.h").read_text()
    assert hold == int(re.search(r"#define VCR_PACE_MIN_MS\s+(\d+)u", pace).group(1))
    f = _blank(_func(m, "hwcFailOpenAfterExclusive(hwcBoardInfo *bInfo)"))
    _order(f, "GetTickCount() - hwcModeSetTick", "if (held < HWC_REFUSAL_HOLD_MS) {",
           "Sleep(HWC_REFUSAL_HOLD_MS - held);", "hwcReleaseDisplay(bInfo, FXTRUE)")
    iv = _blank(_func(m, "hwcInitVideo(hwcBoardInfo *bInfo, FxBool tiled"))
    _order(iv, "setVideoMode( bInfo, refresh )", "hwcModeSetTick = GetTickCount();",
           "ctxReq.which = HWCEXT_HWCSETEXCLUSIVE;", "hwcFailOpenAfterExclusive(bInfo)")
    assert iv.count("hwcModeSetTick = GetTickCount();") == 1


def test_the_close_releases_through_the_same_helper_with_upstreams_semantics():
    """hwcRestoreVideo's release label is the shared helper; a release the
    kernel does not confirm still fails the close with DirectDraw's mode left
    alone (resetAnyway FALSE), as upstream's inline copy did."""
    m = src("minihwc/minihwc.c")
    rv = _func(m, "hwcRestoreVideo(hwcBoardInfo *bInfo)")
    b = _blank(rv)
    lbl = b.index("hwcRestoreVideo_release:")
    assert "if (!hwcReleaseDisplay(bInfo, FXFALSE)) return FXFALSE;" in _ws(b[lbl:])
    assert "HWCEXT_HWCRLSEXCLUSIVE" not in b          # one copy, in the helper
    assert b[lbl:].count("hwcResetVideo(bInfo);") == 1   # the non-HWC_EXT_INIT branch only
    assert m.count("ctxReq.which = HWCEXT_HWCRLSEXCLUSIVE;") == 1


def test_the_old_early_return_is_what_the_placement_check_rejects():
    """The check above can fail: the first SLIAA-GUARD's `return FXFALSE;`
    after the kernel's refusal is caught."""
    m = src("minihwc/minihwc.c")
    old = m.replace("        return hwcFailOpenAfterExclusive(bInfo);\n      }\n\n      /* the w2k",
                    "        return FXFALSE;\n      }\n\n      /* the w2k", 1)
    assert old != m
    iv = _blank(_func(old, "hwcInitVideo(hwcBoardInfo *bInfo, FxBool tiled"))
    req = iv.index("ctxReq.which = HWCEXT_SLI_AA_REQUEST ;")
    returns = re.findall(r"return[^;]*;", iv[req:iv.index("DIOC_DATA DIOC_Data;", req)])
    assert returns != ["return hwcFailOpenAfterExclusive(bInfo);"] * 2
    assert "return FXFALSE;" in returns


# ---- (e') a busy close skips the SLI/AA disable - and says so ---------------------------


def test_a_busy_multi_chip_close_skips_the_disable_escape_and_says_so():
    """Since 631221b the idle wait no longer resets the master of a busy
    multi-chip SLI/AA board, so a close on a board still busy past the bound -
    cfg 5 (4-way SLI) and 2-chip SLI included - goes straight to the release,
    past the SLI/AA disable escape (215a9e7's reset + re-check could still reach
    it). Pinned here, with the trace and RETRO_GLIDE_MAPLOG lines that make it
    visible."""
    m = src("minihwc/minihwc.c")
    rv = _func(m, "hwcRestoreVideo(hwcBoardInfo *bInfo)")
    b = _blank(rv)
    idle, need, log, go, dis = _order(
        b, "if (!hwcIdleHardwareWithTimeout(bInfo)) {", "h5SliAaRequestNeeded(", "hwcLogLine(",
        "goto hwcRestoreVideo_release;", "ctxReq.which = HWCEXT_SLI_AA_REQUEST ;")
    blk = rv[idle:go]
    assert "SLI/AA disable escape NOT sent" in blk and "hwcTrace(" in blk
    assert "IS_NAPALM(bInfo->pciInfo.deviceID)" in blk
    # the busy branch sends nothing itself
    assert "ExtEscape" not in b[idle:go] and "HWC_IO_" not in b[idle:go]
    # cfg 5 on the V5 6000 (4 chips, 4-way SLI, 1 sample): no master reset and
    # an SLI/AA request that the close would have disabled
    h = src("minihwc/h5sliaa.h")
    if HCC is None:
        pytest.skip("no host C compiler - the cfg 5 predicates NOT run")
    import tempfile
    code = h + """
#include <stdio.h>
int main(void) {
  printf("%d %d %d %d\\n", h5SliAaIdleResetOk(4, 4, 1), h5SliAaRequestNeeded(4, 4, 1, 0),
         h5SliAaIdleResetOk(2, 2, 1), h5SliAaIdleResetOk(1, 1, 2));
  return 0; }
"""
    with tempfile.TemporaryDirectory() as d:
        (Path(d) / "p.c").write_text(code)
        r = subprocess.run([HCC, "-std=c99", "-Wall", "-o", str(Path(d) / "p"), str(Path(d) / "p.c")],
                           capture_output=True, text=True)
        assert r.returncode == 0, r.stderr
        out = subprocess.run([str(Path(d) / "p")], capture_output=True, text=True).stdout.split()
    # cfg 5: reset skipped, request needed -> the busy close logs "NOT sent";
    # 2-chip SLI likewise; a single chip keeps upstream's reset
    assert out == ["0", "1", "0", "1"]


def test_vcr_kmd_turns_sli_off_when_glide_releases_exclusive_mode():
    """What the busy close relies on: vcr-kmd's HWCRLSEXCLUSIVE restores the
    desktop mode (IOCTL_VCR_RESTORE_MODE -> VcrHwRestoreMode -> VcrHwSetMode),
    and the mode set turns SLI/AA off in the kernel BEFORE it programs the
    mode, with no Glide MMIO. If this chain breaks, a busy cfg-5 close leaves
    snoop / swap sync / the video mux programmed after the app exits."""
    kmd = Path(__file__).resolve().parents[2] / "voodoo-cleanroom" / "vcr-kmd"
    esc = (kmd / "display" / "vcrdd_escape.c").read_text()
    rls = esc[esc.index("case VCR_HWC_HWCRLSEXCLUSIVE:"):]
    rls = rls[:rls.index("break;")]
    assert "IOCTL_VCR_RESTORE_MODE" in rls and "pd->exclusive_pid = 0;" in rls
    mp = (kmd / "miniport" / "vcrmp.c").read_text()
    io = mp[mp.index("case IOCTL_VCR_RESTORE_MODE:"):]
    assert "VcrHwRestoreMode(x)" in io[:io.index("break;")]
    hw = (kmd / "miniport" / "vcrmp_hw.c").read_text()
    rm = hw[hw.index("VP_STATUS VcrHwRestoreMode(VCR_EXT *x)"):]
    assert "return VcrHwSetMode(x, (ULONG)x->cur_mode);" in rm[:rm.index("\n}\n")]
    sm = hw[hw.index("VP_STATUS VcrHwSetMode(VCR_EXT *x, ULONG idx)"):]
    sm = sm[:sm.index("\n}\n")]
    assert 'VcrSliOff(x, "mode set");' in sm
    assert sm.index('VcrSliOff(x, "mode set");') < sm.index("VCR_EV_MODESET_PLL")

