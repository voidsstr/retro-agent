"""Our h5 Glide's REOPEN-TRACE (fork voidsstr/retro3dfx-glide, branch
fix-unreal-reopen, 2026-09-28) - the steps AA-TRACE left as one line.

Unreal Gold on .124 hung in its second grSstWinOpen, made from inside UE1's
WM_KILLFOCUS handler, with the display still at the desktop mode: the open
never got as far as the DirectDraw mode set taking effect. AA-TRACE named the
whole mode set as ONE step ("hwcInitVideo: setVideoMode"), and the close that
preceded it can take either of two very different paths. These tests pin the
trace that splits them, so the next on-box run names the call:

- win_mode.c: a line BEFORE every DirectDraw call that can block
  (DirectDrawCreate, SetCooperativeLevel, EnumDisplayModes, SetDisplayMode and
  its retries, RestoreDisplayMode, the NORMAL cooperative level, Release), its
  HRESULT after, the 8 bpp detour (Glide res == desktop res) and the window's
  standing (foreground / iconic / visible / focus) at enter and at the
  cooperative-level call;
- gsst.c: the close's entry state and WHICH branch it took (a lost context -
  set by the XP alt-tab hook on a deactivation - skips idle, SLI/AA teardown
  and HWCRLSEXCLUSIVE), read only with the trace on and only from a gc that
  passed the whacked-pointer check; the open's request and board state;
- minihwc.c: the XP alt-tab hook installed / undone / left in the chain, and
  every message it sees that can mark the context lost, plus the focus pair;
- it is all file writes: nothing new runs with FX_GLIDE_TRACE unset but
  hwcTraceOn() (the window-standing helper returns first);
- the three files compile for i686 in both h5 variants.

Reads the fork clone under voodoo-cleanroom/build/ (gitignored; the main tree's
when a worktree has none) and SKIPS loudly when it or the i686 compiler is absent.
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
EDITED = ("minihwc/win_mode.c", "glide3/src/gsst.c", "minihwc/minihwc.c")


def src(rel):
    if TREE is None:
        pytest.skip("retro3dfx-glide clone absent - the REOPEN trace is NOT checked "
                    "(run build-stack.sh)")
    return (TREE / rel).read_text(encoding="latin-1")


def _blank(text):
    """comments and string/char literals blanked, offsets kept"""
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
    s = text.index(header)
    b = _blank(text)
    o = b.index("{", s)
    depth = 0
    for i in range(o, len(b)):
        depth += {"{": 1, "}": -1}.get(b[i], 0)
        if depth == 0:
            return text[o:i + 1]
    raise AssertionError(f"unbalanced braces after {header!r}")


def _before(body, trace, call):
    """the trace line comes before the call, and the call is there"""
    t, c = body.find(trace), body.find(call)
    assert t >= 0, f"no trace {trace!r}"
    assert c >= 0, f"no call {call!r}"
    assert t < c, f"{trace!r} is not written before {call!r}"


# ---- win_mode.c: every DirectDraw call that can block -----------------------------


def test_setvideomode_traces_each_directdraw_call_before_making_it():
    body = _func(src("minihwc/win_mode.c"), "\nsetVideoMode( hwcBoardInfo *bInfo, int refresh )")
    _before(body, '"setVideoMode: enter', "DirectDrawEnumerateExA")
    _before(body, '"setVideoMode: DirectDrawCreate"', "DirectDrawCreate( ddGuid")
    _before(body, '"setVideoMode: SetCooperativeLevel EXCLUSIVE|FULLSCREEN',
            "IDirectDraw2_SetCooperativeLevel(bInfo->lpDD")
    _before(body, '"setVideoMode: EnumDisplayModes"', "IDirectDraw2_EnumDisplayModes(")
    _before(body, '"setVideoMode: SetDisplayMode (reg=', "IDirectDraw2_SetDisplayMode(")
    # the result of each blocking call is traced right after it
    for call, after in (("IDirectDraw2_SetCooperativeLevel(bInfo->lpDD",
                         '"setVideoMode: SetCooperativeLevel returned'),
                        ("IDirectDraw2_EnumDisplayModes(", '"setVideoMode: EnumDisplayModes returned'),
                        ("IDirectDraw2_SetDisplayMode(", '"setVideoMode: SetDisplayMode returned')):
        c = body.index(call)
        assert body.index(after, c) - c < 400, after
    # all three SetDisplayMode calls (the listed mode, its retry, the unlisted
    # mode) have a line before and after
    assert body.count("IDirectDraw2_SetDisplayMode(") == 3
    assert len(re.findall(r'hwcTrace\("setVideoMode: SetDisplayMode[^"]*returned', body)) == 3
    # a failed open says so before it returns
    for m in re.finditer(r"return FXFALSE;", body):
        assert "hwcTrace(" in body[max(0, m.start() - 260):m.start()], body[m.start() - 260:m.start()]


def test_the_8bpp_detour_is_traced_where_it_is_decided():
    body = _func(src("minihwc/win_mode.c"), "\nsetVideoMode( hwcBoardInfo *bInfo, int refresh )")
    hack = body.index("bpp = 8;")
    assert body.rfind('"setVideoMode: Glide res == desktop res - DDraw 8 bpp detour', 0, hack) > \
        body.rfind("if ((devMode.dmPelsWidth", 0, hack)
    assert body.index('"setVideoMode: desktop registry mode') < body.index("bpp = 8;")


def test_resetvideo_traces_the_restore_the_normal_level_and_the_release():
    body = _func(src("minihwc/win_mode.c"), "\nresetVideo( hwcBoardInfo *bInfo )")
    _before(body, '"resetVideo: RestoreDisplayMode', "IDirectDraw2_RestoreDisplayMode(")
    _before(body, '"resetVideo: SetCooperativeLevel NORMAL"', "DDSCL_NORMAL")
    _before(body, '"resetVideo: Release"', "IDirectDraw2_Release(")


def test_the_window_standing_costs_nothing_with_the_trace_off():
    text = src("minihwc/win_mode.c")
    body = _func(text, "\nwinTraceWnd(const char *step, HWND hwnd)")
    code = _blank(body)
    assert code.index("hwcTraceOn()") < code.index("winWndFlags(")
    assert re.search(r"if \(!hwcTraceOn\(\)\)\s*return;", body)
    flags = _func(text, "\nwinWndFlags(HWND hwnd)")
    for api in ("GetForegroundWindow", "IsIconic", "IsWindowVisible", "GetFocus",
                "GetWindowThreadProcessId"):
        assert api in flags, api
    # no message is sent to the window: its thread may be the one that is stuck
    assert not re.search(r"\b(SendMessage|GetWindowText)\w*\s*\(", _blank(flags))


# ---- gsst.c: which close ran, and what the open asked for ------------------------


def test_the_close_says_which_branch_it_took():
    text = src("glide3/src/gsst.c")
    body = _func(text, "GR_ENTRY(grSstWinClose, FxBool, (GrContext_t context))")
    live = body.index('"close: context live - orderly close')
    lost = body.index('"close: context LOST - hwcResetVideo only')
    cond = body.index("if (gc->lostContext && !*gc->lostContext)")
    assert cond < live < body.index("grFlush();")
    assert lost < body.index("hwcResetVideo(gc->bInfo);", lost)
    _before(body, '"close: hwcUnmapMemory9x', "hwcUnmapMemory9x( gc->bInfo );")


def test_the_close_reads_the_gc_only_with_the_trace_on_and_after_its_check():
    body = _func(src("glide3/src/gsst.c"),
                 "GR_ENTRY(grSstWinClose, FxBool, (GrContext_t context))")
    code = _blank(body)
    check = code.index("(gc <= _GlideRoot.GCs + MAX_NUM_SST)")
    enter = body.index('"close: enter')
    assert check < enter
    guard = code.rfind("if (hwcTraceOn())", 0, enter)
    assert check < guard < enter
    assert "*gc->lostContext : 0xffffffffUL" in body[enter:enter + 200]


def test_the_open_traces_its_request_and_whether_it_re_maps():
    # its braces only balance per preprocessor branch: slice to its end marker
    text = src("glide3/src/gsst.c")
    s = text.index("GR_EXT_ENTRY(grSstWinOpenExt, GrContext_t")
    body = text[s:text.index("} /* grSstWinOpenExt */", s)]
    enter = body.index('"winopen: enter (reg=resolution<<16|refresh')
    assert body.index("trSyncInOpen = 1;") < enter < body.index("resolution =\n")
    state = body.index('"winopen: board state (reg=gc->open val=isMapped)"')
    assert "if (gc && hwcTraceOn())" in body[enter:state]


# ---- minihwc.c: the XP alt-tab hook ------------------------------------------------


def test_the_alt_tab_hook_is_traced_in_and_out():
    text = src("minihwc/minihwc.c")
    init = _func(text, "\nhwcInitVideo(hwcBoardInfo *bInfo, FxBool tiled")
    inst = init.index("wpWinProc = (WNDPROC)SetWindowLong(")
    assert init.index('"hwcInitVideo: XP alt-tab hook installed', inst) - inst < 600
    assert '"hwcInitVideo: XP alt-tab hook NOT installed' in init
    assert '"hwcInitVideo: XP alt-tab hook off (FX_GLIDE_ALT_TAB_FIX=0)"' in init
    reset = _func(text, "\nhwcResetVideo(hwcBoardInfo *bInfo)")
    _before(reset, '"hwcResetVideo: XP alt-tab hook undone', "SetWindowLong(")
    assert '"hwcResetVideo: XP alt-tab hook NOT on top - LEFT in the chain' in reset


def test_the_alt_tab_hook_names_the_message_that_loses_the_context():
    text = src("minihwc/minihwc.c")
    proc = _func(text, "static LRESULT CALLBACK _XPAltTabProc(HWND hwnd, UINT uMsg, WPARAM wParam, "
                       "LPARAM lParam)\n{")
    trace = proc.index('"alt-tab hook: message (reg=msg val=wParam)"')
    assert trace < proc.index("*cLostPointer = FXTRUE;")
    sw = proc[:trace]
    assert "if (hwcTraceOn())" in sw
    for m in ("WM_ACTIVATEAPP", "WM_DISPLAYCHANGE", "WM_SIZE", "WM_SHOWWINDOW",
              "WM_STYLECHANGED", "WM_KILLFOCUS", "WM_SETFOCUS", "WM_ACTIVATE"):
        assert "case %s:" % m in sw, m
    # the hook still only marks, and still chains to the proc it replaced
    assert proc.rstrip().endswith("return CallWindowProc(wpWinProc, hwnd, uMsg, wParam, lParam);\n"
                                  "#undef FN_NAME\n}") or \
        "return CallWindowProc(wpWinProc, hwnd, uMsg, wParam, lParam);" in proc


# ---- the tree compiles ---------------------------------------------------------------


def _flags(variant):
    inc = [TREE / "glide3/src", TREE / "incsrc", TREE / "minihwc",
           TREE.parent / "swlibs/fxmisc", TREE.parent / "swlibs/newpci/pcilib",
           TREE.parent / "swlibs/fxmemmap", TREE.parent / "swlibs/texus2/lib"]
    return ["-m32", "-Wall"] + [f"-I{p}" for p in inc] + CDEFS + VARIANTS[variant] + OPT


@pytest.mark.parametrize("variant", sorted(VARIANTS))
@pytest.mark.parametrize("rel", EDITED)
def test_the_reopen_trace_sources_compile_for_i686(rel, variant):
    if TREE is None or CC is None:
        pytest.skip("fork clone or i686-w64-mingw32-gcc absent - the h5 sources were NOT compiled")
    r = subprocess.run([CC] + _flags(variant) + ["-fsyntax-only", str(TREE / rel)],
                       cwd=TREE / "glide3/src", capture_output=True, text=True, errors="replace")
    assert r.returncode == 0, r.stderr[-3000:]
