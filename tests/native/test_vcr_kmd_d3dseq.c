/* test_vcr_kmd_d3dseq.c - TRUE-SOURCE test of the 3D register writes the
 * vcr-kmd display driver makes (voodoo-cleanroom/vcr-kmd/include/vcr_3dseq.h,
 * written by display/vcrdd_3d.c VcrDd3dTarget and display/vcrdd_2d.c
 * VcrDdGlideReset3d) and of the target rules they rest on (vcr_rtfmt.h).
 *
 * The hardening of the 32 bpp render targets (commit 5be6a59) before any
 * deploy to the V5 6000 on .124 (2026-09-27), none of it run on silicon yet:
 *   - a VSA-100 target writes stencilMode = stencilOp = 0 after renderMode:
 *     the stencil byte of a 32 bpp aux buffer is live, and nothing in vcr-kmd
 *     ever wrote those registers - a Glide/OpenGL session's stencil state
 *     would fail or overwrite every D3D pixel;
 *   - EVERYTHING ELSE is the old sequence, write for write: a Banshee/Voodoo3
 *     target is the 6 writes it always was, a VSA-100 16 bpp target is the
 *     old 7 plus exactly those two;
 *   - 32 bpp is programmed only with Diag\D3D32 (rt32) - master offered it on
 *     every VSA-100 - and a format that is neither 16 nor 32 is refused on
 *     every chip (master wrote renderMode 16 bpp for a REFUSED target);
 *   - the Z buffer must fit the target (offset, pitch, size) - master took
 *     any attached Z, and a Z not in video memory put the aux buffer at 0;
 *   - GUID_ZPixelFormats follows the desktop depth, D16 or the 32-bit pair,
 *     never both (master listed all three on a VSA-100);
 *   - the Glide release reset: chipMask first, Glide's 12-NOP 2PPC flush,
 *     then combineMode, aaCtrl, stencil - never sliCtrl or renderMode.
 * Offsets were computed with offsetof() over the GPL h5 h3regs.h SstRegs
 * (tests/python/test_vcr_kmd_d3d.py re-computes them when the clone is here).
 */
#include <string.h>
#include "munit.h"
#include "../../voodoo-cleanroom/vcr-kmd/include/vcr_3dseq.h"

/* what VcrDd3dTarget wrote before this change (5be6a59), for a 16 bpp target
 * at rt_off/rt_pitch with a Z at z_off/z_pitch, width x height */
static unsigned old_target(unsigned napalm, vcr_regw *w, unsigned rt_off, unsigned rt_pitch,
                           unsigned z_off, unsigned z_pitch, unsigned width, unsigned height)
{
    unsigned n = 0;
    if (napalm) {
        w[n].off = 0x1e0; w[n++].val = 0x001e0000;           /* RM_16BPP | RM_RGBA_WRITE */
    }
    w[n].off = 0x1ec; w[n++].val = rt_off;
    w[n].off = 0x1f0; w[n++].val = rt_pitch & 0x3fff;
    w[n].off = 0x1f4; w[n++].val = z_off;
    w[n].off = 0x1f8; w[n++].val = (z_pitch ? z_pitch : rt_pitch) & 0x3fff;
    w[n].off = 0x118; w[n++].val = width & 0xfff;
    w[n].off = 0x11c; w[n++].val = height & 0xfff;
    return n;
}

static int same(const vcr_regw *a, const vcr_regw *b, unsigned n)
{
    unsigned i;
    for (i = 0; i < n; i++)
        if (a[i].off != b[i].off || a[i].val != b[i].val)
            return 0;
    return 1;
}

TEST(the_offsets_are_the_gpl_headers) {
    /* offsetof(SstRegs, ...) over glide3x/h5/incsrc/h3regs.h */
    CHECK_EQ_U(V3D_RENDERMODE, 0x1e0u);
    CHECK_EQ_U(V3D_STENCILMODE, 0x1e4u);
    CHECK_EQ_U(V3D_STENCILOP, 0x1e8u);
    CHECK_EQ_U(V3D_COLBUFFERADDR, 0x1ecu);
    CHECK_EQ_U(V3D_AUXBUFFERSTRIDE, 0x1f8u);
    CHECK_EQ_U(V3D_COMBINEMODE, 0x208u);
    CHECK_EQ_U(V3D_SLICTRL, 0x20cu);
    CHECK_EQ_U(V3D_AACTRL, 0x210u);
    CHECK_EQ_U(V3D_CHIPMASK, 0x214u);
    CHECK_EQ_U(V3D_NOPCMD, 0x120u);
}

TEST(a_voodoo3_target_is_the_six_writes_it_always_was) {
    vcr_regw w[VCR_3D_TARGET_MAX + 4], o[16];
    unsigned n, on;
    /* 800x600x16 back buffer at 2 MB with its Z at 3 MB */
    n = vcr_3d_target_seq(0, 0, VCR_RT_16, 0x200000, 1600, 0x300000, 1600, 800, 600, w);
    on = old_target(0, o, 0x200000, 1600, 0x300000, 1600, 800, 600);
    CHECK_EQ_U(n, 6u);
    CHECK_EQ_U(on, 6u);
    CHECK(same(w, o, 6), "Voodoo3: byte-identical to the old VcrDd3dTarget");
    /* no Z: the aux stride falls back to the colour pitch, as before */
    n = vcr_3d_target_seq(0, 0, VCR_RT_16, 0x200000, 512, 0, 0, 256, 256, w);
    on = old_target(0, o, 0x200000, 512, 0, 0, 256, 256);
    CHECK_EQ_U(n, on);
    CHECK(same(w, o, n), "Voodoo3 without Z: identical");
    /* nothing a Voodoo3 has no register for, whatever rt32 says */
    n = vcr_3d_target_seq(0, 1, VCR_RT_16, 0x200000, 512, 0, 0, 256, 256, w);
    CHECK_EQ_U(n, 6u);
    CHECK(w[0].off != V3D_RENDERMODE && w[0].off != V3D_STENCILMODE, "no VSA-100 register");
    CHECK_EQ_U(vcr_3d_target_seq(0, 1, VCR_RT_32, 0x200000, 1024, 0, 0, 256, 256, w), 0u);
}

TEST(a_vsa100_16bpp_target_is_the_old_seven_plus_the_stencil_pair) {
    vcr_regw w[VCR_3D_TARGET_MAX + 4], o[16], strip[16];
    unsigned n, on, i, k = 0;
    int rt32;
    for (rt32 = 0; rt32 <= 1; rt32++) {         /* Diag\D3D32 does not touch 16 bpp */
        n = vcr_3d_target_seq(1, (unsigned)rt32, VCR_RT_16, 0x200000, 1600, 0x300000, 1600, 800,
                              600, w);
        on = old_target(1, o, 0x200000, 1600, 0x300000, 1600, 800, 600);
        CHECK_EQ_U(n, 9u);
        CHECK_EQ_U(on, 7u);
        CHECK_EQ_U(w[0].off, V3D_RENDERMODE);
        CHECK_EQ_U(w[0].val, 0x001e0000u);                  /* the proven 16 bpp value */
        CHECK_EQ_U(w[1].off, V3D_STENCILMODE);
        CHECK_EQ_U(w[1].val, 0u);                           /* SST_STENCIL_MODE_DISABLE */
        CHECK_EQ_U(w[2].off, V3D_STENCILOP);
        CHECK_EQ_U(w[2].val, 0u);
        k = 0;
        for (i = 0; i < n; i++)
            if (w[i].off != V3D_STENCILMODE && w[i].off != V3D_STENCILOP)
                strip[k++] = w[i];
        CHECK_EQ_U(k, on);
        CHECK(same(strip, o, on), "without the stencil pair: the old sequence exactly");
        CHECK(n != on, "the old sequence never wrote stencilMode/stencilOp");
    }
    CHECK(VCR_3D_TARGET_MAX >= 9, "the array holds a VSA-100 target");
}

TEST(thirty_two_bpp_is_programmed_only_with_d3d32) {
    vcr_regw w[VCR_3D_TARGET_MAX + 4];
    unsigned n;
    /* armed: 32 bpp renderMode (SST_RM_32BPP), the stencil pair, 4-byte strides */
    n = vcr_3d_target_seq(1, 1, VCR_RT_32, 0x200000, 3200, 0x400000, 3200, 800, 600, w);
    CHECK_EQ_U(n, 9u);
    CHECK_EQ_U(w[0].val, 0x001e0002u);
    CHECK_EQ_U(w[1].off, V3D_STENCILMODE);
    CHECK_EQ_U(w[2].off, V3D_STENCILOP);
    CHECK_EQ_U(w[4].val, 3200u);                            /* colBufferStride, bytes */
    CHECK_EQ_U(w[6].val, 3200u);                            /* auxBufferStride */
    /* not armed: refused before any write - 5be6a59 programmed it on every
     * VSA-100 (napalm alone decided) */
    CHECK_EQ_U(vcr_3d_target_seq(1, 0, VCR_RT_32, 0x200000, 3200, 0x400000, 3200, 800, 600, w),
               0u);
    CHECK_EQ_U(vcr_rt_format(0, 32, 32), VCR_RT_REFUSED);   /* rt32 off: refused */
    CHECK_EQ_U(vcr_rt_format(1, 32, 32), VCR_RT_32);
    CHECK_EQ_U(vcr_rt_format(0, 16, 16), VCR_RT_16);        /* 16 bpp never depends on it */
}

TEST(a_format_nobody_decided_on_is_never_programmed) {
    vcr_regw w[VCR_3D_TARGET_MAX + 4];
    /* 5be6a59: on a VSA-100, VCR_RT_REFUSED (0) reached renderMode as 16 bpp */
    CHECK(vcr_rt_rendermode(VCR_RT_REFUSED) == 0x001e0000u, "the old write for a refused target");
    CHECK_EQ_U(vcr_3d_target_seq(1, 1, VCR_RT_REFUSED, 0x200000, 1600, 0, 0, 800, 600, w), 0u);
    CHECK_EQ_U(vcr_3d_target_seq(1, 1, 24, 0x200000, 2400, 0, 0, 800, 600, w), 0u);
    CHECK_EQ_U(vcr_3d_target_seq(1, 1, 8, 0x200000, 800, 0, 0, 800, 600, w), 0u);
    CHECK_EQ_U(vcr_3d_target_seq(0, 0, VCR_RT_REFUSED, 0x200000, 1600, 0, 0, 800, 600, w), 0u);
    CHECK(!vcr_rt_programmable(0, VCR_RT_32), "32 without rt32");
    CHECK(vcr_rt_programmable(0, VCR_RT_16), "16 everywhere");
    CHECK(!vcr_rt_programmable(1, 15), "anything else nowhere");
}

TEST(the_z_buffer_must_fit_the_target) {
    /* 800x600: 1600 bytes a row at 16 bpp, 3200 at 32 */
    CHECK_EQ_U(vcr_rt_zcheck(VCR_RT_16, 800, 600, 0, 0, 0, 0, 0), VCR_RT_OK);   /* no Z */
    CHECK_EQ_U(vcr_rt_zcheck(VCR_RT_16, 800, 600, 1, 0x300000, 1600, 800, 600), VCR_RT_OK);
    CHECK_EQ_U(vcr_rt_zcheck(VCR_RT_32, 800, 600, 1, 0x400000, 3200, 800, 600), VCR_RT_OK);
    /* attached but not in video memory (offset 0): master drew with depth
     * on and the aux buffer at offset 0 */
    CHECK_EQ_U(vcr_rt_zcheck(VCR_RT_16, 800, 600, 1, 0, 1600, 800, 600), VCR_RT_WHY_ZOFF);
    /* a row short (a 16-bit pitch under a 32 bpp target), unaligned, too wide */
    CHECK_EQ_U(vcr_rt_zcheck(VCR_RT_32, 800, 600, 1, 0x400000, 1600, 800, 600), VCR_RT_WHY_ZPITCH);
    CHECK_EQ_U(vcr_rt_zcheck(VCR_RT_16, 100, 100, 1, 0x400000, 200, 100, 100), VCR_RT_WHY_ZPITCH);
    CHECK_EQ_U(vcr_rt_zcheck(VCR_RT_16, 800, 600, 1, 0x400000, 0x4000, 800, 600),
               VCR_RT_WHY_ZPITCH);
    CHECK_EQ_U(vcr_rt_zcheck(VCR_RT_16, 800, 600, 1, 0x400000, (unsigned)-1600, 800, 600),
               VCR_RT_WHY_ZPITCH);                          /* a negative lPitch */
    /* smaller than the target: the clip would reach past its end */
    CHECK_EQ_U(vcr_rt_zcheck(VCR_RT_16, 800, 600, 1, 0x300000, 1600, 640, 600), VCR_RT_WHY_ZSIZE);
    CHECK_EQ_U(vcr_rt_zcheck(VCR_RT_16, 800, 600, 1, 0x300000, 1600, 800, 480), VCR_RT_WHY_ZSIZE);
    /* larger is fine (a windowed target inside a bigger Z) */
    CHECK_EQ_U(vcr_rt_zcheck(VCR_RT_16, 256, 256, 1, 0x300000, 2048, 1024, 768), VCR_RT_OK);
}

TEST(the_z_list_follows_the_desktop_depth) {
    /* not armed, or a Voodoo3 (rt32 0): D16 alone at every depth - what the
     * proven 16 bpp HAL answered */
    CHECK_EQ_U(vcr_rt_zlist(0, 16), VCR_ZL_D16);
    CHECK_EQ_U(vcr_rt_zlist(0, 32), VCR_ZL_D16);
    CHECK_EQ_U(vcr_rt_zlist(0, 8), VCR_ZL_D16);
    /* armed: D16 at 16 bpp, the 32-bit pair at 32 - never both (5be6a59
     * listed D16 + D24X8 + D24S8 on every VSA-100, whatever the depth) */
    CHECK_EQ_U(vcr_rt_zlist(1, 16), VCR_ZL_D16);
    CHECK_EQ_U(vcr_rt_zlist(1, 32), VCR_ZL_D24X8 | VCR_ZL_D24S8);
    CHECK(!(vcr_rt_zlist(1, 32) & VCR_ZL_D16), "no D16 beside a 32 bpp desktop");
    CHECK(vcr_rt_zlist(1, 32) != (VCR_ZL_D16 | VCR_ZL_D24X8 | VCR_ZL_D24S8), "not 5be6a59's list");
    CHECK_EQ_U(vcr_rt_zlist(1, 24), VCR_ZL_D16);
}

TEST(a_depth_clear_never_touches_the_top_byte) {
    /* zaColor[31:24] is SST_ZACOLOR_ALPHA, not stencil: the largest depth a
     * clear writes stays below it at both sizes */
    CHECK_EQ_U(vcr_rt_zmax(VCR_RT_32) & 0xff000000u, 0u);
    CHECK_EQ_U(vcr_rt_zmax(VCR_RT_16) & 0xff000000u, 0u);
    CHECK_EQ_U(vcr_rt_zmax(VCR_RT_32), 0xffffffu);
}

TEST(the_glide_release_reset_is_exactly_this) {
    vcr_regw w[VCR_3D_RESET_MAX + 4];
    unsigned n = vcr_3d_glide_reset_seq(w), i, nops = 0;
    CHECK_EQ_U(n, 17u);
    CHECK_EQ_U(VCR_3D_RESET_MAX, 17u);
    /* chipMask first: a chip whose bit is clear ignores what follows */
    CHECK_EQ_U(w[0].off, V3D_CHIPMASK);
    CHECK_EQ_U(w[0].val, 1u);
    for (i = 1; i <= 12; i++) {
        CHECK_EQ_U(w[i].off, V3D_NOPCMD);
        CHECK_EQ_U(w[i].val, 0u);           /* not SST_NOP_RESET_*_STATS */
        nops++;
    }
    CHECK_EQ_U(nops, 12u);                  /* _grTex2ppc's flush, before combineMode */
    CHECK_EQ_U(w[13].off, V3D_COMBINEMODE);
    CHECK_EQ_U(w[13].val, 0u);
    CHECK_EQ_U(w[14].off, V3D_AACTRL);
    CHECK_EQ_U(w[14].val, 0u);
    CHECK_EQ_U(w[15].off, V3D_STENCILMODE);
    CHECK_EQ_U(w[15].val, 0u);
    CHECK_EQ_U(w[16].off, V3D_STENCILOP);
    CHECK_EQ_U(w[16].val, 0u);
    for (i = 0; i < n; i++) {
        CHECK(w[i].off != V3D_SLICTRL, "sliCtrl belongs to the miniport");
        CHECK(w[i].off != V3D_RENDERMODE, "renderMode is the next target's");
        CHECK((w[i].off & 0x3c00u) == 0, "chip field 0: FBI + every TMU (Glide's BROADCAST_ID)");
        CHECK(w[i].off < 0x400u, "inside the 3D register block");
    }
}

MUNIT_MAIN("vcr-kmd 3D register sequences (targets, Glide release)", {
    RUN(the_offsets_are_the_gpl_headers);
    RUN(a_voodoo3_target_is_the_six_writes_it_always_was);
    RUN(a_vsa100_16bpp_target_is_the_old_seven_plus_the_stencil_pair);
    RUN(thirty_two_bpp_is_programmed_only_with_d3d32);
    RUN(a_format_nobody_decided_on_is_never_programmed);
    RUN(the_z_buffer_must_fit_the_target);
    RUN(the_z_list_follows_the_desktop_depth);
    RUN(a_depth_clear_never_touches_the_top_byte);
    RUN(the_glide_release_reset_is_exactly_this);
})
