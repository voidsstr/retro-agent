"""Our h5 (Voodoo 4/5) Glide carries H1-H7 (voodoo-cleanroom README 16.1).

Fork voidsstr/retro3dfx-glide 839143c (2026-09-24). Verified on .124 (Voodoo 5
6000): glideprobe --noopen -> grGlideInit OK, 1 board, 4 chips, GR_HARDWARE
"Voodoo5 6000 (tm)". Before H1, grGlideInit itself faulted: the gcc inline asm
in getThreadValueFast read fs:[eax] with eax uninitialised (365 such reads in
the old DLL). Reads the fork clone under voodoo-cleanroom/build/ (gitignored),
and SKIPS loudly when it is absent - run build-stack.sh to fetch it.
"""
import os
import re
import subprocess
from pathlib import Path

import pytest

CR = Path(__file__).resolve().parents[2] / "voodoo-cleanroom"
MAIN_CR = Path("/home/voidsstr/development/retro-agent/voodoo-cleanroom")
TREE = next((p / "build/retro3dfx-glide/glide3x/h5" for p in (CR, MAIN_CR)
             if (p / "build/retro3dfx-glide/glide3x/h5").is_dir()), None)


def src(rel):
    if TREE is None:
        pytest.skip("retro3dfx-glide clone absent - H1-H7 NOT checked (run build-stack.sh)")
    return (TREE / rel).read_text(errors="replace")


def test_h1_tls_accessor_uses_tlsgetvalue_not_fs_asm():
    h = src("glide3/src/fxglide.h")
    body = h.split("static __inline unsigned long getThreadValueFast (void)", 1)[1][:1500]
    assert "TlsGetValue(_GlideRoot.tlsIndex)" in body
    assert "%%fs" not in body.split("#else", 1)[0]


def test_h4_slave_regs_checked_and_zero_base_refused():
    c = src("minihwc/minihwc.c")
    blk = c.split("ctxReq.which = HWCEXT_GET_SLAVE_REGS", 1)[1][:1600]
    assert "memset(&ctxRes, 0, sizeof(ctxRes))" in blk
    assert "slaveRegRes.Regs[0] == 0" in blk


def test_h5_sli_aa_result_is_recorded():
    assert "retVal = ExtEscape" in src("minihwc/minihwc.c").split("HWCEXT_SLI_AA_REQUEST ;", 1)[1][:3000]


def test_h6_xp_escape_probed_and_h7_bpp_condition():
    assert "EXT_HWC_WXP, sizeof(ctxReq)" in src("minihwc/minihwc.c")
    g = src("glide3/src/gpci.c")
    assert "outputBpp != 32 &&" in g and "outputBpp != 32 ||" not in g


def test_built_dll_has_no_raw_teb_tls_reads():
    dll = next((p / "out/glide3x_h5.dll" for p in (CR, MAIN_CR) if (p / "out/glide3x_h5.dll").exists()), None)
    if dll is None or not shutil_which("i686-w64-mingw32-objdump"):
        pytest.skip("no built glide3x_h5.dll or objdump - artifact NOT checked")
    dis = subprocess.run(["i686-w64-mingw32-objdump", "-d", str(dll)], capture_output=True, text=True).stdout
    # 2 = the C runtime's own reads, same as the working h3 build; 365 = the H1 bug
    assert len(re.findall(r"mov\s+%fs:\(%eax\),%eax", dis)) <= 2


def shutil_which(x):
    import shutil
    return shutil.which(x)


def test_g3_fifo_and_idle_spins_are_bounded():
    f = src("glide3/src/fifo.c")
    assert "stuckPolls > 4000000UL" in f and "roomToReadPtr = blockSize;" in f
    g = src("glide3/src/gsst.c")
    assert "busyPolls > 4000000UL" in g


def test_audit_fixes_2026_09_24_are_in_the_source():
    """The pre-flight audit of the board-open path (see
    tests/native/test_h5_glide_guards.c for the logic mirrors)."""
    c = src("minihwc/minihwc.c")
    # bounded idle wait that reports, and both callers act on it
    assert "FxBool hwcIdleHardwareWithTimeout(hwcBoardInfo *bInfo)" in c
    assert "if (!hwcIdleHardwareWithTimeout(bInfo)) {" in c
    assert c.count("if (!hwcIdleHardwareWithTimeout(bInfo))") == 2
    assert "goto hwcRestoreVideo_release;" in c and "hwcRestoreVideo_release:" in c
    assert "timeout >= 1000000000" not in c
    # FX_GLIDE_NUM_CHIPS only as 1 or the real count, at every override site
    assert c.count("hwcClampNumChipsOverride(") == 4        # definition + 3 sites
    # mappings validated before the first MMIO
    assert "if (!hwcValidateMappings(bInfo))" in c
    assert "mbi.Type != MEM_MAPPED" in c


def test_h6_escape_field_is_32_bit():
    h = src("minihwc/minihwc.h")
    assert re.search(r"^\s*FxI32 hwcEscape ;", h, re.M)
    assert "FxI16/*FxI32*/ hwcEscape" not in h


def test_a_refused_board_is_skipped_not_touched():
    g = src("glide3/src/gpci.c")
    blk = g.split("if (!hwcMapBoard(bInfo, HWC_BASE_ADDR_MASK)) {", 1)[1][:400]
    assert "continue;" in blk.split("}", 1)[0]


def test_swap_pending_bookkeeping_is_bounded_and_in_range():
    g = src("glide3/src/gglide.c")        # Latin-1 file: src() reads with errors='replace'
    assert "for(i = MAX_BUFF_PENDING; i >= 0; --i)" not in g
    assert g.count("for(i = MAX_BUFF_PENDING - 1; i >= 0; --i)") == 3
    assert g.count("++swapSpins > 4000000UL") == 2
    assert "++stableTries < 1000" in g and "++stableTries < 2000" in g


# --- asm triangle-setup build: offsets from the TARGET, not the host --------
# Fork c41b50d (2026-09-25). fxgasm.c prints GrGC offsets by being RUN; on this
# 64-bit host that made every one of the 50 offsets the asm reads wrong
# (kTriProcOffset 0x95c0 for 0x9558). fxgasm_cross.sh folds them with the
# i686 compiler instead; its output matched fxgasm.exe run on .124 value for
# value, and the USE_X86=1 DLL built from it rendered Quake II on the V5 6000.

_OFF_RE = re.compile(r'^\s*OFFSET\s*\(\s*(\w+)\s*,\s*([\w.\[\]]+)\s*,\s*"(\w+)', re.M)


def test_build_stack_builds_the_asm_variant_with_target_offsets():
    bs = (CR / "build-stack.sh").read_text()
    h5 = bs.split("H5ARGS=(", 1)[1].split(")", 1)[0]
    assert "FXGASM_CROSS=1" in h5
    assert "USE_X86=1 USE_3DNOW=1 USE_MMX=1 USE_SSE=1" in bs
    assert "glide3x_h5_x86.dll" in bs
    assert "USE_SSE2=1" not in bs.split("H5ARGS=(", 1)[1]     # no fleet Voodoo box has SSE2
    mk = src("glide3/src/Makefile.mingw")
    assert "ifeq ($(FXGASM_CROSS),1)" in mk and "sh fxgasm_cross.sh" in mk


def test_fxgasm_cross_offsets_are_the_i686_layout(tmp_path):
    import shutil
    cc = shutil.which("i686-w64-mingw32-gcc")
    if TREE is None or cc is None or not (TREE / "glide3/src/fxgasm_cross.sh").exists():
        pytest.skip("fork clone / fxgasm_cross.sh / i686 gcc absent - asm offsets NOT checked")
    s = TREE / "glide3/src"
    for f in ("fxgasm.c", "fxgasm_cross.sh"):
        shutil.copy(s / f, tmp_path / f)
    inc = " ".join(f"-I{p}" for p in (s, TREE / "incsrc", TREE / "minihwc",
                                       TREE.parent / "swlibs/fxmisc", TREE.parent / "swlibs/newpci/pcilib",
                                       TREE.parent / "swlibs/fxmemmap", TREE.parent / "swlibs/texus2/lib"))
    flags = (f"-m32 {inc} -D__WIN32__ -DFX_DLL_ENABLE -DHWC_ACCESS_DDRAW=1 -DHWC_EXT_INIT=1 "
             "-DGLIDE_ALT_TAB=1 -DBETA=1 -DHWC_MINIVDD_HACK=1 -DWIN40COMPAT=1 -DWINXP_ALT_TAB_FIX=1 "
             "-DWINXP_SAFER_ALT_TAB_FIX=1 -DNEED_MSGFILE_ASSIGN -UWINNT -DGLIDE3 -DGLIDE3_ALPHA "
             "-DGLIDE_HW_TRI_SETUP=1 -DGLIDE_INIT_HWC -DGLIDE_PACKED_RGB=0 -DGLIDE_PACKET3_TRI_SETUP=1 "
             "-DGLIDE_TRI_CULLING=1 -DUSE_PACKET_FIFO=1 -DGLIDE_CHECK_CONTEXT -DH3 -DFX_GLIDE_H5_CSIM=1 "
             "-DFX_GLIDE_NAPALM=1 -DGL_AMD3D -DGL_MMX -DGL_SSE -DGL_X86")
    r = subprocess.run(["sh", "fxgasm_cross.sh", cc, flags], cwd=tmp_path, capture_output=True, text=True)
    assert r.returncode == 0, r.stderr[-2000:]
    gen = dict(re.findall(r"^(\w+)\tequ ([0-9a-f]{8})h$", (tmp_path / "fxgasm.h").read_text(), re.M))
    inl = dict(re.findall(r"^#define (k\w+) (0x[0-9A-Fa-f]+)", (tmp_path / "fxinline.h").read_text(), re.M))
    assert len(gen) >= 40 and "fifoPtr" in gen and "kTriProcOffset" in inl
    # Every OFFSET() in fxgasm.c, asserted against offsetof() in a 32-bit compile.
    body = (s / "fxgasm.c").read_text(errors="replace")
    checks = []
    for inst, path, name in _OFF_RE.findall(body):
        if name in gen:
            typ = {"gc": "GrGC", "gr": "struct _GlideRoot_s"}.get(inst)
            if typ:
                checks.append(f"_Static_assert(__builtin_offsetof({typ}, {path}) == 0x{gen[name]}, \"{name}\");")
    assert len(checks) >= 40
    checks.append(f"_Static_assert(__builtin_offsetof(struct GrGC_s, triSetupProc) == {inl['kTriProcOffset'].rstrip('UL')}, \"kTriProcOffset\");")
    checks.append(f"_Static_assert(__builtin_offsetof(GrGC, lostContext) == {inl['kLostContextOffset'].rstrip('UL')}, \"kLostContextOffset\");")
    (tmp_path / "chk.c").write_text('#include <stddef.h>\n#include <glide.h>\n#include "fxglide.h"\n' + "\n".join(checks) + "\n")
    r = subprocess.run([cc] + flags.split() + ["-fsyntax-only", "chk.c"], cwd=tmp_path, capture_output=True, text=True)
    assert r.returncode == 0, r.stderr[-3000:]


# --- fork 5439bb8 (2026-09-25): the "hang in grGlideInit" was a hidden dialog --
def test_a_callback_installed_before_grglideinit_survives_it():
    g = src("glide3/src/gpci.c")
    blk = g.split("dBorca - play safe", 1)[1][:1200]
    assert "if (!GrErrorCallback)" in blk
    assert blk.index("if (!GrErrorCallback)") < blk.index("grErrorSetCallback(_grErrorDefaultCallback)")


def test_default_fatal_box_is_in_front_and_logged():
    e = src("glide3/src/gerror.c")
    body = e.split("_grErrorDefaultCallback( const char *s, FxBool fatal )", 1)[1][:1500]
    assert "MB_TOPMOST" in body and "MB_SETFOREGROUND" in body
    assert 'getenv("RETRO_GLIDE_MAPLOG")' in body


def test_unmap_falls_back_to_the_pid_the_mapping_was_filed_under():
    c = src("minihwc/minihwc.c")
    for fn in ("hwcUnmapMemory() ", "hwcUnmapMemory9x(hwcBoardInfo *bInfo) "):
        body = c.split(fn, 1)[1][:2500]
        assert "contextHandle\n" in body.replace(" ", "").replace("?", "\n") or "contextHandle" in body
        assert "procHandle;" in body or ": hInfo.boardInfo[i].procHandle" in body
        assert "hwcLogLine(\"UNMAP" in body
    assert 'hwcLogLine("REFUSED pid=%lu %s\\n"' in c
