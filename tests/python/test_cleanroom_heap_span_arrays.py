"""voodoo-cleanroom ICD 0.1.66: Mesa's per-span scratch arrays live on the heap.

The stack variant of DEFARRAY/DEFMARRAY made _mesa_unpack_color_span_chan and
its pack/float siblings 80 KB stack frames (MAX_WIDTH 4096). A Quake II-engine
game already holds ~768 KB of texture scratch on its 1 MB main-thread stack
when it calls glTexImage2D, so SiN Gold died of a stack overflow in
___chkstk_ms under fxDDTexImage2D -> _mesa_texstore_argb8888 ->
_mesa_make_temp_chan_image -> _mesa_unpack_color_span_chan (Dr Watson, .124,
2026-09-24; very likely README bug I12). Mesa's own heap variant (written for
the classic Mac 32 KB stack) is now selected for the Win32 FX build, and the two
s_texture.c crossbar early returns free the array. Verified: SiN runs.
"""
import re
import shutil
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
PATCH = (ROOT / "voodoo-cleanroom" / "patches" / "mesafx-voodoo2-icd.patch").read_text(errors="replace")


def _hunk(path):
    m = re.search(rf"^diff --git a/{re.escape(path)} .*?(?=^diff --git |\Z)", PATCH, re.S | re.M)
    assert m, f"{path} not in the ICD patch"
    return m.group(0)


def test_win32_fx_build_selects_the_heap_variant():
    h = _hunk("src/mesa/main/imports.h")
    assert "+#elif defined(__BEOS__) || (defined(FX) && defined(__WIN32__))" in h


def test_crossbar_returns_free_the_heap_array():
    s = _hunk("src/mesa/swrast/s_texture.c")
    assert s.count("+                  UNDEFARRAY(ccolor);") == 2


def test_every_def_site_frees_on_every_return():
    """Mirror of the audit done for the change: in the patched sources every
    DEF*ARRAY block must reach UNDEFARRAY before each return."""
    tree = next((p for p in (ROOT / "voodoo-cleanroom/build/retro3dfx-gl/src/mesa",
                             Path("/home/voidsstr/development/retro-agent/voodoo-cleanroom/build/retro3dfx-gl/src/mesa"))
                 if p.is_dir()), None)
    if tree is None:
        pytest.skip("retro3dfx-gl clone absent - DEF/UNDEF balance NOT checked")
    for f in ("main/image.c", "swrast/s_readpix.c", "swrast/s_copypix.c", "swrast/s_texture.c"):
        L = (tree / f).read_text(errors="replace").split("\n")
        for i, line in enumerate(L):
            m = re.search(r"\bDEFM?N?ARRAY\(\w+,\s*(\w+)", line)
            if not m or "#" in line.split("DEF")[0]:
                continue
            name = m.group(1)
            d, start = 0, i
            for k in range(i, -1, -1):
                d += L[k].count("}") - L[k].count("{")
                if d < 0:
                    start = k
                    break
            d = 0
            end = start
            for k in range(start, len(L)):
                d += L[k].count("{") - L[k].count("}")
                if d == 0 and k > start:
                    end = k
                    break
            for k in range(i + 1, end + 1):
                if re.search(r"\breturn\b", L[k]) and "CHECKARRAY" not in L[k] and not L[k].strip().startswith("/*"):
                    window = "\n".join(L[max(i, k - 3):k])
                    assert f"UNDEFARRAY({name})" in window, f"{f}:{k+1} returns without UNDEFARRAY({name})"


def test_built_icd_has_no_80k_span_frames():
    dll = next((p for p in (ROOT / "voodoo-cleanroom/out/opengl32_retail.dll",
                            Path("/home/voidsstr/development/retro-agent/voodoo-cleanroom/out/opengl32_retail.dll"))
                if p.exists()), None)
    if dll is None or not shutil.which("i686-w64-mingw32-objdump"):
        pytest.skip("no built ICD or objdump - artifact NOT checked")
    dis = subprocess.run(["i686-w64-mingw32-objdump", "-d", "--no-show-raw-insn", str(dll)],
                         capture_output=True, text=True).stdout
    m = re.search(r"<__mesa_unpack_color_span_chan>:\n((?:.*\n){0,30})", dis)
    assert m
    frame = max([int(x, 16) for x in re.findall(r"mov\s+\$0x([0-9a-f]+),%eax", m.group(1))] + [0])
    assert frame < 32 * 1024, f"_mesa_unpack_color_span_chan frame {frame} B (the SiN overflow was 81,980)"
