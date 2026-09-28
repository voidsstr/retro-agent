/*
 * gdilab.c - a self-checking GDI test program for the display driver's 2D
 * paths: what the 2D engine accelerates (solid fills, BLACKNESS/WHITENESS,
 * screen-to-screen copies, overlapping scrolls, copies through a clip region)
 * and the ordering between the engine and the CPU (the punt layer draws with
 * the CPU into the same memory the engine writes); text (DrvTextOut on the
 * engine: opaque rectangle, glyphs by monochrome expansion, underline /
 * strike-out, clipping) against GDI's own software rendering; and a text
 * throughput benchmark.
 *
 *   gdilab [--log path] [--rounds N] [--tests base,text,bench] [--bench-ms N]
 *          [--require-text-accel]
 *
 * Draws into its own topmost popup (a bare screen DC is repainted by Explorer
 * under the test), reads every result back through a memory DC, and compares
 * with what it computed. The pattern is a per-pixel function of (x, y), so a
 * copy that lands one pixel off, a row that is skipped, a scroll that smears
 * or a clip rectangle that is ignored is counted, not guessed at.
 *
 * TEXT (--tests text, on by default): each case starts the window and a
 * reference bitmap (a memory DC in the screen's own format - drawn by GDI's
 * DIB engine, i.e. in software) from the same background, makes the SAME
 * ExtTextOut on both, and compares every pixel (exactly: the two are the
 * same pixel format). Fonts: raster (MS Sans Serif, Fixedsys), TrueType
 * without anti-aliasing (Tahoma, Times New Roman italic, Courier New, Arial
 * bold 33 and 56 px), underline + strike-out, and two that may be smoothed
 * (the driver sends those to software). Transparent, opaque (bk mode) and
 * ETO_OPAQUE backgrounds; ETO_CLIPPED, a region with holes (DC_COMPLEX), the
 * window's edges (glyphs cut on every side), a string past the end of a
 * STROBJ batch, and text / CPU pixel / text on the same pixels. On our
 * driver the display DLL's 2D counters (VCR_ESC_2D_STATS) are read around
 * every case, so a pass says whether the ENGINE drew it; with
 * --require-text-accel a case that should have been accelerated and was not
 * counts as bad_text_path.
 *
 * BENCH (--tests bench): glyphs per second for five typical strings, each
 * drawn for --bench-ms (default 2000) and then read back (which waits for the
 * engine) - the same numbers on any driver.
 *
 * Every step is flushed to the log before the next. Final line: RESULT {json}.
 */
#include <windows.h>
#include <io.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vcr_ioctl.h"      /* VCR_ESC_2D_STATS, vcr_2d_stats: our driver's 2D counters */

#define W 256
#define H 192
#define CELL 8

static FILE *g_log;
static char g_logpath[MAX_PATH] = "C:\\gdilab.log";
static int g_bpp;

static void say(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    if (g_log) {
        vfprintf(g_log, fmt, ap);
        fputc('\n', g_log);
        fflush(g_log);
        FlushFileBuffers((HANDLE)_get_osfhandle(_fileno(g_log)));
    }
    va_end(ap);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    printf("\n");
    fflush(stdout);
    va_end(ap);
}

static void pump(void)
{
    MSG msg;
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
}

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_ERASEBKGND || m == WM_PAINT) {
        ValidateRect(h, NULL);      /* nothing repaints under the test */
        return 1;
    }
    return DefWindowProcA(h, m, w, l);
}

/* the pattern: 8x8 cells of distinct colours, exactly representable at 16 bpp
 * (every channel a multiple of 8, green of 4) */
static COLORREF pat(int x, int y)
{
    int cx = x / CELL, cy = y / CELL;
    return RGB((cx * 40 + cy * 8) & 0xf8, (cy * 36 + cx * 12) & 0xfc, ((cx ^ cy) * 24 + 64) & 0xf8);
}

static int same(COLORREF a, COLORREF b)
{
    int tol = g_bpp == 16 ? 8 : g_bpp == 8 ? 0 : 1, i;
    for (i = 0; i < 24; i += 8)
        if (abs((int)((a >> i) & 0xff) - (int)((b >> i) & 0xff)) > tol)
            return 0;
    return 1;
}

static HDC g_wdc, g_mem;
static HBITMAP g_bm;

static void draw_pattern_on(HDC dc)
{
    int x, y;
    for (y = 0; y < H; y += CELL)
        for (x = 0; x < W; x += CELL) {
            HBRUSH b = CreateSolidBrush(pat(x, y));
            RECT r;
            SetRect(&r, x, y, x + CELL, y + CELL);
            FillRect(dc, &r, b);
            DeleteObject(b);
        }
}

static void draw_pattern(void)
{
    draw_pattern_on(g_wdc);
}

typedef COLORREF (*expect_fn)(int x, int y, void *ctx);

/* read the window back and compare every 3rd pixel with want() */
static int check(const char *name, expect_fn want, void *ctx, int *first)
{
    int x, y, bad = 0;
    GdiFlush();
    BitBlt(g_mem, 0, 0, W, H, g_wdc, 0, 0, SRCCOPY);
    GdiFlush();
    for (y = 0; y < H; y++)
        for (x = (y % 3); x < W; x += 3) {
            COLORREF got = GetPixel(g_mem, x, y), w = want(x, y, ctx);
            if (!same(got, w)) {
                if (!bad)
                    say("  %s: first bad at (%d,%d) got %06lx want %06lx", name, x, y,
                        (unsigned long)got, (unsigned long)w);
                bad++;
            }
        }
    say("%s: %d bad", name, bad);
    if (bad && first && !*first)
        *first = 1;
    return bad;
}

static COLORREF want_pat(int x, int y, void *c) { (void)c; return pat(x, y); }

/* a move of the whole pattern by (dx, dy), with rect r the destination */
typedef struct { RECT r; int dx, dy; } move_t;
static COLORREF want_move(int x, int y, void *c)
{
    move_t *m = (move_t *)c;
    POINT p = { x, y };
    if (PtInRect(&m->r, p))
        return pat(x - m->dx, y - m->dy);
    return pat(x, y);
}

typedef struct { RECT r; COLORREF c; } fill_t;
static COLORREF want_fill(int x, int y, void *c)
{
    fill_t *f = (fill_t *)c;
    POINT p = { x, y };
    return PtInRect(&f->r, p) ? f->c : pat(x, y);
}

/* 16 copies of 6x6 at x = 8 + 7n, from y 150 to y 160 (1-pixel gaps between) */
static COLORREF want_tiles(int x, int y, void *c)
{
    (void)c;
    if (y >= 160 && y < 166 && x >= 8 && x < 8 + 16 * 7 && (x - 8) % 7 < 6)
        return pat(x, y - 10);
    return pat(x, y);
}

/* a copy through a clip region: two rectangles with a hole between them */
typedef struct { RECT a, b; int dx, dy; } clip_t;
static COLORREF want_clip(int x, int y, void *c)
{
    clip_t *k = (clip_t *)c;
    POINT p = { x, y };
    if (PtInRect(&k->a, p) || PtInRect(&k->b, p))
        return pat(x - k->dx, y - k->dy);
    return pat(x, y);
}

/* ---- text ------------------------------------------------------------------- */

static HDC g_ref, g_bg;                 /* the software reference, the background */
static HBITMAP g_refbm, g_bgbm, g_tmpbm;
static DWORD *g_px_got, *g_px_ref, *g_px_bg;

/* the display DLL's 2D counters: 1 = read (our driver), 0 = not answered */
static int stats2d(vcr_2d_stats *st)
{
    memset(st, 0, sizeof *st);
    GdiFlush();                         /* a batched ExtTextOut must reach the driver first */
    return ExtEscape(g_wdc, VCR_ESC_2D_STATS, 0, NULL, sizeof *st, (LPSTR)st) >= (int)sizeof *st &&
           st->size >= sizeof *st;
}

/* a DDB into 32-bit pixels: the bitmap is taken out of its DC first (GetDIBits
 * wants it selected nowhere) and put back */
static int grab(HDC dc, HBITMAP bm, DWORD *out)
{
    BITMAPINFO bi;
    HGDIOBJ prev;
    int r;
    memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = W;
    bi.bmiHeader.biHeight = -H;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    GdiFlush();
    prev = SelectObject(dc, g_tmpbm);
    r = GetDIBits(g_wdc, bm, 0, H, out, &bi, DIB_RGB_COLORS);
    SelectObject(dc, prev);
    return r == H;
}

enum { F_MSSANS, F_TAHOMA, F_TIMES_I, F_COURIER, F_ARIAL33B, F_ARIAL56, F_FIXEDSYS, F_TAHOMA_US,
       F_TAHOMA_DEF, F_VERDANA_AA, F_COUNT };
static HFONT g_font[F_COUNT];

static HFONT mkfont(const char *face, int h, int weight, int italic, int under, int strike,
                    int quality)
{
    LOGFONTA lf;
    memset(&lf, 0, sizeof lf);
    lf.lfHeight = h;
    lf.lfWeight = weight;
    lf.lfItalic = (BYTE)italic;
    lf.lfUnderline = (BYTE)under;
    lf.lfStrikeOut = (BYTE)strike;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfOutPrecision = OUT_DEFAULT_PRECIS;
    lf.lfClipPrecision = CLIP_DEFAULT_PRECIS;
    lf.lfQuality = (BYTE)quality;
    strncpy(lf.lfFaceName, face, LF_FACESIZE - 1);
    return CreateFontIndirectA(&lf);
}

static void make_fonts(void)
{
    g_font[F_MSSANS] = mkfont("MS Sans Serif", -11, 400, 0, 0, 0, DEFAULT_QUALITY);
    g_font[F_TAHOMA] = mkfont("Tahoma", -11, 400, 0, 0, 0, NONANTIALIASED_QUALITY);
    g_font[F_TIMES_I] = mkfont("Times New Roman", -17, 400, 1, 0, 0, NONANTIALIASED_QUALITY);
    g_font[F_COURIER] = mkfont("Courier New", -13, 400, 0, 0, 0, NONANTIALIASED_QUALITY);
    g_font[F_ARIAL33B] = mkfont("Arial", -33, 700, 0, 0, 0, NONANTIALIASED_QUALITY);
    g_font[F_ARIAL56] = mkfont("Arial", -56, 400, 0, 0, 0, NONANTIALIASED_QUALITY);
    g_font[F_FIXEDSYS] = mkfont("Fixedsys", -15, 400, 0, 0, 0, DEFAULT_QUALITY);
    g_font[F_TAHOMA_US] = mkfont("Tahoma", -13, 400, 0, 1, 1, NONANTIALIASED_QUALITY);
    g_font[F_TAHOMA_DEF] = mkfont("Tahoma", -13, 400, 0, 0, 0, DEFAULT_QUALITY);
    g_font[F_VERDANA_AA] = mkfont("Verdana", -15, 400, 0, 0, 0, ANTIALIASED_QUALITY);
}

enum { T_TRANSPARENT, T_OPAQUE, T_ETO_OPAQUE };
enum { C_NONE, C_ETO, C_REGION, C_INTERLEAVE, C_COMB };

typedef struct tcase {
    const char *name;
    int font, bk, clip, x, y;
    COLORREF fg, bg;
    int accel;              /* 1: 1 bpp glyphs, COPYPEN - the engine should draw it */
    RECT r;                 /* ETO_OPAQUE / ETO_CLIPPED rectangle */
    const char *text;
} tcase;

static const char k_fox[] = "The quick brown fox jumps over the lazy dog";
static const char k_digits[] = "0123456789 !\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~";
static const char k_wide[] = "WMWM@%";
static const char k_long[] =
    "abcdefghijklmnopqrstuvwxyz ABCDEFGHIJKLMNOPQRSTUVWXYZ 0123456789 "
    "abcdefghijklmnopqrstuvwxyz ABCDEFGHIJKLMNOPQRSTUVWXYZ 0123456789 "
    "abcdefghijklmnopqrstuvwxyz ABCDEFGHIJKLMNOPQRSTUVWXYZ 0123456789 ";

/* colours every depth shows exactly (16 bpp: multiples of 8, green of 4;
 * 8 bpp: the reserved system colours) */
#define FG1 RGB(0, 0, 0)
#define BG1 RGB(255, 255, 255)
#define FG2 RGB(128, 0, 0)
#define BG2 RGB(192, 192, 192)
#define FG3 RGB(255, 255, 0)
#define BG3 RGB(0, 0, 128)

static const tcase k_cases[] = {
    { "mssans transparent", F_MSSANS, T_TRANSPARENT, C_NONE, 4, 4, FG1, BG1, 1, {0}, k_fox },
    { "mssans opaque", F_MSSANS, T_OPAQUE, C_NONE, 5, 20, FG2, BG2, 1, {0}, k_digits },
    { "tahoma transparent", F_TAHOMA, T_TRANSPARENT, C_NONE, 6, 36, FG3, BG3, 1, {0}, k_fox },
    { "tahoma opaque odd x", F_TAHOMA, T_OPAQUE, C_NONE, 7, 50, FG1, BG1, 1, {0}, k_digits },
    { "tahoma ETO_OPAQUE wide rect", F_TAHOMA, T_ETO_OPAQUE, C_NONE, 13, 66, FG2, BG2, 1,
      { 3, 63, 250, 84 }, k_fox },
    { "times italic transparent (overlap)", F_TIMES_I, T_TRANSPARENT, C_NONE, 1, 86, FG3, BG3, 1,
      {0}, k_fox },
    { "times italic opaque", F_TIMES_I, T_OPAQUE, C_NONE, 2, 104, FG1, BG1, 1, {0}, k_digits },
    { "courier fixed transparent", F_COURIER, T_TRANSPARENT, C_NONE, 3, 124, FG2, BG2, 1, {0},
      k_fox },
    { "courier fixed opaque", F_COURIER, T_OPAQUE, C_NONE, 9, 140, FG3, BG3, 1, {0}, k_digits },
    { "fixedsys raster opaque", F_FIXEDSYS, T_OPAQUE, C_NONE, 2, 156, FG1, BG2, 1, {0}, k_fox },
    { "fixedsys raster transparent", F_FIXEDSYS, T_TRANSPARENT, C_NONE, 5, 172, FG2, BG1, 1, {0},
      k_digits },
    { "arial33 bold transparent", F_ARIAL33B, T_TRANSPARENT, C_NONE, 3, 10, FG1, BG1, 1, {0},
      k_wide },
    { "arial33 bold opaque", F_ARIAL33B, T_OPAQUE, C_NONE, 11, 60, FG3, BG3, 1, {0}, "Voodoo3!" },
    { "arial56 transparent (wide glyphs)", F_ARIAL56, T_TRANSPARENT, C_NONE, 1, 100, FG2, BG2, 1,
      {0}, k_wide },
    { "underline+strikeout transparent", F_TAHOMA_US, T_TRANSPARENT, C_NONE, 4, 20, FG1, BG1, 1,
      {0}, k_fox },
    { "underline+strikeout opaque", F_TAHOMA_US, T_OPAQUE, C_NONE, 6, 90, FG3, BG3, 1, {0},
      k_digits },
    { "ETO_CLIPPED through the glyphs", F_ARIAL33B, T_TRANSPARENT, C_ETO, 2, 20, FG2, BG2, 1,
      { 17, 29, 203, 47 }, k_wide },
    { "ETO_CLIPPED + ETO_OPAQUE", F_TAHOMA, T_ETO_OPAQUE, C_ETO, 3, 70, FG1, BG1, 1,
      { 20, 72, 111, 80 }, k_fox },
    { "region with holes (complex)", F_ARIAL33B, T_TRANSPARENT, C_REGION, 1, 40, FG3, BG3, 1,
      {0}, k_wide },
    { "region with holes, opaque", F_COURIER, T_OPAQUE, C_REGION, 2, 60, FG2, BG2, 1, {0},
      k_fox },
    { "region comb (40 rectangles)", F_ARIAL33B, T_OPAQUE, C_COMB, 4, 30, FG1, BG2, 1, {0},
      k_wide },
    { "cut by the left edge", F_ARIAL33B, T_OPAQUE, C_NONE, -13, 30, FG1, BG1, 1, {0}, k_wide },
    { "cut by the top edge", F_TIMES_I, T_TRANSPARENT, C_NONE, 10, -7, FG2, BG2, 1, {0}, k_fox },
    { "cut by the right edge", F_ARIAL56, T_OPAQUE, C_NONE, 170, 20, FG3, BG3, 1, {0}, k_wide },
    { "cut by the bottom edge", F_ARIAL33B, T_TRANSPARENT, C_NONE, 30, 175, FG1, BG1, 1, {0},
      k_wide },
    { "long string (STROBJ batches)", F_TAHOMA, T_OPAQUE, C_NONE, -40, 150, FG2, BG2, 1, {0},
      k_long },
    { "text, CPU pixels, text", F_ARIAL33B, T_TRANSPARENT, C_INTERLEAVE, 5, 50, FG3, BG3, 1, {0},
      k_wide },
    { "tahoma default quality", F_TAHOMA_DEF, T_TRANSPARENT, C_NONE, 3, 110, FG1, BG1, 0, {0},
      k_fox },
    { "verdana antialiased", F_VERDANA_AA, T_OPAQUE, C_NONE, 4, 130, FG2, BG2, 0, {0}, k_fox },
};
#define NCASES ((int)(sizeof k_cases / sizeof k_cases[0]))

static void text_draw(HDC dc, const tcase *c)
{
    HGDIOBJ oldf = SelectObject(dc, g_font[c->font]);
    HRGN rgn = NULL;
    UINT opts = 0;
    int n = (int)strlen(c->text);
    SetTextAlign(dc, TA_LEFT | TA_TOP | TA_NOUPDATECP);
    SetTextColor(dc, c->fg);
    SetBkColor(dc, c->bg);
    SetBkMode(dc, c->bk == T_TRANSPARENT ? TRANSPARENT : OPAQUE);
    if (c->bk == T_ETO_OPAQUE)
        opts |= ETO_OPAQUE;
    if (c->clip == C_ETO)
        opts |= ETO_CLIPPED;
    if (c->clip == C_REGION) {          /* three bands with holes between them */
        HRGN b;
        rgn = CreateRectRgn(0, 38, 60, 120);
        b = CreateRectRgn(70, 30, 71, 130);
        CombineRgn(rgn, rgn, b, RGN_OR);
        DeleteObject(b);
        b = CreateRectRgn(90, 45, 180, 62);
        CombineRgn(rgn, rgn, b, RGN_OR);
        DeleteObject(b);
        b = CreateRectRgn(185, 20, 256, 110);
        CombineRgn(rgn, rgn, b, RGN_OR);
        DeleteObject(b);
        SelectClipRgn(dc, rgn);
    }
    if (c->clip == C_COMB) {            /* 40 strips: more than one CLIPOBJ_bEnum batch */
        int i;
        rgn = CreateRectRgn(0, 0, 0, 0);
        for (i = 0; i < 40; i++) {
            HRGN b = CreateRectRgn(i * 6, 20 + (i % 3), i * 6 + 4, 80 - (i % 5));
            CombineRgn(rgn, rgn, b, RGN_OR);
            DeleteObject(b);
        }
        SelectClipRgn(dc, rgn);
    }
    ExtTextOutA(dc, c->x, c->y, opts, opts ? &c->r : NULL, c->text, n, NULL);
    if (c->clip == C_INTERLEAVE) {      /* the CPU on the engine's pixels, then the engine again */
        int i;
        for (i = 0; i < 64; i++)
            SetPixelV(dc, c->x + 3 + i * 3, c->y + 4 + (i % 20), RGB(0, 255, 0));
        SetTextColor(dc, c->bg);
        ExtTextOutA(dc, c->x + 2, c->y + 3, 0, NULL, c->text, n, NULL);
    }
    if (rgn) {
        SelectClipRgn(dc, NULL);
        DeleteObject(rgn);
    }
    SelectObject(dc, oldf);
}

typedef struct ttotal {
    int cases, bad, bad_cases, path_bad, calls, glyphs, clipped, blits, punts, have_stats, empty;
} ttotal;

static void text_case(const tcase *c, ttotal *t, int require)
{
    vcr_2d_stats a, b;
    int x, y, bad = 0, changed = 0, have, calls = 0, glyphs = 0, clipped = 0, punts = 0, blits = 0;
    BitBlt(g_wdc, 0, 0, W, H, g_bg, 0, 0, SRCCOPY);
    BitBlt(g_ref, 0, 0, W, H, g_bg, 0, 0, SRCCOPY);
    have = stats2d(&a);
    text_draw(g_wdc, c);
    have = stats2d(&b) && have;
    text_draw(g_ref, c);
    GdiFlush();
    BitBlt(g_mem, 0, 0, W, H, g_wdc, 0, 0, SRCCOPY);
    if (!grab(g_mem, g_bm, g_px_got) || !grab(g_ref, g_refbm, g_px_ref)) {
        say("  %s: GetDIBits failed", c->name);
        t->bad++;
        t->bad_cases++;
        return;
    }
    for (y = 0; y < H; y++)
        for (x = 0; x < W; x++) {
            DWORD g = g_px_got[y * W + x] & 0xffffff, r = g_px_ref[y * W + x] & 0xffffff;
            changed += r != (g_px_bg[y * W + x] & 0xffffff);
            if (g != r) {
                if (!bad)
                    say("  %s: first bad at (%d,%d) got %06lx want %06lx", c->name, x, y,
                        (unsigned long)g, (unsigned long)r);
                bad++;
            }
        }
    if (have) {
        calls = (int)(b.text_calls - a.text_calls);
        glyphs = (int)(b.text_glyphs - a.text_glyphs);
        clipped = (int)(b.text_clipped - a.text_clipped);
        punts = (int)(b.text_punts - a.text_punts);
        blits = (int)(b.text_blits - a.text_blits);
    }
    if (!changed) {                     /* a case that draws nothing proves nothing */
        say("  %s: the reference drew nothing", c->name);
        t->empty++;
    }
    say("text %-36s %5d bad  %5d px drawn  engine %s: %d call(s), %d glyph(s), %d clipped, "
        "%d blit(s), %d to software", c->name, bad, changed, have ? "counters" : "(none)", calls,
        glyphs, clipped, blits, punts);
    t->cases++;
    t->bad += bad;
    t->bad_cases += bad ? 1 : 0;
    t->calls += calls;
    t->glyphs += glyphs;
    t->clipped += clipped;
    t->blits += blits;
    t->punts += punts;
    t->have_stats |= have;
    if (require && c->accel && (!have || !calls || punts)) {
        say("  %s: expected on the engine (%s)", c->name, have ? "it went to software" :
            "no counters from the driver");
        t->path_bad++;
    }
}

static int text_setup(void)
{
    g_ref = CreateCompatibleDC(g_wdc);
    g_refbm = CreateCompatibleBitmap(g_wdc, W, H);
    g_bg = CreateCompatibleDC(g_wdc);
    g_bgbm = CreateCompatibleBitmap(g_wdc, W, H);
    g_tmpbm = CreateCompatibleBitmap(g_wdc, 1, 1);
    g_px_got = (DWORD *)malloc(W * H * 4);
    g_px_ref = (DWORD *)malloc(W * H * 4);
    g_px_bg = (DWORD *)malloc(W * H * 4);
    if (!g_ref || !g_refbm || !g_bg || !g_bgbm || !g_tmpbm || !g_px_got || !g_px_ref || !g_px_bg)
        return 0;
    SelectObject(g_ref, g_refbm);
    SelectObject(g_bg, g_bgbm);
    draw_pattern_on(g_bg);              /* the background, drawn once in software */
    make_fonts();
    return grab(g_bg, g_bgbm, g_px_bg);
}

/* ---- patterns and lines (2026-09-27) ------------------------------------------
 * Each case draws the same thing on the screen (our driver) and into the
 * software reference, and the two frames must agree pixel for pixel. The
 * driver's counters say whether the engine drew it: the cases marked `accel`
 * must reach the engine when the switch is on; the others (slanted, styled,
 * XOR) must go to GDI. */
typedef void (*pl_draw_fn)(HDC dc);
typedef struct plcase { const char *name; pl_draw_fn draw; int kind; int accel; } plcase;
enum { PL_PAT = 1, PL_LINE = 2 };

static HBITMAP g_grey_bm;               /* 8x8 1 bpp 50 % grey: every drag rectangle */

static void pl_hatches(HDC dc, int bk_transparent)
{
    int i;
    SetBkMode(dc, bk_transparent ? TRANSPARENT : OPAQUE);
    SetBkColor(dc, BG2);
    for (i = 0; i < 6; i++) {
        HBRUSH b = CreateHatchBrush(i, i & 1 ? FG2 : FG3);
        HGDIOBJ o = SelectObject(dc, b);
        PatBlt(dc, 6 + i * 41, 8 + (i & 1) * 3, 37, 70 + i * 5, PATCOPY);
        SelectObject(dc, o);
        DeleteObject(b);
    }
}
static void pl_hatch_opaque(HDC dc) { pl_hatches(dc, 0); }
static void pl_hatch_transparent(HDC dc) { pl_hatches(dc, 1); }

static void pl_grey(HDC dc, DWORD rop, int ox, int oy)
{
    HBRUSH b = CreatePatternBrush(g_grey_bm);
    HGDIOBJ o;
    POINT prev;
    SetTextColor(dc, FG1);
    SetBkColor(dc, BG1);
    SetBrushOrgEx(dc, ox, oy, &prev);
    o = SelectObject(dc, b);
    PatBlt(dc, 13, 21, 201, 3, rop);            /* the drag-frame strips */
    PatBlt(dc, 13, 24, 3, 150, rop);
    PatBlt(dc, 211, 24, 3, 150, rop);
    PatBlt(dc, 13, 171, 201, 3, rop);
    PatBlt(dc, 40, 60, 97, 51, rop);            /* and a block at odd x */
    SelectObject(dc, o);
    SetBrushOrgEx(dc, prev.x, prev.y, NULL);
    DeleteObject(b);
}
static void pl_grey_copy(HDC dc) { pl_grey(dc, PATCOPY, 0, 0); }
static void pl_grey_invert(HDC dc) { pl_grey(dc, PATINVERT, 0, 0); }
static void pl_grey_origin(HDC dc) { pl_grey(dc, PATCOPY, 3, 5); }
static void pl_grey_origin_invert(HDC dc) { pl_grey(dc, PATINVERT, 7, 1); }

static void pl_hatch_clipped(HDC dc)
{
    HRGN r = CreateRectRgn(0, 30, 70, 150), b = CreateRectRgn(90, 10, 150, 60);
    CombineRgn(r, r, b, RGN_OR);
    DeleteObject(b);
    b = CreateRectRgn(160, 80, 250, 200);
    CombineRgn(r, r, b, RGN_OR);
    DeleteObject(b);
    SelectClipRgn(dc, r);
    pl_hatches(dc, 0);
    SelectClipRgn(dc, NULL);
    DeleteObject(r);
}

static void pl_pen(HDC dc, int style, COLORREF c, int rop2, void (*body)(HDC))
{
    HPEN pen = CreatePen(style, 0, c);
    HGDIOBJ o = SelectObject(dc, pen);
    int prev = SetROP2(dc, rop2);
    body(dc);
    SetROP2(dc, prev);
    SelectObject(dc, o);
    DeleteObject(pen);
}
static void body_hv(HDC dc)
{
    int i;
    for (i = 0; i < 20; i++) {
        MoveToEx(dc, 5 + i * 3, 10 + i * 7, NULL);
        LineTo(dc, 240 - i * 5, 10 + i * 7);        /* left to right */
        MoveToEx(dc, 250 - i * 2, 14 + i * 7, NULL);
        LineTo(dc, 9 + i, 14 + i * 7);              /* right to left */
        MoveToEx(dc, 20 + i * 11, 160, NULL);
        LineTo(dc, 20 + i * 11, 190 - i);           /* up */
        MoveToEx(dc, 24 + i * 11, 150 - i * 2, NULL);
        LineTo(dc, 24 + i * 11, 200);               /* down */
    }
    MoveToEx(dc, 100, 100, NULL);
    LineTo(dc, 100, 100);                           /* zero length: nothing */
}
static void body_rects(HDC dc)
{
    int i;
    HGDIOBJ ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
    for (i = 0; i < 12; i++)
        Rectangle(dc, 4 + i * 9, 6 + i * 7, 250 - i * 11, 210 - i * 5);
    SelectObject(dc, ob);
}
static void body_poly(HDC dc)
{
    POINT pt[24];
    int i;
    for (i = 0; i < 24; i++) {
        pt[i].x = 8 + (i / 2) * 20;
        pt[i].y = (i & 1) ? 30 + (i % 4) * 30 : 30 + ((i + 1) % 4) * 30;
        if (i & 1)
            pt[i].x += 20;
    }
    for (i = 1; i < 24; i++)                        /* a staircase: every step axis-aligned */
        if (i & 1) pt[i].y = pt[i - 1].y; else pt[i].x = pt[i - 1].x;
    Polyline(dc, pt, 24);
}
static void body_diag(HDC dc)
{
    int i;
    for (i = 0; i < 16; i++) {
        MoveToEx(dc, 5 + i * 7, 5, NULL);
        LineTo(dc, 120 + i * 9, 200 - i * 3);
    }
}
static void pl_lines(HDC dc) { pl_pen(dc, PS_SOLID, FG2, R2_COPYPEN, body_hv); }
static void pl_rects(HDC dc) { pl_pen(dc, PS_SOLID, FG3, R2_COPYPEN, body_rects); }
static void pl_poly(HDC dc) { pl_pen(dc, PS_SOLID, FG1, R2_COPYPEN, body_poly); }
static void pl_lines_clipped(HDC dc)
{
    HRGN r = CreateRectRgn(0, 0, 90, 120), b = CreateRectRgn(120, 40, 256, 180);
    CombineRgn(r, r, b, RGN_OR);
    DeleteObject(b);
    SelectClipRgn(dc, r);
    pl_pen(dc, PS_SOLID, FG2, R2_COPYPEN, body_hv);
    pl_pen(dc, PS_SOLID, FG3, R2_COPYPEN, body_rects);
    SelectClipRgn(dc, NULL);
    DeleteObject(r);
}
static void pl_diag(HDC dc) { pl_pen(dc, PS_SOLID, FG2, R2_COPYPEN, body_diag); }
static void pl_dotted(HDC dc) { pl_pen(dc, PS_DOT, FG1, R2_COPYPEN, body_rects); }
static void pl_xor(HDC dc) { pl_pen(dc, PS_SOLID, RGB(255, 255, 255), R2_XORPEN, body_hv); }

static const plcase k_pl[] = {
    { "hatches opaque (6 styles)", pl_hatch_opaque, PL_PAT, 1 },
    { "hatches transparent", pl_hatch_transparent, PL_PAT, 1 },   /* ROP4 0xAAF0 */
    { "hatches through a region", pl_hatch_clipped, PL_PAT, 1 },
    { "grey PATCOPY", pl_grey_copy, PL_PAT, 1 },
    { "grey PATINVERT (drag frame)", pl_grey_invert, PL_PAT, 1 },
    { "grey PATCOPY, brush origin 3,5", pl_grey_origin, PL_PAT, 1 },
    { "grey PATINVERT, brush origin 7,1", pl_grey_origin_invert, PL_PAT, 1 },
    { "lines h/v both directions", pl_lines, PL_LINE, 1 },
    { "rectangle outlines (StrokePath)", pl_rects, PL_LINE, 1 },
    { "polyline staircase", pl_poly, PL_LINE, 1 },
    { "lines + outlines through a region", pl_lines_clipped, PL_LINE, 1 },
    { "slanted lines (GDI's)", pl_diag, PL_LINE, 0 },
    { "dotted pen (GDI's)", pl_dotted, PL_LINE, 0 },
    { "XOR pen (GDI's)", pl_xor, PL_LINE, 0 },
};
#define NPL ((int)(sizeof k_pl / sizeof k_pl[0]))

typedef struct pltotal { int cases, bad, bad_cases, path_bad, pat, line, have; } pltotal;

static void pl_case(const plcase *c, pltotal *t)
{
    vcr_2d_stats a, b;
    int x, y, bad = 0, changed = 0, have, pat = 0, line = 0, eng;
    BitBlt(g_wdc, 0, 0, W, H, g_bg, 0, 0, SRCCOPY);
    BitBlt(g_ref, 0, 0, W, H, g_bg, 0, 0, SRCCOPY);
    have = stats2d(&a);
    c->draw(g_wdc);
    have = stats2d(&b) && have;
    c->draw(g_ref);
    GdiFlush();
    BitBlt(g_mem, 0, 0, W, H, g_wdc, 0, 0, SRCCOPY);
    if (!grab(g_mem, g_bm, g_px_got) || !grab(g_ref, g_refbm, g_px_ref)) {
        say("  %s: GetDIBits failed", c->name);
        t->bad++;
        t->bad_cases++;
        return;
    }
    for (y = 0; y < H; y++)
        for (x = 0; x < W; x++) {
            DWORD g = g_px_got[y * W + x] & 0xffffff, r = g_px_ref[y * W + x] & 0xffffff;
            changed += r != (g_px_bg[y * W + x] & 0xffffff);
            if (g != r) {
                if (!bad)
                    say("  %s: first bad at (%d,%d) got %06lx want %06lx", c->name, x, y,
                        (unsigned long)g, (unsigned long)r);
                bad++;
            }
        }
    if (have) {
        pat = (int)(b.pat_fills - a.pat_fills);
        line = (int)(b.line_fills - a.line_fills);
    }
    eng = c->kind == PL_PAT ? pat : line;
    /* the switch on: an accel case must reach the engine, a GDI case must not */
    if (have && (b.flags & (c->kind == PL_PAT ? VCR_2DS_F_PAT : VCR_2DS_F_LINE))) {
        if ((c->accel && !eng) || (!c->accel && c->kind == PL_LINE && eng)) {
            say("  %s: expected %s", c->name, c->accel ? "on the engine" : "in software");
            t->path_bad++;
        }
    }
    say("patline %-36s %5d bad  %5d px drawn  engine: %d pattern fill(s), %d line call(s)%s",
        c->name, bad, changed, pat, line, changed ? "" : "  (the reference drew nothing!)");
    t->cases++;
    t->bad += bad;
    t->bad_cases += bad ? 1 : 0;
    t->pat += pat;
    t->line += line;
    t->have |= have;
}

static int pl_setup(void)
{
    static const BYTE grey[16] = { 0xaa, 0, 0x55, 0, 0xaa, 0, 0x55, 0, 0xaa, 0, 0x55, 0,
                                   0xaa, 0, 0x55, 0 };
    g_grey_bm = CreateBitmap(8, 8, 1, 1, grey);
    return g_grey_bm != NULL;
}

/* ---- text throughput -------------------------------------------------------- */

typedef struct bcase {
    const char *name;
    int font, bk, lh;
    const char *text;
} bcase;

static const bcase k_bench[] = {
    { "tahoma8 transparent", F_TAHOMA, T_TRANSPARENT, 13, "The quick brown fox jumps over the lazy" },
    { "tahoma8 opaque", F_TAHOMA, T_OPAQUE, 13, "The quick brown fox jumps over the lazy" },
    { "mssans8 opaque (raster)", F_MSSANS, T_OPAQUE, 13, "The quick brown fox jumps over the lazy" },
    { "courier13 opaque (fixed)", F_COURIER, T_OPAQUE, 15, "Courier New, fixed pitch: 30 ch" },
    { "arial33b transparent (large)", F_ARIAL33B, T_TRANSPARENT, 36, "Voodoo3 2D!" },
};
#define NBENCH ((int)(sizeof k_bench / sizeof k_bench[0]))

static char g_benchjson[2048];

static void text_bench(int ms)
{
    LARGE_INTEGER f, t0, t1;
    int b;
    char *o = g_benchjson;
    size_t left = sizeof g_benchjson;
    QueryPerformanceFrequency(&f);
    *o = 0;
    for (b = 0; b < NBENCH; b++) {
        const bcase *c = &k_bench[b];
        vcr_2d_stats s0, s1;
        int n = (int)strlen(c->text), have, calls = 0, row = 0, rows = H / c->lh, k;
        double el, gps;
        HGDIOBJ oldf;
        BitBlt(g_wdc, 0, 0, W, H, g_bg, 0, 0, SRCCOPY);
        oldf = SelectObject(g_wdc, g_font[c->font]);
        SetTextAlign(g_wdc, TA_LEFT | TA_TOP | TA_NOUPDATECP);
        SetTextColor(g_wdc, FG2);
        SetBkColor(g_wdc, BG2);
        SetBkMode(g_wdc, c->bk == T_TRANSPARENT ? TRANSPARENT : OPAQUE);
        have = stats2d(&s0);
        QueryPerformanceCounter(&t0);
        do {
            for (k = 0; k < 16; k++, calls++) {
                ExtTextOutA(g_wdc, 2 + (calls & 3), (row % rows) * c->lh, 0, NULL, c->text, n,
                            NULL);
                row++;
            }
            QueryPerformanceCounter(&t1);
        } while ((t1.QuadPart - t0.QuadPart) * 1000 / f.QuadPart < ms);
        GdiFlush();
        GetPixel(g_wdc, 1, 1);          /* read back: waits for the engine to finish */
        QueryPerformanceCounter(&t1);
        have = stats2d(&s1) && have;
        SelectObject(g_wdc, oldf);
        el = (double)(t1.QuadPart - t0.QuadPart) / (double)f.QuadPart;
        gps = el > 0 ? (double)calls * n / el : 0;
        say("bench %-30s %8.0f glyphs/s  (%d calls x %d, %.0f ms; engine glyphs %s%d, blits %d, "
            "FIFO waits %d)", c->name, gps, calls, n, el * 1000, have ? "" : "n/a ",
            have ? (int)(s1.text_glyphs - s0.text_glyphs) : 0,
            have ? (int)(s1.text_blits - s0.text_blits) : 0,
            have ? (int)(s1.text_fifo_waits - s0.text_fifo_waits) : 0);
        if (left > 200) {
            int w = _snprintf(o, left, "%s{\"name\":\"%s\",\"glyphs_s\":%.0f,\"calls\":%d,"
                              "\"chars\":%d,\"ms\":%.0f,\"engine_glyphs\":%d,\"fifo_waits\":%d}",
                              b ? "," : "", c->name, gps, calls, n, el * 1000,
                              have ? (int)(s1.text_glyphs - s0.text_glyphs) : -1,
                              have ? (int)(s1.text_fifo_waits - s0.text_fifo_waits) : -1);
            if (w > 0) {
                o += w;
                left -= (size_t)w;
            }
        }
        pump();
    }
}

int main(int argc, char **argv)
{
    WNDCLASSA wc;
    HWND wnd;
    int i, rounds = 3, r, total = 0, fills = 0, rops = 0, copies = 0, scrolls = 0, clips = 0,
        sync = 0, first = 0, bench_ms = 2000, require = 0, do_base, do_text, do_bench, tsetup = 1,
        do_pl = 0;
    pltotal pt;
    const char *tests = "base,text";
    vcr_2d_stats st;
    ttotal tt;
    memset(&tt, 0, sizeof tt);
    memset(&pt, 0, sizeof pt);
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--log") && i + 1 < argc)
            strncpy(g_logpath, argv[++i], sizeof g_logpath - 1);
        else if (!strcmp(argv[i], "--rounds") && i + 1 < argc)
            rounds = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--tests") && i + 1 < argc)
            tests = argv[++i];
        else if (!strcmp(argv[i], "--bench-ms") && i + 1 < argc)
            bench_ms = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--require-text-accel"))
            require = 1;
    }
    do_base = strstr(tests, "base") != NULL;
    do_text = strstr(tests, "text") != NULL;
    do_bench = strstr(tests, "bench") != NULL;
    do_pl = strstr(tests, "patline") != NULL;
    if (bench_ms < 100)
        bench_ms = 100;
    if (bench_ms > 20000)
        bench_ms = 20000;
    g_log = fopen(g_logpath, "w");
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "gdilab";
    RegisterClassA(&wc);
    wnd = CreateWindowExA(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, "gdilab", "gdilab",
                          WS_POPUP | WS_VISIBLE, 32, 32, W, H, NULL, NULL, wc.hInstance, NULL);
    SetWindowPos(wnd, HWND_TOPMOST, 32, 32, W, H, SWP_SHOWWINDOW);
    pump();
    Sleep(300);
    pump();
    g_wdc = GetDC(wnd);
    g_bpp = GetDeviceCaps(g_wdc, BITSPIXEL);
    g_mem = CreateCompatibleDC(g_wdc);
    g_bm = CreateCompatibleBitmap(g_wdc, W, H);
    SelectObject(g_mem, g_bm);
    say("gdilab: %d bpp, %dx%d window, %d rounds, tests %s", g_bpp, W, H, rounds, tests);
    if (stats2d(&st))
        say("display driver 2D counters: engine %s, text path %s (%u bpp)",
            st.flags & VCR_2DS_F_ENGINE ? "on" : "off", st.flags & VCR_2DS_F_TEXT ? "on" : "off",
            st.bpp);
    else
        say("display driver 2D counters: not answered (not our driver)");
    if ((do_text || do_bench || do_pl) && !(tsetup = text_setup() && (!do_pl || pl_setup())))
        say("text setup failed");

    for (r = 0; r < rounds && do_base; r++) {
        static const int sd[4][2] = { { 0, -8 }, { 0, 8 }, { -8, 0 }, { 8, 0 } };
        move_t m;
        fill_t f;
        clip_t k;
        HRGN rgn;
        int s;

        draw_pattern();
        fills += check("pattern (solid fills)", want_pat, NULL, &first);

        /* BLACKNESS / WHITENESS */
        SetRect(&f.r, 40, 24, 104, 72);
        PatBlt(g_wdc, f.r.left, f.r.top, 64, 48, BLACKNESS);
        f.c = RGB(0, 0, 0);
        rops += check("PatBlt BLACKNESS", want_fill, &f, &first);
        PatBlt(g_wdc, f.r.left, f.r.top, 64, 48, WHITENESS);
        f.c = RGB(255, 255, 255);
        rops += check("PatBlt WHITENESS", want_fill, &f, &first);

        /* a plain screen-to-screen copy, no overlap */
        draw_pattern();
        BitBlt(g_wdc, 160, 100, 64, 48, g_wdc, 16, 8, SRCCOPY);
        SetRect(&m.r, 160, 100, 224, 148);
        m.dx = 160 - 16;
        m.dy = 100 - 8;
        copies += check("BitBlt copy", want_move, &m, &first);

        /* odd positions and sizes, one copy and a row of small ones */
        draw_pattern();
        BitBlt(g_wdc, 13, 101, 37, 11, g_wdc, 3, 5, SRCCOPY);
        SetRect(&m.r, 13, 101, 50, 112);
        m.dx = 10;
        m.dy = 96;
        copies += check("BitBlt copy, odd x/size", want_move, &m, &first);
        draw_pattern();
        {
            int n;
            for (n = 0; n < 16; n++)
                BitBlt(g_wdc, 8 + n * 7, 160, 6, 6, g_wdc, 8 + n * 7, 150, SRCCOPY);
            copies += check("16 small copies, odd x", want_tiles, NULL, &first);
        }
        /* overlapping scrolls, all four directions (the direction bits) */
        for (s = 0; s < 4; s++) {
            int dx = sd[s][0], dy = sd[s][1];
            char name[48];
            draw_pattern();
            BitBlt(g_wdc, 16 + dx, 16 + dy, 160, 120, g_wdc, 16, 16, SRCCOPY);
            SetRect(&m.r, 16 + dx, 16 + dy, 176 + dx, 136 + dy);
            m.dx = dx;
            m.dy = dy;
            _snprintf(name, sizeof name, "scroll (%d,%d)", dx, dy);
            scrolls += check(name, want_move, &m, &first);
        }

        /* a copy through a clip region with a hole */
        draw_pattern();
        SetRect(&k.a, 100, 20, 160, 60);
        SetRect(&k.b, 100, 90, 160, 130);
        rgn = CreateRectRgnIndirect(&k.a);
        {
            HRGN b2 = CreateRectRgnIndirect(&k.b);
            CombineRgn(rgn, rgn, b2, RGN_OR);
            DeleteObject(b2);
        }
        SelectClipRgn(g_wdc, rgn);
        BitBlt(g_wdc, 100, 20, 60, 110, g_wdc, 104, 28, SRCCOPY);  /* overlapping, up-left */
        SelectClipRgn(g_wdc, NULL);
        DeleteObject(rgn);
        k.dx = -4;
        k.dy = -8;
        clips += check("clipped overlapping copy", want_clip, &k, &first);

        /* engine and CPU interleaved on the same pixels: fill (engine), a CPU
         * pixel on top, copy (engine) that reads it, CPU pixel again - any
         * missing sync reorders them */
        draw_pattern();
        {
            int n, bad = 0;
            for (n = 0; n < 32; n++) {
                RECT rr;
                HBRUSH b = CreateSolidBrush(RGB(0, 128, 248));
                COLORREF got;
                SetRect(&rr, 8 + n * 7, 150, 8 + n * 7 + 6, 156);
                FillRect(g_wdc, &rr, b);
                DeleteObject(b);
                SetPixelV(g_wdc, 8 + n * 7 + 2, 152, RGB(248, 0, 0));
                BitBlt(g_wdc, 8 + n * 7, 160, 6, 6, g_wdc, 8 + n * 7, 150, SRCCOPY);
                GdiFlush();
                got = GetPixel(g_wdc, 8 + n * 7 + 2, 162);
                if (!same(got, RGB(248, 0, 0)) && bad++ < 4)
                    say("  interleave %d: copied CPU pixel got %06lx (source now %06lx)", n,
                        (unsigned long)got, (unsigned long)GetPixel(g_wdc, 8 + n * 7 + 2, 152));
                got = GetPixel(g_wdc, 8 + n * 7 + 4, 164);
                if (!same(got, RGB(0, 128, 248)) && bad++ < 4)
                    say("  interleave %d: copied fill got %06lx (source now %06lx)", n,
                        (unsigned long)got, (unsigned long)GetPixel(g_wdc, 8 + n * 7 + 4, 154));
            }
            say("engine/CPU interleave: %d bad", bad);
            sync += bad;
        }
        pump();
    }
    for (r = 0; r < rounds && do_text && tsetup; r++) {
        int c;
        for (c = 0; c < NCASES; c++)
            text_case(&k_cases[c], &tt, require);
        pump();
    }
    for (r = 0; r < rounds && do_pl && tsetup; r++) {
        int c;
        for (c = 0; c < NPL; c++)
            pl_case(&k_pl[c], &pt);
        pump();
    }
    if (do_bench && tsetup)
        text_bench(bench_ms);
    if ((do_text || do_bench || do_pl) && !tsetup)
        tt.bad++;                       /* a text run that could not start is not a pass */
    total = fills + rops + copies + scrolls + clips + sync + tt.bad + tt.path_bad + tt.empty +
            pt.bad + pt.path_bad;
    say("RESULT {\"mode\":\"gdi\",\"bpp\":%d,\"rounds\":%d,\"tests\":\"%s\",\"bad_fill\":%d,"
        "\"bad_rop\":%d,\"bad_copy\":%d,\"bad_scroll\":%d,\"bad_clip\":%d,\"bad_sync\":%d,"
        "\"bad_text\":%d,\"bad_text_cases\":%d,\"bad_text_path\":%d,\"bad_text_empty\":%d,"
        "\"text_cases\":%d,\"text_stats\":%s,\"text_engine_calls\":%d,"
        "\"text_engine_glyphs\":%d,\"text_clipped\":%d,\"text_blits\":%d,\"text_software\":%d,"
        "\"patline_cases\":%d,\"bad_patline\":%d,\"bad_patline_cases\":%d,"
        "\"bad_patline_path\":%d,\"pat_engine_fills\":%d,\"line_engine_calls\":%d,"
        "\"bench\":[%s],\"bad\":%d}",
        g_bpp, rounds, tests, fills, rops, copies, scrolls, clips, sync, tt.bad, tt.bad_cases,
        tt.path_bad, tt.empty, tt.cases, tt.have_stats ? "true" : "false", tt.calls, tt.glyphs,
        tt.clipped, tt.blits, tt.punts, pt.cases, pt.bad, pt.bad_cases, pt.path_bad, pt.pat, pt.line,
        g_benchjson, total);
    DeleteDC(g_mem);
    DeleteObject(g_bm);
    ReleaseDC(wnd, g_wdc);
    DestroyWindow(wnd);
    return total ? 1 : 0;
}
