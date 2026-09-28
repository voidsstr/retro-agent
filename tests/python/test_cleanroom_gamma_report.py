"""voodoo-cleanroom ICD 0.1.77: wglGetDeviceGammaRamp3DFX reports the DAC.

Until 0.1.76 the WGL_3DFX_gamma_control Get returned a static table that
stayed ZERO-filled until the game's first Set, while the DAC really held our
FX_GAMMA ramp (fxapi.c) or Glide's identity. id Tech 3 saves what Get returns
as the "original" ramp and loads it back when the renderer shuts down -
Soldier of Fortune II MP: sof2mp.exe 0x4de050 saves, 0x4de330 (first thing in
GLimp_Shutdown) restores - so every vid_restart and every quit loaded an
all-zero CLUT. Found 2026-09-28 reading sof2mp.exe while chasing SoF2's dark
picture on the V5 6000 (.124); not the cause of that darkness, which is the
engine's r_overBrightBits 0 default (staged autoexec, Games-Library).

Reads the PATCH (the tracked source), not the gitignored build tree. The
arithmetic of the contract is pinned separately in
tests/native/test_icd_gamma_report.c.
"""
import re
from pathlib import Path

PATCH = (Path(__file__).resolve().parents[2] / "voodoo-cleanroom" / "patches"
         / "mesafx-voodoo2-icd.patch").read_text(errors="replace")


def _added(path):
    m = re.search(rf"^diff --git a/{re.escape(path)} .*?(?=^diff --git |\Z)", PATCH, re.S | re.M)
    assert m, f"{path} not in the ICD patch"
    return "\n".join(l[1:] for l in m.group(0).splitlines() if l.startswith("+"))


def test_get_reports_identity_before_any_ramp_is_known():
    src = _added("src/mesa/drivers/glide/fxwgl.c")
    assert "static GLboolean gammaTableKnown = GL_FALSE;" in src
    assert "if (!gammaTableKnown) {" in src
    assert "(GLushort)(i * 0x101)" in src, "identity is v -> v*0x101, never zeros"


def test_every_ramp_the_icd_loads_is_recorded():
    wgl = _added("src/mesa/drivers/glide/fxwgl.c")
    assert re.search(r"^fxWglNoteGamma\(int n, const FxU32 \*r, const FxU32 \*g, const FxU32 \*b\)", wgl, re.M)
    assert "k = i * n / 256;" in wgl, "8-bit Glide entry i*n/256 -> WGL entry i"
    assert "gammaTable[i]       = (GLushort)((r[k] & 0xff) * 0x101);" in wgl
    api = _added("src/mesa/drivers/glide/fxapi.c")
    # both places the ICD itself loads a ramp: the FX_GAMMA default at create,
    # identity at destroy
    assert api.count("fxWglNoteGamma(n, rr, gg, bb);") == 2
    assert "extern void fxWglNoteGamma(" in _added("src/mesa/drivers/glide/fxdrv.h")


def test_set_marks_the_table_known():
    # the memcpy is context in the hunk; the added line must follow it
    assert re.search(r"^ \s*memcpy\(gammaTable, arrays, 3\*256\*sizeof\(GLushort\)\);\n"
                     r"\+\s*gammaTableKnown = GL_TRUE;$", PATCH, re.M)


def test_the_version_is_0_1_77_or_later():
    m = re.search(r"\[voodoo-cleanroom 0\.1\.(\d+)\]", _added("src/mesa/drivers/glide/fxapi.c"))
    assert m and int(m.group(1)) >= 77
