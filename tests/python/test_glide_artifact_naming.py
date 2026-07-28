"""Regression: build-stack.sh must give the DEPLOY name out/glide3x.dll to the
h3 (Voodoo3) build, never the h5 (Voodoo4/5) build.

Root-caused 2026-07-25 (retro-3dfx/FINDINGS.md "Games-OpenGL-broken ROOT
CAUSE"): the script used to name the H5 build out/glide3x.dll. The h5 tree
lacks the four h3 bring-up fixes (TlsGetValue accessor, GETLINEARADDR prime,
zero-base guard, lost-context fallback), so that DLL faults at grGlideInit on
the Voodoo3 — and because both builds export the identical 393-symbol surface,
it silently drop-in replaced the good DLL and broke every OpenGL game on .124.

These are source invariants on build-stack.sh so the trap cannot be
reintroduced by an edit to the emit/copy lines.
"""

import re
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
SCRIPT = REPO / "voodoo-cleanroom" / "build-stack.sh"


def _text():
    assert SCRIPT.is_file(), "voodoo-cleanroom/build-stack.sh missing"
    return SCRIPT.read_text()


def test_h5_emit_does_not_claim_deploy_name():
    """The h5 emit line must produce glide3x_h5.dll, not glide3x.dll."""
    text = _text()
    h5_emits = [
        ln for ln in text.splitlines()
        if ln.strip().startswith("emit ") and "/h5/lib" in ln
    ]
    assert h5_emits, "h5 emit line not found in build-stack.sh"
    for ln in h5_emits:
        assert "glide3x_h5.dll" in ln, (
            "h5 build must be emitted as glide3x_h5.dll (got: %r)" % ln)
        assert not re.search(r"\bglide3x\.dll\b", ln), (
            "h5 build must NOT claim the deploy name glide3x.dll (got: %r)" % ln)


def test_deploy_name_is_copied_from_h3():
    """out/glide3x.dll must be a copy of the h3 (Voodoo3) artifact."""
    text = _text()
    assert re.search(
        r'cp\s+"\$OUT/glide3x_h3\.dll"\s+"\$OUT/glide3x\.dll"', text), (
        "build-stack.sh must copy the h3 build to the deploy name "
        'out/glide3x.dll (cp "$OUT/glide3x_h3.dll" "$OUT/glide3x.dll")')
