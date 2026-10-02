"""The ICD fork's rgl_color_buffers_for() must match tests/native/test_icd_color_buffers.c,
and both board-open paths must pass its answer (not a literal 2) to Glide.

The fork is a gitignored clone under voodoo-cleanroom/build/; on a checkout
without it this SKIPS, loudly - it cannot check what is not there."""
import re
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
FXAPI = ROOT / "voodoo-cleanroom" / "build" / "retro3dfx-gl" / "src" / "mesa" / "drivers" / "glide" / "fxapi.c"
MIRROR = ROOT / "tests" / "native" / "test_icd_color_buffers.c"


def _body(src, name):
    m = re.search(r"static int\s+" + name + r"\s*\([^)]*\)\s*\{(.*?)\n\}", src, re.S)
    assert m, name + " not found"
    return re.sub(r"\s+", " ", re.sub(r"/\*.*?\*/", "", m.group(1), flags=re.S)).strip()


def test_fork_matches_the_native_mirror():
    if not FXAPI.exists():
        pytest.skip(f"ICD fork not cloned at {FXAPI} - nothing to compare")
    fork = FXAPI.read_text(encoding="latin-1")
    mirror = MIRROR.read_text(encoding="latin-1")
    assert _body(fork, "rgl_color_buffers_for") == _body(mirror, "rgl_color_buffers_for")
    opens = re.findall(r"fxMesa->glideContext\s*=\s*(?:Glide->)?grSstWinOpen(?:Ext)?\(([^;]*?)\);", fork, re.S)
    assert len(opens) == 2, f"expected the two fullscreen board opens, found {len(opens)}"
    assert all(re.search(r"\bncol,\s*aux\s*$", o.strip()) for o in opens), \
        "a board open passes a literal buffer count"
