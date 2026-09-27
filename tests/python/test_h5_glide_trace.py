"""Our h5 Glide's AA-TRACE step trace (fork voidsstr/retro3dfx-glide, 2026-09-27).

The V5 6000 AA configurations froze .124 hard, AFTER the kernel's SLI/AA setup
reported SET_DONE: cfg 3 in grLfbReadRegion, cfg 7 and cfg 1 inside
grSstWinOpen (cfg 1: HWC_SLIAA a=4 b=0x102, boot #18, read back 2026-09-27).
glidelab's own step log can only name the Glide CALL that froze; the fork's
FX_GLIDE_TRACE writes every step inside it, each line flushed to the disk
before the hardware access it names, so the file's last line is the step
that never completed. These tests pin what makes that trace trustworthy:

- it is opt-in (process environment only - a registry leftover must never
  turn per-step disk flushes on in a game), leveled (atoi > 0; level 2 also
  waits after each FIFO step of the open until the chips have run it),
  writes where FX_GLIDE_TRACE_FILE says, and SAYS when it cannot open the
  file instead of vanishing;
- the tree compiles (the first version of these edits did not: two extra
  ')' in glfb.c) - an i686 -fsyntax-only compile of the three edited files;
- every hardware step on the open path between the SLI/AA escape and the
  first swap has its own trace line (checked on the preprocessed source, so
  branches our build does not compile - HAL_CSIM, DRI, HWC_GDX_INIT - do not
  count);
- the x86 build no longer calls the trace with live MMX state (objdump: EMMS
  directly before the call; a copy of glfb.c with the old macro is checked
  too, to prove the check tells them apart).

Reads the fork clone under voodoo-cleanroom/build/ (gitignored; the main
tree's when a worktree has none) and SKIPS loudly when it or the i686 cross
compiler is absent.
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
CC = shutil.which("i686-w64-mingw32-gcc")
OBJDUMP = shutil.which("i686-w64-mingw32-objdump")

# glide3x/h5/glide3/src/Makefile.mingw CDEFS for a release h5 build (no
# DEBUG, no TEXUS2), plus build-stack.sh's OPTFLAGS
CDEFS = ("-D__WIN32__ -DFX_DLL_ENABLE -DHWC_ACCESS_DDRAW=1 -DHWC_EXT_INIT=1 -DGLIDE_ALT_TAB=1 "
         "-DBETA=1 -DHWC_MINIVDD_HACK=1 -DWIN40COMPAT=1 -DWINXP_ALT_TAB_FIX=1 "
         "-DWINXP_SAFER_ALT_TAB_FIX=1 -DNEED_MSGFILE_ASSIGN -UWINNT -DGLIDE3 -DGLIDE3_ALPHA "
         "-DGLIDE_HW_TRI_SETUP=1 -DGLIDE_INIT_HWC -DGLIDE_PACKED_RGB=0 "
         "-DGLIDE_PACKET3_TRI_SETUP=1 -DGLIDE_TRI_CULLING=1 -DUSE_PACKET_FIFO=1 "
         "-DGLIDE_CHECK_CONTEXT -DH3 -DFX_GLIDE_H5_CSIM=1 -DFX_GLIDE_NAPALM=1 "
         "-DGLIDE_PLUG -DGLIDE_SPLASH").split()
OPT = "-O2 -ffast-math -march=pentium3 -mtune=pentium3 -mfpmath=sse".split()
VARIANTS = {"c": ["-DGLIDE_USE_C_TRISETUP"],                              # glide3x_h5.dll
            "x86": ["-DGL_AMD3D", "-DGL_MMX", "-DGL_SSE", "-DGL_X86"]}    # glide3x_h5_x86.dll
EDITED = ("glide3/src/glfb.c", "glide3/src/gsst.c", "minihwc/minihwc.c")


def src(rel):
    if TREE is None:
        pytest.skip("retro3dfx-glide clone absent - the AA trace is NOT checked (run build-stack.sh)")
    return (TREE / rel).read_text(encoding="latin-1")


def _need_cc():
    if TREE is None or CC is None:
        pytest.skip("fork clone or i686-w64-mingw32-gcc absent - the h5 sources were NOT compiled")


def _flags(variant):
    inc = [TREE / "glide3/src", TREE / "incsrc", TREE / "minihwc",
           TREE.parent / "swlibs/fxmisc", TREE.parent / "swlibs/newpci/pcilib",
           TREE.parent / "swlibs/fxmemmap", TREE.parent / "swlibs/texus2/lib"]
    return ["-m32", "-Wall"] + [f"-I{p}" for p in inc] + CDEFS + VARIANTS[variant] + OPT


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
    """The body of the C function whose definition line contains `header`."""
    s = text.index(header)
    b = _blank(text)
    o = b.index("{", s)
    depth = 0
    for i in range(o, len(b)):
        depth += {"{": 1, "}": -1}.get(b[i], 0)
        if depth == 0:
            return text[o:i + 1]
    raise AssertionError(f"unbalanced braces after {header!r}")


# ---- (a) the tree compiles -------------------------------------------------------


@pytest.mark.parametrize("variant", sorted(VARIANTS))
@pytest.mark.parametrize("rel", EDITED)
def test_the_trace_edited_sources_compile_for_i686(rel, variant):
    """An -fsyntax-only compile of each edited file, in both h5 variants, with
    the Makefile's release defines. The first AA-TRACE edit left two extra ')'
    in glfb.c (`== GR_LFB_READ_ONLY))`) and nothing in the suite noticed."""
    _need_cc()
    r = subprocess.run([CC] + _flags(variant) + ["-fsyntax-only", str(TREE / rel)],
                       cwd=TREE / "glide3/src", capture_output=True, text=True, errors="replace")
    assert r.returncode == 0, r.stderr[-3000:]
    assert "GR_LFB_READ_ONLY))" not in src("glide3/src/glfb.c")


# ---- (c) opt-in, leveled, flushed, and loud when it cannot write -----------------


def test_the_trace_reads_the_process_environment_only_at_any_positive_level():
    c = src("minihwc/minihwc.c")
    lvl = _func(c, "hwcTraceLevel(void)")
    # plain getenv: GETENV is hwcGetenv, which falls back to the glide
    # registry key - a leftover there would make every Glide app on the box
    # flush the disk at every step
    assert 'getenv("FX_GLIDE_TRACE")' in lvl and "GETENV(" not in lvl
    assert re.search(r"\(level > 0\) \? level : 0", lvl)
    assert "atoi(e) == 1" not in c                    # the old "only 1 turns it on"
    assert "hwcTraceLevel() > 0" in _func(c, "\nhwcTraceOn(void)")
    h = src("minihwc/minihwc.h")
    assert "extern int    hwcTraceLevel(void);" in h


def test_every_trace_line_is_flushed_to_the_disk_before_the_caller_goes_on():
    c = src("minihwc/minihwc.c")
    t = _func(c, "\nhwcTrace(const char *step, FxU32 reg, FxU32 val)")
    assert t.index("fprintf(f,") < t.index("fflush(f);") < t.index("FlushFileBuffers(")


def test_the_trace_file_is_chosen_by_env_and_a_failed_open_is_reported():
    c = src("minihwc/minihwc.c")
    o = _func(c, "\nhwcTraceOpen(void)")
    assert 'getenv("FX_GLIDE_TRACE_FILE")' in o and "path = HWC_TRACE_PATH;" in o
    assert '#define HWC_TRACE_PATH "C:\\\\vcr\\\\glidelab\\\\glidetrace.log"' in c
    fail = o[o.index("if (!hwcTraceFile) {"):]
    fail = fail[:fail.index("return NULL;")]
    # said, three ways, before the trace turns itself off
    for how in ("OutputDebugStringA(msg)", "GDBG_INFO(0,", "hwcLogLine("):
        assert how in fail, how
    assert "hwcTraceState = 0;" in fail
    # the header glidelab_run.py checks for, with the level and the path
    assert '"=== glidetrace pid=%lu tick=%lu level=%d file=%s\\n"' in o


def test_the_sli_aa_request_dump_is_one_helper_call_before_the_escape():
    """The seven-line dump sat inline between `HWCEXT_SLI_AA_REQUEST ;` and the
    escape and pushed it out of test_h5_glide_fixes' 3000-char window; it is
    a static helper now, and returns at once with the trace off."""
    c = src("minihwc/minihwc.c")
    h = _func(c, "hwcTraceSliAAReq(const hwcExtRequest_t *q)")
    assert re.match(r"\{\s*if \(!hwcTraceOn\(\)\)\s*return;", h)
    assert "ExtEscape" not in _blank(h) and h.count("hwcTrace(") == 7
    assert c.index("static void\nhwcTraceSliAAReq(") < c.index("\nhwcInitVideo(hwcBoardInfo")


# ---- level 2: the last "executed" line is a command the chips really ran ------------


OPEN_SYNCS = ("winopen: fifo leftOverlayBuf", "winopen: fifo swapbufferCMD", "winopen: fifo chipMask",
              "winopen: fifo colBufferAddr/auxBufferAddr (primary)",
              "winopen: fifo colBufferAddr/auxBufferAddr (SECONDARY)", "winopen: fifo renderMode",
              "winopen: assertDefaultState", "aaCtrl: fifo write", "sliCtrl: fifo write",
              "clearBuffers: clear 1", "clearBuffers: swap 1", "clearBuffers: clear 2",
              "clearBuffers: swap 2", "clearBuffers: clear 3", "clearBuffers: swap 3",
              "clearBuffers: grRenderBuffer BACK", "splash: grSplash")


def test_level_2_waits_for_each_fifo_step_of_the_open_and_nothing_below_it():
    g = src("glide3/src/gsst.c")
    sync = _func(g, "\n_grTraceSync(const char *step)")
    # level 0/1: returns before touching anything
    assert re.match(r"\{\s*char line\[160\];\s*FxU32 status;\s*if \(hwcTraceLevel\(\) < 2\)\s*return;",
                    sync)
    # the wait is grFinish (bounded: 4M polls) and its outcome is written
    assert sync.index('"sync: grFinish after %.120s"') < sync.index("grFinish();") < \
        sync.index("_grSstStatus()") < sync.index('"executed: %.120s"')
    assert "busyPolls > 4000000UL" in _func(g, "GR_ENTRY(grFinish, void, (void))")
    for step in OPEN_SYNCS:
        assert f'_grTraceSync("{step}");' in g, step


def test_each_fifo_step_of_the_open_is_synced_right_after_it_is_queued():
    g = _blank(src("glide3/src/gsst.c"))
    raw = src("glide3/src/gsst.c")
    for write, step in (("REG_GROUP_SET(hw, swapbufferCMD, 0x0);", "winopen: fifo swapbufferCMD"),
                        ("_grRenderMode(pixelformat);", "winopen: fifo renderMode"),
                        ("REG_GROUP_SET(hw, aaCtrl, aaCtrl);", "aaCtrl: fifo write"),
                        ("REG_GROUP_SET(hw, sliCtrl, sliCtrl);", "sliCtrl: fifo write")):
        at = raw.index(write)
        nxt = raw.index(f'_grTraceSync("{step}")', at)
        between = g[at:nxt]
        # nothing but the group's end between the queued command and its sync
        assert re.fullmatch(r"[^;]*;\s*(REG_GROUP_END\(\);\s*)?", between), (step, between)


# ---- (d) the coverage gaps the post-mortem named -----------------------------------


def test_the_open_close_and_read_paths_name_every_black_box():
    g, c = src("glide3/src/gsst.c"), src("minihwc/minihwc.c")
    need_g = ("winopen: hwcShareContextData (escape)", "winopen: hwcGammaRGB (master DAC)",
              "winopen: fifo swapbufferCMD 0 (immediate swap)",
              "splash: LoadLibrary 3dfxspl3.dll", "splash: plugin present",
              "splash: plugin init (draws through Glide)", "splash: grSplash (reg=noSplash)",
              "_grAAOffsetValue: aaCtrl for chips", "chipMask: fifo write (reg=old val=new)",
              "winopen: re-open - hwcInitRegisters (pciInit0)")
    need_c = ("gamma: io load vidProcCfg, then dacAddr/dacData loop",
              "gamma: dac loop done (reg=dacBase val=entries never read back)",
              "idle: RESET master - io load miscInit0", "idle: RESET done - bounded re-check",
              "idle: NOT idle - poll bound hit", "idle: NOT idle - time bound hit",
              "hwcInitVideo: setVideoMode (reg=refresh", "hwcInitVideo: setVideoMode returned",
              "hwcInitVideo: HWCSETEXCLUSIVE escape", "video regs: master programmed",
              "close: SLI_AA_REQUEST disable - ExtEscape (reg=chips)",
              "close: SLI_AA_REQUEST disable returned retVal / resStatus",
              "close: HWCRLSEXCLUSIVE escape", "close: NOT idle - register restore skipped",
              "sli read enable: FX_GLIDE_A0_READ_ABORT set / nwaySli",
              "sli read disable: FX_GLIDE_A0_READ_ABORT set / nwaySli")
    for s in need_g:
        assert f'"{s}' in g, s
    for s in need_c:
        assert f'"{s}' in c, s


def test_the_gamma_line_carries_the_v56k_dac_fix_state_and_the_entry_count():
    c = src("minihwc/minihwc.c")
    gt = _func(c, "\nhwcGammaTable(hwcBoardInfo *bInfo, FxU32 nEntries")
    fix = gt.index("bInfo->pciInfo.numChips == 4 && bInfo->h3pixelSample >= 4")
    assert gt.index("v56kDacFix = 1;", fix) < gt.index("for (i = 1; i < 128; i++)", fix)
    line = gt.index('"gamma: io load vidProcCfg, then dacAddr/dacData loop"')
    assert "(v56kDacFix << 16) | nEntries" in gt[line:line + 200]
    # before the first DAC access, after the table is final
    assert fix < line < gt.index("HWC_IO_LOAD( bInfo->regInfo, vidProcCfg, vidProcCfg);") < \
        gt.index("HWC_IO_STORE( bInfo->regInfo, dacAddr, dacBase + i);")


def test_the_sli_read_toggles_say_whether_they_do_anything():
    """hwcSLIReadEnable/Disable do nothing unless FX_GLIDE_A0_READ_ABORT is
    set - the trace says which before any config access, and announces each
    read and each write (reg = chip << 16 | offset)."""
    c = src("minihwc/minihwc.c")
    for fn in ("void hwcSLIReadEnable(hwcBoardInfo *bInfo)", "void hwcSLIReadDisable(hwcBoardInfo *bInfo)"):
        b = _func(c, fn)
        assert b.count('GETENV("FX_GLIDE_A0_READ_ABORT")') == 1       # read once, as before
        assert b.index("_READ_ABORT set / nwaySli") < b.index("if(readAbort)")
        for reg in ("cfgSliLfbCtrl", "cfgAALfbCtrl"):
            rd = b.index(f"cfg read {reg}")
            assert rd < b.index("hwcReadConfigRegister", rd) < b.index(f"cfg write {reg}") < \
                b.index("hwcWriteConfigRegister", b.index(f"cfg write {reg}"))


def test_chipmask_changes_are_traced_but_bounded():
    """The state validation and texture paths change chipMask too: only a
    real change is traced, and only the first TR_CHIPMASK_MAX per process."""
    g = src("glide3/src/gsst.c")
    b = _func(g, "\n_grChipMask(FxU32 mask)")
    assert "if (trChipMaskLines < TR_CHIPMASK_MAX && hwcTraceOn())" in b
    assert b.index("if(mask != gc->state.shadow.chipMask)") < b.index("trChipMaskLines++")
    assert "LAST one traced" in b
    assert re.search(r"#define TR_CHIPMASK_MAX (\d+)", g)


# ---- every hardware step of the open has its own line --------------------------------

# a step: something that reaches the chips (MMIO, a config cycle, an escape,
# a FIFO command group) or a call made of them
HW_GSST = re.compile(r"\b(REG_GROUP_BEGIN|GR_SET\w*|GR_GET\w*|GR_CAGP_\w+|HWC_\w*(?:LOAD|STORE)\w*|"
                     r"HW_FIFO_PTR|_grChipMask|_grAAOffsetValue|_grEnableSliCtrl|_grRenderMode|"
                     r"assertDefaultState|hwcInitFifo|hwcInitVideo|hwcGammaRGB|hwcShareContextData|"
                     r"clearBuffers|doSplash|grClipWindow|grBufferClear|grBufferSwap|grRenderBuffer|"
                     r"hwcMapBoard|hwcInitRegisters|hwcInitAGPFifo)\s*\(")
HW_HWC = re.compile(r"\b(ExtEscape|HWC_(?:IO|CAGP)_(?:LOAD|STORE)(?:_SLAVE)?|hwcReadConfigRegister|"
                    r"hwcWriteConfigRegister|hwcIdleHardwareWithTimeout)\s*\(")
TRACE = re.compile(r"\bhwcTrace(?:SliAAReq|Cfg)?\s*\(")


def _preprocessed(rel):
    """rel with the preprocessor's conditionals resolved for our build and
    nothing else done: macros unexpanded, comments kept."""
    _need_cc()
    r = subprocess.run([CC] + _flags("c") + ["-E", "-fdirectives-only", str(TREE / rel)],
                       cwd=TREE / "glide3/src", capture_output=True, text=True, errors="replace")
    assert r.returncode == 0, r.stderr[-2000:]
    return r.stdout


def _untraced(text, start, end, hw):
    """Hardware steps between the markers with no trace line since the
    previous one. A run of the SAME macro (e.g. hwcInitFifo's seven
    HWC_CAGP_STOREs) is one step."""
    s = text.index(start)
    e = text.index(end, s)
    region = _blank(text[s:e])
    events = sorted([(m.start(), "hw", m.group(1)) for m in hw.finditer(region)] +
                    [(m.start(), "tr", "") for m in TRACE.finditer(region)])
    bad, traced, prev = [], False, None
    for pos, kind, name in events:
        if kind == "tr":
            traced, prev = True, None
            continue
        if not traced and name != prev:
            bad.append((name, text[s + pos - 80:s + pos + 40].replace("\n", " ")))
        traced, prev = False, name
    return bad


def test_every_hardware_step_of_the_open_is_traced_before_it_runs():
    """From the re-open mapping to the splash (gsst.c), through the SLI/AA
    escape and everything hwcInitVideo does after it, and hwcInitFifo: each
    step that reaches the hardware has its own trace line since the one
    before, so a freeze between two steps names which. Branches our build
    does not compile (HAL_CSIM, DRI, HWC_GDX_INIT) are gone before looking."""
    g = _preprocessed("glide3/src/gsst.c")
    assert "halCfgStore32" not in g                    # the HAL_CSIM branch really went
    assert not _untraced(g, "if (!gc->bInfo->isMapped) {", 'hwcTrace("winopen: done"', HW_GSST)
    cb = g[g.index("clearBuffers( GrGC *gc )"):]
    assert not _untraced(cb, "{", "} /* clearBuffers */", HW_GSST)
    c = _preprocessed("minihwc/minihwc.c")
    assert "_hrmSLIAA" not in c                        # the HWC_GDX_INIT branch really went
    assert not _untraced(c, "hwcTraceSliAAReq(&ctxReq);", 'hwcTrace("hwcInitVideo: done"', HW_HWC)
    f = c[c.index("\nhwcInitFifo(hwcBoardInfo *bInfo, FxBool enableHoleCounting)"):]
    assert not _untraced(f, 'hwcTrace("hwcInitFifo: idle wait', "} /* hwcInitFifo */", HW_HWC)


def test_the_bracket_check_finds_an_untraced_step():
    """The check above must be able to fail: drop one trace line and it names
    the step."""
    _need_cc()
    g = _preprocessed("glide3/src/gsst.c")
    cut = g.replace('hwcTrace("winopen: fifo renderMode", pixelformat, 0);', "", 1)
    assert cut != g
    bad = _untraced(cut, "if (!gc->bInfo->isMapped) {", 'hwcTrace("winopen: done"', HW_GSST)
    assert [b[0] for b in bad] == ["_grRenderMode"]


# ---- the MMX read loop: EMMS before calling out ----------------------------------------


def _mmx_calls(glfb_text, tmp_path, name):
    """[(call offset, emms within the 12 instructions before it)] for every
    call to hwcTrace in grLfbReadRegionOrigin of glfb.c built as the x86/MMX
    variant; plus whether the function has MMX code at all."""
    f = tmp_path / f"{name}.c"
    f.write_text(glfb_text, encoding="latin-1")
    o = tmp_path / f"{name}.o"
    r = subprocess.run([CC] + _flags("x86") + ["-c", str(f), "-o", str(o)],
                       cwd=TREE / "glide3/src", capture_output=True, text=True, errors="replace")
    assert r.returncode == 0, r.stderr[-3000:]
    dis = subprocess.run([OBJDUMP, "-dr", str(o)], capture_output=True, text=True).stdout
    m = re.search(r"^[0-9a-f]+ <_grLfbReadRegionOrigin[^>]*>:\n(.*?)(?=^[0-9a-f]+ <|\Z)", dis,
                  re.M | re.S)
    assert m, "grLfbReadRegionOrigin not found in the object"
    body = m.group(1).splitlines()
    insns = [ln for ln in body if re.match(r"\s+[0-9a-f]+:\t", ln)]
    out = []
    for i, ln in enumerate(body):
        if "_hwcTrace" in ln and re.search(r"R_386_PC32|DISP32", ln):
            call = body[i - 1]
            k = insns.index(call)
            out.append((call.split(":")[0].strip(), any("emms" in x for x in insns[max(0, k - 12):k])))
    has_mmx = any(re.search(r"\bmovq\s+\(%\w+\),%mm\d", ln) for ln in insns)
    return out, has_mmx


def test_the_mmx_read_loop_does_not_call_the_trace_with_live_mmx_state(tmp_path):
    if TREE is None or CC is None or OBJDUMP is None:
        pytest.skip("fork clone / i686 gcc / objdump absent - the MMX loop was NOT checked")
    g = src("glide3/src/glfb.c")
    mac = g[g.index("#define LFB_TRACE_LINE_MMX(p)"):]
    mac = mac[:mac.index("while (0)")]
    assert mac.index("MMX_RESET();") < mac.index("hwcTrace(")
    loop = g[g.index("if(_GlideRoot.CPUType.os_support & _CPU_FEATURE_MMX) {"):]
    assert loop.index("LFB_TRACE_LINE_MMX(src);") < loop.index("MMX_SRCLINE(src, dst_data, len);")
    calls, has_mmx = _mmx_calls(g, tmp_path, "glfb_fixed")
    assert has_mmx and calls
    assert any(emms for _, emms in calls), calls
    # the old macro in the MMX loop (the first AA-TRACE edit): no EMMS
    # anywhere near a trace call - the check tells the two apart
    old = g.replace("LFB_TRACE_LINE_MMX(src);", "LFB_TRACE_LINE(src);", 1)
    calls_old, _ = _mmx_calls(old, tmp_path, "glfb_old")
    assert calls_old and not any(emms for _, emms in calls_old), calls_old


# ---- the DRI build still links ------------------------------------------------------------


def test_the_dri_build_gets_no_op_trace_stubs():
    lin = src("minihwc/linhwc.c")
    for sig in ("int hwcTraceLevel(void)", "FxBool hwcTraceOn(void)",
                "void hwcTrace(const char *step, FxU32 reg, FxU32 val)",
                "void hwcTraceCfg(hwcBoardInfo *bInfo, const char *why)"):
        assert sig in lin, sig
    assert "return 0;" in _func(lin, "int hwcTraceLevel(void)")
    assert "return FXFALSE;" in _func(lin, "FxBool hwcTraceOn(void)")
