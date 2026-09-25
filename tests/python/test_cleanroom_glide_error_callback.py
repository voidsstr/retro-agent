"""voodoo-cleanroom ICD 0.1.67: a fatal Glide error is logged, never a hidden dialog.

The intermittent Quake III "hang in grGlideInit" on the V5 6000 (.124,
2026-09-25) was Glide's default error callback: MessageBox(NULL, ...) behind the
game's fullscreen window, which nobody can see or dismiss. ntsd put the only
thread in USER32!MessageBoxA called from glide3x!_grErrorDefaultCallback. The
ICD now installs its own callback before grGlideInit; it must be cdecl
(GrErrorCallbackFnc_t carries no FX_CALL - a stdcall callback would unbalance
the stack on every error) and it must return rather than exit.
"""
import re
from pathlib import Path

PATCH = Path(__file__).resolve().parents[2] / "voodoo-cleanroom" / "patches" / "mesafx-voodoo2-icd.patch"


def _post(name, added_only=True):
    """The patched side of a file's hunks: added lines, or added + context."""
    text = PATCH.read_text(errors="replace")
    m = re.search(rf"^diff --git a/{re.escape(name)} .*?(?=^diff --git |\Z)", text, re.S | re.M)
    assert m, f"{name} not in the ICD patch"
    keep = ("+",) if added_only else ("+", " ")
    return "\n".join(l[1:] for l in m.group(0).splitlines()
                     if l.startswith(keep) and not l.startswith("+++"))


def _added(name):
    return _post(name)


def test_callback_is_installed_before_grglideinit():
    a = _post("src/mesa/drivers/glide/fxapi.c", added_only=False)
    assert "grErrorSetCallback(fxGlideErrorCallback);" in a
    q = a.split("grErrorSetCallback(fxGlideErrorCallback);", 1)[1]
    assert q.lstrip().startswith("grGlideInit();")


def test_callback_is_cdecl_logs_and_returns():
    a = _added("src/mesa/drivers/glide/fxapi.c")
    decl = a.split("fxGlideErrorCallback(const char *string, FxBool fatal)", 1)[0].rsplit("\n", 2)[-2:]
    code = re.sub(r"/\*.*?\*/", "", "".join(decl))
    assert "FX_CALL" not in code and "static void" in code
    body = a.split("fxGlideErrorCallback(const char *string, FxBool fatal)", 1)[1].split("\n}", 1)[0]
    assert "rgl_log(" in body
    for banned in ("exit(", "MessageBox", "abort("):
        assert banned not in body


def test_teardown_leaves_breadcrumbs_and_detach_says_why():
    w = _added("src/mesa/drivers/glide/fxwgl.c")
    assert 'rgl_log("wglDeleteContext: enter")' in w and "DLL_PROCESS_DETACH" in w
    assert '"process exit" : "FreeLibrary"' in w
