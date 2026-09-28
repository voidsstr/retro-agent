"""vcr-kmd: text on the 2D engine (clean-room lane, 2026-09-27).

DrvTextOut used to hand every call to EngTextOut - the CPU drawing into the
frame buffer through the punt layer. Now a call the gate in
include/vcr_text.h accepts is drawn by the engine: the opaque rectangle as a
solid fill, the glyphs as ONE host-to-screen blit with a 1 bpp source
(monochrome expansion, transparent) of a mask the CPU composed from the
visible parts of every glyph, then the underline/strike-out rectangles.
Verified on the 86Box Voodoo3 bed with gdilab's text test at 8/16/32 bpp:
0 bad against GDI's own software rendering, every case on the engine
(evidence/86box_v3/2d_*).

The pure parts (the gate, the clip, the dword stream, the mask) are run by
tests/native/test_vcr_kmd_text.c. These pin the glue that cannot run on the
host: what the driver writes, in which order, where it falls back, and the
Diag switch that restores the old behaviour.
"""
import re
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
KMD = REPO / "voodoo-cleanroom" / "vcr-kmd"
PUNT = (KMD / "display" / "vcrdd_punt.c").read_text()
G2D = (KMD / "display" / "vcrdd_2d.c").read_text()
ESC = (KMD / "display" / "vcrdd_escape.c").read_text()
VCRDD_H = (KMD / "display" / "vcrdd.h").read_text()
TEXT_H = (KMD / "include" / "vcr_text.h").read_text()
IOCTL_H = (KMD / "include" / "vcr_ioctl.h").read_text()
MP = (KMD / "miniport" / "vcrmp.c").read_text()
GDILAB = (KMD / "tools" / "gdilab.c").read_text()
LAB_RUN = (KMD / "tools" / "lab_run.py").read_text()
MAKEFILE = (KMD / "Makefile").read_text()


def func(src, sig):
    i = src.index(sig)
    return src[i:src.index("\n}\n", i)]


def test_drvtextout_tries_the_engine_and_otherwise_does_what_it_always_did():
    body = func(PUNT, "BOOL APIENTRY DrvTextOut(")
    assert "if (pd && accel_text(pd, str, fo, co, extra, opaque, fore, back, mix))" in body
    # the old call, unchanged, after bits() (which waits for the engine)
    assert "return EngTextOut(bits(pso), str, fo, co, extra, opaque, fore, back, org, mix);" in body
    assert "VcrDd2dSync(pd);" in func(PUNT, "static SURFOBJ *bits(")


def test_the_gate_decides_and_the_switch_is_looked_at_first():
    acc = func(PUNT, "static BOOL accel_text(")
    assert "why = str ? vcr_text_gate(&q) : VCR_TEXT_R_GLYPH;" in acc
    assert "q.off = pd->text_off;" in acc
    assert "q.engine = pd->g2d_ok && pd->pjRegs && !pd->exclusive_pid;" in acc
    gate = func(TEXT_H, "static inline vcr_u32 vcr_text_gate(")
    order = [gate.index(r) for r in ("VCR_TEXT_R_OFF", "VCR_TEXT_R_ENGINE", "VCR_TEXT_R_MIX",
                                     "VCR_TEXT_R_BRUSH", "VCR_TEXT_R_FONT", "VCR_TEXT_R_LAYOUT",
                                     "VCR_TEXT_R_EXTRA")]
    assert order == sorted(order)
    # a refused call is counted by reason, and a partial draw is redrawn whole
    assert "pd->text_punts++;" in acc
    assert "return FALSE;" in acc


def test_engine_text_is_opt_in_and_reaches_the_display_driver_without_a_reboot():
    """Diag\\Accel2DText = 1 -> VCR_INFO_F_TEXT2D -> pd->text_off = 0. It is a
    POSITIVE flag, default OFF (2026-09-27): the engine text path was 0-bad on
    the 86Box bed but drew fewer glyphs/s there than the software path, and
    has never run on silicon - and a miniport that predates the flag must
    never arm it. Read at every IOCTL_VCR_INFO, so the next PDEV (a mode
    change) picks it up. Old (default-on) value: Accel2DText absent = on via
    a negative VCR_INFO_F_NO_TEXT2D."""
    assert '(VcrDiagGet(L"Accel2DText", 0) ? VCR_INFO_F_TEXT2D : 0)' in MP
    assert '(VcrDiagGet(L"Accel2DText", 1) ? 0 : VCR_INFO_F_NO_TEXT2D)' not in MP
    assert re.search(r"#define VCR_INFO_F_TEXT2D\s+0x40\b", IOCTL_H)
    assert "VCR_INFO_F_NO_TEXT2D" not in IOCTL_H
    flags = [int(v, 16) for v in re.findall(r"#define VCR_INFO_F_\w+\s+(0x[0-9a-fA-F]+)", IOCTL_H)]
    assert len(flags) == len(set(flags)), "two VCR_INFO_F_* share a bit"
    init = func(G2D, "void VcrDd2dInit(")
    assert "pd->text_off = 1;" in init          # also when IOCTL_VCR_INFO fails
    assert init.index("pd->text_off = 1;") < init.index("IOCTL_VCR_INFO")
    assert "pd->text_off = (info.flags & VCR_INFO_F_TEXT2D) ? 0 : 1;" in init


def test_a_glyph_blit_is_a_host_blit_started_by_its_data_and_exactly_that_long():
    glyph = func(G2D, "BOOL VcrDd2dMonoGlyph(")
    seq = [glyph.index(w) for w in ("wr(pd, G_DSTSIZE,", "wr(pd, G_DSTXY,",
                                    "wr(pd, G_COMMAND, VCR_TEXT_CMD);", "vcr_glyph_stream(")]
    assert seq == sorted(seq), "the command is written before its data, after its size and place"
    assert "== want" in glyph and "ULONG want = vcr_glyph_dwords(p);" in glyph
    assert "CMD_GO" not in glyph and "G_CLIP" not in glyph
    assert "#define G_LAUNCH            R2D(0x80)" in G2D
    assert "wr(m->pd, G_LAUNCH, dw);" in func(G2D, "static int mono_put(")
    assert re.search(r"#define VCR_TEXT_CMD\s+\(VCR_2D_CMD_HOST_BLT \| VCR_2D_CMD_TRANSPARENT \| "
                     r"\(VCR_2D_ROP_SRCCOPY << 24\)\)", TEXT_H)
    assert re.search(r"#define VCR_TEXT_SRCFMT \(VCR_2D_SRC_1BPP \| VCR_2D_SRC_PACK_8\)", TEXT_H)


def test_the_run_setup_points_the_engine_at_the_desktop_with_a_mono_source():
    begin = func(G2D, "BOOL VcrDd2dMonoBegin(")
    for w in ("wr(pd, G_SRCFORMAT, VCR_TEXT_SRCFMT);", "wr(pd, G_SRCXY, 0);",
              "wr(pd, G_COMMANDEX, 0);", "wr(pd, G_COLORFORE, color);",
              "wr(pd, G_CLIP0MAX, 0x1fff1fff);", "if (!VcrDdRoom(pd, 8))"):
        assert w in begin, w
    assert "!usable(pd)" in begin


def test_every_fifo_wait_is_bounded_and_giving_up_turns_acceleration_off():
    room = func(G2D, "static BOOL mono_room(")
    assert "i < SPIN_CAP" in room and "give_up(pd, 1, s);" in room
    assert "m->credit >= n" in room            # credit from the last status read
    assert "pd->text_fifo_waits++;" in room
    glyphs = func(PUNT, "static ULONG text_glyphs(")
    assert "return pd->g2d_ok ? VCR_TEXT_R_GLYPH : VCR_TEXT_R_GAVEUP;" in glyphs


def test_a_string_is_one_blit_of_a_mask_composed_on_the_cpu():
    glyphs = func(PUNT, "static ULONG text_glyphs(")
    # pass 1: the union; pass 2 per band: clear, OR every part, one blit
    assert glyphs.index("while ((r = ti_next(&t, &gb, &gx, &gy)) > 0)") < glyphs.index(
        "memset(pd->text_mask, 0,")
    assert "vcr_mask_rows((ULONG)(u.r - u.l), VCR_TEXT_MASK_BYTES)" in glyphs
    assert "vcr_mask_or(pd->text_mask, mst," in glyphs
    assert "VcrDd2dMonoGlyph(&m, pd->text_mask, whole.w, &whole)" in glyphs
    # no buffer: one blit per glyph part, as the first version did
    assert "VcrDd2dMonoGlyph(&m, gb->aj, gb->sizlBitmap.cx, &part)" in glyphs
    init = func(G2D, "void VcrDd2dInit(")
    assert "EngAllocMem(0, VCR_TEXT_MASK_BYTES, VCRDD_TAG)" in init
    term = func(G2D, "void VcrDd2dTerm(")
    assert "EngFreeMem(pd->text_mask);" in term and "pd->text_mask = NULL;" in term


def test_gdis_order_opaque_then_glyphs_then_extras_each_clipped():
    rect = func(PUNT, "static ULONG text_rect(")
    i_opq = rect.index("if (opaque) {")
    i_gly = rect.index("why = text_glyphs(pd, str, clip, fg);")
    i_ext = rect.index("for (; extra && (extra->left || extra->top || extra->right || extra->bottom); extra++)")
    assert i_opq < i_gly < i_ext
    assert rect.count("vcr_rect_isect(") == 2
    acc = func(PUNT, "static BOOL accel_text(")
    # DC_TRIVIAL / DC_RECT through vcr_text_clip_rect; DC_COMPLEX enumerated
    assert "vcr_text_clip_rect(cplx, &rb, &surf, &clip)" in acc
    assert "CLIPOBJ_cEnumStart(co, FALSE, CT_RECTANGLES, CD_ANY, 0);" in acc
    assert "vcr_rect_isect(&rb, &surf, &clip)" in acc


def test_fixed_pitch_positions_and_glyph_limits():
    it = func(PUNT, "static int ti_next(")
    assert "vcr_text_pos(t->str->ulCharInc, t->k++, t->x0, t->y0, g->ptl.x, g->ptl.y, &x, &y);" in it
    assert "t->more == (BOOL)DDI_ERROR" in it
    ok = func(PUNT, "static int glyph_ok(")
    assert "VCR_TEXT_MAX_GLYPH" in ok


def test_the_ddi_values_are_asserted_against_winddi_at_compile_time():
    for a in ("VCR_FO_GRAY16 == FO_GRAY16", "VCR_FO_CLEARTYPE_X == FO_CLEARTYPE_X",
              "VCR_SO_VERTICAL == SO_VERTICAL", "VCR_DC_COMPLEX == DC_COMPLEX",
              "VCR_MIX_COPYPEN_BOTH == ((R2_COPYPEN << 8) | R2_COPYPEN)",
              "VCR_2DS_PUNT_SLOTS == VCR_TEXT_R_MAX"):
        assert a in PUNT, a
    # and the header's copies are winddi.h's values, where the header is here
    w = Path("/usr/i686-w64-mingw32/include/winddi.h")
    if w.exists():
        t = w.read_text(errors="replace")
        for name, val in (("FO_GRAY16", 0x10000), ("FO_CLEARTYPE_X", 0x10000000),
                          ("FO_CLEARTYPE_Y", 0x20000000), ("SO_VERTICAL", 4),
                          ("SO_REVERSED", 8)):
            m = re.search(r"#define %s\s+(0[xX][0-9a-fA-F]+|\d+)" % name, t)
            assert m and int(m.group(1), 0) == val, name


def test_the_stats_escape_is_read_only_and_answers_only_a_whole_struct():
    st = ESC[ESC.index("case VCR_ESC_2D_STATS: {"):]
    st = st[:st.index("return n;")]
    # a whole struct: today's, or the one before the 2026-09-27 counters
    # (test_vcr_kmd_patline.py; the layout is pinned in test_vcr_kmd_abi.c)
    assert "if (!pvOut || n < VCR_2DS_SIZE_V1)" in st
    assert "st->text_calls = pd->text_calls;" in st and "st->text_fifo_waits" in st
    assert "pd->" in st and not re.search(r"pd->\w+\s*=[^=]", st), "the escape changes nothing"
    assert "return esc > VCR_ESC_BASE && esc <= VCR_ESC_2D_STATS;" in ESC
    # DD_STATS keeps its 16 bytes (vcrctl info reads it)
    assert "return 4 * sizeof(ULONG);" in ESC


def test_gdilab_compares_text_with_gdis_software_rendering_and_knows_who_drew_it():
    for c in ('"region with holes (complex)"', '"ETO_CLIPPED through the glyphs"',
              '"cut by the left edge"', '"long string (STROBJ batches)"',
              '"underline+strikeout transparent"', '"courier fixed opaque"',
              '"times italic transparent (overlap)"', '"text, CPU pixels, text"'):
        assert c in GDILAB, c
    case = func(GDILAB, "static void text_case(")
    # the same ExtTextOut on the screen and on a DIB-engine bitmap, compared exactly
    assert "text_draw(g_wdc, c);" in case and "text_draw(g_ref, c);" in case
    assert "if (g != r)" in case
    assert "have = stats2d(&a);" in case and "have = stats2d(&b) && have;" in case
    assert "t->empty++" in case                   # a case that draws nothing is not a pass
    assert "--require-text-accel" in GDILAB and "bad_text_path" in GDILAB
    assert "ExtEscape(g_wdc, VCR_ESC_2D_STATS" in GDILAB and "GdiFlush();" in func(
        GDILAB, "static int stats2d(")
    # the bench reads back (waits for the engine) before it stops the clock
    bench = func(GDILAB, "static void text_bench(")
    assert bench.index("GetPixel(g_wdc, 1, 1);") < bench.rindex("QueryPerformanceCounter(&t1);")
    assert '"--bench-ms"' in LAB_RUN and '"--tests"' in LAB_RUN
    assert "-Iinclude tools/gdilab.c" in MAKEFILE
