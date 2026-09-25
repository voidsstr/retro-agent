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
    assert "dispatched < 256 && PeekMessage(&pumpMsg" in src
    assert "PostQuitMessage((int) pumpMsg.wParam);" in src


def test_the_old_unbounded_loop_is_gone():
    src = _added("src/mesa/drivers/glide/fxwgl.c")
    assert "while (PeekMessage(&pumpMsg, NULL, 0, 0, PM_REMOVE)) {\n           TranslateMessage" not in src
