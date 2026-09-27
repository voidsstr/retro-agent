"""vcr-kmd Direct3D HAL (display/vcrdd_d3d.c + vcrdd_3d.c) - the traps.

Proven on the 86Box Voodoo3 test bed with d3dprobe render, windowed AND
fullscreen, 26/26 each (2026-09-26) - the same matrix XP's in-box Voodoo3
driver passes on the same emulated card. Each invariant failed a run first:

- XP does NOT move video memory between the surfaces of a flip chain. The
  surfaces keep their memory and the runtime re-targets rendering with DP2
  SETRENDERTARGET by surface HANDLE - and a complex surface is named by its
  ROOT only in CreateSurfaceEx: every attached surface has to be found through
  the attach lists (a ring, for a flip chain). Without the walk the back
  buffer's handle is unknown: fullscreen drew every other frame into the front
  buffer (d3dprobe fullscreen 18/26).
- On NT a Direct3D texture's DD_SURFACE_LOCAL does NOT carry
  DDRAWISURF_HASPIXELFORMAT although ddpfSurface is filled (a 64x64 R5G6B5
  managed texture: dwFlags 0). Trusting the flag refused every texture
  (tex/modulate/bigtex drew the diffuse colour).
- the render target is read from the surface at every call, never cached.
- the DP2 walk parses every DX7 opcode, bounds-checked, and hands anything
  else to the runtime's parse-unknown callback.
- float arithmetic only in vcrdd_3d.c, the one object built with the x87,
  always inside EngSave/RestoreFloatingPointState.
"""
import re
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
KMD = REPO / "voodoo-cleanroom" / "vcr-kmd"
D3D = (KMD / "display" / "vcrdd_d3d.c").read_text()
E3D = (KMD / "display" / "vcrdd_3d.c").read_text()
MK = (KMD / "Makefile").read_text()
DD = (KMD / "display" / "vcrdd_ddraw.c").read_text()


def func(src, sig):
    i = src.index(sig)
    return src[i:src.index("\n}\n", i)]


def test_a_complex_surface_is_named_through_its_attach_lists():
    ex = func(D3D, "static DWORD APIENTRY Dd_CreateSurfaceEx(")
    assert "name_surface(p->lpDDLcl, p->lpDDSLcl)" in ex
    walk = func(D3D, "static void name_surface(")
    assert "lpAttachList" in walk and "lpAttached" in walk
    assert "seen[k] != a->lpAttached" in walk          # a flip chain is a ring
    assert "handle_set(l, cur->lpSurfMore->dwSurfaceHandle, cur)" in walk


def test_the_pixel_format_is_trusted_without_the_flag():
    f = func(D3D, "static int has_pixfmt(")
    assert "ddpfSurface.dwRGBBitCount" in f
    tex = func(D3D, "static BOOL tex_view(")
    assert "has_pixfmt(s)" in tex
    assert "DDRAWISURF_HASPIXELFORMAT))" not in tex


def test_the_render_target_is_read_at_every_call():
    dp2 = func(D3D, "static DWORD APIENTRY D3d_DrawPrimitives2(")
    assert "target_of(c);" in dp2
    clr = func(D3D, "static DWORD APIENTRY D3d_Clear2(")
    assert "target_of(c);" in clr
    # and SETRENDERTARGET resolves handles, loudly when it cannot
    assert "SETRENDERTARGET: no surface for handle" in D3D


def test_every_dx7_opcode_is_parsed_or_handed_to_the_runtime():
    walk = func(D3D, "static HRESULT walk(")
    for op in ("RENDERSTATE", "TEXTURESTAGESTATE", "TRIANGLELIST", "TRIANGLESTRIP",
               "TRIANGLEFAN", "INDEXEDTRIANGLELIST", "INDEXEDTRIANGLELIST2",
               "INDEXEDTRIANGLESTRIP", "INDEXEDTRIANGLEFAN", "TRIANGLEFAN_IMM",
               "LINELIST_IMM", "POINTS", "LINELIST", "LINESTRIP", "INDEXEDLINELIST",
               "INDEXEDLINELIST2", "INDEXEDLINESTRIP", "UPDATEPALETTE", "SETRENDERTARGET",
               "CLEAR", "TEXBLT", "EXT"):
        assert f"case D3DNTDP2OP_{op}:" in walk, op
    sizes = func(D3D, "static ULONG rec_size(")
    for op in ("VIEWPORTINFO", "WINFO", "SETPALETTE", "ZRANGE", "SETMATERIAL", "CREATELIGHT",
               "SETTRANSFORM", "STATESET", "SETPRIORITY", "SETTEXLOD", "SETCLIPPLANE"):
        assert f"case D3DNTDP2OP_{op}:" in sizes, op
    assert "g_parse_unknown((LPVOID)hdr, &next)" in walk
    assert "D3DNTERR_COMMAND_UNPARSED" in walk
    # every read is bounded by the command length
    assert walk.count("NEED(") >= 20


def test_floats_live_in_one_object_behind_the_fpu_bracket():
    assert "$(filter-out -mgeneral-regs-only,$(KCFLAGS)) -c display/vcrdd_3d.c" in MK
    assert "vcrdd_3d.c" not in MK.split("DD_SRC  :=")[1].split("\n\n")[0]
    dp2 = func(D3D, "static DWORD APIENTRY D3d_DrawPrimitives2(")
    assert dp2.index("EngSaveFloatingPointState(") < dp2.index("walk(&w,") < \
        dp2.index("EngRestoreFloatingPointState(")
    assert "float" not in re.sub(r"/\*.*?\*/", "", D3D, flags=re.S).replace("floats", "")


def test_the_triangle_protocol_of_the_setup_unit():
    t = func(E3D, "BOOL VcrDd3dTriangle(")
    # first vertex, BEGIN; second and third, DRAW
    assert t.index("V3D_SBEGINTRICMD") < t.index("V3D_SDRAWTRICMD")
    assert t.count("V3D_SDRAWTRICMD") == 2


def test_a_destroyed_surface_leaves_the_handle_table():
    assert "VcrDdD3dSurfaceGone(p->lpDDSurface);" in DD
    assert "DDHAL_SURFCB32_DESTROYSURFACE" in DD


def test_a_mipmap_chain_is_one_block_from_directdraws_heap():
    """The TMU finds level n by adding the sizes of the larger levels to the
    base: a chain must be ONE block, packed back to back. On XP the runtime
    creates every level with its own CreateSurface (measured: seven calls for a
    64x64 D3D8 texture, DDSD_MIPMAPCOUNT 7 on each), so the driver allocates
    from DirectDraw's own heap - the VIDEOMEMORY array it filled in
    DrvGetDirectDrawInfo, whose lpHeap DirectDraw sets - and marks what it owns.
    Without DDSCAPS_MIPMAP Unreal's D3DDrv stops: "Failed to preallocate
    initial textures, 4x4: DDERR_NOMIPMAPHW"."""
    dd = (KMD / "display" / "vcrdd_ddraw.c").read_text()
    assert "pd->pvmList = vm;" in dd
    assert "if (VcrDdD3dCreateMipChain(pd, p))" in dd
    assert "if (VcrDdD3dFreeMipChain(pd, p->lpDDSurface))" in dd
    mk = func(D3D, "int VcrDdD3dCreateMipChain(")
    assert "HeapVidMemAllocAligned(vm, mip_offset(w0, h0, levels), 1, &al, &pitch)" in mk
    assert "s->lpGbl->fpVidMem = base + mip_offset(w0, h0, k);" in mk
    assert "!vm->lpHeap" in mk                     # no heap: the runtime places it
    fr = func(D3D, "int VcrDdD3dFreeMipChain(")
    assert "VidMemFree(" in fr and "MIP_TOP" in fr
    assert "DDSCAPS_MIPMAP;" in D3D                # advertised


def test_fog_table_on_w_and_vertex_fog_through_a_synthetic_w():
    """The Voodoo fog unit only reads W (a 64-entry table indexed by 1/W,
    common/vcr_fog.c). D3D table fog fills the table from FOGSTART/END/DENSITY;
    D3D vertex fog - the factor in the specular alpha, which is how every
    pre-transformed and every runtime-lit vertex arrives - loads a ramp and
    sends a synthetic 1/W per vertex. d3dprobe fogtable/fogvertex: 6/6 on the
    86Box Voodoo3."""
    e3d = (KMD / "display" / "vcrdd_3d.c").read_text()
    st = func(e3d, "BOOL VcrDd3dState(")
    assert "vcr_fog_table(" in st and "V3D_FOGTABLE + 4 * i" in st
    assert "memcmp(pd->fog_loaded, r->fog_table" in st       # loaded once per change
    assert "vcr_fog_ramp_oow(1.0f - (float)sa * (1.0f / 255.0f))" in e3d
    cr = func(D3D, "static void compute_regs(")
    assert "D3DRENDERSTATE_FOGTABLEMODE" in cr and "c->fog_vertex = 1;" in cr
    assert "$(OUT)/vcr_fog.o: common/vcr_fog.c" in MK        # floats: the FPU object


def test_a_texture_the_cpu_wrote_is_flushed_before_the_tmu_samples_it():
    """The runtime uploads managed textures by Lock + CPU writes (no TEXBLT,
    measured), and the TMU's texture cache does not see LFB writes: a new
    texture reusing an old one's address sampled the OLD texels (d3dprobe
    tex2add: grey instead of green). Unlock/TEXBLT flag the surface; before a
    draw samples it, Glide's download-coherency sequence runs (2D NOP,
    texBaseAddr away and back, nopCMD - the silicon's need) plus one dword
    written back through the texture port (86Box's cache watches that).
    Also: CreateSurfaceEx with fpVidMem 0 is the DESTROY notice."""
    e3d = (KMD / "display" / "vcrdd_3d.c").read_text()
    fl = func(e3d, "BOOL VcrDd3dTexFlush(")
    assert "~base & 0x00fffff0u" in fl and "V3D_NOPCMD" in fl
    assert "pd->pjRegs + 0x600000 + (addr - base)" in fl and "!pd->no_texport" in fl
    dd = (KMD / "display" / "vcrdd_ddraw.c").read_text()
    assert "VcrDdD3dTexWritten(p->lpDDSurface);" in func(dd, "static DWORD APIENTRY Dd_Unlock(")
    pr = func(D3D, "static BOOL prepare(")
    assert "g_tex_writes != c->tex_writes_seen" in pr and "flush_written(c, c->tex_surf0);" in pr
    ns = func(D3D, "static void name_surface(")
    assert "!cur->lpGbl->fpVidMem" in ns and "handle_forget(NULL, cur)" in ns
    mp = (KMD / "miniport" / "vcrmp.h").read_text()
    assert "#define VCR_DD_MAP_LEN      0xa00000" in mp


def test_vertex_colour_goes_to_the_setup_unit_as_floats_not_packed_sargb():
    """vertex() feeds the setup unit as 3dfx's h5 Glide does (built
    GLIDE_PACKED_RGB=0): sRed/sGreen/sBlue/sAlpha as floats 0..255, never the
    packed sARGB, and the four register offsets are Glide's h3regs.h. (The
    .124 gouraud failure this was first written for was PARMADJUST - see
    test_the_colour_path_always_carries_parmadjust.)"""
    src = (KMD / "display" / "vcrdd_3d.c").read_text()
    body = src[src.index("static BOOL vertex("):src.index("BOOL VcrDd3dTriangle(")]
    code = re.sub(r"/\*.*?\*/", "", body, flags=re.S)
    assert "V3D_SARGB" not in code
    for reg, expr in (("V3D_SRED", r"\(argb >> 16\) & 0xff"), ("V3D_SGREEN", r"\(argb >> 8\) & 0xff"),
                      ("V3D_SBLUE", r"argb & 0xff"), ("V3D_SALPHA", r"argb >> 24")):
        assert re.search(rf"wf\(pd, {reg}, \(float\)\(?{expr}\)?\);", code), reg
    regs = (KMD / "include" / "vcr_3dregs.h").read_text()
    for name, off in (("SRED", 0x270), ("SGREEN", 0x274), ("SBLUE", 0x278), ("SALPHA", 0x27c)):
        assert re.search(rf"#define V3D_{name}\s+0x{off:x}\b", regs), name


def test_the_colour_path_always_carries_parmadjust():
    """On the V5 6000's VSA-100 silicon (.124, 2026-09-26) every DECREASING
    parameter iterated as a constant: d3dprobe gouraud read fffb00 at the green
    corner and ff00ff at the blue one (red falling from its first vertex stayed
    ff), gouraudb the same with blue first - while rising channels were right
    and 86Box reproduced none of it. 3dfx's h5 Glide starts every context with
    fbzColorPath = SST_PARMADJUST (gsst.c) and never clears it; compute_regs
    now sets it on every fbzColorPath the HAL writes."""
    src = (KMD / "display" / "vcrdd_d3d.c").read_text()
    assert re.search(r"r->fbzColorPath = color_path\(c, tex\) \| CP_PARMADJUST;", src)
    assert len(re.findall(r"r->fbzColorPath = ", src)) == 1
    regs = (KMD / "include" / "vcr_3dregs.h").read_text()
    assert re.search(r"#define CP_PARMADJUST\s+\(1u << 26\)", regs)


RTFMT = KMD / "include" / "vcr_rtfmt.h"


def test_32bpp_targets_are_vsa100_only_and_refused_on_voodoo3():
    """32 bpp Direct3D render targets (X8R8G8B8, with a 24+8 Z) exist on the
    VSA-100 only: its renderMode[1:0] = 2 is 3dfx's h5 Glide's SST_RM_32BPP
    (gsst.c _grRenderMode), and in 32 bpp the aux buffer is 32 bits a pixel
    (gglide.c: "the depth buffer is 24bpp"). The Banshee/Voodoo3 3D engine is
    16 bpp only, so there the caps offer none and a 32 bpp target is refused -
    never drawn into memory laid out for another depth. A colour/Z size
    mismatch is refused on both (no aux-format bit exists). Before this the
    HAL offered DDBD_16 alone and windowed D3D on the V5 6000's 32 bpp
    desktop failed CreateDevice with 0x8876086c. VSA-100 half UNPROVEN on
    silicon until d3dprobe render --full --bpp 32 passes on the card - and
    OFF unless the miniport's Diag\\D3D32 = 1 (the first argument of
    vcr_rt_format is "32 bpp allowed": pd->rt32 = napalm AND the switch;
    test_d3d32_is_a_default_off_switch)."""
    import shutil, subprocess, tempfile
    cc = shutil.which("gcc") or shutil.which("cc")
    if not cc:
        import pytest
        pytest.skip("no host C compiler")
    prog = r'''
#include <stdio.h>
#include "vcr_rtfmt.h"
int main(void) {
    /* napalm, rt bits, z bits -> format */
    printf("%u %u %u %u %u %u %u %u %u %u\n",
        vcr_rt_format(0, 16, 16), vcr_rt_format(0, 16, 0),
        vcr_rt_format(0, 32, 0),  vcr_rt_format(0, 32, 32),     /* Voodoo3: refused */
        vcr_rt_format(1, 32, 32), vcr_rt_format(1, 32, 0),
        vcr_rt_format(1, 32, 16), vcr_rt_format(1, 16, 32),     /* mismatch: refused */
        vcr_rt_format(1, 16, 16), vcr_rt_format(1, 24, 0));
    printf("%x %x %x %x\n", vcr_rt_rendermode(VCR_RT_16), vcr_rt_rendermode(VCR_RT_32),
        vcr_rt_zmax(VCR_RT_16), vcr_rt_zmax(VCR_RT_32));
    return 0;
}
'''
    with tempfile.TemporaryDirectory() as d:
        c = Path(d) / "t.c"
        c.write_text(prog)
        exe = Path(d) / "t"
        subprocess.run([cc, "-Wall", "-Werror", "-I", str(KMD / "include"), str(c), "-o", str(exe)],
                       check=True)
        out = subprocess.run([str(exe)], check=True, capture_output=True, text=True).stdout.split("\n")
    assert out[0].split() == ["16", "16", "0", "0", "32", "32", "0", "0", "16", "0"]
    # renderMode: 16 bpp = 0, 32 bpp = 2 (SST_RM_32BPP), RGBA write enables bits 17..20
    assert out[1].split() == ["1e0000", "1e0002", "ffff", "ffffff"]

    e3d = (KMD / "display" / "vcrdd_3d.c").read_text()
    tg = func(e3d, "BOOL VcrDd3dTarget(")
    # the writes come from vcr_3dseq.h (register by register in
    # tests/native/test_vcr_kmd_d3dseq.c): renderMode behind napalm, 32 bpp
    # behind rt32, strides in BYTES
    assert "vcr_3d_target_seq(pd->napalm, pd->rt32, t->fmt," in tg
    assert "if (!n)\n        return FALSE;" in tg                  # refused: no write at all
    seq = (KMD / "include" / "vcr_3dseq.h").read_text()
    ts = func(seq, "static __inline unsigned vcr_3d_target_seq(")
    assert "if (!vcr_rt_programmable(napalm && rt32, fmt))" in ts
    assert ts.index("if (napalm) {") < ts.index("V3D_RENDERMODE")
    assert "vcr_rt_rendermode(fmt)" in ts and "BS_LINEAR_STRIDE(rt_pitch)" in ts
    # Z spans the aux buffer: 16 bits, or 24 at 32 bpp - iterated and cleared
    assert "wf(pd, V3D_SVZ, p[2] * d->z_scale);" in e3d
    assert "t->fmt == VCR_RT_32 ? 16777215.0f : 65535.0f" in func(e3d, "void VcrDd3dDrawTarget(")
    assert "z * 16777215.0f + 0.5f" in func(e3d, "BOOL VcrDd3dClear(")

    to = func(D3D, "static ULONG target_of(")
    assert "vcr_rt_format(c->pd->rt32, rtb, zb)" in to
    assert to.index("if (fmt == VCR_RT_REFUSED)") < to.index("t->rt_off =")   # refused: empty
    cc_ = func(D3D, "static DWORD APIENTRY D3d_ContextCreate(")
    assert "DDERR_INVALIDPIXELFORMAT" in cc_ and 'target_refused(&g_ctx[i], bits, "ContextCreate")' in cc_
    assert '"%s refused: %u bpp target with a %u-bit Z on %s"' in D3D
    walk = func(D3D, "static HRESULT walk(")
    assert "VcrDd3dDrawTarget(&w->d, &c->target);" in walk
    assert "w->drawable || (c->target.rt_off" not in walk          # a refused switch stops drawing
    hal = func(D3D, "void VcrDdD3dHalInfo(")
    assert "d->dwDeviceRenderBitDepth = DDBD_16 | (pd->rt32 ? DDBD_32 : 0);" in hal
    # the Z depths follow the Z list: DDBD_16 alone unless 32 bpp is armed
    # AND the desktop is 32 bpp (then DDBD_32 alone)
    assert "ULONG zl = vcr_rt_zlist(pd->rt32, pd->bpp);" in hal
    assert "((zl & VCR_ZL_D16) ? DDBD_16 : 0) |" in hal
    assert "((zl & (VCR_ZL_D24X8 | VCR_ZL_D24S8)) ? DDBD_32 : 0)" in hal
    assert "d->dwDeviceZBufferBitDepth = zbd;" in hal
    assert "hal->ddCaps.dwZBufferBitDepths = zbd;" in hal
    info = func(D3D, "int VcrDdD3dDriverInfo(")
    assert "vcr_rt_zlist(pd->rt32, pd->bpp)" in info               # D16 alone on a Voodoo3
    assert "dwStencilBitMask = 0xff000000;" in info and "0x00ffffff" in info
    can = func(DD, "static DWORD APIENTRY Dd_CanCreateSurface(")
    assert "!pd->rt32" in can and "DDSCAPS_ZBUFFER" in can and "DDERR_INVALIDPIXELFORMAT" in can


# ---- 32 bpp hardening before any deploy (critic plan step 4, 2026-09-27) ----------
# None of this has run on silicon; the register sequences are pinned write by
# write in tests/native/test_vcr_kmd_d3dseq.c. These check the wiring.

E2D = (KMD / "display" / "vcrdd_2d.c").read_text()
ESC = (KMD / "display" / "vcrdd_escape.c").read_text()
MPC = (KMD / "miniport" / "vcrmp.c").read_text()
IOH = (KMD / "include" / "vcr_ioctl.h").read_text()
SEQ = (KMD / "include" / "vcr_3dseq.h").read_text()
RT = (KMD / "include" / "vcr_rtfmt.h").read_text()
PROBE = (KMD / "tools" / "d3dprobe.c").read_text()


def code(src):
    return re.sub(r"/\*.*?\*/", "", src, flags=re.S)


def test_every_vsa100_target_disables_stencil_and_the_comments_say_alpha():
    """(a) stencilMode (0x1e4) / stencilOp (0x1e8) were written NOWHERE in
    vcr-kmd, and at 32 bpp the aux buffer's top byte is live stencil: a
    Glide/OpenGL session's stencil state would fail or rewrite every D3D
    pixel. Every VSA-100 target now writes both 0 right after renderMode (the
    vendor V5 HAL does it per context and per clear), and the FIFO wait covers
    the 9 writes. zaColor[31:24] is SST_ZACOLOR_ALPHA - the old comments
    called it the stencil clear."""
    ts = func(SEQ, "static __inline unsigned vcr_3d_target_seq(")
    napalm = ts[ts.index("if (napalm) {"):ts.index("}", ts.index("if (napalm) {"))]
    assert "V3D_RENDERMODE" in napalm and "V3D_STENCILMODE; w[n++].val = 0;" in napalm \
        and "V3D_STENCILOP;   w[n++].val = 0;" in napalm
    assert napalm.index("V3D_RENDERMODE") < napalm.index("V3D_STENCILMODE") < napalm.index("V3D_STENCILOP")
    regs = (KMD / "include" / "vcr_3dregs.h").read_text()
    assert re.search(r"#define V3D_STENCILMODE\s+0x1e4\b", regs)
    assert re.search(r"#define V3D_STENCILOP\s+0x1e8\b", regs)
    tg = func(E3D, "BOOL VcrDd3dTarget(")
    assert "VcrDdRoom(pd, n > 7 ? n : 7)" in tg                    # V3: 7 as ever, VSA-100: 9
    assert "#define VCR_3D_TARGET_MAX   9" in SEQ
    # the wrong claim is gone from both places, and the right one is stated
    assert "the stencil of a 24+8 aux buffer, cleared to 0" not in E3D
    assert "SST_ZACOLOR_ALPHA" in func(E3D, "BOOL VcrDd3dClear(")
    assert "clears put\n *                      (stencil << 24) | depth" not in RT
    assert "zaColor[31:24] is NOT the stencil clear value" in RT
    assert "STENCIL IS NOT SUPPORTED" in RT
    # stencil clears stay dropped, and the caps claim none
    assert "D3DCLEAR_STENCIL" not in code(func(D3D, "static void clear_rects("))
    assert "dwStencilCaps" not in code(D3D) or "dwStencilCaps = 0" in code(D3D)


def test_the_z_list_follows_the_desktop_depth():
    """(b) GUID_ZPixelFormats: D16 at a 16 bpp desktop, the 32-bit pair
    (D24X8, D24S8) at 32 bpp with Diag\\D3D32 - never both. 5be6a59 listed all
    three on every VSA-100, so a game could pick a 24-bit Z for a 16 bpp mode
    (or D16 for 32) and ContextCreate would refuse the pair. A mode change is
    a new PDEV, so the runtime asks again at the new depth."""
    info = func(D3D, "int VcrDdD3dDriverInfo(")
    z = info[info.index("geq(&p->guidInfo, &zpf)"):info.index("geq(&p->guidInfo, &misc2)")]
    assert "ULONG zl = vcr_rt_zlist(pd->rt32, pd->bpp);" in z
    for flag in ("VCR_ZL_D16", "VCR_ZL_D24X8", "VCR_ZL_D24S8"):
        assert f"if (zl & {flag})" in z, flag
    assert "answer(p, &z, sizeof(DWORD) + z.n * sizeof(DDPIXELFORMAT));" in z
    assert "z.n = pd->napalm ? 3 : 1;" not in z                    # 5be6a59's depth-blind list
    zl = func(RT, "static __inline unsigned vcr_rt_zlist(")
    assert "rt32 && desktop_bpp == 32 ? (VCR_ZL_D24X8 | VCR_ZL_D24S8) : VCR_ZL_D16" in zl


def test_d3d32_is_a_default_off_switch():
    """(c) .124's desktop is 32 bpp, so once DDBD_32 is offered every
    windowed D3D client there takes the never-run 32 bpp path. Diag\\D3D32
    (default 0) withholds it: no DDBD_32, no 32-bit Z, no 32-bit Z surface,
    and a 32 bpp target is refused like on a Voodoo3. The flag is POSITIVE,
    so a miniport that predates it never arms it."""
    assert 'x->d3d32 = VcrDiagGet(L"D3D32", 0);' in MPC
    assert "(x->d3d32 ? VCR_INFO_F_D3D32 : 0)" in MPC
    assert re.search(r"#define VCR_INFO_F_D3D32\s+0x10\b", IOH)
    flags = [int(v, 16) for v in re.findall(r"#define VCR_INFO_F_\w+\s+(0x[0-9a-f]+)", IOH)]
    assert len(flags) == len(set(flags)), "two Diag flags share a bit"
    assert "pd->rt32 = pd->napalm && (info.flags & VCR_INFO_F_D3D32) ? 1 : 0;" in E2D
    # nothing offers 32 bpp on napalm alone any more
    assert "pd->napalm ? DDBD_32" not in D3D
    assert "vcr_rt_format(c->pd->napalm" not in D3D
    can = func(DD, "static DWORD APIENTRY Dd_CanCreateSurface(")
    assert "!pd->rt32 &&" in can and "!pd->napalm &&" not in can


def test_target_validation_refuses_what_it_cannot_draw():
    """(d) VcrDd3dTarget refuses any format but 16 (or 32 where allowed) on
    every chip - 5be6a59 wrote renderMode 16 bpp for a REFUSED target on a
    VSA-100. target_of refuses a Z that does not fit (no video-memory offset,
    pitch short of a row / unaligned / past the stride field, smaller than
    the target), and depth follows the TARGET's Z: with depth keyed on the
    surface pointer, a Z the target had no offset for tested and wrote the
    aux buffer at offset 0."""
    to = func(D3D, "static ULONG target_of(")
    assert "why = vcr_rt_zcheck(fmt, c->rt->lpGbl->wWidth, c->rt->lpGbl->wHeight, c->zb != NULL, z_off," in to
    assert to.index("vcr_rt_zcheck(") < to.index("t->fmt = fmt;")
    assert "z_off = in_vidmem(c->zb) ? (ULONG)c->zb->lpGbl->fpVidMem : 0;" in to
    assert "c->target_why = why;" in to
    zc = func(RT, "static __inline unsigned vcr_rt_zcheck(")
    assert "if (!z_off)\n        return VCR_RT_WHY_ZOFF;" in zc
    assert "z_pitch < width * bytes || (z_pitch & 0xfu) || z_pitch > 0x3fffu" in zc
    assert "z_w < width || z_h < height" in zc
    cr = code(func(D3D, "static void compute_regs("))
    assert "if (c->target.z_off && c->rs[D3DRENDERSTATE_ZENABLE] == D3DZB_TRUE)" in cr
    assert "c->zb &&" not in cr
    pr = func(RT, "static __inline int vcr_rt_programmable(")
    assert "fmt == VCR_RT_16 || (fmt == VCR_RT_32 && rt32)" in pr
    # a Z refusal is logged with its reason (event 513, what 15)
    tr = func(D3D, "static void target_refused(")
    assert "VCR_EV_DD_D3D, 15, c->target_why" in tr
    ev = (KMD / "include" / "vcr_events.h").read_text()
    assert "15 target Z refused" in ev and "16 armed" in ev


def test_the_glide_release_resets_3d_state_and_keeps_the_owner_on_failure():
    """(e) A Glide client that dies without grSstWinClose leaves chipMask,
    aaCtrl (the AA state behind the V5 6000 wedges), an extended/2PPC combine
    and stencil state on chip 0 for the next D3D user. At HWCRLSEXCLUSIVE,
    after RESTORE_MODE (whose mode set turns SLI off), a VSA-100 with
    Diag\\Reset3D = 1 writes vcr_3dseq.h's reset through the bounded PCI-FIFO
    path. Default OFF: it has not run on silicon. And when RESTORE_MODE fails
    the owner is KEPT - the desktop mode is not back, SLI may still be on -
    so the 2D engine, DirectDraw and D3D stay off the chip until
    DrvAssertMode(TRUE) clears a stale owner."""
    rel = ESC[ESC.index("case VCR_HWC_HWCRLSEXCLUSIVE:"):ESC.index("case VCR_HWC_UNMAP_MEMORY:")]
    body = code(rel)
    assert body.index("IOCTL_VCR_RESTORE_MODE") < body.index("if (rc) {") < \
        body.index("VcrDdGlideReset3d(pd)") < body.index("pd->exclusive_pid = 0;")
    fail = body[body.index("if (rc) {"):body.index("}", body.index("if (rc) {"))]
    assert "break;" in fail and "exclusive_pid = 0" not in fail   # the owner is kept
    assert body.count("pd->exclusive_pid = 0;") == 1
    assert "if (pd->reset3d) {" in body
    rs = func(E2D, "BOOL VcrDdGlideReset3d(")
    assert "if (!pd->reset3d || !pd->napalm || !pd->pjRegs || !pd->g2d_ok)" in rs
    assert "n = vcr_3d_glide_reset_seq(w);" in rs
    assert "VcrDdRoom(pd, n - i < 8 ? n - i : 8)" in rs            # bounded, in FIFO-sized chunks
    assert "wr(pd, V3D_BASE + w[i].off, w[i].val);" in rs          # chip 0's window only
    assert "VcrDd2dSync(pd);" in rs
    # a Glide client KILLED without releasing: its owner is cleared at
    # DrvAssertMode(TRUE), after the mode set - the reset runs there too
    am = code(func((KMD / "display" / "vcrdd.c").read_text(), "BOOL APIENTRY DrvAssertMode("))
    assert am.index("ok = VcrDdSetMode(pd);") < am.index("if (ok && stale && pd->reset3d) {") < \
        am.index("VcrDdGlideReset3d(pd)")
    assert 'x->reset3d = VcrDiagGet(L"Reset3D", 0);' in MPC
    assert "(x->reset3d ? VCR_INFO_F_RESET3D : 0)" in MPC
    assert "pd->reset3d = pd->napalm && (info.flags & VCR_INFO_F_RESET3D) ? 1 : 0;" in E2D
    gs = func(SEQ, "static __inline unsigned vcr_3d_glide_reset_seq(")
    order = [gs.index(r) for r in ("V3D_CHIPMASK", "V3D_NOPCMD", "V3D_COMBINEMODE", "V3D_AACTRL",
                                   "V3D_STENCILMODE", "V3D_STENCILOP")]
    assert order == sorted(order)
    assert "V3D_SLICTRL" not in gs and "V3D_RENDERMODE" not in gs
    regs = (KMD / "include" / "vcr_3dregs.h").read_text()
    for name, off in (("COMBINEMODE", 0x208), ("SLICTRL", 0x20c), ("AACTRL", 0x210),
                      ("CHIPMASK", 0x214)):
        assert re.search(rf"#define V3D_{name}\s+0x{off:x}\b", regs), name
    vr = (KMD / "include" / "vcr_regs.h").read_text()
    assert re.search(r"#define VCR_3D_AACTRL\s+\(VCR_MB0_3D \+ 0x210\)", vr)   # the miniport agrees


def test_the_register_offsets_are_computed_from_the_gpl_header():
    """Every offset the new writes use, re-computed with offsetof() over the
    GPL h5 h3regs.h SstRegs - the rule vcr_3dregs.h states. Skips LOUDLY when
    the retro3dfx-glide clone is absent (run build-stack.sh)."""
    import shutil, subprocess, tempfile
    import pytest
    main_cr = Path("/home/voidsstr/development/retro-agent/voodoo-cleanroom")
    glide = next((p / "build/retro3dfx-glide" for p in (REPO / "voodoo-cleanroom", main_cr)
                  if (p / "build/retro3dfx-glide/glide3x/h5/incsrc/h3regs.h").is_file()), None)
    cc = shutil.which("gcc") or shutil.which("cc")
    if glide is None or not cc:
        pytest.skip("retro3dfx-glide clone or host compiler absent - offsets NOT re-computed")
    prog = r"""
#include <stdio.h>
#include <stddef.h>
#include "3dfx.h"
#include "h3regs.h"
#include "vcr_3dregs.h"
#define P(f, v) printf("%s %d\n", #f, (int)(offsetof(SstRegs, f) == (v)))
int main(void) {
    P(renderMode, V3D_RENDERMODE); P(stencilMode, V3D_STENCILMODE); P(stencilOp, V3D_STENCILOP);
    P(colBufferAddr, V3D_COLBUFFERADDR); P(auxBufferStride, V3D_AUXBUFFERSTRIDE);
    P(combineMode, V3D_COMBINEMODE); P(sliCtrl, V3D_SLICTRL); P(aaCtrl, V3D_AACTRL);
    P(chipMask, V3D_CHIPMASK); P(nopCMD, V3D_NOPCMD); P(zaColor, V3D_ZACOLOR);
    return 0;
}
"""
    with tempfile.TemporaryDirectory() as d:
        c = Path(d) / "o.c"
        c.write_text(prog)
        exe = Path(d) / "o"
        subprocess.run([cc, "-I", str(glide / "glide3x/h5/incsrc"), "-I", str(glide / "swlibs/fxmisc"),
                        "-I", str(KMD / "include"), str(c), "-o", str(exe)], check=True)
        out = subprocess.run([str(exe)], check=True, capture_output=True, text=True).stdout
    bad = [ln for ln in out.split("\n") if ln and not ln.endswith(" 1")]
    assert not bad, bad


def test_d3dprobe_noz_and_the_depth_stencil_caps_report():
    """(f) --noz (EnableAutoDepthStencil FALSE) lets the first 32 bpp step
    on silicon run renderMode + the colour fastfill with no aux buffer write
    at all: clears drop D3DCLEAR_ZBUFFER (with no Z it fails the whole Clear)
    and ztest is skipped. caps answers, with no device and no mode switch,
    how the runtime maps the HAL's Z list: CheckDeviceFormat and
    CheckDepthStencilMatch of D24X8 / D24S8 / D16 for X8R8G8B8 and R5G6B5,
    and CheckDeviceType of a fullscreen HAL device in each."""
    assert 'else if (!strcmp(a, "--noz")) g_noz = 1;' in PROBE
    assert "pp.EnableAutoDepthStencil = !g_noz;" in PROBE
    assert "D3DCLEAR_TARGET | (g_noz ? 0 : D3DCLEAR_ZBUFFER)" in func(PROBE, "static int frame_begin(")
    zt = PROBE[PROBE.index('!strcmp(t, "ztest")'):]
    assert zt.index("if (g_noz) {") < zt.index("frame_begin(0)")
    assert '\\"noz\\":%d' in PROBE
    main = PROBE[PROBE.index("int main("):]
    caps = main[main.index('if (!strcmp(mode, "caps")) {'):main.index("/* a device: windowed")]
    for f in ("D3DFMT_D24X8", "D3DFMT_D24S8", "D3DFMT_D16", "D3DFMT_X8R8G8B8", "D3DFMT_R5G6B5"):
        assert f in caps, f
    assert "IDirect3D8_CheckDepthStencilMatch(d3d, 0, D3DDEVTYPE_HAL, zrt[i].f," in caps
    assert "IDirect3D8_CheckDeviceType(d3d, 0, D3DDEVTYPE_HAL, zrt[i].f, zrt[i].f," in caps
    assert '"z24x8"' in caps and "zmatch" in caps
    # one key per name: "hal" is already GetDeviceCaps' HRESULT
    assert re.findall(r'\\"(\w+)\\":\{', caps).count("hal") == 0
    assert '\\"hal_fullscreen\\":{' in caps
    # caps makes no device and switches nothing
    assert "CreateDevice" not in code(caps) and "vcr_pace_before_switch" not in caps
