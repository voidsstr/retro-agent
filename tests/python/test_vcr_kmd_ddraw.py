"""vcr-kmd DirectDraw HAL: the traps that kept XP from using it.

Each of these cost an iteration in the VM test bed (2026-09-26) and each fails
SILENTLY - DirectDraw simply reports DDCAPS_NOHARDWARE and every application
falls back to the HEL (surfaces in system memory, the primary not lockable),
with nothing in any log:

- DDCAPS_GDI in the core caps: XP probes the HAL at PDEV creation, calls
  GetDriverInfo ten times, then switches it off - on every PDEV. Found by
  bisection against VirtualBox's minimal set; XP's own Cirrus driver reports
  BLT | READSCANLINE | BLTCOLORFILL.
- the runtime hands Blt its raster op as 0x00CC0000, not GDI's SRCCOPY
  (0x00CC0020): an equality test declined every copy, and since the HAL
  claims DDCAPS_BLT the application got E_NOTIMPL instead of a HEL fallback.
- a monitor child means XP polls IOCTL_VIDEO_GET_CHILD_STATE.
The primary is an opaque device surface with the drawing calls hooked and
punted to the DIB engine - the shape of the DDK samples and VirtualBox's XPDM
driver, and where 2D acceleration plugs in.
"""
import re
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
KMD = REPO / "voodoo-cleanroom" / "vcr-kmd"
DD = (KMD / "display" / "vcrdd_ddraw.c").read_text()
DDC = (KMD / "display" / "vcrdd.c").read_text()
FLIP_H = (KMD / "include" / "vcr_flip.h").read_text()


def func(src, sig):
    i = src.index(sig)
    return src[i:src.index("\n}\n", i)]


def test_the_core_caps_never_claim_ddcaps_gdi():
    caps = [ln for ln in DD.splitlines()
            if "ddCaps.dwCaps =" in ln or "ddCaps.dwCaps |=" in ln]
    assert caps, "the HAL sets no core caps"
    assert all("DDCAPS_GDI" not in ln for ln in caps), caps


def test_the_device_advertises_directdraw():
    assert "di->flGraphicsCaps |= GCAPS_DIRECTDRAW;" in DDC


def test_srccopy_is_matched_by_the_rop_byte():
    blt = func(DD, "static DWORD APIENTRY Dd_Blt(")
    assert "((p->bltFX.dwROP >> 16) & 0xff) == 0xcc" in blt
    assert "dwROP == SRCCOPY" not in blt


def test_a_blit_we_cannot_do_goes_back_to_the_hel():
    blt = func(DD, "static DWORD APIENTRY Dd_Blt(")
    # stretch, format change, clip lists, system memory: NOTHANDLED, never an
    # error. The one non-OK answer is "still drawing" (a pending flip), which
    # DDBLT_WAIT retries.
    assert blt.count("return DDHAL_DRIVER_NOTHANDLED;") >= 5
    assert set(re.findall(r"DDERR_\w+", blt)) <= {"DDERR_WASSTILLDRAWING"}


def test_the_primary_is_a_hooked_device_surface():
    enable = func(DDC, "HSURF APIENTRY DrvEnableSurface(")
    assert "EngCreateDeviceSurface(" in enable
    assert "EngAssociateSurface(hs, pd->hdevEng, VCRDD_HOOKS)" in enable
    assert "EngLockSurface(pd->hsurfBits)" in enable      # the bitmap every hook draws on
    # every HOOK_ flag has its Drv function in the entry table
    hooks = re.search(r"#define VCRDD_HOOKS \((.*?)\)\n", (KMD / "display" / "vcrdd.h").read_text(),
                      re.S).group(1)
    names = {"BITBLT": "BitBlt", "COPYBITS": "CopyBits", "TEXTOUT": "TextOut",
             "STROKEPATH": "StrokePath", "FILLPATH": "FillPath", "LINETO": "LineTo",
             "STRETCHBLT": "StretchBlt", "STRETCHBLTROP": "StretchBltROP",
             "ALPHABLEND": "AlphaBlend", "GRADIENTFILL": "GradientFill",
             "TRANSPARENTBLT": "TransparentBlt"}
    for flag in re.findall(r"HOOK_(\w+)", hooks):
        assert f"INDEX_Drv{names[flag]}," in DDC, flag
        assert f"(PFN)Drv{names[flag]};" in DDC, flag


def test_the_miniport_answers_what_directdraw_and_the_monitor_ask():
    mp = (KMD / "miniport" / "vcrmp.c").read_text()
    child = mp[mp.index("case IOCTL_VIDEO_GET_CHILD_STATE:"):]
    child = child[:child.index("break;")]
    assert "VIDEO_CHILD_ACTIVE" in child
    assert child.index("rp->InputBuffer") < child.index("rp->OutputBuffer")
    dd = (KMD / "miniport" / "vcrmp_dd.c").read_text()
    share = dd[dd.index("case IOCTL_VIDEO_SHARE_VIDEO_MEMORY:"):]
    share = share[:share.index("return NO_ERROR;")]
    assert share.index("VideoPortMoveMemory(&req, in") < share.index("res->")


def test_microsofts_ddi_headers_are_not_committed():
    """They come from a local DDK at build time (Makefile HAVE_DDI)."""
    mk = (KMD / "Makefile").read_text()
    assert "HAVE_DDI" in mk and "$(OUT)/ddi/.stamp" in mk
    stub = (KMD / "compat" / "ddrawint.h").read_text()
    assert "VCR_DDRAWINT_STUB" in stub and "DD_HALINFO\n" not in stub


def test_a_novsync_flip_does_not_wait_for_the_retrace():
    """D3D's PRESENT_INTERVAL_IMMEDIATE reaches the HAL as DDFLIP_NOVSYNC:
    held pending like a vsync'd flip, d3dprobe perf --novsync ran at exactly
    the refresh on .124 (85.0 fps at 85 Hz, 2026-09-26). A NOVSYNC flip is
    neither refused while the previous one is in flight nor left pending."""
    import re as _re
    src = (KMD / "display" / "vcrdd_ddraw.c").read_text()
    body = src[src.index("static DWORD APIENTRY Dd_Flip("):src.index("static DWORD APIENTRY Dd_GetFlipStatus(")]
    assert "int novsync = (p->dwFlags & DDFLIP_NOVSYNC) != 0;" in body
    assert _re.search(r"if \(!novsync && !flip_done\(pd\)\) \{", body)
    assert "vcr_flip_begin(&pd->flip, novsync," in body
    # the rule is include/vcr_flip.h's: a NOVSYNC flip is never pending
    # (tests/native/test_vcr_kmd_flip.c a_novsync_flip_is_never_pending)
    begin = func(FLIP_H, "static inline void vcr_flip_begin(")
    assert "s->pending = novsync ? 0 : 1;" in begin
    assert "s->pending = 1;" not in begin
    # and the HAL SAYS so - the runtime sends DDFLIP_NOVSYNC to no other
    # (the first fix alone left perf --novsync at exactly 85.0 fps on .124)
    assert "hal->ddCaps.dwCaps2 = DDCAPS2_FLIPNOVSYNC;" in src


# ---- flip completion hardening (2026-09-27; include/vcr_flip.h) ----------------------
# The .124 16 bpp flip half rate (46.9 / 42.1 flips/s at 85 Hz) is BELOW the
# floor flip_done's own deadline guarantees (~77 flips/s even if every retrace
# were missed), so it was not the completion rule; the rule was factored into a
# pure header, instrumented, and pinned by tests/native/test_vcr_kmd_flip.c.
# The scanline-based proposal (P1/P2) was rejected: see the header.


def _strip_c_comments(src):
    return re.sub(r"/\*.*?\*/|//[^\n]*", "", src, flags=re.S)


def test_the_flip_rule_is_a_pure_header():
    """Win32-free, so the host test compiles the code the DLL runs."""
    includes = re.findall(r'#include\s+[<"]([^>"]+)[>"]', FLIP_H)
    assert includes == ["vcr_types.h"], includes
    code = _strip_c_comments(FLIP_H)
    for w in ("LONGLONG", "EngQuery", "VcrIoctl", "IOCTL_", "float", "double"):
        assert w not in code, w
    assert '#include "../include/vcr_flip.h"' in (KMD / "display" / "vcrdd.h").read_text()
    assert "vcr_flip_state flip;" in (KMD / "display" / "vcrdd.h").read_text()


def test_vidcurrentline_never_reaches_flip_completion():
    """86Box reads it as 0x7ff; on the VSA-100 it very probably reads 0 through
    the blank. A rule built on it is always-in-blank in the bed (every flip by
    deadline) and a no-op or early on silicon."""
    code = _strip_c_comments(FLIP_H).lower()
    assert "scanline" not in code and "vidcurrentline" not in code
    for sig in ("static int flip_done(", "static int flip_sample(",
                "static DWORD APIENTRY Dd_Flip("):
        assert "scanline" not in _strip_c_comments(func(DD, sig)), sig


def test_the_retrace_state_is_read_after_the_start_address_write():
    """Sampled after the write, a write racing into a retrace is reported a
    frame LATE; sampled before, a frame EARLY (the native test shows both)."""
    flip = _strip_c_comments(func(DD, "static DWORD APIENTRY Dd_Flip("))
    w = flip.index("rc = flip_to(pd, off);")
    s = flip.index("have = flip_sample(pd, &v, &now);")
    b = flip.index("vcr_flip_begin(&pd->flip, novsync, have, have && v.in_vblank, now,")
    assert w < s < b
    assert "vblank(pd" not in flip[:w]          # no retrace read before the write
    sample = func(DD, "static int flip_sample(")
    # the clock AFTER the read is what the rule compares; the read is timed
    assert sample.index("LONGLONG a = qpc();") < sample.index("vblank(pd, v)") \
        < sample.index("*now = qpc();") < sample.index("vcr_flip_note_poll(")


def test_getscanline_never_answers_garbage():
    """ddlab read 2293576 (stack garbage) from our GetScanLine where XP's own
    driver said 0: dwScanLine was never set on the vertical-blank path."""
    gsl = func(DD, "static DWORD APIENTRY Dd_GetScanLine(")
    assert gsl.index("p->dwScanLine = 0;") < gsl.index("p->ddRVal")
    # a line at or past the visible height (86Box: 0x7ff every read) is the blank
    assert "} else if (v.in_vblank || v.scanline >= pd->cy) {" in gsl
    blank = gsl[gsl.index("v.scanline >= pd->cy"):]
    assert blank.index("DDERR_VERTICALBLANKINPROGRESS") < blank.index("p->dwScanLine = v.scanline;")


def test_the_achieved_refresh_deadline_is_off_by_default():
    """frame + 1/32 of the ACHIEVED rate is proven only in the model: until the
    counters show how often the deadline decides on real titles, the default
    stays the nominal 1/8 rule the silicon runs were measured with."""
    hw = (KMD / "miniport" / "vcrmp_hw.c").read_text()
    setm = func(hw, "VP_STATUS VcrHwSetMode(")
    assert re.search(r'x->dd_refresh_mhz = x->backend == VCR_HW_VOODOO && '
                     r'VcrDiagGet\(L"FlipDeadline", 0\)\s*\? m\.refresh_mhz : 0;', setm)
    assert "x->dd_refresh_mhz = 0;" in func(hw, "void VcrHwResetToVga(")
    mpdd = (KMD / "miniport" / "vcrmp_dd.c").read_text()
    vb = mpdd[mpdd.index("case IOCTL_VCR_VBLANK:"):]
    vb = vb[:vb.index("return NO_ERROR;")]
    assert "v->refresh_mhz = x->dd_refresh_mhz;" in vb
    flip = func(DD, "static DWORD APIENTRY Dd_Flip(")
    assert "vcr_flip_deadline(f, have ? v.refresh_mhz : 0, pd->freq)" in flip
    # the IOCTL word is the old `reserved`: same size, same offset
    ioctl = (KMD / "include" / "vcr_ioctl.h").read_text()
    vbs = ioctl[ioctl.index("typedef struct vcr_dd_vblank {"):ioctl.index("} vcr_dd_vblank;")]
    fields = re.findall(r"vcr_u32 (\w+);", vbs)
    assert fields == ["in_vblank", "scanline", "scan_offset", "refresh_mhz"], fields


def test_flip_counters_are_logged_when_exclusive_mode_ends():
    excl = func(DD, "static DWORD APIENTRY Dd_SetExclusiveMode(")
    assert "if (!p->dwEnterExcl)\n        flip_stats_log(pd);" in excl
    log = func(DD, "static void flip_stats_log(")
    for f in ("s->by_retrace", "s->by_deadline", "s->superseded", "s->max_poll",
              "s->max_wait", "vcr_flip_stats_reset(s);"):
        assert f in log, f
    # every flip-completion read is counted and timed
    assert "flip_sample(pd, &v, &now)" in func(DD, "static int flip_done(")


def test_ddlab_reports_the_frames_and_takes_work_us():
    lab = (KMD / "tools" / "ddlab.c").read_text()
    for k in ("first_frame_ms", "max_frame_ms", "slow_frames", "flips_s_first_last",
              "work_us"):
        assert f'\\"{k}\\":' in lab, k
    assert '!strcmp(a, "--work-us")' in lab
    # the busy work sits right after the Flip, before any other DirectDraw call
    loop = lab[lab.index("IDirectDrawSurface7_Flip(prim, NULL, DDFLIP_WAIT);"):]
    assert loop.index("busy_us(g_work_us);") < loop.index("IDirectDrawSurface7_Lock(")
    run = (KMD / "tools" / "ddlab_run.py").read_text()
    assert 'ap.add_argument("--work-us"' in run
    import sys
    sys.path.insert(0, str(KMD / "tools"))
    sys.path.insert(0, str(REPO))
    import argparse
    import ddlab_run
    ns = argparse.Namespace(mode="flip", res="800x600", bpp=16, frames=60, timeout=120)
    # today's command line, byte for byte, unless --work-us is asked for
    assert ddlab_run.lab_args(ns, "L") == "flip --res 800x600 --bpp 16 --frames 60 --log L"
    ns.work_us = 17400
    assert ddlab_run.lab_args(ns, "L") == ("flip --res 800x600 --bpp 16 --frames 60 "
                                           "--work-us 17400 --log L")
    assert ddlab_run.work_refusal(ns) is None
    ns.frames = 10000                        # 174 s of busy work in a 120 s budget
    assert ddlab_run.work_refusal(ns)
    ns.frames, ns.work_us = 60, -1
    assert ddlab_run.work_refusal(ns)
