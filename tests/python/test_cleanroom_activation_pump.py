"""voodoo-cleanroom ICD 0.1.65: wglCreateContext's activation pump is bounded.

The pump (added for the idTech2 ref_gl activation deadlock) looped
`while (PeekMessage(...)) DispatchMessage(...)` with no bound. WM_PAINT is
synthesised for as long as a window has an update region, so when the DLL is
the SYSTEM ICD and Microsoft's opengl32 subclasses the window too, SDL's
ioquake3 never cleared it and the thread sat in wglCreateContext forever.
Captured with ntsd on .124 (V5 6000) 2026-09-24:
DispatchMessageA(WM_PAINT 0x0f) -> __wglMonitor -> opengl32 hook -> SDL WndProc
-> EndPaint, on two separate attaches. Fixed: WM_PAINT is validated, not
dispatched; the total is capped; a WM_QUIT is handed back. Verified: ioquake3
creates its 1024x768 context and runs; Quake II's staged launcher still does.
Reads the PATCH (the tracked source), not the gitignored build tree.
"""
import re
from pathlib import Path

PATCH = (Path(__file__).resolve().parents[2] / "voodoo-cleanroom" / "patches"
         / "mesafx-voodoo2-icd.patch").read_text(errors="replace")


def _added(path):
    m = re.search(rf"^diff --git a/{re.escape(path)} .*?(?=^diff --git |\Z)", PATCH, re.S | re.M)
    assert m, f"{path} not in the ICD patch"
    return "\n".join(l[1:] for l in m.group(0).splitlines() if l.startswith("+"))


def test_pump_never_dispatches_wm_paint():
    src = _added("src/mesa/drivers/glide/fxwgl.c")
    assert "pumpMsg.message == WM_PAINT" in src
    assert "ValidateRect(pumpMsg.hwnd, NULL);" in src


def test_pump_is_capped_and_hands_back_wm_quit():
    src = _added("src/mesa/drivers/glide/fxwgl.c")
    assert re.search(r"while \(dispatched < 256 && painted < RGL_PUMP_PAINTS &&\s+PeekMessage\(&pumpMsg", src)
    assert "PostQuitMessage((int) pumpMsg.wParam);" in src


def test_a_validated_paint_counts_against_the_pump_0_1_76():
    """0.1.76 (2026-09-28): the WM_PAINT branch `continue`d without counting
    against any bound, so a paint ValidateRect does not end spun the loop -
    Unreal Tournament 436's OpenGLDrv viewport on .124 logged
    `paints-validated=87500081`, 195 s of a 220 s startup. Both loops now stop
    at RGL_PUMP_PAINTS validated paints."""
    src = _added("src/mesa/drivers/glide/fxwgl.c")
    m = re.search(r"#define RGL_PUMP_PAINTS (\d+)", src)
    assert m and 1 <= int(m.group(1)) <= 1024
    outer = re.search(r"for \(pumpI = 0; pumpI < 40 && dispatched < 256 && painted < RGL_PUMP_PAINTS && !quit;", src)
    assert outer, "the outer loop is not bounded by the paint count"
    branch = src[src.index("pumpMsg.message == WM_PAINT"):]
    branch = branch[:branch.index("continue;")]
    assert "painted++;" in branch
    # the old shape - a paint counted nowhere the loops look - is gone
    assert "for (pumpI = 0; pumpI < 40 && dispatched < 256 && !quit; pumpI++)" not in src


def test_the_old_unbounded_loop_is_gone():
    src = _added("src/mesa/drivers/glide/fxwgl.c")
    assert "while (PeekMessage(&pumpMsg, NULL, 0, 0, PM_REMOVE)) {\n           TranslateMessage" not in src
