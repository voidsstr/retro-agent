"""ICD 0.1.84: Glide opens on the drawable's TOP-LEVEL window.

Glide takes the screen with DirectDraw exclusive mode, and
SetCooperativeLevel(DDSCL_EXCLUSIVE|DDSCL_FULLSCREEN) refuses a WS_CHILD with
DDERR_INVALIDPARAMS (0x80070057). Serious Sam TFE draws into a child of its
own window when ogl_bExclusive is 0 (style 0x56000000), and until 0.1.84 it
then got no context at all and exited ~20 s in (.124, 2026-10-03, measured
with an instrumented debug Glide). From 0.1.84 wglCreateContext hands Glide
GetAncestor(hWnd, GA_ROOT) for a child, and keeps the drawable for everything
else. Verified on .124: TFE forced to ogl_bExclusive=0 ran its demo at 79.1 fps
(evidence gametune_1001/serioussam_preset/tfe_child_canvas_icd0184/).

The fork is a gitignored clone under voodoo-cleanroom/build/; without it these
checks SKIP, loudly."""
import re
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
WGL = ROOT / "voodoo-cleanroom" / "build" / "retro3dfx-gl" / "src" / "mesa" / "drivers" / "glide" / "fxwgl.c"


def _src():
    if not WGL.exists():
        pytest.skip(f"ICD fork not cloned at {WGL} - the child-window checks did NOT run")
    return WGL.read_text(encoding="latin-1")


def _fn(src, name):
    i = src.index(name + "(HWND hWnd)")
    return src[i:src.index("\n}\n", i)]


def test_a_child_drawable_gives_glide_its_top_level_window():
    f = _fn(_src(), "rgl_glide_window")
    assert "GetWindowLong(hWnd, GWL_STYLE) & WS_CHILD" in f
    assert "GetAncestor(hWnd, GA_ROOT)" in f
    # a top-level drawable is passed through untouched, and a failed lookup
    # falls back to the drawable rather than to NULL
    assert "return hWnd;" in f and "return root ? root : hWnd;" in f


def test_only_glide_gets_the_root_the_drawable_keeps_everything_else():
    src = _src()
    i = src.index("wglCreateContext(HDC hdc)\n{")
    body = src[i:src.index("\n}\n", i)]
    assert "hGlideWnd = rgl_glide_window(hWnd);" in body
    calls = re.findall(r"fxMesaCreateBestContext\(\(GLuint\) (\w+),", body)
    assert calls == ["hGlideWnd"], calls
    # the size, the subclassing and the current-window bookkeeping stay the drawable's
    assert "GetClientRect(hWnd, &cliRect);" in body
    assert "SetWindowLong(hWnd, GWL_WNDPROC, (LONG) __wglMonitor);" in body
    assert "hWND = hWnd;" in body
    # and the switch is logged where a failure would be diagnosed
    assert "is a WS_CHILD (style %08lx) -> Glide gets its" in body
