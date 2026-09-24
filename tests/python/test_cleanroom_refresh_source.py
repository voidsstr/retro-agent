"""The 0.1.34 fullscreen-refresh fix must exist in SOURCE, not only in a test.

0.1.34 (monitor-max refresh) was lost from every source while its test,
tests/native/test_fx_best_refresh.c, kept passing - it mirrors the logic
rather than reading it, so it could not notice (README §15.4, bug I1). 0.1.64
re-implements fxBestRefresh() in fxapi.c, carried in the ICD patch; this test
reads the PATCH so the fix cannot silently vanish again, and pins the snap
table to the one the native test mirrors.
"""
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
PATCH = (ROOT / "voodoo-cleanroom" / "patches" / "mesafx-voodoo2-icd.patch").read_text(errors="replace")
NATIVE = (ROOT / "tests" / "native" / "test_fx_best_refresh.c").read_text()


def _added_fxapi():
    m = re.search(r"^diff --git a/src/mesa/drivers/glide/fxapi.c .*?(?=^diff --git |\Z)", PATCH, re.S | re.M)
    assert m, "fxapi.c is not in the ICD patch"
    return "\n".join(l[1:] for l in m.group(0).splitlines() if l.startswith("+"))


def test_best_context_no_longer_hardcodes_60hz():
    src = _added_fxapi()
    assert "fxBestRefresh(width, height)" in src
    assert "return fxMesaCreateContext(win, res, ref, attribList);" in src
    assert "fxMesaCreateContext(win, res, GR_REFRESH_60Hz, attribList)" not in src


def test_snap_table_matches_the_native_mirror():
    pairs = lambda t: re.findall(r"\{(\d+),\s*GR_REFRESH_(\d+)Hz\}", t)
    src = pairs(_added_fxapi())
    native = pairs(NATIVE)
    assert src and src == native, (src, native)
    assert src[0] == ("120", "120") and src[-1] == ("60", "60")


def test_env_override_and_driver_sentinels():
    src = _added_fxapi()
    for var in ("FX_GLIDE_REFRESH_RATE", "SSTV2_REFRESH_RATE", "MESA_FX_REFRESH"):
        assert f'getenv("{var}")' in src
    assert "dmDisplayFrequency > 1" in src    # 0/1 Hz are "driver default", never a rate
