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
