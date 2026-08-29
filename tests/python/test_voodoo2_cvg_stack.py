"""Regression: the Voodoo 2 (cvg) clean-room lane.

Encodes what was established on hardware (.171, Pentium 4 / Voodoo 2 12MB) on
2026-08-29. Pure static checks — no hardware, no network, runs in the normal
suite.

Measured baselines that motivate these invariants (Quake II demo1, 640x480,
vsync OFF, 689 frames):
    stock 3dfx MiniGL   91.1 fps
    our MesaFX ICD      51.0 fps
    Intel 865G control  58.8 fps
"""
import os
import re

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
VC = os.path.join(ROOT, "voodoo-cleanroom")
BUILD = os.path.join(VC, "build-stack.sh")
PATCH = os.path.join(VC, "patches", "mesafx-sgis-multitexture.patch")


def _build():
    return open(BUILD).read()


def test_cvg_lane_exists_for_both_glides():
    """A Voodoo 2 needs FX_GLIDE_HW=cvg; h3/h5 do not drive DEV_0002."""
    s = _build()
    assert "FX_GLIDE_HW=cvg" in s, "no cvg lane in build-stack.sh"
    assert s.count("FX_GLIDE_HW=cvg") >= 2, "need cvg for BOTH glide3x and glide2x"
    assert "glide3x_cvg.dll" in s
    assert "glide2x_cvg.dll" in s


def test_cvg_relink_includes_shared_pcilib_objects():
    """The bug that made the first cvg build emit no DLL.

    glide3x/cvg links objects from the SHARED swlibs/newpci/pcilib tree (fxnt.c
    — the NT layer that opens \\\\.\\GpdDev and \\\\.\\MAPMEM). dual_abi_relink
    globbed only the chip dir, so the relink failed, gcc deleted its output and
    the following cp aborted the script under `set -euo pipefail`.
    """
    s = _build()
    for fn in ("dual_abi_relink", "dual_abi_relink2"):
        m = re.search(rf'^{fn} "\$GTREE/glide[23]x/cvg" (.+)$', s, re.M)
        assert m, f"{fn} is not called for cvg with an extra object dir"
        assert "pcilib" in m.group(1), (
            f"{fn} for cvg must also glob swlibs/newpci/pcilib or the relink "
            f"silently produces no DLL")


def test_sgis_multitexture_is_opt_in_not_default():
    """Advertising GL_SGIS_multitexture is a REGRESSION on this hardware.

    Quake II probes only for the SGIS name, so advertising it does flip Q2 to
    single-pass multitexture ('...using GL_SGIS_multitexture' appears in
    qconsole.log) — but the demo1 timedemo then does not finish inside 180s,
    against 13.5s / 51.0 fps with it off. Until that is root-caused it must stay
    behind FX_SGIS_MULTITEXTURE so the default build cannot regress.
    """
    assert os.path.exists(PATCH), "the SGIS patch should be kept in patches/"
    p = open(PATCH).read()
    assert "FX_SGIS_MULTITEXTURE" in p, "SGIS must be env-gated"
    assert 'getenv("FX_SGIS_MULTITEXTURE")' in p
    # and it must NOT share the ARB flag, which would auto-enable it
    assert "F(SGIS_multitexture)" in p, "SGIS needs its own gl_extensions flag"
    assert "F(ARB_multitexture) }," not in p.split("GL_SGIS_multitexture")[-1][:200], (
        "SGIS must not reuse the ARB_multitexture flag — that turns it on "
        "whenever ARB is on, which is exactly the regression")


def test_sgis_shim_sets_both_active_and_client_active_texture():
    """SGIS has ONE unit selector; ARB split it in two.

    Quake II enables GL_EXT_compiled_vertex_array and calls glTexCoordPointer
    for unit 1 after selecting it, so a shim that only sets the server unit
    sends those pointers to unit 0.
    """
    p = open(PATCH).read()
    assert "fx_glSelectTextureSGIS" in p
    assert "glActiveTextureARB" in p
    assert "glClientActiveTextureARB" in p, (
        "glSelectTextureSGIS must set the CLIENT active texture too")


def test_voodoo2_tuned_for_the_pentium4_that_runs_it():
    """The cvg box is a P4; the other fleet lanes are P3.

    -march stays pentium3 (no SSE2 dependency, so one artifact still runs on
    .124's Pentium III) but the schedule is tuned for the P4.
    """
    s = _build()
    m = re.search(r"GLIDEOPT_CVG='OPTFLAGS=([^']+)'", s)
    assert m, "no cvg-specific optimisation flags"
    flags = m.group(1)
    assert "-mtune=pentium4" in flags
    assert "-march=pentium3" in flags, "must not require SSE2 — .124 is a PIII"
