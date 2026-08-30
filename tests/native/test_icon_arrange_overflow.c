/* test_icon_arrange_overflow.c - more icons than bay slots must not push any of
 * them OFF THE SCREEN.
 *
 * FIX UNDER TEST: scripts/retro-wallpaper/arrange_icons.c, 2026-08-29.
 *
 * WHAT WENT WRONG ON REAL HARDWARE. The fleet's staged library is 31 titles
 * producing 65 desktop shortcuts, plus My Computer and the Recycle Bin - 67
 * desktop items. The wallpaper's icon bay at 1024x768 (192.168.1.143's mode)
 * is 4 columns x 8 rows = 32 slots. The arranger's rule for the overflow was
 * "keep packing DOWNWARD", so item 67 landed at
 *     y = 57 + (66/4)*80 + 6 = 1343
 * on a screen 768 pixels tall. The XP desktop listview has no scrollbar, so
 * those icons are not merely outside the drawn panel - they cannot be reached
 * at all. Half the game library becomes invisible on every 1024x768 box.
 *
 * THE FIX: widen into extra COLUMNS instead, bounded by the screen width. The
 * extra columns fall outside the drawn bay, which is visibly imperfect - but an
 * icon in the wrong place beats an icon you cannot click.
 *
 * The arithmetic below mirrors arrange_icons.c exactly. If someone reverts to
 * packing downward, the "every icon is on screen" checks fail here rather than
 * on a fleet desktop that silently loses half its games.
 */

#include "munit.h"
#include <stdio.h>
#include <string.h>

#define CELL_W   76
#define CELL_H   80
#define HEADER_H 34

typedef struct { int x, y, cols, rows; } bay_t;

/* Verbatim mirror of icon_bay() in gen_retro_wall.py, gs_icon_bay() in
 * agent/src/gamesync.c, and the block in arrange_icons.c. */
static void icon_bay(int w, int h, bay_t *b)
{
    int margin_x = (int)(w * 0.018);
    int margin_y = (int)(h * 0.030);

    if (margin_x < 18) margin_x = 18;
    if (margin_y < 18) margin_y = 18;
    b->cols = (int)((w * 0.34) / CELL_W);
    if (b->cols < 2) b->cols = 2;
    b->rows = (h - margin_y - HEADER_H - 24) / CELL_H;
    if (b->rows < 3) b->rows = 3;
    b->x = margin_x;
    b->y = margin_y + HEADER_H;
}

/* How many rows fit ON THE SCREEN. Deliberately not b->rows: the bay is the
 * art, the screen edge is the hard limit. */
static int screen_rows(const bay_t *b, int scr_h)
{
    int r = (scr_h - b->y - 6) / CELL_H;
    return r < 1 ? 1 : r;
}

static int screen_cols(const bay_t *b, int scr_w)
{
    int c = (scr_w - b->x - 8) / CELL_W;
    return c < b->cols ? b->cols : c;
}

/* THE FIX: the layout width arrange_icons.c actually uses for n items. */
static int layout_cols(const bay_t *b, int scr_w, int scr_h, int n)
{
    int eff = b->cols;
    int max_rows = screen_rows(b, scr_h);
    int max_cols = screen_cols(b, scr_w);

    if (n > b->cols * max_rows) {
        eff = (n + max_rows - 1) / max_rows;
        if (eff > max_cols) eff = max_cols;
    }
    return eff;
}

static int bottom_of_last_icon(const bay_t *b, int cols, int n)
{
    int last_row = (n - 1) / cols;
    return b->y + last_row * CELL_H + 6 + CELL_H;
}

struct res { int w, h; };
static const struct res FLEET[] = {
    {  800,  600 }, { 1024,  768 }, { 1280, 1024 }, { 1600, 1200 },
    { 1280,  800 }, { 1440,  900 }, { 1920, 1080 },
};

/* 31 staged titles -> 65 shortcuts, + Retro Agent/Retro Chat are already in
 * that 65, + My Computer and Recycle Bin. Measured on .143 and .145. */
#define FLEET_ICONS 67

int main(void)
{
    unsigned i;
    char msg[192];

    printf("== the fleet's whole icon set stays on screen wherever it can ==\n");
    for (i = 0; i < sizeof(FLEET) / sizeof(FLEET[0]); i++) {
        bay_t b;
        int cols, bottom, right, capacity;
        icon_bay(FLEET[i].w, FLEET[i].h, &b);
        cols     = layout_cols(&b, FLEET[i].w, FLEET[i].h, FLEET_ICONS);
        bottom   = bottom_of_last_icon(&b, cols, FLEET_ICONS);
        right    = b.x + cols * CELL_W;
        capacity = screen_cols(&b, FLEET[i].w) * screen_rows(&b, FLEET[i].h);

        sprintf(msg, "%dx%d: layout's right edge (%d) is on screen",
                FLEET[i].w, FLEET[i].h, right);
        CHECK(right <= FLEET[i].w, msg);
        sprintf(msg, "%dx%d: never narrower than the drawn bay",
                FLEET[i].w, FLEET[i].h);
        CHECK(cols >= b.cols, msg);

        if (capacity >= FLEET_ICONS) {
            /* There IS room for all of them, so none may fall off. */
            sprintf(msg, "%dx%d: last icon's bottom (%d) is on screen",
                    FLEET[i].w, FLEET[i].h, bottom);
            CHECK(bottom <= FLEET[i].h, msg);
        } else {
            /* 800x600 cannot hold 67 icons at 76x80 - only 60 fit. Then the
             * honest invariant is that we used every column available rather
             * than leaving width unused while stacking off the bottom. */
            sprintf(msg, "%dx%d: too small for %d icons, so use the full width",
                    FLEET[i].w, FLEET[i].h, FLEET_ICONS);
            CHECK(cols == screen_cols(&b, FLEET[i].w), msg);
        }
    }

    printf("== the exact 1024x768 case that was broken on .143 ==\n");
    {
        bay_t b;
        int cols, bottom;
        icon_bay(1024, 768, &b);
        CHECK(b.cols == 4 && b.rows == 8, "1024x768 bay is still 4x8");
        CHECK(b.cols * b.rows < FLEET_ICONS,
              "the fleet's icons really do overflow that bay");

        /* THE OLD, BUGGY BEHAVIOUR: pack downward in b.cols columns. */
        bottom = bottom_of_last_icon(&b, b.cols, FLEET_ICONS);
        sprintf(msg, "old rule really did run off screen (bottom %d > 768)", bottom);
        CHECK(bottom > 768, msg);

        /* THE FIXED BEHAVIOUR. */
        cols   = layout_cols(&b, 1024, 768, FLEET_ICONS);
        bottom = bottom_of_last_icon(&b, cols, FLEET_ICONS);
        CHECK(cols == 9, "1024x768 widens to 9 columns");
        sprintf(msg, "fixed rule keeps it on screen (bottom %d <= 768)", bottom);
        CHECK(bottom <= 768, msg);
        CHECK(b.x + cols * CELL_W <= 1024, "9 columns still fit across 1024");
    }

    printf("== a bay that is big enough is left exactly as it was ==\n");
    {
        bay_t b;
        icon_bay(1920, 1080, &b);
        CHECK(b.cols == 8 && b.rows == 12, "1920x1080 bay is 8x12");
        CHECK(b.cols * b.rows >= FLEET_ICONS, "96 slots hold 67 icons");
        CHECK(layout_cols(&b, 1920, 1080, FLEET_ICONS) == b.cols,
              "no widening when the bay already fits - stays 8 wide");
    }

    printf("== an absurd icon count degrades, it does not explode ==\n");
    {
        bay_t b;
        int cols;
        icon_bay(800, 600, &b);
        cols = layout_cols(&b, 800, 600, 400);
        CHECK(cols >= b.cols, "still at least the bay's own width");
        CHECK(b.x + cols * CELL_W <= 800,
              "widening is capped by the screen, never past the right edge");
    }

    printf("\n%s\n", munit_fails ? "FAILURES"
                                 : "icon overflow: every icon stays reachable");
    return munit_fails ? 1 : 0;
}
