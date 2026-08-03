/* test_fx_cursor_overlay.c
 *
 * Guards the 0.1.35 fullscreen-cursor fix in MY open-source ICD — the MesaFX
 * fork retro3dfx-gl (github voidsstr/retro3dfx-gl), file
 *   src/mesa/drivers/glide/fxapi.c  ->  fxDrawCursorOverlay()
 * deployed as game-local opengl32.dll / system32 retrogl.dll on .124.
 *
 * The bug: fullscreen grSstWinOpen scans out from Glide's buffers, so the
 * GDI/hardware cursor plane never appears — CS 1.6's OpenGL menu had an
 * invisible (but live) mouse pointer, while D3D mode (GDI-managed primary)
 * showed it. The fix stamps a classic arrow into the Glide back buffer via
 * LFB right before grBufferSwap whenever the OS cursor is showing. Verified
 * on .124 via FX_DUMP_FRONT front-buffer capture (arrow visible at the
 * clicked coordinates in the CS menu).
 *
 * This test mirrors the arrow bitmap + the stamp/clip loop exactly (565
 * path) against a simulated framebuffer and asserts: outline/fill colors
 * land where the bitmap says, transparent pixels never write (the old-buggy
 * alternative — a filled rectangle or unclipped write — would), and edge
 * clipping stays in bounds at all four corners.
 */
#include "munit.h"
#include <string.h>

/* EXACT mirror of fxapi.c fxCursorArrow[] (19 rows, 11 cols max). */
static const char *fxCursorArrow[] = {
    "X..........",
    "XX.........",
    "XoX........",
    "XooX.......",
    "XoooX......",
    "XooooX.....",
    "XoooooX....",
    "XooooooX...",
    "XoooooooX..",
    "XooooooooX.",
    "XoooooXXXXX",
    "XooXooX....",
    "XoX.XooX...",
    "XX..XooX...",
    "X....XooX..",
    ".....XooX..",
    "......XooX.",
    "......XooX.",
    ".......XX..",
};
#define ROWS ((int)(sizeof(fxCursorArrow) / sizeof(fxCursorArrow[0])))

#define W 64
#define H 48
#define BG 0x1234  /* sentinel background */

/* EXACT mirror of the 565 stamp/clip loop in fxDrawCursorOverlay(). */
static void stamp(unsigned short *fb, int cx, int cy) {
    int row, col;
    for (row = 0; row < ROWS; row++) {
        int y = cy + row;
        if (y < 0 || y >= H)
            continue;
        for (col = 0; fxCursorArrow[row][col]; col++) {
            char p = fxCursorArrow[row][col];
            int x = cx + col;
            if (p == '.' || x < 0 || x >= W)
                continue;
            fb[y * W + x] = (p == 'X') ? 0x0000 : 0xFFFF;
        }
    }
}

static void reset(unsigned short *fb) {
    int i;
    for (i = 0; i < W * H; i++)
        fb[i] = BG;
}

TEST(outline_fill_and_transparency_land_exactly) {
    unsigned short fb[W * H];
    reset(fb);
    stamp(fb, 10, 10);
    CHECK_EQ_U(fb[10 * W + 10], 0x0000);  /* row0 col0 'X' = black outline */
    CHECK_EQ_U(fb[12 * W + 11], 0xFFFF);  /* row2 col1 'o' = white fill */
    CHECK_EQ_U(fb[10 * W + 11], BG);      /* row0 col1 '.' untouched */
    CHECK_EQ_U(fb[9 * W + 10], BG);       /* above hotspot untouched */
    /* the old-buggy alternative (no transparency) would have written row0
     * col1..10 — all must still be background */
    {
        int c;
        for (c = 1; c <= 10; c++)
            CHECK_EQ_U(fb[10 * W + 10 + c], BG);
    }
}

TEST(clips_at_all_four_screen_edges_without_wrap) {
    unsigned short fb[W * H];
    int x, y;
    /* bottom-right: most of the arrow is off-screen */
    reset(fb);
    stamp(fb, W - 3, H - 3);
    CHECK_EQ_U(fb[(H - 3) * W + (W - 3)], 0x0000);
    for (y = 0; y < H; y++)   /* column 0 must never be touched (no wrap) */
        CHECK_EQ_U(fb[y * W + 0], BG);
    /* top-left: negative origin */
    reset(fb);
    stamp(fb, -5, -5);
    for (y = 0; y < H; y++)
        for (x = 0; x < W; x++)
            if (fb[y * W + x] != BG) {
                CHECK(x <= 6 && y <= ROWS - 5,
                      "clipped stamp writes only inside the visible remnant");
            }
    /* fully off-screen: nothing written */
    reset(fb);
    stamp(fb, -20, -30);
    stamp(fb, W + 1, H + 1);
    for (y = 0; y < H; y++)
        for (x = 0; x < W; x++)
            CHECK(fb[y * W + x] == BG, "fully off-screen stamp writes nothing");
}

TEST(arrow_bitmap_shape_is_the_shipped_one) {
    int row;
    /* every row fits the 11-col pattern and uses only the 3 legal glyphs */
    for (row = 0; row < ROWS; row++) {
        const char *r = fxCursorArrow[row];
        int col;
        CHECK(strlen(r) == 11, "row is exactly 11 columns");
        for (col = 0; r[col]; col++)
            CHECK(r[col] == 'X' || r[col] == 'o' || r[col] == '.',
                  "only outline/fill/transparent glyphs");
    }
    CHECK_EQ_U(ROWS, 19);
    /* hotspot pixel (0,0) is outline — the click point matches the tip */
    CHECK(fxCursorArrow[0][0] == 'X', "arrow tip at the hotspot");
}

MUNIT_MAIN("MesaFX fxDrawCursorOverlay stamp/clip (cursor fix 0.1.35)", {
    RUN(outline_fill_and_transparency_land_exactly);
    RUN(clips_at_all_four_screen_edges_without_wrap);
    RUN(arrow_bitmap_shape_is_the_shipped_one);
})
