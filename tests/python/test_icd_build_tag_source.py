"""ICD 0.1.83: the build tag lives in GL_VERSION, never in GL_RENDERER.

id Tech 3 (r_lastValidRenderer) and Torque ($pref::Video::profiledRenderer)
remember the renderer string and drop to their low graphics preset whenever it
changes - so "[voodoo-cleanroom 0.1.N]" in GL_RENDERER reset Quake III, RtCW,
SoF II, Jedi Academy and Tribes 2 after EVERY driver update (.124,
2026-10-02, scripts/benchmarks/v56k_tune.py had to re-pin them each time).
From 0.1.83 GL_RENDERER is "Mesa Glide v0.62 <board>" for every build and
GL_VERSION is Mesa's own answer with the tag after it.

The fork is a gitignored clone under voodoo-cleanroom/build/; on a checkout
without it the fork checks SKIP, loudly. The build script is in this repo."""
import re
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
GLIDE = ROOT / "voodoo-cleanroom" / "build" / "retro3dfx-gl" / "src" / "mesa" / "drivers" / "glide"
BUILD = ROOT / "voodoo-cleanroom" / "build-mesafx-retail.sh"


def _fork(name):
    p = GLIDE / name
    if not p.exists():
        pytest.skip(f"ICD fork not cloned at {p} - the build-tag source checks did NOT run")
    return p.read_text(encoding="latin-1")


def test_the_renderer_string_carries_no_build_tag():
    src = _fork("fxapi.c")
    fmt = re.findall(r'sprintf\(fxMesa->rendererString,\s*"([^"]*)"', src)
    assert fmt == ["Mesa %s v0.62 %s%s"], fmt
    assert "voodoo-cleanroom" not in src


def test_gl_version_is_mesas_answer_plus_the_tag():
    src = _fork("fxdd.c")
    tags = re.findall(r'static const char rgl_build_tag\[\] = "(\[voodoo-cleanroom \d+\.\d+\.\d+\])";', src)
    assert len(tags) == 1, "rgl_build_tag is missing or duplicated"
    m = re.search(r"case GL_VERSION:\s*\{(.*?)\n        \}", src, re.S)
    assert m, "fxDDGetString no longer answers GL_VERSION"
    body = m.group(1)
    # Mesa's own answer first (a game parses the leading major.minor), the tag after
    assert "_mesa_GetString(GL_VERSION)" in body and "rgl_build_tag" in body
    assert re.search(r'_mesa_sprintf\(\(char \*\)version, "%s %s"', body)
    # the re-entry guard: without it _mesa_GetString -> Driver.GetString recurses
    assert re.search(r"if \(asking\)\s*return NULL;", body)
    assert body.index("asking = 1;") < body.index("_mesa_GetString(GL_VERSION)") < body.index("asking = 0;")


def test_the_build_refreshes_the_tag_where_gl_version_reads_it():
    sh = BUILD.read_text()
    assert 'FXDD="$GLTREE/src/mesa/drivers/glide/fxdd.c"' in sh
    assert 'sed -i "s/\\[voodoo-cleanroom [0-9.]*\\]/[voodoo-cleanroom $DRVVER]/" "$FXDD"' in sh
    assert '! grep -q "voodoo-cleanroom" "$FXAPI"' in sh, "the build must refuse a tag left in the renderer string"
    assert "[voodoo-cleanroom $DRVVER]\\\"/\" \"$FXAPI\"" not in sh, "the old renderer-string inject is back"
