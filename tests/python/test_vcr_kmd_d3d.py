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
    silicon until d3dprobe render --full --bpp 32 passes on the card."""
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
    assert "w3(pd, V3D_RENDERMODE, vcr_rt_rendermode(t->fmt));" in tg
    assert "else if (t->fmt != VCR_RT_16)" in tg                  # never 32 bpp on a Voodoo3
    assert tg.index("if (pd->napalm)") < tg.index("V3D_RENDERMODE")
    assert "BS_LINEAR_STRIDE(t->rt_pitch)" in tg                   # stride in BYTES
    # Z spans the aux buffer: 16 bits, or 24 at 32 bpp - iterated and cleared
    assert "wf(pd, V3D_SVZ, p[2] * d->z_scale);" in e3d
    assert "t->fmt == VCR_RT_32 ? 16777215.0f : 65535.0f" in func(e3d, "void VcrDd3dDrawTarget(")
    assert "z * 16777215.0f + 0.5f" in func(e3d, "BOOL VcrDd3dClear(")

    to = func(D3D, "static ULONG target_of(")
    assert "vcr_rt_format(c->pd->napalm, rtb, zb)" in to
    assert to.index("if (fmt == VCR_RT_REFUSED)") < to.index("t->rt_off =")   # refused: empty
    cc_ = func(D3D, "static DWORD APIENTRY D3d_ContextCreate(")
    assert "DDERR_INVALIDPIXELFORMAT" in cc_ and "ContextCreate refused" in cc_
    walk = func(D3D, "static HRESULT walk(")
    assert "VcrDd3dDrawTarget(&w->d, &c->target);" in walk
    assert "w->drawable || (c->target.rt_off" not in walk          # a refused switch stops drawing
    hal = func(D3D, "void VcrDdD3dHalInfo(")
    assert "d->dwDeviceRenderBitDepth = DDBD_16 | (pd->napalm ? DDBD_32 : 0);" in hal
    assert "d->dwDeviceZBufferBitDepth = DDBD_16 | (pd->napalm ? DDBD_32 : 0);" in hal
    assert "hal->ddCaps.dwZBufferBitDepths = DDBD_16 | (pd->napalm ? DDBD_32 : 0);" in hal
    info = func(D3D, "int VcrDdD3dDriverInfo(")
    assert "z.n = pd->napalm ? 3 : 1;" in info                     # D16 alone on a Voodoo3
    assert "z.f[2].dwStencilBitMask = 0xff000000;" in info and "0x00ffffff" in info
    can = func(DD, "static DWORD APIENTRY Dd_CanCreateSurface(")
    assert "!pd->napalm" in can and "DDSCAPS_ZBUFFER" in can and "DDERR_INVALIDPIXELFORMAT" in can
