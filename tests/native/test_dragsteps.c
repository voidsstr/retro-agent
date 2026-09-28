/* agent/shared/dragsteps.h - UIDRAG's step count with no floating point
 * (agent/src/input.c:handle_uidrag, fix 2026-09-28, the release after 1.89.1).
 *
 * .243 (Compaq Deskpro 2000, Pentium P54C, Win98SE): every UIDRAG died with
 * "[3] Exception 0xC000001D processing command". handle_uidrag() computed
 *     steps = (int)(sqrt((double)(dx*dx + dy*dy)) / 4.0), clamped to [5,100]
 * and that sqrt() linked mingw's static _sqrt from libmsvcrt.a, whose path for
 * any positive argument executes FUCOMI - a Pentium Pro instruction. The fix
 * is drag_steps(): the same answer from integer arithmetic alone. The binary
 * half of the fix (no FUCOMI/_sqrt in the built agent) is pinned by
 * tests/python/test_agent_is_pentium1_safe.py; this file pins that the number
 * did not change for any drag a real screen can produce.
 *
 * old_steps() below IS the removed code, with its int arithmetic made explicit
 * (wrapping 32-bit, which is what the i686 build did with the overflow). */
#include <stdio.h>
#include <stdint.h>
#include <limits.h>
#include <math.h>
#include "../../agent/shared/dragsteps.h"

static int fails = 0, runs = 0;
#define CHECK(c, msg) do { runs++; if (!(c)) { fails++; printf("  [FAIL] %s\n", msg); } else printf("  [ ok ] %s\n", msg); } while (0)

/* The pre-fix handle_uidrag() arithmetic, verbatim apart from making the
 * 32-bit wrap of dx*dx + dy*dy explicit instead of undefined. */
static int old_steps(int dx, int dy)
{
    uint32_t sq = (uint32_t)dx * (uint32_t)dx + (uint32_t)dy * (uint32_t)dy;
    int d2 = (int)sq;                       /* what the int expression held */
    double dist = sqrt((double)d2);
    int steps;
    if (dist != dist)                       /* sqrt(negative): NaN; x86     */
        steps = INT_MIN;                    /* cvttsd2si gives 0x80000000   */
    else
        steps = (int)(dist / 4.0);
    if (steps < 5) steps = 5;
    if (steps > 100) steps = 100;
    return steps;
}

int main(void)
{
    int dx, dy, mismatches = 0, first_dx = 0, first_dy = 0;
    int out_of_range = 0, nonmono = 0, prev;
    char msg[200];

    /* Every drag on any screen up to 1600x1200 (and then some): identical. */
    for (dx = -1700; dx <= 1700; dx++) {
        for (dy = -1300; dy <= 1300; dy += (dx % 7 == 0) ? 1 : 13) {
            if (drag_steps(dx, dy) != old_steps(dx, dy)) {
                if (!mismatches) { first_dx = dx; first_dy = dy; }
                mismatches++;
            }
        }
    }
    snprintf(msg, sizeof msg,
             "drag_steps == the old sqrt formula for every on-screen drag "
             "(%d mismatches; first at %d,%d)", mismatches, first_dx, first_dy);
    CHECK(mismatches == 0, msg);

    /* ...including exhaustively around the only region where it varies */
    mismatches = 0;
    for (dx = -420; dx <= 420; dx++)
        for (dy = -420; dy <= 420; dy++)
            if (drag_steps(dx, dy) != old_steps(dx, dy)) mismatches++;
    snprintf(msg, sizeof msg, "exhaustive |dx|,|dy| <= 420: %d mismatches", mismatches);
    CHECK(mismatches == 0, msg);

    /* The boundaries, by hand. */
    CHECK(drag_steps(0, 0) == 5, "a zero-length drag still takes the floor of 5 steps");
    CHECK(drag_steps(3, 4) == 5, "5 px -> 1, clamped up to 5");
    CHECK(drag_steps(23, 0) == 5 && drag_steps(24, 0) == 6, "24 px is the first drag above the floor (5.75 -> 5, 6.0 -> 6)");
    CHECK(drag_steps(80, 60) == 25 && drag_steps(-80, -60) == 25, "a 100 px diagonal is 25 steps, either direction");
    CHECK(drag_steps(399, 0) == 99, "399 px -> 99 steps");
    CHECK(drag_steps(400, 0) == 100 && drag_steps(0, -400) == 100, "400 px on one axis is the cap");
    CHECK(drag_steps(283, 283) == 100 && drag_steps(282, 282) == 99,
          "the cap on a diagonal: sqrt(2)*283 = 400.2 -> 100, sqrt(2)*282 = 398.8 -> 99");
    CHECK(drag_steps(1919, -1079) == 100, "corner to corner on a 1080p panel -> 100");

    /* Old-buggy vs fixed: a drag long enough to overflow the old int square.
     * 65536^2 wraps to exactly 0, so the old code took the MINIMUM 5 steps for
     * the longest drag it was handed; 46341^2 goes negative -> sqrt NaN -> 5. */
    CHECK(old_steps(65536, 0) == 5, "(old) a 65536 px drag wrapped dx*dx to 0 -> 5 steps");
    CHECK(drag_steps(65536, 0) == 100, "(fixed) a 65536 px drag -> the 100-step cap");
    CHECK(old_steps(46341, 0) == 5, "(old) 46341 px: dx*dx went negative -> NaN -> 5 steps");
    CHECK(drag_steps(46341, 0) == 100, "(fixed) 46341 px -> 100");
    CHECK(drag_steps(INT_MIN, INT_MIN) == 100 && drag_steps(INT_MAX, INT_MIN) == 100,
          "INT_MIN / INT_MAX deltas are defined and capped (no signed overflow)");

    /* Always in range, and monotone along an axis. */
    for (dx = -200000; dx <= 200000; dx += 37) {
        int s = drag_steps(dx, dx / 3);
        if (s < DRAG_STEPS_MIN || s > DRAG_STEPS_MAX) out_of_range++;
    }
    CHECK(out_of_range == 0, "every result lies in [5, 100]");
    prev = drag_steps(0, 0);
    for (dx = 1; dx <= 5000; dx++) {
        int s = drag_steps(dx, 0);
        if (s < prev) nonmono++;
        prev = s;
    }
    CHECK(nonmono == 0, "a longer drag never takes fewer steps");

    printf("-- dragsteps (UIDRAG with no sqrt: FUCOMI faulted on .243's Pentium P54C): %d/%d tests passed --\n",
           runs - fails, runs);
    return fails ? 1 : 0;
}
