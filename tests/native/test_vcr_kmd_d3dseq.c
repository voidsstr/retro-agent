/* test_vcr_kmd_d3dseq.c - TRUE-SOURCE test of the 3D register writes the
 * vcr-kmd display driver makes (voodoo-cleanroom/vcr-kmd/include/vcr_3dseq.h,
 * written by display/vcrdd_3d.c VcrDd3dTarget and display/vcrdd_2d.c
 * VcrDdGlideReset3d) and of the target rules they rest on (vcr_rtfmt.h).
 *
 * The hardening of the 32 bpp render targets (commit 5be6a59) before any
 * deploy to the V5 6000 on .124 (2026-09-27, 4a9793b + its review), none of
 * it run on silicon yet:
 *   - a 32 bpp target writes stencilMode = stencilOp = 0 after renderMode:
 *     the stencil byte of a 32 bpp aux buffer is live, and nothing in vcr-kmd
 *     ever wrote those registers - a Glide/OpenGL session's stencil state
 *     would fail or overwrite every D3D pixel;
 *   - EVERY 16 bpp TARGET IS THE OLD SEQUENCE, write for write: a
 *     Banshee/Voodoo3 target is the 6 writes it always was, a VSA-100 16 bpp
 *     target the 7 it was proven with (40/40 on .124), whatever Diag\D3D32
 *     says (4a9793b added the stencil pair there too - new traffic on the
 *     proven lane with no switch);
 *   - 32 bpp is programmed only with Diag\D3D32 (rt32) - master offered it on
 *     every VSA-100 - and a format that is neither 16 nor 32 is refused on
 *     every chip (master wrote renderMode 16 bpp for a REFUSED target);
 *   - the Z buffer must fit the target (offset, pitch, size) - master took
 *     any attached Z, and a Z not in video memory put the aux buffer at 0 -
 *     and the target's own pitch obeys the same rule (DP2 refused an
 *     unaligned target, Clear2 fastfilled it, ContextCreate accepted it);
 *   - GUID_ZPixelFormats follows the RENDER depths: D16 always, the 32-bit
 *     pair beside it with D3D32 (4a9793b followed the desktop and dropped D16
 *     at a 32 bpp desktop - the proven 16 bpp fullscreen device's Z);
 *   - the Glide release reset: chipMask ALL first (4a9793b: 1), chip 0's
 *     sliCtrl = 0 right after it (the miniport's SLI disable writes it with
 *     no chipMask first), Glide's 12-NOP 2PPC flush, then combineMode,
 *     aaCtrl, stencil - never renderMode, never a non-zero sliCtrl;
 *   - and (third review, 2026-09-27) that reset writes NOTHING unless the
 *     chip reads idle first - VcrDd2dSync's rule, 3 reads in a row - and
 *     Glide's command FIFO is off (4a9793b queued its writes straight into
 *     whatever state the kernel's mode set left, STILL BUSY included);
 *   - video-memory offset 0 is a place, not "none": a Z the heap put at 0 is
 *     a Z (4a9793b refused it and failed ContextCreate for it).
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

/* what 4a9793b's vcr_3d_target_seq wrote: the stencil pair on EVERY
 * VSA-100 target, 16 bpp included - the defect the review found */
static unsigned rev_4a9793b_target(unsigned napalm, unsigned fmt, vcr_regw *w, unsigned rt_off,
                                   unsigned rt_pitch, unsigned z_off, unsigned z_pitch,
                                   unsigned width, unsigned height)
{
    unsigned n = 0;
    if (napalm) {
        w[n].off = 0x1e0; w[n++].val = fmt == 32 ? 0x001e0002u : 0x001e0000u;
        w[n].off = 0x1e4; w[n++].val = 0;
        w[n].off = 0x1e8; w[n++].val = 0;
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
    /* the reset's pre-check reads (vcr_regs.h; h3regs.h SstCRegs cmdFifo0,
     * h3defs.h SST_BUSY / SST_PCIFIFO_FREE / SST_CMDFIFOEN) */
    CHECK_EQ_U(VCR_CMD_BASESIZE0, 0x80024u);
    CHECK_EQ_U(VCR_CMDFIFO_EN, 0x100u);
    CHECK_EQ_U(VCR_STATUS_BUSY, 0x200u);
    CHECK_EQ_U(VCR_STATUS_FIFOLEVEL_MASK, 0x1fu);
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

TEST(a_vsa100_16bpp_target_is_the_proven_seven_whatever_d3d32_says) {
    vcr_regw w[VCR_3D_TARGET_MAX + 4], o[16], r[16];
    unsigned n, on, rn, i;
    int rt32;
    for (rt32 = 0; rt32 <= 1; rt32++) {         /* Diag\D3D32 does not touch 16 bpp */
        n = vcr_3d_target_seq(1, (unsigned)rt32, VCR_RT_16, 0x200000, 1600, 0x300000, 1600, 800,
                              600, w);
        on = old_target(1, o, 0x200000, 1600, 0x300000, 1600, 800, 600);
        CHECK_EQ_U(on, 7u);
        CHECK_EQ_U(n, 7u);                                  /* the proven 7 */
        CHECK(same(w, o, 7), "VSA-100 16 bpp: byte-identical to the proven VcrDd3dTarget");
        CHECK_EQ_U(w[0].off, V3D_RENDERMODE);
        CHECK_EQ_U(w[0].val, 0x001e0000u);                  /* the proven 16 bpp value */
        for (i = 0; i < n; i++)
            CHECK(w[i].off != V3D_STENCILMODE && w[i].off != V3D_STENCILOP,
                  "no stencil write on a 16 bpp target (it has no stencil byte)");
        /* 4a9793b wrote 9 here: the pair on the proven lane, with no switch */
        rn = rev_4a9793b_target(1, 16, r, 0x200000, 1600, 0x300000, 1600, 800, 600);
        CHECK_EQ_U(rn, 9u);
        CHECK(n != rn, "not 4a9793b's 9");
        /* no Z (d3dprobe --noz): also the old sequence */
        n = vcr_3d_target_seq(1, (unsigned)rt32, VCR_RT_16, 0x200000, 512, 0, 0, 256, 256, w);
        on = old_target(1, o, 0x200000, 512, 0, 0, 256, 256);
        CHECK_EQ_U(n, on);
        CHECK(same(w, o, n), "VSA-100 16 bpp without Z: identical");
    }
    CHECK(VCR_3D_TARGET_MAX >= 9, "the array holds a 32 bpp target");
}

TEST(thirty_two_bpp_is_programmed_only_with_d3d32) {
    vcr_regw w[VCR_3D_TARGET_MAX + 4];
    unsigned n;
    vcr_regw r[16];
    /* armed: 32 bpp renderMode (SST_RM_32BPP), the stencil pair, 4-byte strides */
    n = vcr_3d_target_seq(1, 1, VCR_RT_32, 0x200000, 3200, 0x400000, 3200, 800, 600, w);
    CHECK_EQ_U(n, 9u);
    CHECK_EQ_U(w[0].val, 0x001e0002u);
    CHECK_EQ_U(w[1].off, V3D_STENCILMODE);
    CHECK_EQ_U(w[1].val, 0u);                               /* SST_STENCIL_MODE_DISABLE */
    CHECK_EQ_U(w[2].off, V3D_STENCILOP);
    CHECK_EQ_U(w[2].val, 0u);
    CHECK(rev_4a9793b_target(1, 32, r, 0x200000, 3200, 0x400000, 3200, 800, 600) == n &&
          same(w, r, n), "32 bpp: the pair stays exactly where 4a9793b put it");
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
    /* 800x600: 1600 bytes a row at 16 bpp, 3200 at 32. The fifth argument
     * is "in video memory" (a flag), not the offset */
    CHECK_EQ_U(vcr_rt_zcheck(VCR_RT_16, 800, 600, 0, 0, 0, 0, 0), VCR_RT_OK);   /* no Z */
    CHECK_EQ_U(vcr_rt_zcheck(VCR_RT_16, 800, 600, 1, 1, 1600, 800, 600), VCR_RT_OK);
    CHECK_EQ_U(vcr_rt_zcheck(VCR_RT_32, 800, 600, 1, 1, 3200, 800, 600), VCR_RT_OK);
    /* attached but not in video memory: master drew with depth on and the
     * aux buffer at offset 0, over whatever surface is really there */
    CHECK_EQ_U(vcr_rt_zcheck(VCR_RT_16, 800, 600, 1, 0, 1600, 800, 600), VCR_RT_WHY_ZOFF);
    /* a row short (a 16-bit pitch under a 32 bpp target), unaligned, too wide */
    CHECK_EQ_U(vcr_rt_zcheck(VCR_RT_32, 800, 600, 1, 1, 1600, 800, 600), VCR_RT_WHY_ZPITCH);
    CHECK_EQ_U(vcr_rt_zcheck(VCR_RT_16, 100, 100, 1, 1, 200, 100, 100), VCR_RT_WHY_ZPITCH);
    CHECK_EQ_U(vcr_rt_zcheck(VCR_RT_16, 800, 600, 1, 1, 0x4000, 800, 600), VCR_RT_WHY_ZPITCH);
    CHECK_EQ_U(vcr_rt_zcheck(VCR_RT_16, 800, 600, 1, 1, (unsigned)-1600, 800, 600),
               VCR_RT_WHY_ZPITCH);                          /* a negative lPitch */
    /* smaller than the target: the clip would reach past its end */
    CHECK_EQ_U(vcr_rt_zcheck(VCR_RT_16, 800, 600, 1, 1, 1600, 640, 600), VCR_RT_WHY_ZSIZE);
    CHECK_EQ_U(vcr_rt_zcheck(VCR_RT_16, 800, 600, 1, 1, 1600, 800, 480), VCR_RT_WHY_ZSIZE);
    /* larger is fine (a windowed target inside a bigger Z) */
    CHECK_EQ_U(vcr_rt_zcheck(VCR_RT_16, 256, 256, 1, 1, 2048, 1024, 768), VCR_RT_OK);
}

/* 4a9793b's vcr_rt_zcheck was handed the Z's OFFSET and read 0 as "not in
 * video memory" - its first gate, the only one that differs */
static unsigned rev_4a9793b_zgate(unsigned z_set, unsigned z_off)
{
    return z_set && !z_off ? VCR_RT_WHY_ZOFF : VCR_RT_OK;
}

TEST(a_z_at_video_memory_offset_zero_is_a_z) {
    vcr_regw w[VCR_3D_TARGET_MAX + 4], o[16];
    unsigned n, on;
    /* .124 and 86Box put the desktop at the TOP of video memory, so the
     * DirectDraw heap is [0, desktop) and the first surface allocated after
     * a mode set can sit at offset 0 (vcrdd_ddraw.c heap_range; the
     * recorder's "heap 0-1aff000"). A Z there, in video memory and fitting
     * the target, is accepted - 4a9793b refused it (ContextCreate failed
     * with DDERR_INVALIDPIXELFORMAT for a Z the proven HAL drew with) */
    CHECK_EQ_U(vcr_rt_zcheck(VCR_RT_16, 800, 600, 1, 1, 1600, 800, 600), VCR_RT_OK);
    CHECK_EQ_U(rev_4a9793b_zgate(1, 0), VCR_RT_WHY_ZOFF);
    CHECK(vcr_rt_zcheck(VCR_RT_16, 800, 600, 1, 1, 1600, 800, 600) != rev_4a9793b_zgate(1, 0),
          "offset 0 in video memory: accepted now, refused by 4a9793b");
    /* ...and one that is NOT in video memory is still refused, whatever its
     * offset field holds */
    CHECK_EQ_U(vcr_rt_zcheck(VCR_RT_16, 800, 600, 1, 0, 1600, 800, 600), VCR_RT_WHY_ZOFF);
    /* the aux buffer at 0 is programmed as the proven HAL programmed it:
     * auxBufferAddr 0 with the Z's own stride */
    n = vcr_3d_target_seq(1, 0, VCR_RT_16, 0x200000, 1600, 0, 1600, 800, 600, w);
    on = old_target(1, o, 0x200000, 1600, 0, 1600, 800, 600);
    CHECK_EQ_U(n, on);
    CHECK(same(w, o, n), "a Z at 0: byte-identical to the proven sequence");
    CHECK_EQ_U(w[3].off, V3D_AUXBUFFERADDR);
    CHECK_EQ_U(w[3].val, 0u);
}

TEST(the_target_pitch_obeys_the_z_rule) {
    /* the proven sizes pass: fullscreen modes at 16 and 32 bpp, d3dprobe's
     * 256x256 window */
    CHECK_EQ_U(vcr_rt_rtcheck(VCR_RT_16, 640, 1280), VCR_RT_OK);
    CHECK_EQ_U(vcr_rt_rtcheck(VCR_RT_16, 800, 1600), VCR_RT_OK);
    CHECK_EQ_U(vcr_rt_rtcheck(VCR_RT_16, 1024, 2048), VCR_RT_OK);
    CHECK_EQ_U(vcr_rt_rtcheck(VCR_RT_16, 1152, 2304), VCR_RT_OK);
    CHECK_EQ_U(vcr_rt_rtcheck(VCR_RT_16, 1600, 3200), VCR_RT_OK);
    CHECK_EQ_U(vcr_rt_rtcheck(VCR_RT_16, 256, 512), VCR_RT_OK);
    CHECK_EQ_U(vcr_rt_rtcheck(VCR_RT_32, 1600, 6400), VCR_RT_OK);
    CHECK_EQ_U(vcr_rt_rtcheck(VCR_RT_16, 256, 1024), VCR_RT_OK);   /* wider pitch is fine */
    /* a 300 px window at 16 bpp: 600 bytes a row, as the heap hands it out
     * (no pitch alignment is requested). The Z of the same width was
     * refused at ContextCreate while the target itself was accepted: the
     * same answer now for both */
    CHECK_EQ_U(vcr_rt_rtcheck(VCR_RT_16, 300, 600), VCR_RT_WHY_RTPITCH);
    CHECK_EQ_U(vcr_rt_zcheck(VCR_RT_16, 300, 300, 1, 0x300000, 600, 300, 300), VCR_RT_WHY_ZPITCH);
    CHECK((600u & 0xfu) != 0, "DP2's own drawable rule refused it all along");
    /* short of a row, past the 14-bit stride field, negative */
    CHECK_EQ_U(vcr_rt_rtcheck(VCR_RT_32, 800, 1600), VCR_RT_WHY_RTPITCH);
    CHECK_EQ_U(vcr_rt_rtcheck(VCR_RT_16, 800, 0x4000), VCR_RT_WHY_RTPITCH);
    CHECK_EQ_U(vcr_rt_rtcheck(VCR_RT_16, 800, (unsigned)-1600), VCR_RT_WHY_RTPITCH);
    CHECK(VCR_RT_WHY_RTPITCH != VCR_RT_WHY_ZPITCH && VCR_RT_WHY_RTPITCH != VCR_RT_OK,
          "its own reason in the log");
}

TEST(the_z_list_follows_the_render_depths_not_the_desktop) {
    /* not armed, or a Voodoo3 (rt32 0): D16 alone - what the proven 16 bpp
     * HAL answered at every desktop */
    CHECK_EQ_U(vcr_rt_zlist(0), VCR_ZL_D16);
    /* armed: D16 AND the 32-bit pair, at every desktop. 4a9793b gave the
     * pair alone at a 32 bpp desktop - and the D3D8 runtime checks a
     * fullscreen device's Z against the list it read AT THE DESKTOP, so the
     * proven 16 bpp fullscreen device from .124's 32 bpp desktop lost D16
     * the moment Diag\D3D32 was armed */
    CHECK_EQ_U(vcr_rt_zlist(1), VCR_ZL_D16 | VCR_ZL_D24X8 | VCR_ZL_D24S8);
    CHECK(vcr_rt_zlist(1) & VCR_ZL_D16, "D16 beside a 32 bpp desktop");
    {
        unsigned rev_4a9793b_at_32 = VCR_ZL_D24X8 | VCR_ZL_D24S8;   /* rt32 && desktop 32 */
        CHECK(!(rev_4a9793b_at_32 & VCR_ZL_D16), "the old list had no D16 there");
        CHECK(vcr_rt_zlist(1) != rev_4a9793b_at_32, "not 4a9793b's list");
    }
    /* a pair of the wrong sizes stays refused before any write: the list
     * offering both sizes is safe because ContextCreate asks vcr_rt_format */
    CHECK_EQ_U(vcr_rt_format(1, 32, 16), VCR_RT_REFUSED);
    CHECK_EQ_U(vcr_rt_format(1, 16, 32), VCR_RT_REFUSED);
    CHECK_EQ_U(vcr_rt_format(1, 16, 16), VCR_RT_16);
    CHECK_EQ_U(vcr_rt_format(1, 32, 32), VCR_RT_32);
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
    unsigned n = vcr_3d_glide_reset_seq(w), i, nops = 0, sli = 0;
    CHECK_EQ_U(n, 18u);
    CHECK_EQ_U(VCR_3D_RESET_MAX, 18u);
    /* chipMask first, ALL chips (h3defs.h SST_CHIP_MASK_ALL_CHIPS - Glide's
     * init and clean-close value): it does not depend on chip 0's ID after
     * an AA session that never tore down. 4a9793b wrote 1 */
    CHECK_EQ_U(w[0].off, V3D_CHIPMASK);
    CHECK_EQ_U(w[0].val, 0xffffffffu);
    CHECK(w[0].val != 1u, "not 4a9793b's chipMask = 1");
    /* chip 0's sliCtrl = 0 right after the mask: the miniport's SLI disable
     * wrote it while a killed client may have had chip 0 masked out */
    CHECK_EQ_U(w[1].off, V3D_SLICTRL);
    CHECK_EQ_U(w[1].val, 0u);
    for (i = 2; i <= 13; i++) {
        CHECK_EQ_U(w[i].off, V3D_NOPCMD);
        CHECK_EQ_U(w[i].val, 0u);           /* not SST_NOP_RESET_*_STATS */
        nops++;
    }
    CHECK_EQ_U(nops, 12u);                  /* _grTex2ppc's flush, before combineMode */
    CHECK_EQ_U(w[14].off, V3D_COMBINEMODE);
    CHECK_EQ_U(w[14].val, 0u);
    CHECK_EQ_U(w[15].off, V3D_AACTRL);
    CHECK_EQ_U(w[15].val, 0u);
    CHECK_EQ_U(w[16].off, V3D_STENCILMODE);
    CHECK_EQ_U(w[16].val, 0u);
    CHECK_EQ_U(w[17].off, V3D_STENCILOP);
    CHECK_EQ_U(w[17].val, 0u);
    for (i = 0; i < n; i++) {
        if (w[i].off == V3D_SLICTRL) {
            sli++;
            CHECK_EQ_U(w[i].val, 0u);       /* SLI stays the miniport's: never enabled here */
            CHECK(i > 0 && w[i - 1].off == V3D_CHIPMASK, "sliCtrl only behind the chip mask");
        }
        CHECK(w[i].off != V3D_RENDERMODE, "renderMode is the next target's");
        CHECK((w[i].off & 0x3c00u) == 0, "chip field 0: broadcast (grFlush's nopCMD 0 too)");
        CHECK(w[i].off < 0x400u, "inside the 3D register block");
    }
    CHECK_EQ_U(sli, 1u);
}

/* the display driver's VcrDdGlideReset3d (vcrdd_2d.c), modelled on a
 * sequence of status reads: the bounded pre-check (idle_before_write), then
 * cmdFifo0.baseSize (read only once the chip reads idle), then the gate, then
 * the writes. Returns the writes made; *result the VCR_R3D_* outcome. */
static unsigned model_reset(const unsigned *st, unsigned nst, unsigned fifo_full,
                            unsigned basesize, unsigned *result)
{
    vcr_regw w[VCR_3D_RESET_MAX];
    unsigned i, run = 0, why;
    for (i = 0; i < nst && run < VCR_IDLE_READS; i++)
        run = vcr_idle_run(st[i], fifo_full, run);
    why = vcr_3d_reset_blocked(run, run >= VCR_IDLE_READS ? basesize : 0);
    if (why) {
        *result = why;
        return 0;
    }
    *result = VCR_R3D_DONE;
    return vcr_3d_glide_reset_seq(w);
}

/* 4a9793b's reset: no pre-check at all - chipMask = 1, 12 nopCMD,
 * combineMode, aaCtrl, stencilMode, stencilOp went into the FIFO whatever
 * the chip was doing */
static unsigned rev_4a9793b_reset_writes(const unsigned *st, unsigned nst)
{
    (void)st; (void)nst;
    return 1u + 12u + 4u;
}

#define IDLE    0x1fu                       /* not busy, FIFO back at 31 free */
#define BUSY    (0x200u | 0x1fu)            /* SST_BUSY with an empty FIFO */
#define QUEUED  0x10u                       /* not busy yet, 16 writes still queued */

TEST(the_idle_rule_is_vcr_dd2dsync_s) {
    /* VcrDd2dSync: idle = !(s & ST_BUSY) && (s & ST_FIFO_FREE) >= fifo_full,
     * three reads in a row (tests/python pins that expression to this one) */
    CHECK_EQ_U(vcr_idle_run(IDLE, 0x1f, 0), 1u);
    CHECK_EQ_U(vcr_idle_run(IDLE, 0x1f, 2), 3u);
    CHECK_EQ_U(vcr_idle_run(BUSY, 0x1f, 2), 0u);            /* busy restarts the run */
    CHECK_EQ_U(vcr_idle_run(QUEUED, 0x1f, 2), 0u);          /* not drained: not idle */
    CHECK_EQ_U(vcr_idle_run(QUEUED, 0x10, 2), 3u);          /* the full count is learned */
    CHECK_EQ_U(VCR_IDLE_READS, 3u);
}

TEST(the_reset_writes_nothing_into_a_chip_that_is_not_idle) {
    unsigned r, n;
    const unsigned busy[] = { BUSY, BUSY, BUSY, BUSY, BUSY, BUSY, BUSY, BUSY };
    const unsigned flicker[] = { BUSY, IDLE, IDLE, BUSY, IDLE, IDLE, BUSY, IDLE };
    const unsigned queued[] = { QUEUED, QUEUED, QUEUED, QUEUED, QUEUED, QUEUED };
    const unsigned settles[] = { BUSY, BUSY, QUEUED, IDLE, IDLE, IDLE };
    /* the chip Glide's hwcRestoreVideo left un-idled, and the kernel's mode
     * set reset "STILL BUSY": nothing written (4a9793b wrote all 17) */
    n = model_reset(busy, 8, 0x1f, 0, &r);
    CHECK_EQ_U(n, 0u);
    CHECK_EQ_U(r, VCR_R3D_BUSY);
    CHECK_EQ_U(rev_4a9793b_reset_writes(busy, 8), 17u);
    CHECK(n != rev_4a9793b_reset_writes(busy, 8), "not 4a9793b's blind writes");
    /* busy dropping for a read or two between operations is not idle */
    n = model_reset(flicker, 8, 0x1f, 0, &r);
    CHECK_EQ_U(n, 0u);
    CHECK_EQ_U(r, VCR_R3D_BUSY);
    /* writes still queued in the PCI FIFO: not idle either */
    n = model_reset(queued, 6, 0x1f, 0, &r);
    CHECK_EQ_U(n, 0u);
    CHECK_EQ_U(r, VCR_R3D_BUSY);
    /* a chip that settles within the bound is reset, all 18 writes */
    n = model_reset(settles, 6, 0x1f, 0, &r);
    CHECK_EQ_U(r, VCR_R3D_DONE);
    CHECK_EQ_U(n, VCR_3D_RESET_MAX);
}

TEST(the_reset_writes_nothing_while_glide_s_command_fifo_is_on) {
    unsigned r, n;
    const unsigned idle[] = { IDLE, IDLE, IDLE };
    /* idle, but cmdFifo0.baseSize still has SST_CMDFIFOEN (Glide skipped
     * its "disable the CMD fifo" store after a failed idle wait): refused */
    n = model_reset(idle, 3, 0x1f, 0x100u | 0x0fu, &r);
    CHECK_EQ_U(n, 0u);
    CHECK_EQ_U(r, VCR_R3D_CMDFIFO);
    CHECK_EQ_U(vcr_3d_reset_blocked(3, 0x100u), VCR_R3D_CMDFIFO);
    CHECK_EQ_U(vcr_3d_reset_blocked(3, 0x300u | 0x7u), VCR_R3D_CMDFIFO);    /* AGP FIFO */
    /* off (Glide's clean close stores 0; the size bits alone do not count) */
    CHECK_EQ_U(vcr_3d_reset_blocked(3, 0), 0u);
    CHECK_EQ_U(vcr_3d_reset_blocked(3, 0x0fu), 0u);
    n = model_reset(idle, 3, 0x1f, 0, &r);
    CHECK_EQ_U(r, VCR_R3D_DONE);
    CHECK_EQ_U(n, 18u);
    /* not idle is the first answer: the FIFO register is read only once the
     * chip reads idle */
    CHECK_EQ_U(vcr_3d_reset_blocked(2, 0x100u), VCR_R3D_BUSY);
    CHECK(VCR_R3D_OFF == 0u && VCR_R3D_DONE == 1u && VCR_R3D_BUSY != VCR_R3D_CMDFIFO &&
          VCR_R3D_GAVEUP != VCR_R3D_BUSY, "distinct outcomes in the log");
}

MUNIT_MAIN("vcr-kmd 3D register sequences (targets, Glide release)", {
    RUN(the_offsets_are_the_gpl_headers);
    RUN(a_voodoo3_target_is_the_six_writes_it_always_was);
    RUN(a_vsa100_16bpp_target_is_the_proven_seven_whatever_d3d32_says);
    RUN(thirty_two_bpp_is_programmed_only_with_d3d32);
    RUN(a_format_nobody_decided_on_is_never_programmed);
    RUN(the_z_buffer_must_fit_the_target);
    RUN(a_z_at_video_memory_offset_zero_is_a_z);
    RUN(the_target_pitch_obeys_the_z_rule);
    RUN(the_z_list_follows_the_render_depths_not_the_desktop);
    RUN(a_depth_clear_never_touches_the_top_byte);
    RUN(the_glide_release_reset_is_exactly_this);
    RUN(the_idle_rule_is_vcr_dd2dsync_s);
    RUN(the_reset_writes_nothing_into_a_chip_that_is_not_idle);
    RUN(the_reset_writes_nothing_while_glide_s_command_fifo_is_on);
})
