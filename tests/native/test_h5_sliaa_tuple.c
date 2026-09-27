/* test_h5_sliaa_tuple.c
 *
 * OUR h5 (Voodoo 4/5) Glide's SLI/AA guards - fork voidsstr/retro3dfx-glide,
 * glide3x/h5/minihwc/h5sliaa.h ([retro3dfx SLIAA-GUARD], 2026-09-27; clean-room
 * lane, NOT the vintage retro-3dfx Glide). This file includes THAT header - the
 * functions gpci.c, gsst.c, glfb.c and minihwc.c call - and vcr-kmd's own
 * miniport/vcrmp_sli.c, so both sides of the SLI_AA_REQUEST are the shipped
 * code.
 *
 * Why: SSTH3_SLI_AA_CONFIGURATION=1 froze the V5 6000 in .124 on AmigaMerlin's
 * kernel and on ours (boot #18: HWC_SLIAA a=4 b=0x102, then nothing). Glide laid
 * 2-sample AA out for all four chips, only then applied "single chip", and sent
 * {4 chips, no SLI, 2-sample AA, analog} - a combination with no video-mux
 * branch in dos_mode.c, the vendor W2K miniport or vcr-kmd, all of which still
 * program every other SLI/AA register of every chip.
 *
 *  1. every request Glide can send, numChips {1,2,4} x cfg 0-8 (x forceOldAA,
 *     x a resolution that asks for analog SLI), is one the kernel programs -
 *     checked against the kernel code itself, not a list;
 *  2. cfg 1 on 4 chips is refused before any request, and the old order sent
 *     exactly the tuple the kernel has no branch for;
 *  3. MIRROR: Glide's table (hwcSliAaTupleSupported) never accepts a tuple
 *     vcr_sli_set() does not program with a video-mux branch, and equals that
 *     set - exactly once the kernel carries its safety net (vcr_sli_combo_ok,
 *     412b03c), and before it minus exactly the six shapes video_mux() would
 *     program as another sample count; mutated tables are shown to fail;
 *  4. the other guards: single-chip AA PCI_OP loop bound, the READ lock in a
 *     multi-chip AA mode, the idle wait's master reset, chips driven;
 *  5. the refactored upstream decisions (control panel mapping, forced sample
 *     count, the per-board layout) keep upstream's values row for row.
 * Fixed AND old values are asserted throughout.
 *
 * The header lives in the fork clone under voodoo-cleanroom/build/ (gitignored;
 * the main tree's when a worktree has none). Without it this test SKIPS
 * loudly - it does not pass.
 */
#include <string.h>
#include "munit.h"

/* -DH5SLIAA_TEST_NO_HEADER exercises the skip path */
#if defined(__has_include) && !defined(H5SLIAA_TEST_NO_HEADER)
#  if __has_include("../../voodoo-cleanroom/build/retro3dfx-glide/glide3x/h5/minihwc/h5sliaa.h")
#    include "../../voodoo-cleanroom/build/retro3dfx-glide/glide3x/h5/minihwc/h5sliaa.h"
#    define HAVE_H5SLIAA 1
#  elif __has_include("/home/voidsstr/development/retro-agent/voodoo-cleanroom/build/retro3dfx-glide/glide3x/h5/minihwc/h5sliaa.h")
#    include "/home/voidsstr/development/retro-agent/voodoo-cleanroom/build/retro3dfx-glide/glide3x/h5/minihwc/h5sliaa.h"
#    define HAVE_H5SLIAA 1
#  endif
#endif

#include "../../voodoo-cleanroom/vcr-kmd/miniport/vcrmp_sli.c"

#ifndef HAVE_H5SLIAA
int main(void)
{
    (void)munit_fails; (void)munit_total_fails; (void)munit_total_tests;
    printf("== h5 Glide SLI/AA guards ==\n");
    printf("  [SKIP] retro3dfx-glide clone absent (glide3x/h5/minihwc/h5sliaa.h) - the SLI/AA\n"
           "         request guards were NOT checked. Run voodoo-cleanroom/build-stack.sh.\n");
    return 0;
}
#else

/* ---- a four-chip VSA-100 board for vcr_sli_set() ------------------------------ */

#define NCH 4
typedef struct {
    vcr_u32 cfg[NCH][64], io[NCH][1024];
    vcr_u8 vga[NCH][0x40];
    unsigned writes, set_begin, set_done, nomux, refused;
} kmock;
static kmock K;

static vcr_u32 k_cfg_rd(void *c, vcr_u32 chip, vcr_u32 off)
{
    (void)c;
    if (chip >= NCH)
        return 0xffffffffu;
    return K.cfg[chip][(off & 0xfc) >> 2];
}
static void k_cfg_wr(void *c, vcr_u32 chip, vcr_u32 off, vcr_u32 v)
{
    (void)c;
    K.writes++;
    if (chip < NCH && (off & 0xfc) != 0)        /* the ID stays read-only */
        K.cfg[chip][(off & 0xfc) >> 2] = v;
}
static vcr_u32 k_io_rd(void *c, vcr_u32 chip, vcr_u32 off)
{
    (void)c;
    if (chip >= NCH)
        return 0xffffffffu;
    if (off == VCR_R_STATUS)
        return 0x3f;                            /* FIFO room, never busy */
    return (off >> 2) < 1024 ? K.io[chip][off >> 2] : 0;
}
static void k_io_wr(void *c, vcr_u32 chip, vcr_u32 off, vcr_u32 v)
{
    (void)c;
    K.writes++;
    if (chip < NCH && (off >> 2) < 1024)
        K.io[chip][off >> 2] = v;
}
static vcr_u8 k_vga_rd(void *c, vcr_u32 chip, vcr_u32 port)
{
    (void)c;
    return chip < NCH ? K.vga[chip][port & 0x3f] : 0xff;
}
static void k_vga_wr(void *c, vcr_u32 chip, vcr_u32 port, vcr_u8 v)
{
    (void)c;
    K.writes++;
    if (chip < NCH)
        K.vga[chip][port & 0x3f] = v;
}
static void k_stall(void *c, vcr_u32 us) { (void)c; (void)us; }
static void k_log(void *c, vcr_u32 step, vcr_u32 chip, vcr_u32 reg, vcr_u32 val, const char *w)
{
    (void)c; (void)chip; (void)reg; (void)val; (void)w;
    if (step == VCR_SLI_S_SET_BEGIN) K.set_begin++;
    if (step == VCR_SLI_S_SET_DONE) K.set_done++;
    if (step == VCR_SLI_S_NOMUX) K.nomux++;
    if (step == VCR_SLI_S_REFUSED) K.refused++;
}
static const vcr_sli_io KIO = { NULL, k_cfg_rd, k_cfg_wr, k_io_rd, k_io_wr,
                                k_vga_rd, k_vga_wr, k_stall, k_log };

static void kmock_reset(void)
{
    int c;
    memset(&K, 0, sizeof K);
    for (c = 0; c < NCH; c++) {
        K.cfg[c][0] = 0x0009121au;              /* 3dfx VSA-100 (Voodoo 5) */
        K.cfg[c][0x10 >> 2] = 0xd0000000u + ((vcr_u32)c << 25);
        K.cfg[c][0x14 >> 2] = 0xc0000000u;
        K.cfg[c][0x40 >> 2] = c ? 0x301u : 0x0u;
    }
}

/* What vcr-kmd does with a tuple: 1 = programmed with a video-mux branch;
 * 0 = no branch (VCR_SLI_W_NOMUX) or refused before writing. *disable is set
 * for a request with neither SLI nor AA (the kernel's disable path). */
static int kernel_programs(unsigned long n, unsigned long sli, unsigned long aa,
                           unsigned long high, unsigned long analog, int *disable)
{
    vcr_sli_aa_req r;
    int rc;
    memset(&r, 0, sizeof r);
    r.ChipInfo.dwChips = n;
    r.ChipInfo.dwsliEn = sli;
    r.ChipInfo.dwaaEn = aa;
    r.ChipInfo.dwaaSampleHigh = high;
    r.ChipInfo.dwsliAaAnalog = analog;
    r.ChipInfo.dwsli_nlines = 32;               /* Glide's 1024x768 band */
    r.ChipInfo.dwCfgSwapAlgorithm = 1;
    r.MemInfo.dwBpp = 16;
    kmock_reset();
    rc = vcr_sli_set(&KIO, &r);
    *disable = !sli && !aa;
    if (*disable)
        return 0;
    return rc >= 0 && !(rc & VCR_SLI_W_NOMUX) && K.set_done && !K.nomux && !K.refused;
}

/* ---- Glide's open, in grSstWinOpen/hwcInitVideo order ------------------------ */

typedef struct {
    int refused;                /* grSstWinOpen / hwcInitVideo failed the open */
    int request;                /* HWCEXT_SLI_AA_REQUEST sent */
    h5SliAaTuple t;
    int pciOpLoop;              /* single-chip AA's own PCI_OP loop ran */
    unsigned long opChips, chipCount, pixelSample;
} glide_open_t;

/* An app opening RGB565 at 1024x768 (no low-res forcing). aaSampleEnv >= 0 is
 * FX_GLIDE_AA_SAMPLE. resAnalog is gsst.c's resolution-driven analog SLI.
 * guards 0 replays the pre-2026-09-27 order (no refusal, every chip). */
static glide_open_t glide_open(unsigned long numChips, long cfg, long aaSampleEnv,
                               long forceOldAA, unsigned long resAnalog, int guards)
{
    glide_open_t o;
    unsigned long aaSample, single, chipCount = numChips, sliCount = 1, analog;
    h5SliAaLayoutT lay;
    int enable2nd;

    memset(&o, 0, sizeof o);
    h5SliAaConfigEnv(cfg, &aaSample, &single);                  /* gpci.c */
    if (aaSampleEnv >= 0)
        aaSample = (unsigned long)aaSampleEnv;
    o.pixelSample = h5SliAaForcedSample(aaSample, chipCount);   /* gsst.c */
    lay.samplesPerChip = 1;
    if (h5SliAaLayout(chipCount, o.pixelSample, forceOldAA, &lay))
        sliCount = lay.sliCount;
    enable2nd = lay.samplesPerChip > 1;
    if (single) {                                               /* gsst.c, late */
        if (guards && !h5SliAaSingleChipOk(numChips, single, o.pixelSample)) {
            o.refused = 1;
            return o;
        }
        sliCount = 1;
        chipCount = 1;
    }
    analog = resAnalog || chipCount == 4;                       /* gsst.c */
    analog = h5SliAaAnalog(numChips, analog);                   /* hwcInitVideo */
    o.chipCount = chipCount;
    if (h5SliAaRequestNeeded(numChips, sliCount, o.pixelSample, enable2nd)) {
        o.t = h5SliAaRequestTuple(numChips, sliCount, o.pixelSample, analog);
        if (guards && (!h5SliAaChipsDrivenOk(numChips, chipCount) ||
                       !hwcSliAaTupleSupported(o.t.chips, o.t.sli, o.t.aa, o.t.high,
                                               o.t.analog))) {
            o.refused = 1;                                      /* hwcSliAaOpenOk */
            return o;
        }
        o.request = 1;
    } else if (o.pixelSample == 2) {
        o.pciOpLoop = 1;
        o.opChips = guards ? h5SliAaPciOpChips(numChips, chipCount) : numChips;
    }
    return o;
}

static int is_tuple(h5SliAaTuple t, unsigned long n, unsigned long s, unsigned long a,
                    unsigned long h, unsigned long an)
{
    return t.chips == n && t.sli == s && t.aa == a && t.high == h && t.analog == an;
}

/* ---- 1. every request is one the kernel programs ------------------------------ */

TEST(every_request_glide_sends_is_programmed_by_the_kernel) {
    static const unsigned long chips[] = { 1, 2, 4 };
    unsigned i, requests = 0, refused = 0, narrowed = 0;
    long cfg, old, env;
    unsigned long res;
    int dis;

    for (i = 0; i < 3; i++)
        for (cfg = 0; cfg <= 8; cfg++)
            for (old = 0; old <= 1; old++)
                for (res = 0; res <= 1; res++)
                    for (env = -1; env <= 8; env++) {
                        glide_open_t o, was;
                        if (env >= 0 && env != 2 && env != 4 && env != 8)
                            continue;
                        o = glide_open(chips[i], cfg, env, old, res, 1);
                        was = glide_open(chips[i], cfg, env, old, res, 0);
                        if (o.refused) {
                            /* only what the old order sent unprogrammable, sent
                             * naming chips Glide did not drive, or ran as
                             * single-chip AA over the slaves */
                            int bad_req = was.request &&
                                (!kernel_programs(was.t.chips, was.t.sli, was.t.aa, was.t.high,
                                                  was.t.analog, &dis) ||
                                 was.chipCount != was.t.chips);
                            int bad_loop = was.pciOpLoop && was.opChips > was.chipCount;
                            CHECK(bad_req || bad_loop, "a refusal the old request did not earn");
                            refused++;
                            continue;
                        }
                        /* not refused: exactly what the old order did, except
                         * that single-chip AA's own loop stops at the chips
                         * Glide drives: 2 chips, FX_GLIDE_FORCE_OLD_AA, and
                         * single chip with 2 samples (cfg 1, or cfg 0 with
                         * FX_GLIDE_AA_SAMPLE=2) */
                        CHECK_EQ_I(o.request, was.request);
                        CHECK(!o.request || memcmp(&o.t, &was.t, sizeof o.t) == 0,
                              "a guarded open sends a different request");
                        CHECK_EQ_I(o.pciOpLoop, was.pciOpLoop);
                        if (o.pciOpLoop && o.opChips != was.opChips) {
                            CHECK(was.opChips > was.chipCount && o.opChips == was.chipCount,
                                  "the loop changed other than stopping at the driven chips");
                            if (!(chips[i] == 2 && old && o.chipCount == 1 && o.pixelSample == 2))
                                fprintf(stderr, "    narrowed: chips %lu cfg %ld env %ld old %ld\n",
                                        chips[i], cfg, env, old);
                            CHECK(chips[i] == 2 && old && o.chipCount == 1 && o.pixelSample == 2,
                                  "the loop narrowed outside 2-chip single-chip 2-sample FORCE_OLD_AA");
                            narrowed++;
                        }
                        if (!o.request)
                            continue;
                        requests++;
                        CHECK(hwcSliAaTupleSupported(o.t.chips, o.t.sli, o.t.aa, o.t.high,
                                                     o.t.analog), "Glide sends a tuple its table refuses");
                        CHECK(kernel_programs(o.t.chips, o.t.sli, o.t.aa, o.t.high,
                                              o.t.analog, &dis), "the kernel has no branch for a sent tuple");
                        CHECK(!is_tuple(o.t, 4, 0, 1, 0, 1), "cfg 1's {4,0,1,0,1} was sent");
                        CHECK(o.t.chips != 4 || o.chipCount == 4,
                              "a 4-chip request while Glide drives fewer chips");
                    }
    CHECK(requests > 100, "the sweep sent requests");
    CHECK(refused > 0, "the sweep met the refusals");
    CHECK(narrowed > 0, "the sweep met the narrowed loop");
}

TEST(the_request_per_config_at_1024x768_16bpp) {
    /* 4 chips (analog forced), control panel only */
    glide_open_t o;
    o = glide_open(4, 0, -1, 0, 0, 1);  CHECK(!o.request && !o.refused && !o.pciOpLoop, "cfg 0: one chip, nothing sent");
    o = glide_open(4, 1, -1, 0, 0, 1);  CHECK(o.refused && !o.request, "cfg 1 on 4 chips: refused");
    o = glide_open(4, 2, -1, 0, 0, 1);  CHECK(o.request && is_tuple(o.t, 4, 1, 0, 0, 1), "cfg 2: 4-way analog SLI");
    o = glide_open(4, 3, -1, 0, 0, 1);  CHECK(o.request && is_tuple(o.t, 4, 1, 1, 0, 1), "cfg 3: 2-sample, 2 analog SLI units");
    o = glide_open(4, 4, -1, 0, 0, 1);  CHECK(o.request && is_tuple(o.t, 4, 0, 1, 1, 1), "cfg 4: 4-sample analog");
    o = glide_open(4, 5, -1, 0, 0, 1);  CHECK(o.request && is_tuple(o.t, 4, 1, 0, 0, 1), "cfg 5: 4-way analog SLI");
    o = glide_open(4, 6, -1, 0, 0, 1);  CHECK(o.request && is_tuple(o.t, 4, 1, 1, 0, 1), "cfg 6 = cfg 3");
    o = glide_open(4, 7, -1, 0, 0, 1);  CHECK(o.request && is_tuple(o.t, 4, 0, 1, 1, 1), "cfg 7 = cfg 4");
    o = glide_open(4, 8, -1, 0, 0, 1);  CHECK(o.request && is_tuple(o.t, 4, 0, 1, 2, 1), "cfg 8: 8-sample analog");
    /* 2 chips (digital at 1024x768) - cfg 1 keeps its 2-chip request */
    o = glide_open(2, 0, -1, 0, 0, 1);  CHECK(!o.request && !o.refused, "2 chips cfg 0: nothing sent");
    o = glide_open(2, 1, -1, 0, 0, 1);  CHECK(o.request && is_tuple(o.t, 2, 0, 1, 0, 0), "2 chips cfg 1: unchanged");
    o = glide_open(2, 2, -1, 0, 0, 1);  CHECK(o.request && is_tuple(o.t, 2, 1, 0, 0, 0), "2 chips cfg 2: digital SLI");
    o = glide_open(2, 3, -1, 0, 0, 1);  CHECK(o.request && is_tuple(o.t, 2, 0, 1, 0, 0), "2 chips cfg 3: 2-sample");
    o = glide_open(2, 4, -1, 0, 0, 1);  CHECK(o.request && is_tuple(o.t, 2, 0, 1, 1, 0), "2 chips cfg 4: 4-sample");
    o = glide_open(2, 8, -1, 0, 0, 1);  CHECK(o.request && is_tuple(o.t, 2, 1, 0, 0, 0), "2 chips cfg 8: no 8xaa, SLI");
    /* 1 chip never sends a request; 2-sample AA runs Glide's own loop */
    o = glide_open(1, 1, -1, 0, 0, 1);  CHECK(!o.request && o.pciOpLoop && o.opChips == 1, "1 chip cfg 1: own loop");
    o = glide_open(1, 7, -1, 0, 0, 1);  CHECK(!o.request && !o.pciOpLoop, "1 chip cfg 7: no 4-sample");
}

/* ---- 2. cfg 1 on four chips ---------------------------------------------------- */

TEST(cfg1_on_four_chips_is_refused_the_old_order_sent_the_wedge_tuple) {
    glide_open_t fixed = glide_open(4, 1, -1, 0, 0, 1);
    glide_open_t old = glide_open(4, 1, -1, 0, 0, 0);
    int dis;

    CHECK(fixed.refused && !fixed.request, "fixed: refused before any request");
    CHECK_EQ_U(h5SliAaSingleChipOk(4, 1, 2), 0);
    /* old: the layout for 4 chips, then one chip, and the request names all four */
    CHECK(old.request, "old: the request was sent");
    CHECK(is_tuple(old.t, 4, 0, 1, 0, 1), "old: {4 chips, no SLI, 2-sample AA, analog}");
    CHECK_EQ_U(old.chipCount, 1);
    CHECK_EQ_U(hwcSliAaTupleSupported(4, 0, 1, 0, 1), 0);
    CHECK_EQ_U(kernel_programs(4, 0, 1, 0, 1, &dis), 0);
    /* before the kernel safety net (vcr-kmd 412b03c) that tuple is NOT refused:
     * it is programmed without a video mux, every other SLI/AA register
     * written - the wedge. With the net it is refused before SET_BEGIN, with
     * nothing written. */
#ifdef VCR_SLI_R_COMBO
    CHECK(K.refused && !K.set_begin && K.writes == 0, "the kernel refuses it before any write");
#else
    CHECK(K.set_begin && K.nomux && K.writes > 0, "the kernel programs it and logs NOMUX");
#endif
    /* FX_GLIDE_AA_SAMPLE=4 with single chip: the same late order, a supported
     * tuple naming four chips while Glide drives one */
    old = glide_open(4, 0, 4, 0, 0, 0);
    CHECK(old.request && is_tuple(old.t, 4, 0, 1, 1, 1) && old.chipCount == 1,
          "old: 4-sample request, Glide on one chip");
    CHECK(glide_open(4, 0, 4, 0, 0, 1).refused, "fixed: refused");
    /* 2-chip and 1-chip boards keep cfg 1 */
    CHECK_EQ_U(h5SliAaSingleChipOk(2, 1, 2), 1);
    CHECK_EQ_U(h5SliAaSingleChipOk(1, 1, 2), 1);
    CHECK_EQ_U(h5SliAaSingleChipOk(4, 1, 1), 1);        /* cfg 0 on 4 chips */
}

/* ---- 3. mirror: Glide's table = the kernel's set -------------------------------
 * "The kernel" is vcrmp_sli.c's vcr_sli_set() itself, run against the mock.
 * Two versions of it are in play:
 *  - before the kernel safety net (vcr-kmd 412b03c): video_mux() decides, and
 *    it also programs six shapes as a DIFFERENT sample count - 1 chip with a
 *    4/8-sample field (its branch ignores the field: 2-sample) and 8 samples on
 *    2 chips without SLI (the 4-sample branches test the field as a boolean);
 *  - with the net (VCR_SLI_R_COMBO defined): vcr_sli_combo_ok() refuses every
 *    shape outside its set before the first write, those six included.
 * Glide's table must never accept a shape the kernel does not program (the
 * safety half), and must otherwise equal it: exactly, with the net; before it,
 * minus exactly those six. Glide sends none of the six from any setting. */

static int reinterpreted(unsigned long n, unsigned long s, unsigned long a, unsigned long h,
                         unsigned long an)
{
    (void)an;
    if (!a)
        return 0;
    return !s && ((n == 1 && (h == 1 || h == 2)) || (n == 2 && h == 2));
}

typedef struct {
    unsigned kernel, glide;     /* shapes each side accepts (disables excluded) */
    unsigned extra;             /* Glide accepts, the kernel does not program - never allowed */
    unsigned missing;           /* the kernel programs, Glide refuses */
    unsigned missing_reinterp;  /* ...of which are the six documented shapes */
    unsigned disable;           /* Glide accepts a disable as a mode - never allowed */
} mirror_t;

static mirror_t mirror(int (*pred)(unsigned long, unsigned long, unsigned long, unsigned long,
                                   unsigned long), int quiet)
{
    unsigned long n, s, a, h, an;
    mirror_t m;
    int dis;
    memset(&m, 0, sizeof m);
    for (n = 0; n <= 5; n++)
        for (s = 0; s <= 1; s++)
            for (a = 0; a <= 1; a++)
                for (h = 0; h <= 3; h++)
                    for (an = 0; an <= 1; an++) {
                        int k = kernel_programs(n, s, a, h, an, &dis);
                        int g = pred(n, s, a, h, an);
                        if (dis) {
                            m.disable += (unsigned)(g != 0);
                            continue;
                        }
                        m.kernel += (unsigned)k;
                        m.glide += (unsigned)(g != 0);
                        if (g && !k)
                            m.extra++;
                        if (k && !g) {
                            m.missing++;
                            m.missing_reinterp += (unsigned)reinterpreted(n, s, a, h, an);
                        }
                        /* print what is not expected: the six are, before the net */
                        if (!quiet && (g != 0) != k) {
                            int expected = k && !g && reinterpreted(n, s, a, h, an);
#ifdef VCR_SLI_R_COMBO
                            expected = 0;
#endif
                            if (!expected)
                                fprintf(stderr, "    mirror: {%lu,%lu,%lu,%lu,%lu} kernel %d glide %d\n",
                                        n, s, a, h, an, k, g != 0);
                        }
                    }
    return m;
}

static int table_ok(unsigned long n, unsigned long s, unsigned long a, unsigned long h,
                    unsigned long an)
{
    return hwcSliAaTupleSupported(n, s, a, h, an);
}
/* the table before 2026-09-27's narrowing: video_mux()'s set, six shapes wider */
static int table_old(unsigned long n, unsigned long s, unsigned long a, unsigned long h,
                     unsigned long an)
{
    return hwcSliAaTupleSupported(n, s, a, h, an) || reinterpreted(n, s, a, h, an);
}
/* a table that forgot cfg 1's missing branch - the mirror must catch it */
static int table_mutant(unsigned long n, unsigned long s, unsigned long a, unsigned long h,
                        unsigned long an)
{
    return hwcSliAaTupleSupported(n, s, a, h, an) || (n == 4 && !s && a && h == 0 && an);
}
/* a table that took 8 samples on 2 chips back - the mirror must catch it too */
static int table_mutant8(unsigned long n, unsigned long s, unsigned long a, unsigned long h,
                         unsigned long an)
{
    return hwcSliAaTupleSupported(n, s, a, h, an) || (n == 2 && !s && a && h == 2);
}

TEST(mirror_glide_tuple_table_equals_the_kernel_set) {
    mirror_t m = mirror(table_ok, 0), old, mut;

    /* fixed: never a shape the kernel does not program, never a disable, and
     * 30 shapes: 12 AA branches of 2+ chips, 4 SLI-only branches for each of
     * the 4 values of the sample field the kernel ignores without AA, the
     * 1-chip 2-sample branch for either analog bit */
    CHECK_EQ_U(m.extra, 0);
    CHECK_EQ_U(m.disable, 0);
    CHECK_EQ_U(m.glide, 12 + 4 * 4 + 2);
#ifdef VCR_SLI_R_COMBO
    /* the kernel with its safety net: exactly its set */
    CHECK_EQ_U(m.missing, 0);
    CHECK_EQ_U(m.kernel, 30);
    {
        unsigned long n, s, a, h, an;
        unsigned diff = 0;
        for (n = 0; n <= 5; n++)
            for (s = 0; s <= 1; s++)
                for (a = 0; a <= 1; a++)
                    for (h = 0; h <= 3; h++)
                        for (an = 0; an <= 1; an++)
                            diff += (unsigned)(!vcr_sli_combo_ok((vcr_u32)n, (vcr_u32)s, (vcr_u32)a,
                                                                 (vcr_u32)h, (vcr_u32)an) !=
                                               !hwcSliAaTupleSupported(n, s, a, h, an));
        CHECK_EQ_U(diff, 0);    /* the kernel's own predicate, shape for shape */
    }
#else
    /* the kernel before its safety net: video_mux()'s 36 shapes, of which
     * Glide refuses exactly the six it would program as another sample count.
     * The first draft of the table had 34 - this comparison caught the 8. */
    CHECK_EQ_U(m.kernel, 36);
    CHECK_EQ_U(m.missing, 6);
    CHECK_EQ_U(m.missing_reinterp, 6);
#endif
    /* old: the table accepted video_mux()'s whole set, 36 shapes */
    old = mirror(table_old, 1);
    CHECK_EQ_U(old.glide, 36);
#ifdef VCR_SLI_R_COMBO
    CHECK_EQ_U(old.extra, 6);   /* six shapes the kernel now refuses */
#else
    CHECK_EQ_U(old.extra, 0);
    CHECK_EQ_U(old.missing, 0);
#endif
    /* the check can fail */
    mut = mirror(table_mutant, 1);
    CHECK(mut.extra == 1, "a table accepting cfg 1's {4,0,1,0,1} is caught");
    mut = mirror(table_mutant8, 1);
#ifdef VCR_SLI_R_COMBO
    CHECK(mut.extra == 2, "a table accepting 8 samples on 2 chips is caught");
#else
    CHECK(mut.missing_reinterp == 4, "a table accepting 8 samples on 2 chips is caught");
#endif
}

TEST(the_table_normalises_like_the_kernel) {
    CHECK_EQ_U(hwcSliAaTupleSupported(4, 1, 0, 2, 1), 1);    /* sample field ignored without AA */
    CHECK_EQ_U(hwcSliAaTupleSupported(4, 7, 0, 0, 9), 1);    /* flags are booleans */
    CHECK_EQ_U(hwcSliAaTupleSupported(4, 0, 0, 0, 1), 0);    /* a disable, not a mode */
    CHECK_EQ_U(hwcSliAaTupleSupported(1, 0, 1, 3, 0), 0);    /* sample field > 2 */
    CHECK_EQ_U(hwcSliAaTupleSupported(3, 1, 0, 0, 1), 0);    /* no 3-chip board */
    CHECK_EQ_U(hwcSliAaTupleSupported(1, 1, 1, 0, 0), 0);    /* SLI on one chip */
    CHECK_EQ_U(hwcSliAaTupleSupported(4, 0, 1, 1, 0), 0);    /* 4-sample digital, no SLI */
    CHECK_EQ_U(hwcSliAaTupleSupported(2, 0, 1, 2, 0), 0);    /* 8 on 2 chips (old 1: video_mux took it as 4) */
    CHECK_EQ_U(hwcSliAaTupleSupported(2, 0, 1, 1, 0), 1);    /* 4 on 2 chips */
    CHECK_EQ_U(hwcSliAaTupleSupported(1, 0, 1, 1, 1), 0);    /* 4 on 1 chip (old 1: taken as 2) */
    CHECK_EQ_U(hwcSliAaTupleSupported(1, 0, 1, 0, 1), 1);    /* 2 on 1 chip, either analog bit */
    CHECK_EQ_U(hwcSliAaTupleSupported(2, 1, 1, 1, 0), 0);    /* 4-sample + SLI on 2 chips */
    CHECK_EQ_U(hwcSliAaTupleSupported(4, 1, 1, 2, 1), 0);    /* 8-sample + SLI on 4 chips */
}

/* ---- 4. the other guards -------------------------------------------------------- */

TEST(single_chip_aa_pci_op_loop_writes_only_the_chips_glide_drives) {
    CHECK_EQ_U(h5SliAaPciOpChips(4, 1), 1);      /* old: 4 - the master's mux into the slaves */
    CHECK_EQ_U(h5SliAaPciOpChips(2, 1), 1);
    CHECK_EQ_U(h5SliAaPciOpChips(1, 1), 1);      /* the V4 4500 case, unchanged */
    CHECK_EQ_U(h5SliAaPciOpChips(4, 4), 4);
    CHECK_EQ_U(h5SliAaPciOpChips(4, 0), 4);      /* unknown -> upstream's bound */
    CHECK_EQ_U(h5SliAaPciOpChips(2, 3), 2);      /* never more than the board */
}

TEST(a_read_lock_in_multi_chip_aa_is_refused_unless_opted_in) {
    CHECK_EQ_U(h5SliAaLfbReadOk(2, 4, 0), 0);    /* cfg 3 bands: the read that froze .124 */
    CHECK_EQ_U(h5SliAaLfbReadOk(4, 4, 0), 0);
    CHECK_EQ_U(h5SliAaLfbReadOk(2, 2, 0), 0);
    CHECK_EQ_U(h5SliAaLfbReadOk(2, 4, 1), 1);    /* RETRO_GLIDE_AA_LFB_READ=1 */
    CHECK_EQ_U(h5SliAaLfbReadOk(1, 4, 0), 1);    /* cfg 5 SLI reads: unchanged (0 bad lines) */
    CHECK_EQ_U(h5SliAaLfbReadOk(2, 1, 0), 1);    /* single-chip AA: unchanged */
    CHECK_EQ_U(h5SliAaLfbReadOk(1, 1, 0), 1);
}

TEST(the_idle_wait_never_resets_the_master_of_a_multi_chip_sliaa_board) {
    CHECK_EQ_U(h5SliAaIdleResetOk(4, 4, 1), 0);  /* cfg 5 */
    CHECK_EQ_U(h5SliAaIdleResetOk(4, 2, 2), 0);  /* cfg 3 */
    CHECK_EQ_U(h5SliAaIdleResetOk(4, 1, 4), 0);  /* cfg 7 */
    CHECK_EQ_U(h5SliAaIdleResetOk(2, 1, 2), 0);  /* 2-chip 2-sample */
    CHECK_EQ_U(h5SliAaIdleResetOk(4, 1, 1), 1);  /* cfg 0: one chip, nothing snoops */
    CHECK_EQ_U(h5SliAaIdleResetOk(1, 1, 2), 1);  /* single-chip board */
}

TEST(a_request_never_names_more_chips_than_glide_drives_on_a_4_chip_board) {
    CHECK_EQ_U(h5SliAaChipsDrivenOk(4, 1), 0);
    CHECK_EQ_U(h5SliAaChipsDrivenOk(4, 2), 0);
    CHECK_EQ_U(h5SliAaChipsDrivenOk(4, 4), 1);
    CHECK_EQ_U(h5SliAaChipsDrivenOk(4, 0), 1);   /* not known: not refused */
    CHECK_EQ_U(h5SliAaChipsDrivenOk(2, 1), 1);   /* 2-chip cfg 1 unchanged */
}

TEST(the_request_is_sent_exactly_when_upstream_sent_it) {
    CHECK_EQ_U(h5SliAaRequestNeeded(1, 1, 2, 0), 0);
    CHECK_EQ_U(h5SliAaRequestNeeded(4, 4, 1, 0), 1);
    CHECK_EQ_U(h5SliAaRequestNeeded(4, 1, 1, 0), 0);
    CHECK_EQ_U(h5SliAaRequestNeeded(4, 1, 4, 0), 1);
    CHECK_EQ_U(h5SliAaRequestNeeded(2, 1, 2, 0), 1);
    CHECK_EQ_U(h5SliAaRequestNeeded(2, 1, 2, 1), 0);
    CHECK_EQ_U(h5SliAaAnalog(4, 0), 1);
    CHECK_EQ_U(h5SliAaAnalog(2, 0), 0);
    CHECK_EQ_U(h5SliAaAnalog(2, 1), 1);
    CHECK_EQ_U(h5SliAaAnalog(2, 5), 5);        /* left as it was, as upstream */
}

/* ---- 5. the refactored upstream decisions, row for row ----------------------- */

TEST(control_panel_settings_map_as_upstream) {
    static const struct { long cfg; unsigned long aa, single; } want[] = {
        { 0, 0, 1 }, { 1, 2, 1 }, { 2, 0, 0 }, { 3, 2, 0 }, { 4, 4, 0 },
        { 5, 0, 0 }, { 6, 2, 0 }, { 7, 4, 0 }, { 8, 8, 0 }, { 9, 0, 0 }, { -1, 0, 0 },
    };
    unsigned i;
    for (i = 0; i < sizeof want / sizeof want[0]; i++) {
        unsigned long aa = 99, single = 99;
        h5SliAaConfigEnv(want[i].cfg, &aa, &single);
        CHECK_EQ_U(aa, want[i].aa);
        CHECK_EQ_U(single, want[i].single);
    }
}

TEST(forced_sample_count_as_upstream) {
    CHECK_EQ_U(h5SliAaForcedSample(8, 4), 8);
    CHECK_EQ_U(h5SliAaForcedSample(8, 2), 1);    /* no 8xaa below 3 chips */
    CHECK_EQ_U(h5SliAaForcedSample(4, 2), 4);
    CHECK_EQ_U(h5SliAaForcedSample(4, 1), 1);
    CHECK_EQ_U(h5SliAaForcedSample(2, 1), 2);
    CHECK_EQ_U(h5SliAaForcedSample(0, 4), 1);
}

TEST(per_board_layout_as_upstream) {
    static const struct { unsigned long chips, ps; long old; int set;
                          unsigned long sli, spc, idx; int filt; } want[] = {
        { 4, 8, 0, 1, 1, 2, 9, 0 }, { 4, 4, 0, 1, 1, 1, 8, 0 }, { 4, 2, 0, 1, 2, 1, 7, 1 },
        { 4, 2, 1, 1, 4, 2, 7, 0 }, { 4, 1, 0, 1, 4, 1, 0, 0 }, { 2, 4, 0, 1, 1, 2, 3, 0 },
        { 2, 2, 0, 1, 1, 1, 2, 1 }, { 2, 2, 1, 1, 2, 2, 1, 0 }, { 2, 1, 0, 1, 2, 1, 0, 0 },
        { 1, 2, 0, 1, 1, 2, 1, 0 }, { 1, 1, 0, 1, 1, 1, 0, 0 }, { 3, 2, 0, 1, 1, 1, 0, 0 },
        { 2, 8, 0, 0, 0, 0, 0, 0 }, { 1, 4, 0, 0, 0, 0, 0, 0 }, { 4, 3, 0, 0, 0, 0, 0, 0 },
    };
    unsigned i;
    for (i = 0; i < sizeof want / sizeof want[0]; i++) {
        h5SliAaLayoutT l;
        int set = h5SliAaLayout(want[i].chips, want[i].ps, want[i].old, &l);
        CHECK_EQ_I(set, want[i].set);
        if (set && want[i].set) {
            CHECK_EQ_U(l.sliCount, want[i].sli);
            CHECK_EQ_U(l.samplesPerChip, want[i].spc);
            CHECK_EQ_U(l.offsetIndex, want[i].idx);
            CHECK_EQ_I(l.filterD, want[i].filt);
        }
    }
}

MUNIT_MAIN("h5 Glide SLI/AA guards: request tuple vs vcr-kmd video mux (2026-09-27)", {
    RUN(every_request_glide_sends_is_programmed_by_the_kernel);
    RUN(the_request_per_config_at_1024x768_16bpp);
    RUN(cfg1_on_four_chips_is_refused_the_old_order_sent_the_wedge_tuple);
    RUN(mirror_glide_tuple_table_equals_the_kernel_set);
    RUN(the_table_normalises_like_the_kernel);
    RUN(single_chip_aa_pci_op_loop_writes_only_the_chips_glide_drives);
    RUN(a_read_lock_in_multi_chip_aa_is_refused_unless_opted_in);
    RUN(the_idle_wait_never_resets_the_master_of_a_multi_chip_sliaa_board);
    RUN(a_request_never_names_more_chips_than_glide_drives_on_a_4_chip_board);
    RUN(the_request_is_sent_exactly_when_upstream_sent_it);
    RUN(control_panel_settings_map_as_upstream);
    RUN(forced_sample_count_as_upstream);
    RUN(per_board_layout_as_upstream);
})
#endif /* HAVE_H5SLIAA */
