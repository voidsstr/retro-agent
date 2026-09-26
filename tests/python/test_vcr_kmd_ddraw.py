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
