/* test_vcr_kmd_text.c - TRUE-SOURCE test of voodoo-cleanroom/vcr-kmd/include/vcr_text.h,
 * the text path of the display driver's 2D engine (display/vcrdd_punt.c
 * DrvTextOut -> display/vcrdd_2d.c VcrDd2dMonoGlyph), 2026-09-27.
 *
 * DrvTextOut used to hand every call to EngTextOut (the CPU drawing into the
 * frame buffer). Now a call the gate accepts is drawn by the engine: the
 * opaque rectangle as a solid fill, each glyph as a host-to-screen blit with
 * a 1 bpp source (monochrome expansion, transparent), clipped on the CPU.
 *
 * The engine is modelled here as it consumes a host blit (the Glide GPL
 * h3gdefs.h semantics, the same the 86Box Voodoo3 implements): srcFormat
 * 1 bpp + SRC_PACK_8, so every row is ceil(w/8) bytes and the rows follow
 * each other; bytes come out of each dword lowest first; the first pixel of
 * a byte is bit 7; a 0 bit leaves the destination (TRANSPARENT). What it
 * proves, against a per-pixel reference that draws the glyph's set bits
 * inside the clip rectangle:
 *   - every glyph width 1..40 and height 1..9, clipped at every column and
 *     row on every side and at random, with random bits in the padding: the
 *     expanded part paints exactly the reference;
 *   - the stream is EXACTLY vcr_glyph_dwords() long - what the engine takes
 *     (one short hangs the blit waiting for data, one extra is a launch write
 *     after it ended) - and it never reads outside the bitmap (the bitmap is
 *     placed against a PROT_NONE page on both sides);
 *   - the old way would be wrong: the whole glyph's bytes with the clipped
 *     part's dstSize paint something else as soon as a column or row is cut;
 *   - whole strings through several clip rectangles (the DC_COMPLEX loop)
 *     paint the reference clipped to their union, fixed pitch included;
 *   - a string as ONE blit (the glyphs OR-ed into a mask covering the union
 *     of what shows, in bands of rows that fit the buffer): random strings of
 *     overlapping glyphs, random clips, buffers from one row up - the
 *     reference, nothing written past the band, one blit instead of one per
 *     glyph;
 *   - the gate: each reason in its order, and what was always accepted;
 *   - the command and source format: HOST_BLT | TRANSPARENT | ROP 0xCC, no
 *     GO; 1 bpp, byte-packed, no swizzle (= 0x00400000).
 */
#define _DEFAULT_SOURCE
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include "munit.h"
#include "../../voodoo-cleanroom/vcr-kmd/include/vcr_text.h"

#define FBW 96
#define FBH 64
typedef struct fb {
    unsigned px[FBH][FBW];
} fb;

static void fb_clear(fb *f)
{
    int x, y;
    for (y = 0; y < FBH; y++)
        for (x = 0; x < FBW; x++)
            f->px[y][x] = 0x1000u + (unsigned)(y * FBW + x);   /* a distinct background */
}

/* ---- the engine: a host-to-screen mono blit ------------------------------- */
#define MAXDW 4096
typedef struct sink {
    vcr_u32 dw[MAXDW];
    vcr_u32 n;
    vcr_u32 stop_after;     /* put answers 0 from this dword on (0 = never) */
} sink;

static int sink_put(void *ctx, vcr_u32 d)
{
    sink *s = (sink *)ctx;
    if (s->stop_after && s->n >= s->stop_after)
        return 0;
    if (s->n < MAXDW)
        s->dw[s->n] = d;
    s->n++;
    return 1;
}

/* the dwords the engine consumes for a w x h blit, and what it paints */
static vcr_u32 engine_needs(vcr_u32 w, vcr_u32 h)
{
    return ((w + 7) / 8 * h + 3) / 4;
}

static void engine_expand(fb *f, int x, int y, vcr_u32 w, vcr_u32 h, const sink *s,
                          unsigned color)
{
    vcr_u32 rb = (w + 7) / 8, i, j;
    for (j = 0; j < h; j++)
        for (i = 0; i < w; i++) {
            vcr_u32 at = j * rb + (i >> 3);
            vcr_u32 byte = (s->dw[at >> 2] >> (8 * (at & 3))) & 0xff;
            if (byte & (0x80u >> (i & 7)))
                f->px[y + (int)j][x + (int)i] = color;
        }
}

/* ---- the reference: the glyph's set bits inside the clip ------------------ */
static void ref_glyph(fb *f, const vcr_u8 *bits, vcr_u32 cx, vcr_u32 cy, int gx, int gy,
                      const vcr_rect *clip, unsigned color)
{
    vcr_u32 stride = (cx + 7) / 8, i, j;
    for (j = 0; j < cy; j++)
        for (i = 0; i < cx; i++) {
            int x = gx + (int)i, y = gy + (int)j;
            if (x < clip->l || x >= clip->r || y < clip->t || y >= clip->b)
                continue;
            if (bits[j * stride + (i >> 3)] & (0x80u >> (i & 7)))
                f->px[y][x] = color;
        }
}

static int fb_diff(const fb *a, const fb *b)
{
    int x, y, n = 0;
    for (y = 0; y < FBH; y++)
        for (x = 0; x < FBW; x++)
            n += a->px[y][x] != b->px[y][x];
    return n;
}

/* a bitmap against a PROT_NONE page: after it (at_end) or before it */
static long g_page;
static vcr_u8 *guarded(size_t n, int at_end, void **base)
{
    size_t span = (n + (size_t)g_page - 1) / (size_t)g_page * (size_t)g_page + 2 * (size_t)g_page;
    vcr_u8 *m = mmap(NULL, span, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (m == MAP_FAILED)
        return NULL;
    mprotect(m, (size_t)g_page, PROT_NONE);
    mprotect(m + span - (size_t)g_page, (size_t)g_page, PROT_NONE);
    *base = m;
    return at_end ? m + span - (size_t)g_page - n : m + g_page;
}

static void unguard(void *base, size_t n)
{
    size_t span = (n + (size_t)g_page - 1) / (size_t)g_page * (size_t)g_page + 2 * (size_t)g_page;
    munmap(base, span);
}

static unsigned g_seed = 12345;
static unsigned rnd(void)
{
    g_seed = g_seed * 1103515245u + 12345u;
    return (g_seed >> 8) & 0xffffff;
}

/* one glyph part through the stream and the engine vs the reference;
 * returns the pixels that differ (and -1000 on a wrong dword count) */
static int one(const vcr_u8 *bits, vcr_u32 cx, vcr_u32 cy, int gx, int gy, const vcr_rect *clip)
{
    static fb a, b;
    static sink s;
    vcr_glyph_part p;
    int bad;
    fb_clear(&a);
    fb_clear(&b);
    ref_glyph(&b, bits, cx, cy, gx, gy, clip, 0xabcdefu);
    s.n = 0;
    s.stop_after = 0;
    if (vcr_glyph_clip(gx, gy, cx, cy, clip, &p)) {
        vcr_u32 got = vcr_glyph_stream(bits, cx, &p, sink_put, &s);
        if (got != s.n || got != vcr_glyph_dwords(&p) || got != engine_needs(p.w, p.h))
            return -1000;
        /* the part lies inside the clip AND inside the glyph */
        if (p.x < clip->l || p.y < clip->t || p.x + (int)p.w > clip->r ||
            p.y + (int)p.h > clip->b || p.col0 + p.w > cx || p.row0 + p.h > cy)
            return -2000;
        engine_expand(&a, p.x, p.y, p.w, p.h, &s, 0xabcdefu);
    }
    bad = fb_diff(&a, &b);
    return bad;
}

TEST(every_glyph_size_clipped_every_way_paints_the_reference) {
    vcr_u32 cx, cy;
    long cases = 0, bad = 0;
    for (cx = 1; cx <= 40; cx++)
        for (cy = 1; cy <= 9; cy++) {
            vcr_u32 stride = (cx + 7) / 8, n = stride * cy, i;
            void *base1, *base2;
            vcr_u8 *end = guarded(n, 1, &base1), *start = guarded(n, 0, &base2);
            int gx = 20, gy = 20, c;
            CHECK(end && start, "mmap");
            if (!end || !start)
                return;
            for (i = 0; i < n; i++)
                end[i] = start[i] = (vcr_u8)rnd();      /* padding bits included */
            for (c = 0; c <= (int)cx; c++) {
                /* cut c columns from the left, from the right; c rows likewise */
                vcr_rect k[6];
                int q;
                k[0] = (vcr_rect){ gx + c, 0, FBW, FBH };
                k[1] = (vcr_rect){ 0, 0, gx + (int)cx - c, FBH };
                k[2] = (vcr_rect){ 0, gy + (c < (int)cy ? c : (int)cy), FBW, FBH };
                k[3] = (vcr_rect){ 0, 0, FBW, gy + (int)cy - (c < (int)cy ? c : (int)cy) };
                k[4] = (vcr_rect){ gx + c / 2, gy + 1, gx + (int)cx - c / 3, gy + (int)cy - 1 };
                k[5] = (vcr_rect){ gx + (int)(rnd() % (cx + 2)) - 1, gy + (int)(rnd() % (cy + 2)) - 1,
                                   gx + (int)(rnd() % (cx + 2)), gy + (int)(rnd() % (cy + 2)) };
                for (q = 0; q < 6; q++) {
                    int r1 = one(end, cx, cy, gx, gy, &k[q]), r2 = one(start, cx, cy, gx, gy, &k[q]);
                    cases += 2;
                    if (r1 || r2) {
                        if (!bad)
                            printf("    first mismatch: %ux%u clip %d (%d,%d)-(%d,%d): %d / %d\n", cx,
                                   cy, q, k[q].l, k[q].t, k[q].r, k[q].b, r1, r2);
                        bad++;
                    }
                }
            }
            unguard(base1, n);
            unguard(base2, n);
        }
    printf("    %ld glyph/clip cases\n", cases);
    CHECK(cases > 5000, "the sweep ran");
    CHECK_EQ_I(bad, 0);
}

TEST(the_stream_is_exactly_what_the_engine_takes) {
    static const vcr_u8 g[3 * 10] = { 0 };
    vcr_glyph_part p;
    sink s;
    vcr_rect all = { 0, 0, 100, 100 };
    /* 20 x 10: 3 bytes a row, 30 bytes, 8 dwords (the last half used) */
    CHECK(vcr_glyph_clip(5, 5, 20, 10, &all, &p), "visible");
    CHECK_EQ_U(vcr_glyph_dwords(&p), 8u);
    s.n = 0;
    s.stop_after = 0;
    CHECK_EQ_U(vcr_glyph_stream(g, 20, &p, sink_put, &s), 8u);
    CHECK_EQ_U(s.n, 8u);
    /* 1 x 1: one dword; 8 x 4: exactly one; 9 x 4: 2 bytes a row, 2 dwords */
    CHECK(vcr_glyph_clip(0, 0, 1, 1, &all, &p), "1x1");
    CHECK_EQ_U(vcr_glyph_dwords(&p), 1u);
    CHECK(vcr_glyph_clip(0, 0, 8, 4, &all, &p), "8x4");
    CHECK_EQ_U(vcr_glyph_dwords(&p), 1u);
    CHECK(vcr_glyph_clip(0, 0, 9, 4, &all, &p), "9x4");
    CHECK_EQ_U(vcr_glyph_dwords(&p), 2u);
    /* a put that stops (the FIFO wait gave up) ends the stream short: the
     * driver compares with vcr_glyph_dwords and falls back to software */
    CHECK(vcr_glyph_clip(5, 5, 20, 10, &all, &p), "again");
    s.n = 0;
    s.stop_after = 3;
    CHECK_EQ_U(vcr_glyph_stream(g, 20, &p, sink_put, &s), 3u);
    CHECK(3u != vcr_glyph_dwords(&p), "short is detectable");
}

TEST(clipping_on_the_cpu_is_needed_the_whole_glyph_is_not_the_part) {
    /* the old/naive way: send the glyph's own rows with the part's dstSize.
     * For a 12 x 6 glyph cut 3 columns on the left and 2 rows at the top, the
     * engine then reads 2-byte rows from the top-left of the glyph - another
     * picture. The stream's repacked rows paint the reference. */
    vcr_u8 bits[2 * 6];
    vcr_rect clip = { 23, 22, 96, 64 };
    vcr_glyph_part p;
    static fb a, b;
    sink s;
    vcr_u32 i;
    for (i = 0; i < sizeof bits; i++)
        bits[i] = (vcr_u8)(0x5a ^ (i * 37));
    CHECK_EQ_I(one(bits, 12, 6, 20, 20, &clip), 0);
    CHECK(vcr_glyph_clip(20, 20, 12, 6, &clip, &p), "visible");
    CHECK_EQ_U(p.col0, 3u);
    CHECK_EQ_U(p.row0, 2u);
    CHECK_EQ_U(p.w, 9u);
    CHECK_EQ_U(p.h, 4u);
    fb_clear(&a);
    fb_clear(&b);
    ref_glyph(&b, bits, 12, 6, 20, 20, &clip, 7);
    memset(&s, 0, sizeof s);
    for (i = 0; i < sizeof bits; i += 4)
        s.dw[s.n++] = bits[i] | (vcr_u32)bits[i + 1] << 8 | (vcr_u32)bits[i + 2] << 16 |
                      (vcr_u32)bits[i + 3] << 24;
    engine_expand(&a, p.x, p.y, p.w, p.h, &s, 7);
    CHECK(fb_diff(&a, &b) > 0, "the naive stream paints something else");
}

/* a string: glyphs 7 x 9 at fixed pitch 8, through several disjoint clip
 * rectangles (the DC_COMPLEX loop: each rectangle visits every glyph) */
TEST(a_string_through_a_complex_clip_paints_the_reference) {
    enum { NG = 10, CX = 7, CY = 9, INC = 8 };
    vcr_u8 g[NG][CY];
    vcr_rect rects[4] = { { 0, 0, 30, 64 }, { 30, 0, 50, 24 }, { 55, 20, 96, 64 },
                          { 30, 27, 55, 29 } };
    static fb a, b;
    int i, r, k;
    vcr_i32 x0 = 3, y0 = 20;
    for (k = 0; k < NG; k++)
        for (i = 0; i < CY; i++)
            g[k][i] = (vcr_u8)(rnd() & 0xfe);           /* bit 0 is padding: cleared */
    fb_clear(&a);
    fb_clear(&b);
    for (r = 0; r < 4; r++)
        for (k = 0; k < NG; k++) {
            vcr_i32 x, y;
            vcr_glyph_part p;
            sink s;
            /* ptl is garbage past the first glyph: fixed pitch must not read it */
            vcr_text_pos(INC, (vcr_u32)k, x0, y0, k ? -999 : x0, k ? -999 : y0, &x, &y);
            ref_glyph(&b, g[k], CX, CY, x, y - 2, &rects[r], 0x42);
            if (!vcr_glyph_clip(x, y - 2, CX, CY, &rects[r], &p))
                continue;
            s.n = 0;
            s.stop_after = 0;
            CHECK_EQ_U(vcr_glyph_stream(g[k], CX, &p, sink_put, &s), vcr_glyph_dwords(&p));
            engine_expand(&a, p.x, p.y, p.w, p.h, &s, 0x42);
        }
    CHECK_EQ_I(fb_diff(&a, &b), 0);
    /* and GDI's own positions when the pitch is not fixed */
    {
        vcr_i32 x, y;
        vcr_text_pos(0, 5, 3, 20, 71, 22, &x, &y);
        CHECK_EQ_I(x, 71);
        CHECK_EQ_I(y, 22);
        vcr_text_pos(8, 5, 3, 20, 71, 22, &x, &y);
        CHECK_EQ_I(x, 43);
        CHECK_EQ_I(y, 20);
    }
}

/* The string as one blit (vcrdd_punt.c text_glyphs): pass 1 takes the union
 * of the visible parts, pass 2 ORs every part into a mask per band of rows
 * that fits the buffer, and the mask is expanded as one bitmap. Against the
 * reference, for random strings (overlapping glyphs included), random clips
 * and mask buffers from one row to all of them. */
static int string_via_mask(const vcr_u8 *const *g, const vcr_u32 *cx, const vcr_u32 *cy,
                           const vcr_i32 *gx, const vcr_i32 *gy, int ng, const vcr_rect *clip,
                           vcr_u32 mask_bytes, int *blits)
{
    static fb a, b;
    vcr_u8 *mask;
    void *base;
    int res;
    vcr_rect u = { 0, 0, 0, 0 };
    vcr_glyph_part p;
    int k, n = 0;
    vcr_i32 y0;
    vcr_u32 rows, mst;
    fb_clear(&a);
    fb_clear(&b);
    for (k = 0; k < ng; k++) {
        ref_glyph(&b, g[k], cx[k], cy[k], gx[k], gy[k], clip, 0x77);
        if (!vcr_glyph_clip(gx[k], gy[k], cx[k], cy[k], clip, &p))
            continue;
        if (!n++)
            u = (vcr_rect){ p.x, p.y, p.x + (vcr_i32)p.w, p.y + (vcr_i32)p.h };
        else {
            u.l = p.x < u.l ? p.x : u.l;
            u.t = p.y < u.t ? p.y : u.t;
            u.r = p.x + (vcr_i32)p.w > u.r ? p.x + (vcr_i32)p.w : u.r;
            u.b = p.y + (vcr_i32)p.h > u.b ? p.y + (vcr_i32)p.h : u.b;
        }
    }
    *blits = 0;
    /* the buffer ends against a PROT_NONE page: a write past it faults */
    mask = guarded(mask_bytes, 1, &base);
    if (!mask)
        return -5;
    res = 0;
    if (n) {
        rows = vcr_mask_rows((vcr_u32)(u.r - u.l), mask_bytes);
        if (!rows) {
            unguard(base, mask_bytes);
            return -1;
        }
        mst = (vcr_u32)(u.r - u.l + 7) >> 3;
        for (y0 = u.t; y0 < u.b; y0 += (vcr_i32)rows) {
            vcr_rect band = { u.l, y0, u.r, y0 + (vcr_i32)rows < u.b ? y0 + (vcr_i32)rows : u.b };
            vcr_glyph_part whole = { u.l, y0, (vcr_u32)(u.r - u.l), (vcr_u32)(band.b - band.t), 0, 0 };
            sink s;
            if (mst * whole.h > mask_bytes) {
                res = -3;                                   /* more than the buffer */
                break;
            }
            memset(mask, 0xee, mask_bytes);                 /* past the band: must stay */
            memset(mask, 0, mst * whole.h);
            for (k = 0; k < ng; k++)
                if (vcr_glyph_clip(gx[k], gy[k], cx[k], cy[k], &band, &p))
                    vcr_mask_or(mask, mst, (vcr_u32)(p.x - u.l), (vcr_u32)(p.y - band.t), g[k],
                                cx[k], &p);
            if (mst * whole.h < mask_bytes && mask[mst * whole.h] != 0xee) {
                res = -2;                                   /* wrote past the band */
                break;
            }
            s.n = 0;
            s.stop_after = 0;
            if (vcr_glyph_stream(mask, whole.w, &whole, sink_put, &s) != vcr_glyph_dwords(&whole)) {
                res = -4;
                break;
            }
            engine_expand(&a, whole.x, whole.y, whole.w, whole.h, &s, 0x77);
            (*blits)++;
        }
    }
    unguard(base, mask_bytes);
    return res ? res : fb_diff(&a, &b);
}

TEST(a_string_as_one_blit_paints_the_reference) {
    enum { NG = 12 };
    static vcr_u8 store[NG][6 * 20];
    const vcr_u8 *g[NG];
    vcr_u32 cx[NG], cy[NG];
    vcr_i32 gx[NG], gy[NG];
    int trial, bad = 0, blits, k, runs = 0;
    for (trial = 0; trial < 3000; trial++) {
        vcr_rect clip;
        vcr_u32 mb;
        vcr_i32 x = (vcr_i32)(rnd() % 30) - 10;
        int r;
        for (k = 0; k < NG; k++) {
            vcr_u32 i;
            cx[k] = 1 + rnd() % 40;
            cy[k] = 1 + rnd() % 20;
            for (i = 0; i < ((cx[k] + 7) / 8) * cy[k]; i++)
                store[k][i] = (vcr_u8)rnd();
            g[k] = store[k];
            gx[k] = x + (vcr_i32)(rnd() % 5) - 2;       /* overlapping neighbours */
            gy[k] = 10 + (vcr_i32)(rnd() % 30) - 15;
            x += (vcr_i32)(cx[k] * 2 / 3);
        }
        clip.l = (vcr_i32)(rnd() % 30) - 5;
        clip.t = (vcr_i32)(rnd() % 20) - 5;
        clip.r = clip.l + 1 + (vcr_i32)(rnd() % 100);
        clip.b = clip.t + 1 + (vcr_i32)(rnd() % 50);
        if (clip.l < 0) clip.l = 0;
        if (clip.t < 0) clip.t = 0;
        if (clip.r > FBW) clip.r = FBW;
        if (clip.b > FBH) clip.b = FBH;
        if (clip.r <= clip.l || clip.b <= clip.t)
            continue;
        /* a buffer of one row of the widest possible union .. plenty */
        /* 13: one row of the widest union, ending at the buffer's last byte */
        mb = trial % 3 == 0 ? 13 : trial % 3 == 1 ? 40 + rnd() % 200 : 8192;
        r = string_via_mask(g, cx, cy, gx, gy, NG, &clip, mb, &blits);
        runs++;
        if (r) {
            if (!bad)
                printf("    trial %d: %d (mask %u bytes, %d blits)\n", trial, r, mb, blits);
            bad++;
        }
    }
    printf("    %d strings\n", runs);
    CHECK(runs > 2000, "the strings ran");
    CHECK_EQ_I(bad, 0);
    /* one blit for a whole string when the buffer holds it, one per band otherwise */
    {
        static const vcr_u8 z[20] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
                                      0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
        const vcr_u8 *gg[4] = { z, z, z, z };
        vcr_u32 c4[4] = { 8, 8, 8, 8 }, h4[4] = { 10, 10, 10, 10 };
        vcr_i32 x4[4] = { 0, 9, 18, 27 }, y4[4] = { 5, 5, 5, 5 };
        vcr_rect all = { 0, 0, FBW, FBH };
        CHECK_EQ_I(string_via_mask(gg, c4, h4, x4, y4, 4, &all, VCR_TEXT_MASK_BYTES, &blits), 0);
        CHECK_EQ_I(blits, 1);                   /* was 4: one per glyph */
        CHECK_EQ_I(string_via_mask(gg, c4, h4, x4, y4, 4, &all, 5 * 3, &blits), 0);
        CHECK_EQ_I(blits, 4);                   /* 35 px = 5 bytes a row, 3 rows a band: 10 rows */
        CHECK_EQ_U(vcr_mask_rows(1600, VCR_TEXT_MASK_BYTES), 81u);
        CHECK_EQ_U(vcr_mask_rows(0, VCR_TEXT_MASK_BYTES), 0u);
    }
}

TEST(clip_decomposition) {
    vcr_rect surf = { 0, 0, 800, 600 }, o, b;
    /* DC_TRIVIAL: the surface, whatever the bounds say */
    b = (vcr_rect){ 5, 5, 6, 6 };
    CHECK(vcr_text_clip_rect(VCR_DC_TRIVIAL, &b, &surf, &o), "trivial");
    CHECK(o.l == 0 && o.t == 0 && o.r == 800 && o.b == 600, "the surface");
    /* DC_RECT: the bounds on the surface */
    b = (vcr_rect){ -10, 590, 30, 700 };
    CHECK(vcr_text_clip_rect(VCR_DC_RECT, &b, &surf, &o), "rect");
    CHECK(o.l == 0 && o.t == 590 && o.r == 30 && o.b == 600, "cut to the surface");
    b = (vcr_rect){ 800, 0, 900, 10 };
    CHECK(!vcr_text_clip_rect(VCR_DC_RECT, &b, &surf, &o), "off the surface: nothing");
    b = (vcr_rect){ 10, 10, 10, 20 };
    CHECK(!vcr_text_clip_rect(VCR_DC_RECT, &b, &surf, &o), "empty");
    /* DC_COMPLEX is enumerated, never taken as one rectangle */
    b = (vcr_rect){ 0, 0, 800, 600 };
    CHECK(!vcr_text_clip_rect(VCR_DC_COMPLEX, &b, &surf, &o), "complex");
    /* right/bottom exclusive: touching rectangles do not overlap */
    {
        vcr_rect p = { 0, 0, 10, 10 }, q = { 10, 0, 20, 10 };
        CHECK(!vcr_rect_isect(&p, &q, &o), "touching");
        q.l = 9;
        CHECK(vcr_rect_isect(&p, &q, &o) && o.l == 9 && o.r == 10, "one column");
    }
    /* a glyph entirely outside: no part */
    {
        vcr_glyph_part p;
        vcr_rect c = { 100, 100, 200, 200 };
        CHECK(!vcr_glyph_clip(90, 100, 10, 10, &c, &p), "left of it");
        CHECK(vcr_glyph_clip(91, 100, 10, 10, &c, &p) && p.w == 1 && p.col0 == 9, "one column in");
        CHECK(!vcr_glyph_clip(100, 100, 0, 10, &c, &p), "zero width");
    }
}

TEST(extra_rectangles_end_at_the_zero_one) {
    vcr_rect e[12];
    memset(e, 0, sizeof e);
    CHECK_EQ_U(vcr_text_extra_count(NULL, VCR_TEXT_MAX_EXTRA), 0u);
    CHECK_EQ_U(vcr_text_extra_count(e, VCR_TEXT_MAX_EXTRA), 0u);
    e[0] = (vcr_rect){ 1, 2, 3, 4 };
    e[1] = (vcr_rect){ 0, 0, 5, 1 };             /* not the terminator: one corner is not 0 */
    CHECK_EQ_U(vcr_text_extra_count(e, VCR_TEXT_MAX_EXTRA), 2u);
    {
        int i;
        for (i = 0; i < 11; i++)
            e[i] = (vcr_rect){ i + 1, 0, i + 2, 1 };
        CHECK_EQ_U(vcr_text_extra_count(e, VCR_TEXT_MAX_EXTRA), VCR_TEXT_MAX_EXTRA + 1);
    }
}

static vcr_text_req ok_req(void)
{
    vcr_text_req q;
    memset(&q, 0, sizeof q);
    q.engine = 1;
    q.bpp = 16;
    q.mix = 0x0d0d;
    q.fore = 0xf800;
    q.opaque = VCR_NO_BRUSH;
    q.font_type = 0x1 /* FO_TYPE_RASTER */;
    return q;
}

TEST(the_gate) {
    vcr_text_req q = ok_req();
    CHECK_EQ_U(vcr_text_gate(&q), VCR_TEXT_OK);
    q.bpp = 8;
    CHECK_EQ_U(vcr_text_gate(&q), VCR_TEXT_OK);
    q.bpp = 32;
    CHECK_EQ_U(vcr_text_gate(&q), VCR_TEXT_OK);
    q.bpp = 24;
    CHECK_EQ_U(vcr_text_gate(&q), VCR_TEXT_R_ENGINE);
    /* the switch: off is EngTextOut for every call - the behaviour before */
    q = ok_req();
    q.off = 1;
    CHECK_EQ_U(vcr_text_gate(&q), VCR_TEXT_R_OFF);
    q.mix = 0x0606;
    CHECK_EQ_U(vcr_text_gate(&q), VCR_TEXT_R_OFF);      /* the switch is looked at first */
    q = ok_req();
    q.engine = 0;
    CHECK_EQ_U(vcr_text_gate(&q), VCR_TEXT_R_ENGINE);
    q = ok_req();
    q.mix = 0x0d06;                                     /* R2_XORPEN background */
    CHECK_EQ_U(vcr_text_gate(&q), VCR_TEXT_R_MIX);
    q.mix = 0x060d;
    CHECK_EQ_U(vcr_text_gate(&q), VCR_TEXT_R_MIX);
    q = ok_req();
    q.fore = VCR_NO_BRUSH;
    CHECK_EQ_U(vcr_text_gate(&q), VCR_TEXT_R_BRUSH);
    q = ok_req();
    q.has_opaque = 1;                                   /* an opaque rectangle needs a solid brush */
    CHECK_EQ_U(vcr_text_gate(&q), VCR_TEXT_R_BRUSH);
    q.opaque = 0xffff;
    CHECK_EQ_U(vcr_text_gate(&q), VCR_TEXT_OK);
    q = ok_req();
    q.font_type = 0x4 /* TRUETYPE */ | VCR_FO_GRAY16;
    CHECK_EQ_U(vcr_text_gate(&q), VCR_TEXT_R_FONT);
    q.font_type = 0x4 | VCR_FO_CLEARTYPE_X;
    CHECK_EQ_U(vcr_text_gate(&q), VCR_TEXT_R_FONT);
    q.font_type = 0x4 | 0x00020000 /* FO_NOGRAY16 */ | 0x00002000 /* FO_SIM_BOLD */;
    CHECK_EQ_U(vcr_text_gate(&q), VCR_TEXT_OK);
    q = ok_req();
    q.char_inc = 8;
    q.accel = 0x2 /* SO_HORIZONTAL */ | 0x1;
    CHECK_EQ_U(vcr_text_gate(&q), VCR_TEXT_OK);
    q.accel = VCR_SO_VERTICAL;
    CHECK_EQ_U(vcr_text_gate(&q), VCR_TEXT_R_LAYOUT);
    q.accel = VCR_SO_REVERSED;
    CHECK_EQ_U(vcr_text_gate(&q), VCR_TEXT_R_LAYOUT);
    q.char_inc = 0;                                     /* GDI's own positions: any layout */
    CHECK_EQ_U(vcr_text_gate(&q), VCR_TEXT_OK);
    q = ok_req();
    q.extras = VCR_TEXT_MAX_EXTRA;
    CHECK_EQ_U(vcr_text_gate(&q), VCR_TEXT_OK);
    q.extras = VCR_TEXT_MAX_EXTRA + 1;
    CHECK_EQ_U(vcr_text_gate(&q), VCR_TEXT_R_EXTRA);
    CHECK(strcmp(vcr_text_reason(VCR_TEXT_R_OFF), "off (Diag\\Accel2DText not 1)") == 0, "names");
    CHECK_EQ_U(VCR_TEXT_R_MAX, 10u);
}

TEST(the_command_and_the_source_format) {
    /* SSTG_HOST_BLT 3, SSTG_TRANSPARENT bit 16, ROP0 0xCC in 31:24 */
    CHECK_EQ_U(VCR_TEXT_CMD, 0xcc010003u);
    CHECK_EQ_U(VCR_TEXT_CMD & VCR_2D_CMD_GO, 0u);           /* started by its data */
    CHECK_EQ_U(VCR_TEXT_CMD & 0xfu, 3u);
    /* SSTG_PIXFMT_1BPP, SSTG_SRC_PACK_8 (bit 22), stride 0, no swizzle */
    CHECK_EQ_U(VCR_TEXT_SRCFMT, 0x00400000u);
    CHECK_EQ_U(VCR_TEXT_SRCFMT & (VCR_2D_SRC_BYTE_SWIZZLE | VCR_2D_SRC_WORD_SWIZZLE), 0u);
    /* the byte order: a single dword carries bytes 0..3 lowest first, and a
     * pixel is the byte's high bit first - an 8 x 4 glyph whose rows are
     * 0x80, 0x40, 0x20, 0x10 is one dword 0x10204080 */
    {
        const vcr_u8 g[4] = { 0x80, 0x40, 0x20, 0x10 };
        vcr_rect all = { 0, 0, 64, 64 };
        vcr_glyph_part p;
        sink s;
        s.n = 0;
        s.stop_after = 0;
        CHECK(vcr_glyph_clip(0, 0, 8, 4, &all, &p), "visible");
        CHECK_EQ_U(vcr_glyph_stream(g, 8, &p, sink_put, &s), 1u);
        CHECK_EQ_U(s.dw[0], 0x10204080u);
    }
    /* a row cut at column 3 is shifted left 3 and its tail cleared */
    {
        const vcr_u8 g[2] = { 0xff, 0xff };           /* one row, 16 wide */
        vcr_rect c = { 3, 0, 12, 1 };
        vcr_glyph_part p;
        sink s;
        s.n = 0;
        s.stop_after = 0;
        CHECK(vcr_glyph_clip(0, 0, 16, 1, &c, &p), "visible");
        CHECK_EQ_U(p.w, 9u);
        CHECK_EQ_U(vcr_glyph_stream(g, 16, &p, sink_put, &s), 1u);
        CHECK_EQ_U(s.dw[0], 0x80ffu);                 /* 0xff, then 0x80: 9 pixels */
    }
}

MUNIT_MAIN("vcr-kmd text on the 2D engine (vcr_text.h)", {
    g_page = sysconf(_SC_PAGESIZE);
    RUN(every_glyph_size_clipped_every_way_paints_the_reference);
    RUN(the_stream_is_exactly_what_the_engine_takes);
    RUN(clipping_on_the_cpu_is_needed_the_whole_glyph_is_not_the_part);
    RUN(a_string_through_a_complex_clip_paints_the_reference);
    RUN(a_string_as_one_blit_paints_the_reference);
    RUN(clip_decomposition);
    RUN(extra_rectangles_end_at_the_zero_one);
    RUN(the_gate);
    RUN(the_command_and_the_source_format);
})
