"""vcr-kmd: the VSA-100 texture path behind Diag\\D3DBigTex (2026-09-28,
clean-room lane, default OFF, NOT yet run on silicon).

UT2004's Direct3D 8 device opened on our HAL (.124, "Video: vcr-kmd Voodoo 5
6000 (open driver)") and died at "CreateTexture failed (D3DERR_INVALIDCALL)"
loading its first textures: the HAL advertised the Voodoo3's 256x256 limit
and no FOURCC formats. The VSA-100 does 2048x2048 (tLOD TBIG), DXT1-5 and
ARGB8888. `Diag\\D3DBigTex` arms it bit by bit - 1 sizes to 2048, 2 DXT1/3/5,
4 A8R8G8B8 - and any bit also gives texBaseAddr its 26 bits (the Voodoo3's
24-bit field sampled a texture above 16 MB 16 MB lower; .124's heap is 27 MB).

The arithmetic is tests/native/test_vcr_kmd_texlod.c (against 3dfx's GPL
tables). This file pins the glue: the switch is read once at boot, reaches the
HAL only on a VSA-100, and with it off - the default, and every
Banshee/Voodoo3 - the texture path is the proven one, call for call.
"""
import re
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
KMD = REPO / "voodoo-cleanroom" / "vcr-kmd"
D3D = (KMD / "display" / "vcrdd_d3d.c").read_text()
E3D = (KMD / "display" / "vcrdd_3d.c").read_text()
DD = (KMD / "display" / "vcrdd_ddraw.c").read_text()
D2 = (KMD / "display" / "vcrdd_2d.c").read_text()
MPC = (KMD / "miniport" / "vcrmp.c").read_text()
IOH = (KMD / "include" / "vcr_ioctl.h").read_text()
REGS = (KMD / "include" / "vcr_3dregs.h").read_text()
README = (KMD / "README.md").read_text()
PROBE = (KMD / "tools" / "d3dprobe.c").read_text()

CR = REPO / "voodoo-cleanroom"
MAIN_CR = Path("/home/voidsstr/development/retro-agent/voodoo-cleanroom")
H5 = next((p / "build/retro3dfx-glide/glide3x/h5" for p in (CR, MAIN_CR)
           if (p / "build/retro3dfx-glide/glide3x/h5").is_dir()), None)


def func(src, sig):
    i = src.index(sig)
    return src[i:src.index("\n}\n", i)]


def test_the_switch_is_read_once_at_boot_and_absent_means_off():
    fa = MPC[MPC.index("x->allow_poke = VcrDiagGet"):]
    fa = fa[:fa.index("hwinfo(x);")]
    assert 'x->d3dbigtex = VcrDiagGet(L"D3DBigTex", 0);' in fa
    fill = MPC[MPC.index("v->flags ="):]
    fill = fill[:fill.index(";")]
    for bit, flag in ((1, "VCR_INFO_F_BIGTEX"), (2, "VCR_INFO_F_TEXDXT"), (4, "VCR_INFO_F_TEX32")):
        assert f"((x->d3dbigtex & {bit}) ? {flag} : 0)" in fill, flag
    for flag, val in (("BIGTEX", 0x200), ("TEXDXT", 0x400), ("TEX32", 0x800)):
        assert re.search(rf"#define VCR_INFO_F_{flag}\s+0x{val:x}\b", IOH), flag
    vals = [int(v, 16) for v in re.findall(r"#define VCR_INFO_F_\w+\s+(0x[0-9a-fA-F]+)", IOH)]
    assert len(vals) == len(set(vals)), "two VCR_INFO_F_* share a bit"


def test_the_display_driver_arms_it_on_a_vsa100_only():
    init = D2[D2.index("void VcrDd2dInit(VCR_PDEV *pd)"):]
    init = init[:init.index("IOCTL_VIDEO_QUERY_PUBLIC_ACCESS_RANGES")]
    reset = "pd->tex_big = pd->tex_dxt = pd->tex_32 = pd->tex_ext = 0;"
    assert reset in init and init.index(reset) < init.index("IOCTL_VCR_INFO")
    for f, flag in (("tex_big", "BIGTEX"), ("tex_dxt", "TEXDXT"), ("tex_32", "TEX32")):
        assert f"pd->{f} = pd->napalm && (info.flags & VCR_INFO_F_{flag}) ? 1 : 0;" in init, f
    assert "pd->tex_ext = pd->tex_big | pd->tex_dxt | pd->tex_32;" in init
    assert init.index("pd->napalm = info.device == 0x0009;") < init.index("pd->tex_big = pd->napalm")


def test_off_the_hal_is_the_proven_one():
    # caps: 256 unless bit 0
    assert "x.dwMaxTextureWidth = x.dwMaxTextureHeight = pd->tex_big ? 2048 : 256;" in D3D
    # the texture list: the three 16 bpp formats, the rest only with their bits
    hal = func(D3D, "void VcrDdD3dHalInfo(")
    assert "g_gd.dwNumTextureFormats = 3;" in hal
    assert hal.index("g_gd.dwNumTextureFormats = 3;") < hal.index("if (pd->tex_32) {") < \
        hal.index("if (pd->tex_dxt) {") < hal.index("g_gd.lpTextureFormats = g_texfmt;")
    # the view: the proven tex_view unless tex_ext, and tex_view is still the
    # Voodoo3 function's 16 bpp <= 256 call
    assert "return pd->tex_ext ? tex_view_ext(pd, tss, s, v) : tex_view(tss, s, v);" in D3D
    tv = func(D3D, "static BOOL tex_view(const DWORD *tss")
    assert "vcr_texlod_compute(s->lpGbl->wWidth, s->lpGbl->wHeight, 2," in tv
    assert "_ext" not in tv and "pf->dwRGBBitCount != 16" in tv
    cr = func(D3D, "static void compute_regs(")
    assert "view_of(c->pd, c->tss, t, &v0)" in cr and "view_of(c->pd, c->tss1, t1, &v1)" in cr
    # the chain: the VSA-100 function only with tex_ext, before anything else
    mk = func(D3D, "int VcrDdD3dCreateMipChain(")
    body = mk[mk.index("{"):]
    first = re.search(r"\n    (if|return)[^\n]*", body[body.index("LONG pitch = 0;"):]).group(0)
    assert first.strip() == "if (pd->tex_ext)"
    assert "return mipchain_ext(pd, p);" in mk
    assert "if (bpp != 16 || vcr_texlod_compute(w0, h0, 2, w0 * 2, 0, &t))" in mk
    # the flush: the old computation stands, the VSA-100 one is fenced
    fw = func(D3D, "static void flush_written(")
    assert fw.index("if (c->pd->tex_ext) {") < fw.index("vcr_texlod_compute(s->lpGbl->wWidth")
    # FOURCC, compressed surfaces, compressed TEXBLT, the Blt guard: only with bit 1
    fcc = func(D3D, "ULONG VcrDdD3dFourCC(")
    assert "!pd->tex_dxt" in fcc and "return 0;" in fcc
    assert "*nfourcc = VcrDdD3dFourCC(pd, fourcc);" in DD
    assert "hal->ddCaps.dwNumFourCCCodes = *nfourcc;" in DD
    cts = func(D3D, "int VcrDdD3dCreateTexSurface(")
    assert "if (!pd || !pd->tex_dxt || !p->dwSCnt)" in cts
    tb = func(D3D, "static void texblt(dp2walk *w")
    assert "if (pd->tex_dxt && (surf_fcc(d) || surf_fcc(s))) {" in tb
    blt = func(DD, "static DWORD APIENTRY Dd_Blt(")
    assert "if (pd->tex_dxt && (VcrDdD3dIsFourCC(d) || (s && VcrDdD3dIsFourCC(s))))" in blt
    assert blt.index("VcrDdD3dIsFourCC") < blt.index("if (p->IsClipped)")


def test_the_vsa100_view_bounds_a_chain_by_what_the_tmu_can_walk():
    tv = func(D3D, "static BOOL tex_view_ext(")
    assert "ext_layout(pd, s, &kind, &tmfmt, &t)" in tv
    assert "vcr_tex_chain_usable(kind, s->lpGbl->wWidth, s->lpGbl->wHeight, n)" in tv
    assert "if (lodmax > t.lod_limit)" in tv
    assert "v->textureMode = TM_PERSPECTIVE | TM_CLAMPW | tmfmt;" in tv
    lay = func(D3D, "static BOOL ext_layout(")
    assert "vcr_texlod_compute_ext(" in lay and "ext_flags(pd)" in lay
    assert "VCR_TEXF_NAPALM | (pd->tex_big ? VCR_TEXF_BIG : 0)" in D3D
    # a format the armed bits do not offer is never sampled
    k = func(D3D, "static ULONG pf_kind(")
    assert "if (!pd->tex_dxt)" in k and "pd->tex_32" in k
    chain = func(D3D, "static int mipchain_ext(")
    assert "HeapVidMemAllocAligned(vm, total, 1, &al, &pitch)" in chain
    assert "s->lpGbl->fpVidMem = base + vcr_tex_chain_offset(kind, w0, h0, k);" in chain


def test_the_texture_port_write_back_stays_where_it_was_proven():
    """The flush's texture-port write was proven for a 16 bpp texture no wider
    than 256 (the port decodes from TMU0's texBaseAddr under the texture's
    tLOD). The VSA-100 path passes ~0 - no write - for TBIG, 32-bit and
    compressed textures, and the port offset comes from the LINEAR base."""
    fw = func(D3D, "static void flush_written(")
    # the decision is vcr_tex_writeback_addr (common/vcr_texlod.c, run by
    # native/test_vcr_kmd_texlod.c): RGB16, not TBIG, and below 16 MB
    assert "vcr_tex_writeback_addr(kind, x.tbig, (ULONG)s->lpGbl->fpVidMem)" in fw
    assert "kind == VCR_TEXK_RGB16 && !tbig && addr < VCR_TEX_WRITEBACK_LIMIT ? addr : ~0u" in \
        (KMD / "common" / "vcr_texlod.c").read_text()
    fl = func(E3D, "BOOL VcrDd3dTexFlush(")
    assert "addr >= lin && addr - lin < 0x200000" in fl


def test_a_compressed_texture_idles_the_tmus_before_its_state():
    st = func(E3D, "BOOL VcrDd3dState(")
    nop = "w3(pd, V3D_TMU0 + V3D_TMU1 + V3D_NOPCMD, 0);"
    assert nop in st
    assert st.index("TM_COMPRESSED") < st.index(nop) < \
        st.index("w3(pd, V3D_TMU0 + V3D_TEXTUREMODE, r->textureMode);")
    assert "VcrDdRoom(pd, 5)" in st[st.index("TM_COMPRESSED"):st.index(nop)]


def test_compressed_texblt_is_bounded_by_both_levels_and_video_memory():
    lv = func(D3D, "static void texblt_dxt_level(")
    for guard in ("fcc != surf_fcc(s)", "r->right > (LONG)s->lpGbl->wWidth",
                  "((dx - r->left) & 3) || ((dy - r->top) & 3)",
                  "bx1 > (LONG)sbw || by1 > (LONG)sbh", "dbx + (bx1 - bx0) > (LONG)dbw",
                  "> pd->cjVram"):
        assert guard in lv, guard
    assert lv.index("> pd->cjVram") < lv.index("memcpy(")


def test_register_codes_are_the_gpl_h5_headers():
    assert re.search(r"#define TM_COMPRESSED\s+\(1u << 31\)", REGS)
    assert re.search(r"#define\s+TF_ARGB8888\s+15u", REGS)
    assert re.search(r"#define\s+TF_CMP_DXT1\s+1u", REGS)
    assert re.search(r"#define\s+TF_CMP_DXT23\s+2u", REGS)
    assert re.search(r"#define\s+TF_CMP_DXT45\s+3u", REGS)
    assert re.search(r"#define TL_TBIG\s+\(1u << 30\)", REGS)
    if H5 is None:
        pytest.skip("retro3dfx-glide clone absent - codes NOT checked against h5 h3defs.h")
    h3 = (H5 / "incsrc" / "h3defs.h").read_text(encoding="latin-1")
    assert re.search(r"#define SST_TBIG\s+BIT\(30\)", h3)
    assert re.search(r"#define SST_COMPRESSED_TEXTURES BIT\(31\)", h3)
    assert re.search(r"#\s*define SST_ARGB8888\s+\(15<<SST_TFORMAT_SHIFT\)", h3)
    for name, code in (("DXT1", 1), ("DXT3", 2), ("DXT5", 3)):
        assert re.search(rf"#\s*define SST_{name}\s+\({code}<<SST_TFORMAT_SHIFT\)", h3), name
    assert "#define SST_TEXTURE_ADDRESS     ((SST_MASK(21)<<4) | BIT(1))" in h3


def test_the_probe_can_verify_it_on_the_card_and_the_readme_says_how():
    for t in ('"tex512"', '"tex1024"', '"tex2048"', '"mip2048"', '"dxt1"', '"dxt3"', '"dxt5"',
              '"tex8888"', '"texhigh"'):
        assert t in PROBE, t
    row = next(line for line in README.splitlines() if line.startswith("| `D3DBigTex`"))
    assert "at boot" in row and "0x200" in row and "0x400" in row and "0x800" in row
