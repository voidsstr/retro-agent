"""vcr-kmd: the 2026-09-28 integration of three offline branches (clean-room
lane) - D3DBigTex (VSA-100 textures), DdHeapFloor (DirectDraw heap off offset
0) and the fbshot tile aperture + read-only CLUT kind - onto the GDI-gamma
base, for a deploy to .124 before a LAN party. Stability over features.

What this file pins:

1. THE DEFAULT IS THE BASE. With no Diag value set the built driver must do
   what the base did, apart from fixes that are default-on by design. Every
   Diag switch the miniport reads, with its default, is the base's set plus
   exactly three new ones - D3DBigTex, DdHeapFloor and ClutRead - all 0, all
   POSITIVE flags (absent = the flag is never set), and the display driver
   arms nothing from them without the flag.
2. THE FLAG MAP. The three branches each took vcr_info flag 0x200. Final:
   BIGTEX 0x200, TEXDXT 0x400, TEX32 0x800, DDHEAPFLOOR 0x1000, CLUT_READ
   0x2000, NO_GDIGAMMA 0x4000 - every flag one bit of its own (DdHeapFloor = 1
   on a shared bit would have armed 2048 textures).
3. THE REVIEW FIXES, where the native tests cannot reach the glue:
   (b) mipchain_ext packs a chain in the storage the later readers use;
   (c) d3dprobe_run: a check named with --tests that was skipped fails the run;
   (d)/(e)/(f) vcrctl fbshot re-checks the multi-chip gate while it reads,
       always reads the master's SLI/AA registers on a multi-chip board, and
       reports a frame with black lines as ok:false;
   (g) the offset-0 comments agree with vcr_ddheap.h.
   (a) and the fbshot arithmetic are native: test_vcr_kmd_texlod.c,
   test_vcr_kmd_fbshot.c, test_vcr_kmd_abi.c.
"""
import argparse
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
KMD = REPO / "voodoo-cleanroom" / "vcr-kmd"
MPC = (KMD / "miniport" / "vcrmp.c").read_text()
MPH = (KMD / "miniport" / "vcrmp.h").read_text()
IOH = (KMD / "include" / "vcr_ioctl.h").read_text()
D2 = (KMD / "display" / "vcrdd_2d.c").read_text()
D3D = (KMD / "display" / "vcrdd_d3d.c").read_text()
CTL = (KMD / "tools" / "vcrctl.c").read_text()
FBH = (KMD / "include" / "vcr_fbshot.h").read_text()
README = (KMD / "README.md").read_text()


def func(src, sig):
    i = src.index(sig)
    return src[i:src.index("\n}\n", i)]


def code(src):
    """src without its comments"""
    return re.sub(r"/\*.*?\*/", "", src, flags=re.S)


# ---- 1. the default is the base ------------------------------------------------------

# every Diag switch the miniport of the base (worktree-v56k-bench 77024e2) read,
# with its default - measured from that tree. A switch whose default moved, or
# a new switch, has to be named here on purpose.
BASE_DIAG = {
    "Accel2D": "1", "Accel2DLine": "1", "Accel2DPattern": "1", "Accel2DText": "0",
    "AllowPoke": "0", "BootAttempts": "0", "BootCount": "0", "D3D": "1", "D3D32": "0",
    "DbgPrint": "0", "Ddc": "1", "DebugPort": "0", "DeclinedBoots": "0",
    "DesktopOffset": "0", "Disable": "0", "EdidFilter": "1", "FixPciDecode": "1",
    "FlipDeadline": "0", "GdiGamma": "1", "GoodBoots": "0", "HwCursor": "1",
    "LfbMemoryConfig": "0x01803fff", "LogEntries": "entries", "LogLevel": "VCR_LV_DEBUG",
    "MaxBootAttempts": "3", "MaxPixclkKhz": "0", "MonHmaxKhz": "0", "MonHminKhz": "0",
    "MonId": "0", "MonMaxPixclkKhz": "0", "MonReset": "0", "MonTrustEnvelope": "0",
    "MonVmaxHz": "0", "MonVminHz": "0", "NapalmVpcExtra": "0", "OpenGLDriverVersion": "1",
    "OpenGLVersion": "2", "PciLowThresh": "10", "Reset3D": "0", "Sli": "1",
    "Sli6kClock": "1", "SliAA": "0", "SliAAFeederLead": "0", "SliAAFifoGate": "0",
    "SliAAReadback": "0", "SliPersistAll": "0",
    "TexPortFlush": "1", "TwoXAboveKhz": "0",
}
# the integration's new switches: every one OFF unless set
NEW_DIAG = {"D3DBigTex": "0", "DdHeapFloor": "0", "ClutRead": "0"}
# KILL switches: they only gate an EXPLICIT request, so absent = the request is
# honoured and the base behaviour is unchanged - nothing touches the hardware
# until a caller asks. CoreClock (2026-09-29): 0 refuses IOCTL_VCR_CLOCK's
# SET/RESTORE; the driver itself never writes pllCtrl1 unasked.
KILL_DIAG = {"CoreClock": "1"}
# RECORDS the driver writes and reads back, not switches (2026-09-30): the AA
# auto-disarm's live marker and the boot it names (include/vcr_aaguard.h).
# Absent = 0 = "the last boot did not end with AA live".
STATE_DIAG = {"SliAALive": "0", "PrevBootCount": "0"}
# DEFAULTS MOVED ON PURPOSE (each with its evidence): SliAAVendorRecipe was 0
# in the base. 2026-09-30 on .124: the dos_mode.c arm leaves cfgAALfbCtrl
# READ_EN off on every chip and every in-game AA session froze hard; with the
# vendor arm (READ_EN on the master pair, as 3dfx) 2x, 4x and 8x ran clean.
# vcrmp_multi.c sli_recipe(), tests/native/test_vcr_kmd_sli.c.
MOVED_DIAG = {"SliAAVendorRecipe": "1"}
# PREPARED, NOT YET ON SILICON (2026-10-07): the SLI/AA disable also zeroes
# every chip's 3D aaCtrl - for a killed AA client, whose AA enables otherwise
# outlive it (vcr_sli.h VCR_SLI_F_OFF_AACTRL). OFF until a supervised run.
AA_PREP_DIAG = {"SliOffAaCtrl": "0"}


def test_every_diag_default_is_the_bases_and_the_new_switches_are_off():
    got = {}
    for f in (KMD / "miniport").glob("*.c"):
        for name, dflt in re.findall(r'VcrDiagGet\(L"(\w+)",\s*([^)]+)\)', f.read_text()):
            assert got.setdefault(name, dflt.strip()) == dflt.strip(), \
                f"Diag\\{name} read with two defaults"
    want = dict(BASE_DIAG, **NEW_DIAG, **KILL_DIAG, **STATE_DIAG, **MOVED_DIAG, **AA_PREP_DIAG)
    assert got == want, {k: (got.get(k), want.get(k)) for k in set(got) | set(want)
                         if got.get(k) != want.get(k)}


def test_the_new_switches_are_positive_flags_absent_means_never_set():
    fa = MPC[MPC.index("static VP_STATUS NTAPI VcrFindAdapter("):]
    fa = fa[:fa.index("hwinfo(x);")]
    assert 'x->d3dbigtex = VcrDiagGet(L"D3DBigTex", 0);' in fa
    assert 'x->clut_read = VcrDiagGet(L"ClutRead", 0);' in fa
    fill = code(func(MPC, "static void fill_info("))
    fl = fill[fill.index("v->flags ="):]
    fl = fl[:fl.index(";")]
    assert "((x->d3dbigtex & 1) ? VCR_INFO_F_BIGTEX : 0)" in fl
    assert "((x->d3dbigtex & 2) ? VCR_INFO_F_TEXDXT : 0)" in fl
    assert "((x->d3dbigtex & 4) ? VCR_INFO_F_TEX32 : 0)" in fl
    assert '(VcrDiagGet(L"DdHeapFloor", 0) ? VCR_INFO_F_DDHEAPFLOOR : 0)' in fl
    assert "(x->clut_read && x->backend == VCR_HW_VOODOO ? VCR_INFO_F_CLUT_READ : 0)" in fl
    # the CLUT kind's gate is the same switch: 0 refuses it (vcr_clutread.h)
    rop = func(MPC, "static VP_STATUS reg_op(")
    assert "x->clut_read != 0" in rop
    assert "Diag\\\\ClutRead (default 0)" in MPH


def test_the_display_driver_arms_nothing_new_without_its_flag():
    init = func(D2, "void VcrDd2dInit(VCR_PDEV *pd)")
    # reset before the miniport is asked, so an older miniport arms nothing
    reset = init.index("pd->tex_big = pd->tex_dxt = pd->tex_32 = pd->tex_ext = 0;")
    assert reset < init.index("IOCTL_VCR_INFO")
    for line in ("pd->tex_big = pd->napalm && (info.flags & VCR_INFO_F_BIGTEX) ? 1 : 0;",
                 "pd->tex_dxt = pd->napalm && (info.flags & VCR_INFO_F_TEXDXT) ? 1 : 0;",
                 "pd->tex_32 = pd->napalm && (info.flags & VCR_INFO_F_TEX32) ? 1 : 0;",
                 "pd->tex_ext = pd->tex_big | pd->tex_dxt | pd->tex_32;",
                 "pd->dd_heap_floor = (info.flags & VCR_INFO_F_DDHEAPFLOOR) ? 1 : 0;"):
        assert line in init, line
    # tex_ext is the only way into the VSA-100 texture code
    mk = func(D3D, "int VcrDdD3dCreateMipChain(")
    assert "if (pd->tex_ext)\n        return mipchain_ext(pd, p);" in mk
    assert "if (c->pd->tex_ext) {" in func(D3D, "static void flush_written(")
    # vcrctl asks the kernel's CLUT kind only when the flag says it is there
    assert CTL.count("(v.flags & VCR_INFO_F_CLUT_READ)") == 2


def test_the_readme_rows_say_off():
    for name in ("D3DBigTex", "DdHeapFloor", "ClutRead"):
        row = next(ln for ln in README.splitlines() if ln.startswith(f"| `{name}` |"))
        assert re.search(r"default OFF|NOT yet on silicon", row, re.I), name
    row = next(ln for ln in README.splitlines() if ln.startswith("| `ClutRead` |"))
    assert "absent = 0" in row and "0x2000" in row and "default ON" not in row
    row = next(ln for ln in README.splitlines() if ln.startswith("| `DdHeapFloor` |"))
    assert "0x1000" in row


# ---- 2. the flag map -----------------------------------------------------------------

def test_the_vcr_info_flag_map():
    flags = dict((n, int(v, 16)) for n, v in
                 re.findall(r"#define VCR_INFO_F_(\w+)\s+(0x[0-9a-fA-F]+)", IOH))
    for name, val in (("BIGTEX", 0x200), ("TEXDXT", 0x400), ("TEX32", 0x800),
                      ("DDHEAPFLOOR", 0x1000), ("CLUT_READ", 0x2000), ("NO_GDIGAMMA", 0x4000)):
        assert flags.get(name) == val, name
    vals = list(flags.values())
    assert len(vals) == len(set(vals)), "two vcr_info flags share a bit"
    assert all(v and v & (v - 1) == 0 for v in vals), "a flag is one bit"


# ---- 3. the review fixes ---------------------------------------------------------------

def test_b_a_chain_is_packed_in_the_storage_its_readers_use():
    ch = func(D3D, "static int mipchain_ext(")
    c = code(ch)
    own = c.index("pf = top->lpGbl->ddpfSurface;")
    desc = c.index("pf = sd->ddpfPixelFormat;")
    desk = c.index("pf.dwRGBBitCount = pd->bpp;")
    assert own < desc < desk, "the surface's own format first, then the descriptor, then the desktop"
    assert "(top->lpGbl->ddpfSurface.dwFlags & DDPF_FOURCC) ||" in c
    assert "top->lpGbl->ddpfSurface.dwRGBBitCount" in c
    assert own < c.index("kind = pf_kind(pd, &pf);")
    # every level stored as the chain is packed, before anything is allocated
    guard = "if (surf_fcc(s) != ((pf.dwFlags & DDPF_FOURCC) ? pf.dwFourCC : 0))"
    assert guard in c
    assert c.index(guard) < c.index("HeapVidMemAllocAligned(")
    # the readers it must agree with key on the surface's own format
    assert "surf_fcc(d)" in func(D3D, "static void texblt_dxt_level(")
    assert "pf = &s->lpGbl->ddpfSurface;" in func(D3D, "static BOOL ext_layout(")


def _d3dprobe_run():
    tools = str(KMD / "tools")
    if tools not in sys.path:
        sys.path.insert(0, tools)
    import d3dprobe_run
    return d3dprobe_run


def test_c_a_named_check_that_was_skipped_is_not_a_pass():
    run = _d3dprobe_run()
    named = argparse.Namespace(tests="tex2048,dxt1")
    default = argparse.Namespace(tests="")
    clean = {"mode": "render", "pass": 9, "fail": 0, "skipped": 0}
    skipped = {"mode": "render", "pass": 0, "fail": 0, "skipped": 2}
    assert run.exit_code(named, clean) == 0
    assert run.exit_code(named, skipped) == 1          # the fix
    assert run.named_skips(named, skipped)
    assert run.exit_code(default, skipped) == 0        # a default run reports, does not fail
    assert run.exit_code(named, {"skipped": "x"}) == 1  # an unreadable count is not clean
    assert run.exit_code(named, dict(clean, fail=1)) == 1
    assert run.exit_code(named, dict(clean, error="no RESULT line")) == 1
    src = (KMD / "tools" / "d3dprobe_run.py").read_text()
    assert "return exit_code(a, res)" in src
    assert 'res["skipped_named"] = skip_note' in src


def test_d_e_f_fbshot_rechecks_the_gate_while_it_reads():
    fb = func(CTL, "static int cmd_fbshot(const char *path, int probe)")
    c = code(fb)
    # (e) the board's chips decide; the registers are read on any multi-chip card
    assert "board_chips = have_info ? v.nchips : 0;" in c
    assert "multi = board_chips > 1 || sli_chips > 1;" in c
    assert "if (multi && (have_info || io.probe))" in c
    assert "gate = vcr_fb_mb1_gate_board(board_chips, sli_chips, have_cfg, sli_ctrl, aa_ctrl);" in c
    assert "vcr_fb_mb1_gate(sli_chips" not in c
    # (d) asked again before the line's read, at flips and every N lines, and it stops
    loop = c[c.index("for (y = 0; y < l.h; y++) {"):]
    re_at = loop.index("if (multi && vcr_fb_recheck_due(y, flipped)) {")
    assert re_at < loop.index("fb_lfb(&io, off, line + xb, run)")
    assert "int hc = fb_sliaa(&io, &s2, &a2);" in loop
    sliaa = code(func(CTL, "static int fb_sliaa("))
    assert "pci_rd(0, VCR_CFG_SLILFBCTRL, sli) && pci_rd(0, VCR_CFG_AALFBCTRL, aa)" in sliaa
    assert "vcr_fb_mb1_recheck(board_chips, sli_chips, hc, s2, a2, ap.sli_shift)" in loop
    assert "break;" in loop[re_at:loop.index("if (!planned)")]
    at = c.index("\n    if (stop_gate) {")          # after the loop: nothing more is read
    stop = c[at:c.index("f = fopen(path", at)]
    assert "return 1;" in stop and "free(bmp);" in stop
    assert '\\"ok\\":false,\\"stopped_at_line\\"' in fb
    assert re.search(r"#define VCR_FB_RECHECK_LINES\s+64u", FBH)
    # (f) black lines are not a frame; raw reads under SLI are refused in the plan
    assert 'unmapped || read_errors ? "false" : "true"' in c
    assert '\\"partial\\":true' in fb
    assert "return unmapped || read_errors ? 1 : 0;" in c
    pm = func(FBH, "static __inline int vcr_fb_plan_make(")
    assert "if (ap->sli_shift)\n            return p->method = VCR_FB_R_SLI_RAW;" in pm


def test_fbshot_probe_asks_the_display_driver_nothing_and_keeps_the_aa_gate():
    """`fbshot --probe` (2026-10-04). Every escape runs under win32k's display
    lock, and a Direct3D game holding it (Max Payne on .124) parked the default
    fbshot in the kernel, where no timeout ends it. The probe path takes the
    board, the registers and memBase1 from vcrprobe.sys and calls no GDI - and
    it must not weaken the rule that keeps an LFB read away from live
    multi-chip AA (include/vcr_fbshot.h)."""
    fb = code(func(CTL, "static int cmd_fbshot(const char *path, int probe)"))
    at = fb.index("if (probe) {")
    branch = fb[at:fb.index("} else {", at)]
    assert "fb_probe_open(&io, &why)" in branch
    for gdi in ("esc(", "hwc_open(", "pci_rd(", "ExtEscape", "GetDC"):
        assert gdi not in branch, gdi
    # a VSA-100 counts as multi-chip: the SLI/AA registers are always read
    assert "board_chips = vcr_fb_probe_board_chips(io.device);" in branch
    assert "if (multi && (have_info || io.probe))" in fb
    # every read goes through the I/O layer, whose probe half is vcrprobe only
    for name, ioctl in (("static int fb_reg(", "fb_probe_mem(io, io->bar0 + off"),
                        ("static int fb_sliaa(", "fb_probe_cfg(io, io->bus, io->dev"),
                        ("static int fb_lfb(", "fb_probe_mem(io, io->bar1 + a")):
        body = code(func(CTL, name))
        assert body.index("if (!io->probe)") < body.index(ioctl), name
        for gdi in ("esc(", "hwc_open(", "ExtEscape"):
            assert gdi not in body[body.index(ioctl) - 40:], (name, gdi)
    assert "IOCTL_VCRPROBE_MEM" in code(func(CTL, "static int fb_probe_mem("))
    cfg = code(func(CTL, "static int fb_probe_cfg("))
    assert "IOCTL_VCRPROBE_PCI" in cfg
    assert "raw" not in cfg      # raw 0xCF8 cycles race the HAL's own config access
    # memBase1's extent only from vcr-kmd's MemorySize; no CLUT read (a write)
    assert "lfb_len = io.fb_per_chip * 2;" in fb
    assert "vcr_fb_probe_is_vcrkmd(as, alen)" in code(func(CTL, "static ULONG fb_probe_memsize("))
    pal = fb.index("if (fmt == VCR_VPC_FMT_PAL8 && io.probe) {")
    assert pal < fb.index("clut_read(")
    # main() takes no DC for it
    main = code(func(CTL, "int main(int argc, char **argv)"))
    assert "if (!fbprobe) {\n        g_dc = GetDC(NULL);" in main
    assert "rc = cmd_fbshot(fbpath, fbprobe);" in main
    assert "if (g_dc)\n        ReleaseDC(NULL, g_dc);" in main


def test_g_the_offset_0_comments_agree_with_the_heap_manager():
    h3d = (KMD / "display" / "vcrdd_3d.h").read_text()
    rt = (KMD / "include" / "vcr_rtfmt.h").read_text()
    heap = (KMD / "include" / "vcr_ddheap.h").read_text()
    assert "returns 0 for a failed allocation" in heap
    for src in (h3d, rt):
        assert "HeapVidMemAllocAligned returns 0 for a failed" in src
        assert "DdHeapFloor" in src
    assert "so the first surface allocated after a mode\n * set can be there" not in h3d
    assert "one the proven\n * HAL drew with.)" not in rt
