/* test_h5_glide_guards.c
 *
 * Guards four fixes in OUR h5 (Voodoo 4/5) Glide - fork voidsstr/retro3dfx-glide,
 * glide3x/h5/minihwc/minihwc.c - found by the 2026-09-24 pre-flight audit of the
 * board-open path (clean-room lane, NOT the vintage retro-3dfx Glide):
 *
 *  1. hwcClampNumChipsOverride(): FX_GLIDE_NUM_CHIPS is honoured only as 1 or
 *     the real chip count. 2 on the 4-chip V5 6000 made the SLI/AA request name
 *     fewer chips than the board has - recorded as a whole-box wedge
 *     (retro-3dfx V56K-SLI-FINDINGS.md: "=1 is safe", "=4/unset are safe").
 *  2. hwcIdleHardwareWithTimeout(): bounded, returns success/failure. The
 *     original counted to 1e9 polls, reset only the master, and then looped
 *     forever because its counter was never reset - on every board open and
 *     every close, including from DLL_PROCESS_DETACH.
 *  3. hwcMappingLooksLive(): a mapping must be MEM_COMMIT + MEM_MAPPED with
 *     AllocationBase == base and a minimum size, else it is refused before the
 *     first MMIO. A force-killed Glide process leaves its slot in the driver
 *     (keyed on the PID alone) and the next process that reuses the PID is
 *     handed dead, non-zero mappings. Good mappings measured on .124.
 *  4. hwcEscape is FxI32: as FxI16 the XP escape 0x13df3 was stored as 0x3df3.
 *
 * Each block mirrors the source exactly and asserts BOTH the fixed and the
 * old-buggy value.
 */
#include "munit.h"
#include <stdint.h>

/* ---- 1. FX_GLIDE_NUM_CHIPS clamp (mirror of hwcClampNumChipsOverride) ---- */
static uint32_t clamp_new(int32_t requested, uint32_t real)
{
    if (requested == 1) return 1;
    if (requested > 0 && (uint32_t) requested == real) return real;
    return real;
}
/* the original: "if (numChips < 1) numChips = 1; if (numChips <= real) use it" */
static uint32_t clamp_old(int32_t requested, uint32_t real)
{
    uint32_t n = (uint32_t) requested;
    if ((int32_t) n < 1) n = 1;
    return n <= real ? n : real;
}

TEST(numchips_two_on_a_four_chip_board_is_refused) {
    CHECK_EQ_U(clamp_new(2, 4), 4);
    CHECK_EQ_U(clamp_old(2, 4), 2);          /* the board-killer */
}

TEST(numchips_one_and_real_count_are_honoured) {
    CHECK_EQ_U(clamp_new(1, 4), 1);
    CHECK_EQ_U(clamp_new(4, 4), 4);
    CHECK_EQ_U(clamp_new(1, 2), 1);
    CHECK_EQ_U(clamp_new(2, 2), 2);
}

TEST(numchips_three_zero_negative_fall_back_to_real) {
    CHECK_EQ_U(clamp_new(3, 4), 4);
    CHECK_EQ_U(clamp_new(0, 4), 4);
    CHECK_EQ_U(clamp_new(-1, 4), 4);
    CHECK_EQ_U(clamp_new(8, 4), 4);
}

/* ---- 2. bounded idle wait (mirror of hwcWaitIdleBounded + the one reset) ---- */
#define POLL_BOUND 2000000UL
static int busy_forever(void *ctx) { (void) ctx; return 1; }
static int busy_then_idle(void *ctx) { int *n = (int *) ctx; return (*n)-- > 0; }

static int wait_idle_bounded(int (*busy)(void *), void *ctx, unsigned long *polls_out)
{
    unsigned long polls = 0, streak = 0;
    while (streak < 3) {
        if (busy(ctx)) streak = 0; else streak++;
        if (++polls >= POLL_BOUND) { *polls_out = polls; return 0; }
    }
    *polls_out = polls;
    return 1;
}

/* idle-with-timeout: wait, reset master once, wait once more, report */
static int idle_with_timeout(int (*busy)(void *), void *ctx, int *resets, unsigned long *total)
{
    unsigned long p1 = 0, p2 = 0;
    if (wait_idle_bounded(busy, ctx, &p1)) { *total = p1; return 1; }
    (*resets)++;
    {
        int ok = wait_idle_bounded(busy, ctx, &p2);
        *total = p1 + p2;
        return ok;
    }
}

/* the original, modelled with a guard so the test itself terminates: it never
 * returned once the timeout fired - it reset and re-entered forever. */
static unsigned long old_resets_before_giving_up(int (*busy)(void *), void *ctx, int cap)
{
    unsigned long timeout = 0, idle = 0;
    int resets = 0;
check:
    do {
        if (busy(ctx)) idle = 0; else idle++;
        timeout++;
        if (timeout >= 1000UL) break;       /* 1e9 in the source; scaled */
    } while (idle < 3);
    if (timeout >= 1000UL) {
        if (++resets >= cap) return resets; /* the source has no such exit */
        goto check;                          /* timeout never reset */
    }
    return resets;
}

TEST(stuck_busy_returns_failure_within_the_bound_after_one_reset) {
    int resets = 0; unsigned long total = 0;
    CHECK_EQ_U(idle_with_timeout(busy_forever, NULL, &resets, &total), 0);
    CHECK_EQ_U(resets, 1);
    CHECK(total <= 2 * POLL_BOUND, "must give up, not spin");
}

TEST(old_code_resets_forever_on_stuck_busy) {
    /* it hits the cap only because the test supplies one */
    CHECK_EQ_U(old_resets_before_giving_up(busy_forever, NULL, 50), 50);
}

TEST(briefly_busy_hardware_still_reports_idle_without_a_reset) {
    int n = 1000, resets = 0; unsigned long total = 0;
    CHECK_EQ_U(idle_with_timeout(busy_then_idle, &n, &resets, &total), 1);
    CHECK_EQ_U(resets, 0);
}

/* ---- 3. mapping liveness (mirror of hwcMappingLooksLive) ---- */
#define MEM_COMMIT_  0x1000u
#define MEM_FREE_    0x10000u
#define MEM_RESERVE_ 0x2000u
#define MEM_MAPPED_  0x40000u
#define MEM_PRIVATE_ 0x20000u
#define MEM_IMAGE_   0x1000000u
struct mbi { uint32_t state, type; unsigned long allocBase, regionSize; };

static int looks_live(unsigned long va, const struct mbi *m, unsigned long minSize)
{
    return m->state == MEM_COMMIT_ && m->type == MEM_MAPPED_ &&
           m->allocBase == va && m->regionSize >= minSize;
}

TEST(the_measured_good_mappings_pass) {
    /* .124, V5 6000, AmigaMerlin 3.1-R11, 2026-09-24 */
    struct mbi reg = { MEM_COMMIT_, MEM_MAPPED_, 0x009c0000ul, 0x8000000ul };
    struct mbi lfb = { MEM_COMMIT_, MEM_MAPPED_, 0x089c0000ul, 0x10000000ul };
    struct mbi slv = { MEM_COMMIT_, MEM_MAPPED_, 0x189c0000ul, 0x1000ul };
    CHECK(looks_live(0x009c0000ul, &reg, 0x00800000ul), "register window");
    CHECK(looks_live(0x089c0000ul, &lfb, 0x00100000ul), "LFB");
    CHECK(looks_live(0x189c0000ul, &slv, 0x1000ul), "slave register page");
}

TEST(a_dead_processes_freed_mapping_is_refused) {
    struct mbi freed = { MEM_FREE_, 0, 0, 0x8000000ul };
    CHECK(!looks_live(0x02330000ul, &freed, 0x00800000ul), "MEM_FREE");
    /* the old guard only refused a ZERO base, so this non-zero one passed */
    CHECK(0x02330000ul != 0, "non-zero: invisible to the zero-base guard");
}

TEST(a_stale_address_now_holding_our_own_memory_is_refused) {
    struct mbi heap = { MEM_COMMIT_, MEM_PRIVATE_, 0x02300000ul, 0x100000ul };
    struct mbi dll  = { MEM_COMMIT_, MEM_IMAGE_,  0x02330000ul, 0x800000ul };
    struct mbi resv = { MEM_RESERVE_, MEM_MAPPED_, 0x02330000ul, 0x8000000ul };
    CHECK(!looks_live(0x02330000ul, &heap, 0x00800000ul), "MEM_PRIVATE heap");
    CHECK(!looks_live(0x02330000ul, &dll, 0x00800000ul), "MEM_IMAGE");
    CHECK(!looks_live(0x02330000ul, &resv, 0x00800000ul), "MEM_RESERVE");
}

TEST(an_interior_address_or_short_view_is_refused) {
    struct mbi interior = { MEM_COMMIT_, MEM_MAPPED_, 0x009c0000ul, 0x8000000ul };
    struct mbi tiny     = { MEM_COMMIT_, MEM_MAPPED_, 0x009c0000ul, 0x1000ul };
    CHECK(!looks_live(0x009c1000ul, &interior, 0x00800000ul), "AllocationBase != va");
    CHECK(!looks_live(0x009c0000ul, &tiny, 0x00800000ul), "view smaller than the register window");
}

/* ---- 4. the XP escape survives storage ---- */
TEST(xp_escape_code_survives_a_32_bit_field_not_a_16_bit_one) {
    int32_t wide = 0x13df3;
    int16_t narrow = (int16_t) 0x13df3;
    CHECK_EQ_U((uint32_t) wide, 0x13df3u);
    CHECK_EQ_U((uint32_t) (uint16_t) narrow, 0x3df3u);   /* the old H6 dead code */
}

/* ---- 5. swap-pending bookkeeping (gglide.c, fork after 215a9e7) ----
 * bufferSwaps has MAX_BUFF_PENDING (7) entries; three loops started at index 7
 * and so read AND wrote the field after the array. A decrement per stray match
 * can wrap swapsPending (unsigned) to ~4e9, after which
 * while (_grBufferNumPending() > swapPendingCount) never ends. */
#define MAX_BUFF_PENDING 7
struct swapctx { uint32_t swapsPending, lastSwapCheck, curSwap, bufferSwaps[MAX_BUFF_PENDING], next; };

static void retire_old(struct swapctx *g, uint32_t readPtr)
{   /* "it's behind us" branch, original bounds; next models the field after */
    uint32_t *slots = g->bufferSwaps;
    int i;
    for (i = MAX_BUFF_PENDING; i >= 0; --i) {
        uint32_t *slot = (i == MAX_BUFF_PENDING) ? &g->next : &slots[i];
        if (*slot != 0xffffffffu && *slot >= g->lastSwapCheck && *slot <= readPtr) {
            --g->swapsPending;
            *slot = 0xffffffffu;
        }
    }
}
static void retire_new(struct swapctx *g, uint32_t readPtr)
{
    int i;
    for (i = MAX_BUFF_PENDING - 1; i >= 0; --i)
        if (g->bufferSwaps[i] != 0xffffffffu &&
            g->bufferSwaps[i] >= g->lastSwapCheck && g->bufferSwaps[i] <= readPtr) {
            --g->swapsPending;
            g->bufferSwaps[i] = 0xffffffffu;
        }
}
static void init(struct swapctx *g)
{
    int i;
    for (i = 0; i < MAX_BUFF_PENDING; i++) g->bufferSwaps[i] = 0xffffffffu;
    g->swapsPending = 1; g->lastSwapCheck = 0x100; g->bufferSwaps[0] = 0x200;
    g->next = 0x300;          /* an unrelated field whose value lies in range */
}

TEST(retire_never_touches_the_field_after_the_array) {
    struct swapctx g;
    init(&g); retire_new(&g, 0x400);
    CHECK_EQ_U(g.swapsPending, 0);
    CHECK_EQ_U(g.next, 0x300);                 /* untouched */
    init(&g); retire_old(&g, 0x400);
    CHECK_EQ_U(g.next, 0xffffffffu);           /* the old code stamped it */
    CHECK_EQ_U(g.swapsPending, 0xffffffffu);   /* and wrapped the counter */
}

static int pending_forever(void *ctx) { (void) ctx; return 3; }
static unsigned long bounded_swap_wait(int (*pending)(void *), void *ctx, int limit, int *broke)
{
    unsigned long spins = 0;
    *broke = 0;
    while (pending(ctx) > limit) {
        if (++spins > 4000000UL) { *broke = 1; break; }
    }
    return spins;
}

TEST(swap_pending_wait_is_bounded) {
    int broke = 0;
    unsigned long n = bounded_swap_wait(pending_forever, NULL, 2, &broke);
    CHECK_EQ_U(broke, 1);
    CHECK(n <= 4000001UL, "gives up after the bound");
}

/* ---- 6. grTexDownloadMipMapLevelPartialRowExt min_s alignment (fork, 2026-09-25) --
 * "64 bit Align minS": the 3dfx source wrote `min_s &= 8 / 4 / 2`, keeping ONE
 * bit instead of clearing the low ones, so a 32-bit row patch starting at s=16
 * was sent from s=0 - the wrong texels, silently. The fix clears the low bits,
 * and clamps the rounded-up width at the row end. */
static int align_old(int bpp, int s) { return bpp == 8 ? (s & 8) : bpp == 16 ? (s & 4) : (s & 2); }
static int align_new(int bpp, int s) { return bpp == 8 ? (s & ~7) : bpp == 16 ? (s & ~3) : (s & ~1); }

TEST(partial_row_min_s_aligns_down_to_64_bits) {
    CHECK_EQ_U(align_new(32, 16), 16);
    CHECK_EQ_U(align_old(32, 16), 0);        /* the bug: sent from column 0 */
    CHECK_EQ_U(align_new(32, 17), 16);
    CHECK_EQ_U(align_new(16, 13), 12);
    CHECK_EQ_U(align_old(16, 13), 4);
    CHECK_EQ_U(align_new(8, 21), 16);
    CHECK_EQ_U(align_old(8, 21), 0);
    {   /* aligned start never passes the requested one, never by >= 64 bits */
        int bpp, sidx;
        for (bpp = 8; bpp <= 32; bpp *= 2)
            for (sidx = 0; sidx < 256; sidx++) {
                int a = align_new(bpp, sidx);
                CHECK(a <= sidx && (sidx - a) * bpp < 64, "aligned down within 64 bits");
            }
    }
}

TEST(partial_row_width_never_runs_past_the_row) {
    /* 32-bit, 128-wide row, patch [126, 127]: min_s 126, width 2 - fine;
     * patch [127,127] after align min_s 126, width 2: stays inside */
    int real_width = 128, min_s = align_new(32, 127), width = 128 - min_s;
    if (width > 1) width = (width + 1) & ~1;
    if (min_s + width > real_width && real_width > min_s) width = real_width - min_s;
    CHECK(min_s + width <= real_width, "clamped to the row");
}

MUNIT_MAIN("h5 Glide guards: chip clamp, bounded idle, live mappings, XP escape (2026-09-24)", {
    RUN(numchips_two_on_a_four_chip_board_is_refused);
    RUN(numchips_one_and_real_count_are_honoured);
    RUN(numchips_three_zero_negative_fall_back_to_real);
    RUN(stuck_busy_returns_failure_within_the_bound_after_one_reset);
    RUN(old_code_resets_forever_on_stuck_busy);
    RUN(briefly_busy_hardware_still_reports_idle_without_a_reset);
    RUN(the_measured_good_mappings_pass);
    RUN(a_dead_processes_freed_mapping_is_refused);
    RUN(a_stale_address_now_holding_our_own_memory_is_refused);
    RUN(an_interior_address_or_short_view_is_refused);
    RUN(xp_escape_code_survives_a_32_bit_field_not_a_16_bit_one);
    RUN(retire_never_touches_the_field_after_the_array);
    RUN(swap_pending_wait_is_bounded);
    RUN(partial_row_min_s_aligns_down_to_64_bits);
    RUN(partial_row_width_never_runs_past_the_row);
})
