"""voodoo-cleanroom ICD 0.1.63: the DLL is also a Microsoft ICD.

Before 0.1.63 our MesaFX DLL could only be reached by games that load an
OpenGL library by name (Quake II gl_driver, Quake III r_glDriver). Everything
that links the system opengl32.dll - Counter-Strike 1.6 / GoldSrc, UT99's
OpenGLDrv - never saw it: opengl32 is a KnownDLL on XP, so a game-local copy
is ignored, and Microsoft's opengl32 only talks to an ICD through Drv* entry
points and the dispatch table DrvSetContext returns.

0.1.63 adds src/mesa/drivers/glide/fxicd.c (carried in
voodoo-cleanroom/patches/mesafx-voodoo2-icd.patch). Verified on .124 (V5 6000)
2026-09-24 with the DLL registered under OpenGLDrivers\\3dfx: CS 1.6 ran at
119.9 fps (640x480, 1 chip) and UT99 OpenGLDrv - which GPFs at init on
AmigaMerlin's own ICD - ran at 58.3 fps (800x600, 1 chip).
"""
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
PATCH = ROOT / "voodoo-cleanroom" / "patches" / "mesafx-voodoo2-icd.patch"

# Every entry point Microsoft's opengl32 resolves from an ICD on XP.
DRV = ["DrvCopyContext", "DrvCreateContext", "DrvCreateLayerContext",
       "DrvDeleteContext", "DrvDescribeLayerPlane", "DrvDescribePixelFormat",
       "DrvGetLayerPaletteEntries", "DrvGetProcAddress", "DrvRealizeLayerPalette",
       "DrvReleaseContext", "DrvSetContext", "DrvSetLayerPaletteEntries",
       "DrvSetPixelFormat", "DrvShareLists", "DrvSwapBuffers",
       "DrvSwapLayerBuffers", "DrvValidateVersion"]


def _file_hunk(name):
    text = PATCH.read_text(errors="replace")
    m = re.search(rf"^diff --git a/{re.escape(name)} .*?(?=^diff --git |\Z)", text, re.S | re.M)
    assert m, f"{name} is not in the ICD patch"
    return m.group(0)


def test_every_drv_entry_point_is_defined_and_exported():
    src = _file_hunk("src/mesa/drivers/glide/fxicd.c")
    deff = _file_hunk("src/mesa/drivers/glide/fxopengl.def")
    for d in DRV:
        assert re.search(rf"^\+DRV .*APIENTRY {d}\(", src, re.M), f"{d} not defined"
        assert re.search(rf"^\+ {d}$", deff, re.M), f"{d} not exported by the .def"


def test_the_dispatch_table_is_microsofts_336_entries_from_mesas_list():
    src = _file_hunk("src/mesa/drivers/glide/fxicd.c")
    assert "PROC  table[336]" in src and "{ 336, {" in src
    assert '#include "../windows/icd/icdlist.h"' in src


def test_drvsetcontext_returns_the_table_only_when_the_context_binds():
    src = _file_hunk("src/mesa/drivers/glide/fxicd.c")
    body = src.split("DrvSetContext(", 1)[1].split("\n+}", 1)[0]
    assert "wglMakeCurrent(hdc, rc)" in body
    assert "return NULL" in body and "return &icdTable" in body


def test_fxicd_is_built_into_the_fx_driver():
    mk = _file_hunk("src/mesa/Makefile.mgw")
    assert "+\tdrivers/glide/fxicd.c \\" in mk
