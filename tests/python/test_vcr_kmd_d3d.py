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
    tex = func(D3D, "static BOOL tex_regs(")
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
