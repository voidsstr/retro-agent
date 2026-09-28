"""vcr-kmd: 8x8 mono pattern fills and axis-aligned lines on the 2D engine
(clean-room lane, 2026-09-27).

Both used to be GDI's alone. With Diag\\Accel2DPattern = 1 a brush realized
from an 8x8 1 bpp bitmap (the hatch brushes, and the 50 % grey of every drag
rectangle and dotted focus frame) is filled by the engine's rectangle fill
with the mono pattern on; with Diag\\Accel2DLine = 1 a solid cosmetic
R2_COPYPEN line that is horizontal or vertical is an engine rectangle. Both
default OFF. Verified on the 86Box Voodoo3 bed with gdilab's patline test at
8/16/32 bpp: 0 bad against GDI's own software rendering, every accel case on
the engine and the slanted/dotted/XOR cases in software
(evidence/86box_v3/patline).

The pure parts (the line rectangle, the pattern packing and offset, the ROPs)
are run by tests/native/test_vcr_kmd_line.c. These pin the glue that cannot
run on the host.
"""
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
KMD = REPO / "voodoo-cleanroom" / "vcr-kmd"
PUNT = (KMD / "display" / "vcrdd_punt.c").read_text()
G2D = (KMD / "display" / "vcrdd_2d.c").read_text()
VCRDD = (KMD / "display" / "vcrdd.c").read_text()
ESC = (KMD / "display" / "vcrdd_escape.c").read_text()
MP = (KMD / "miniport" / "vcrmp.c").read_text()
GDILAB = (KMD / "tools" / "gdilab.c").read_text()


def func(src, sig):
    i = src.index(sig)
    return src[i:src.index("\n}\n", i)]


def test_both_switches_default_on_and_are_read_at_every_info_query():
    """Default ON since 2026-09-28 (0 bad on .124 at 16/32 bpp, faster than
    the software path in every gdilab plbench case); Diag = 0 turns it off."""
    fill = func(MP, "static void fill_info(")
    assert '(VcrDiagGet(L"Accel2DPattern", 1) ? VCR_INFO_F_PAT2D : 0)' in fill
    assert '(VcrDiagGet(L"Accel2DLine", 1) ? VCR_INFO_F_LINE2D : 0)' in fill
    init = func(G2D, "void VcrDd2dInit(")
    # cleared BEFORE the query, so a failed IOCTL leaves them off
    assert init.index("pd->pat_on = pd->line_on = 0;") < init.index("IOCTL_VCR_INFO")
    assert "pd->pat_on = (info.flags & VCR_INFO_F_PAT2D) ? 1 : 0;" in init
    assert "pd->line_on = (info.flags & VCR_INFO_F_LINE2D) ? 1 : 0;" in init


def test_drvrealizebrush_is_hooked_and_takes_only_8x8_1bpp_brushes():
    assert "{ INDEX_DrvRealizeBrush,   (PFN)0 }," in VCRDD
    assert "g_drvfn[23].pfn = (PFN)DrvRealizeBrush;" in VCRDD
    # the DirectDraw entries moved up one (and again for
    # DrvIcmSetDeviceGammaRamp, 2026-09-28), and nothing else shares 23
    assert "g_drvfn[24].pfn = (PFN)DrvIcmSetDeviceGammaRamp;" in VCRDD
    assert "g_drvfn[25].pfn = (PFN)DrvGetDirectDrawInfo;" in VCRDD
    assert "g_drvfn[27].pfn = (PFN)DrvDisableDirectDraw;" in VCRDD
    assert VCRDD.count("g_drvfn[23]") == 1
    rb = func(PUNT, "BOOL APIENTRY DrvRealizeBrush(")
    assert "pat->iBitmapFormat != BMF_1BPP" in rb
    assert "pat->sizlBitmap.cx != 8 || pat->sizlBitmap.cy != 8" in rb
    assert "!pd->pat_on" in rb                      # switch off: nothing is realized
    assert "rb->magic = VCR_RB_MAGIC;" in rb
    # 1 bpp: bit 1 = palette entry 1 = fore, bit 0 = entry 0 = back
    assert "rb->fore = xo ? XLATEOBJ_iXlate(xo, 1)" in rb
    assert "rb->back = xo ? XLATEOBJ_iXlate(xo, 0)" in rb


def test_a_hatch_brush_mask_is_taken_only_when_it_is_the_patterns_own_bits():
    """GDI hands a hatch brush over WITH a mask (the 86Box bed: every hatch
    style); ignoring it would draw a different brush, refusing it left every
    hatch in software. Taken only when mask == pattern."""
    rb = func(PUNT, "BOOL APIENTRY DrvRealizeBrush(")
    assert "if (pd && pd->pat_on && pat && msk && !mask_is_pattern(pat, msk))" in rb
    assert rb.index("mask_is_pattern") < rb.index("BRUSHOBJ_pvAllocRbrush")
    m = func(PUNT, "static int mask_is_pattern(")
    assert "msk->iBitmapFormat != BMF_1BPP" in m
    assert "return p0 == m0 && p1 == m1;" in m


def test_drvbitblt_tries_the_pattern_only_for_a_brush_with_no_source_and_no_mask():
    bb = func(PUNT, "BOOL APIENTRY DrvBitBlt(")
    assert "if (pd && !mask) {" in bb
    assert ("if (pd->pat_on && bo && bo->iSolidColor == 0xffffffffu && !src &&\n"
            "            accel_patfill(pd, co, rd, bo, pb, rop))") in bb
    # after the solid fill (a solid brush never reaches it) and before the fallback
    assert bb.index("accel_fill(pd, co, rd, bo->iSolidColor)") < bb.index("accel_patfill(")
    assert bb.index("accel_patfill(") < bb.index("return EngBitBlt(")


def test_the_pattern_fill_refuses_what_it_does_not_know():
    pf = func(PUNT, "static BOOL accel_patfill(")
    for guard in ("!pd->pat_on", "!pd->g2d_ok", "pd->exclusive_pid", "pd->bpp == 24",
                  "!vcr_pat_rop(rop, &rop3, &transparent)"):
        assert guard in pf, guard
    assert "if (!rb || rb->magic != VCR_RB_MAGIC) {" in pf     # a brush realized by GDI
    assert "px = vcr_pat_offset(pb ? pb->x : 0, 0);" in pf
    assert "py = vcr_pat_offset(pb ? pb->y : 0, 0);" in pf
    assert "CLIPOBJ_cEnumStart(co, FALSE, CT_RECTANGLES, CD_ANY, 0);" in pf


def test_the_engine_command_is_a_mono_pattern_rectangle_fill():
    assert "#define G_COLORBACK         R2D(0x60)" in G2D
    assert "#define G_PATTERN0          R2D(0x100)" in G2D
    assert "#define G_PATTERN1          R2D(0x104)" in G2D
    assert "#define CMD_MONO_PATTERN    (1u << 13)" in G2D
    assert "#define CMD_TRANSPARENT     (1u << 16)" in G2D
    assert "#define CMD_PATX(x)         ((ULONG)((x) & 7) << 17)" in G2D
    assert "#define CMD_PATY(y)         ((ULONG)((y) & 7) << 20)" in G2D
    f = func(G2D, "BOOL VcrDd2dPatFill(")
    assert "CMD_RECTFILL | CMD_GO | CMD_MONO_PATTERN | CMD_ROP(rop3 & 0xff)" in f
    # the pattern offset is relative to the engine's origin: the base skew off
    assert "CMD_PATX(patx - dsk)" in f
    assert "(transparent ? CMD_TRANSPARENT : 0)" in f
    for reg in ("G_PATTERN0, pat0", "G_PATTERN1, pat1", "G_COLORBACK, back", "G_COLORFORE, fore"):
        assert reg in f, reg
    # bounded FIFO wait, and the command last
    assert "if (!VcrDdRoom(pd, 12))" in f
    assert f.index("wr(pd, G_DSTXY") < f.index("wr(pd, G_COMMAND, cmd);")


def test_a_line_is_taken_only_when_solid_cosmetic_copypen_and_axis_aligned():
    sp = func(PUNT, "BOOL APIENTRY DrvStrokePath(")
    for guard in ("pd->line_on", "pd->g2d_ok", "!pd->exclusive_pid", "pd->bpp != 24",
                  "bo->iSolidColor != 0xffffffffu", "!(la->fl & (LA_GEOMETRIC | LA_ALTERNATE))",
                  "!la->pstyle", "line_mix_ok(mix)"):
        assert guard in sp, guard
    # a path is taken whole or not at all: a check pass, then the draw pass
    assert sp.index("stroke_axis_path(pd, po, co, bo->iSolidColor, 0)") < \
        sp.index("stroke_axis_path(pd, po, co, bo->iSolidColor, 1)")
    assert "return EngStrokePath(bits(pso), po, co, xo, bo, org, la, mix);" in sp
    walk = func(PUNT, "static BOOL stroke_axis_path(")
    assert "if (d.flags & PD_BEZIERS)" in walk
    assert "if (x != lx && y != ly)" in walk          # slanted: GDI's
    assert "vcr_fix_whole(d.pptfx[i].x, &x)" in walk  # only whole pixels
    assert "(d.flags & PD_ENDSUBPATH) && (d.flags & PD_CLOSEFIGURE)" in walk
    lt = func(PUNT, "BOOL APIENTRY DrvLineTo(")
    assert "(x1 == x2 || y1 == y2)" in lt
    assert "return EngLineTo(bits(pso), co, bo, x1, y1, x2, y2, bounds, mix);" in lt
    assert "return (mix & 0xff) == VCR_R2_COPYPEN;" in func(PUNT, "static int line_mix_ok(")
    assert "#define VCR_R2_COPYPEN 13" in PUNT
    hl = func(PUNT, "static BOOL accel_hline(")
    assert "if (!vcr_line_rect(x1, y1, x2, y2, &h))" in hl
    assert "return accel_fill(pd, co, &r, color);" in hl


def test_the_stats_escape_reports_the_new_counters_and_still_answers_an_old_caller():
    esc = ESC[ESC.index("case VCR_ESC_2D_STATS:"):]
    esc = esc[:esc.index("return n;")]
    assert "(pd->g2d_ok && pd->pat_on ? VCR_2DS_F_PAT : 0)" in esc
    assert "(pd->g2d_ok && pd->line_on ? VCR_2DS_F_LINE : 0)" in esc
    for c in ("pat_fills", "pat_punts", "line_fills", "line_punts"):
        assert f"st->{c} = pd->{c};" in esc, c
    # the size asked for, from the pre-2026-09-27 struct up to the whole one
    assert "n = cjOut < sizeof all ? cjOut : (ULONG)sizeof all;" in esc
    assert "if (!pvOut || n < VCR_2DS_SIZE_V1)" in esc
    assert "memcpy(pvOut, st, n);" in esc


def test_gdilab_patline_compares_with_gdi_and_knows_who_drew_it():
    assert 'do_pl = has_test(tests, "patline");' in GDILAB
    case = func(GDILAB, "static void pl_case(")
    assert "c->draw(g_wdc);" in case and "c->draw(g_ref);" in case
    assert "(c->accel && !eng) || (!c->accel && c->kind == PL_LINE && eng)" in case
    for name in ('"hatches opaque (6 styles)", pl_hatch_opaque, PL_PAT, 1',
                 '"hatches transparent", pl_hatch_transparent, PL_PAT, 1',
                 '"grey PATINVERT, brush origin 7,1", pl_grey_origin_invert, PL_PAT, 1',
                 '"slanted lines (GDI\'s)", pl_diag, PL_LINE, 0',
                 '"XOR pen (GDI\'s)", pl_xor, PL_LINE, 0'):
        assert name in GDILAB, name
    assert '"\\"bad_patline_path\\":%d,\\"pat_engine_fills\\":%d,\\"line_engine_calls\\":%d,"' \
        in GDILAB
    # the throughput bench: its own token ("plbench" must not also run "bench")
    assert 'do_plb = has_test(tests, "plbench");' in GDILAB
    assert 'do_bench = has_test(tests, "bench");' in GDILAB
    assert "if (do_plb && tsetup)\n        pl_bench(bench_ms);" in GDILAB
    # a patline failure fails the run
    assert "pt.bad + pt.path_bad;" in GDILAB
