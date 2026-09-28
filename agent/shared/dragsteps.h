/*
 * dragsteps.h - how many intermediate cursor positions a UIDRAG takes.
 *
 * One step per ~4 px of straight-line distance, clamped to [5, 100]:
 *
 *     steps = clamp( floor( sqrt(dx*dx + dy*dy) / 4 ), 5, 100 )
 *
 * computed with INTEGER arithmetic only. That is the whole point of this file.
 *
 * Found 2026-09-28 on .243 (Compaq Deskpro 2000, Pentium P54C, Win98SE): every
 * UIDRAG killed the command with "[3] Exception 0xC000001D processing command"
 * (STATUS_ILLEGAL_INSTRUCTION). handle_uidrag() used to call sqrt(), and the
 * sqrt() that links is NOT msvcrt.dll's - it is mingw's STATIC _sqrt
 * (libmsvcrt.a: lib32_libmsvcrt_common_a-sqrt.o), prebuilt for the i686
 * baseline, whose normal-positive-argument path is
 *
 *     fld1 ; fxch %st(1) ; fucomi %st(1),%st ; ...  fsqrt
 *
 * FUCOMI (like FCOMI, FCMOVcc and CMOVcc) is a Pentium Pro instruction; a P5
 * raises #UD on it. -march=i586 cannot reach a prebuilt runtime object, so no
 * compiler flag fixes this - the only fix is not to link it. Zero-length drags
 * (the fxam "zero" class) skipped the FUCOMI, which is why a do-nothing test
 * drag would have "passed".
 *
 * floor(sqrt(d2)/4) == the largest k with (4k)^2 <= d2, so the answer needs no
 * root and no division: start at the floor (5) and walk up to the cap (100).
 * If either axis alone is >= 400 px the distance is >= 400 and the cap
 * applies, so every square computed here is < 2 * 400^2 and cannot overflow
 * (the old int dx*dx + dy*dy overflowed - undefined - past ~46341 px).
 *
 * Win32-free so tests/native/test_dragsteps.c compiles the code the agent runs.
 */
#ifndef RETRO_DRAGSTEPS_H
#define RETRO_DRAGSTEPS_H

#ifdef __GNUC__
#define DS_UNUSED __attribute__((unused))
#else
#define DS_UNUSED
#endif

#define DRAG_STEPS_MIN   5
#define DRAG_STEPS_MAX   100
#define DRAG_STEP_PX     4

static DS_UNUSED int drag_steps(int dx, int dy)
{
    /* |dx| as unsigned: 0u - (unsigned)INT_MIN is well defined (2^31) */
    unsigned ax = (dx < 0) ? 0u - (unsigned)dx : (unsigned)dx;
    unsigned ay = (dy < 0) ? 0u - (unsigned)dy : (unsigned)dy;
    unsigned lim = (unsigned)(DRAG_STEPS_MAX * DRAG_STEP_PX);   /* 400 */
    unsigned d2, next;
    int k;

    if (ax >= lim || ay >= lim)
        return DRAG_STEPS_MAX;

    d2 = ax * ax + ay * ay;                     /* < 320000 */
    k = DRAG_STEPS_MIN;
    while (k < DRAG_STEPS_MAX) {
        next = (unsigned)(k + 1) * DRAG_STEP_PX;
        if (next * next > d2)
            break;
        k++;
    }
    return k;
}

#endif /* RETRO_DRAGSTEPS_H */
