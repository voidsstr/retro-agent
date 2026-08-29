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
PATCH = os.path.join(VC, "patches", "mesafx-voodoo2-icd.patch")


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


def test_our_proc_table_is_searched_before_mesa_glapi():
    """Mesa's glapi SYNTHESIZES a stub for any unknown gl* name.

    So if _glapi_get_proc_address() is consulted first, every entry point we add
    to wgl_ext[] whose name starts with "gl" is unreachable — glapi answers with
    a stub wired to nothing. Quake II called exactly that for
    glSelectTextureSGIS and the timedemo stopped completing (2026-08-29).

    Reconstruct the POST-patch text of the hunk (context + added lines, removed
    lines dropped) and assert the table search precedes the glapi call.
    """
    lines = open(PATCH).read().splitlines()
    after, in_hunk = [], False
    for ln in lines:
        if ln.startswith("@@"):
            in_hunk = True
            continue
        if not in_hunk:
            continue
        if ln.startswith("diff --git"):
            in_hunk = False
            continue
        if ln.startswith("-"):
            continue                      # removed: not in the result
        if ln.startswith("+") or ln.startswith(" "):
            after.append(ln[1:])
    body = "\n".join(after)
    i_tbl = body.find("wgl_ext[i].name")
    i_api = body.find("_glapi_get_proc_address((const char *)")
    assert i_tbl != -1, "wgl_ext[] search not present in the patched result"
    assert i_api != -1, "the glapi call should still be there as a fallback"
    assert i_tbl < i_api, (
        "wgl_ext[] must be searched BEFORE _glapi_get_proc_address(), or our "
        "gl*-named entry points are shadowed by synthesized stubs")


def test_point_parameters_is_withdrawn_by_default():
    """We advertised an extension we do not accelerate.

    Mesa expands distance-attenuated points into geometry, so an app that takes
    GL_EXT_point_parameters gets a slower path than its own fallback. 3dfx's
    MiniGL never advertised it. Measured on .171 (Q2 demo1, 640x480, vsync off,
    4 runs each, zero variance): advertised 51.0 fps, withdrawn 57.2 = +12.2%.
    """
    p = open(PATCH).read()
    assert 'getenv("FX_POINT_PARAMS")' in p, (
        "point_parameters must be OPT-IN (FX_POINT_PARAMS), not on by default")
    assert 'getenv("FX_NO_POINT_PARAMS")' not in p, (
        "the old opt-out form means it is still advertised by default")


def test_icd_links_static_libgcc():
    """Any libgcc helper (a 64-bit divide is enough) adds an import on
    libgcc_s_dw2-1.dll, which is NOT on the retro boxes. The ICD then fails
    LoadLibrary and the game says only 'could not load <driver>' — no hint that
    a DLL is missing. Cost an hour on .171 (2026-08-29) after adding profiling
    counters that divided a 64-bit cycle count.
    """
    p = open(PATCH).read()
    assert "-static-libgcc" in p, "the ICD link must use -static-libgcc"


def test_march_and_mtune_are_separable():
    """They were welded to one variable, so the ICD could not be scheduled for
    the CPU that runs it without also raising the instruction-set floor (which
    would fault on .124's Pentium III).
    """
    p = open(PATCH).read()
    assert "TUNE ?= $(CPU)" in p
    assert "-march=$(CPU) -mtune=$(TUNE)" in p


def test_grtexcombine_is_shadowed():
    """grTexCombine was the one texture-state call the 0.1.5 Glide shadow
    missed; on a 2-TMU part it is issued twice per bind and each call runs
    Glide's full _grRebuildDataList.
    """
    p = open(PATCH).read()
    assert "fx_sh_grTexCombine" in p
    assert "sh_tcomb" in p


def test_profiler_instruments_the_whole_frame():
    """FX_PROFILE=1 is what turned the multitexture hunt from guessing into
    measuring — it retired seven theories. Keep the coverage: without the
    pipeline timer and vertex count in particular you cannot show that the cost
    is outside our driver.
    """
    p = open(PATCH).read()
    for probe in ("fxp_pipeline_cycles", "fxp_verts", "fxp_setup_cycles",
                  "fxp_swap_cycles", "fxp_fixup"):
        assert probe in p, f"profiler lost its {probe} probe"
    assert 'getenv("FX_PROFILE")' in p, "profiling must stay opt-in"


def test_glide_lanes_link_static_libgcc_too():
    """The ICD learned this the hard way; the Glide lanes had the same defect.

    glide3x_cvg.dll imported libgcc_s_dw2-1.dll, absent on the retro boxes, so
    it would have failed LoadLibrary on first use with no diagnostic beyond the
    game reporting it could not load the driver. Both dual-ABI relink helpers
    build the final DLL with an explicit gcc -shared, so both need the flag.
    """
    s = open(BUILD).read()
    # the -o lands on a continuation line, so match the compiler invocation only
    relink_lines = [l for l in s.splitlines() if "${CROSS}gcc -shared" in l]
    assert relink_lines, "no explicit glide relink found in build-stack.sh"
    for l in relink_lines:
        assert "-static-libgcc" in l, (
            "every glide relink must use -static-libgcc or the DLL silently "
            f"gains a libgcc import the retro boxes cannot satisfy: {l.strip()[:80]}")


def test_build_documents_the_sse_requirement():
    """Our artifacts require SSE, and the obvious fix for an SSE-less target is
    wrong. Measured 2026-08-29 with our own toolchain:

        -march=pentium3 -mfpmath=387   ->  4 SSE instructions  (STILL faults)
        -march=athlon   -mfpmath=387   ->  0

    -march=pentium3 alone declares SSE available and gcc keeps emitting it, so
    -mfpmath=387 by itself is a trap: the build looks fixed and still faults
    with c000001d on an SSE-less CPU. The build script must carry that warning
    so the next person does not "fix" it the half way.
    """
    s = open(BUILD).read()
    assert "-mfpmath=387 IS NOT ENOUGH" in s or "IS NOT ENOUGH" in s, \
        "build-stack.sh must warn that -mfpmath=387 alone does not remove SSE"
    assert "cvtsi2ss" in s, "the warning should name the faulting opcode class"
    assert "objdump" in s, "the warning should give the verification command"
