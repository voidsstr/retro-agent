"""voodoo-cleanroom ICD 0.1.78: wglGetProcAddress never hands out a stub.

Mesa's _glapi_get_proc_address() SYNTHESIZES a dispatch stub at offset ~0 for
any unknown "gl*" name ("If that never happens, and the user calls this
function, he'll segfault"). Our wglGetProcAddress passed that stub on, so an
application that trusts a non-NULL pointer called into nothing: WON
Half-Life's hw.dll asks for glPNTrianglesiATI (ATI TruForm, enums
0x87F0-0x87F7) and calls it - Deathmatch Classic on .124 (V5 6000,
2026-09-28) died with eip 0x010c01d9, a heap stub, from hw.dll+0x76d3c. With
0.1.78 no Dr. Watson entry. Reads the PATCH, not the gitignored build tree.
"""
import re
from pathlib import Path

PATCH = (Path(__file__).resolve().parents[2] / "voodoo-cleanroom" / "patches"
         / "mesafx-voodoo2-icd.patch").read_text(errors="replace")


def _wgl_added():
    m = re.search(r"^diff --git a/src/mesa/drivers/glide/fxwgl.c .*?(?=^diff --git |\Z)", PATCH, re.S | re.M)
    assert m
    return "\n".join(l[1:] for l in m.group(0).splitlines() if l.startswith("+"))


def test_a_name_with_no_dispatch_slot_answers_null_before_glapi_is_asked():
    src = _wgl_added()
    guard = src.index("if (_glapi_get_proc_offset((const char *) lpszProc) < 0) {")
    tail = src[guard:guard + 120]
    assert "return (NULL);" in tail
    # the guard must come BEFORE the call that would synthesize the stub
    assert guard < src.index("p = (PROC) _glapi_get_proc_address((const char *) lpszProc);")


def test_our_own_wgl_table_still_wins_first():
    # the loop over our own table is patch text (added or context); ours must
    # still be consulted first
    assert PATCH.index("for (i = 0; wgl_ext[i].name; i++) {") < PATCH.index("_glapi_get_proc_offset((const char *) lpszProc)")


def test_the_version_is_0_1_78_or_later():
    added = [int(v) for v in re.findall(r"^\+.*\[voodoo-cleanroom 0\.1\.(\d+)\]", PATCH, re.M)]
    assert added and max(added) >= 78
