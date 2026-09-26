/*
 * gdilab.c - a self-checking GDI test program for the display driver's 2D
 * paths: what the 2D engine accelerates (solid fills, BLACKNESS/WHITENESS,
 * screen-to-screen copies, overlapping scrolls, copies through a clip region)
 * and the ordering between the engine and the CPU (the punt layer draws with
 * the CPU into the same memory the engine writes).
 *
 *   gdilab [--log path] [--rounds N]
 *
 * Draws into its own topmost popup (a bare screen DC is repainted by Explorer
 * under the test), reads every result back through a memory DC, and compares
 * with what it computed. The pattern is a per-pixel function of (x, y), so a
 * copy that lands one pixel off, a row that is skipped, a scroll that smears
 * or a clip rectangle that is ignored is counted, not guessed at.
 *
 * Every step is flushed to the log before the next. Final line: RESULT {json}.
 */
#include <windows.h>
#include <io.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static void draw_pattern(void)
{
    int x, y;
    for (y = 0; y < H; y += CELL)
        for (x = 0; x < W; x += CELL) {
            HBRUSH b = CreateSolidBrush(pat(x, y));
            RECT r;
            SetRect(&r, x, y, x + CELL, y + CELL);
            FillRect(g_wdc, &r, b);
            DeleteObject(b);
        }
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

int main(int argc, char **argv)
{
    WNDCLASSA wc;
    HWND wnd;
    int i, rounds = 3, r, total = 0, fills = 0, rops = 0, copies = 0, scrolls = 0, clips = 0,
        sync = 0, first = 0;
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--log") && i + 1 < argc)
            strncpy(g_logpath, argv[++i], sizeof g_logpath - 1);
        else if (!strcmp(argv[i], "--rounds") && i + 1 < argc)
            rounds = atoi(argv[++i]);
    }
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
    say("gdilab: %d bpp, %dx%d window, %d rounds", g_bpp, W, H, rounds);

    for (r = 0; r < rounds; r++) {
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
    total = fills + rops + copies + scrolls + clips + sync;
    say("RESULT {\"mode\":\"gdi\",\"bpp\":%d,\"rounds\":%d,\"bad_fill\":%d,\"bad_rop\":%d,"
        "\"bad_copy\":%d,\"bad_scroll\":%d,\"bad_clip\":%d,\"bad_sync\":%d,\"bad\":%d}",
        g_bpp, rounds, fills, rops, copies, scrolls, clips, sync, total);
    DeleteDC(g_mem);
    DeleteObject(g_bm);
    ReleaseDC(wnd, g_wdc);
    DestroyWindow(wnd);
    return total ? 1 : 0;
}
