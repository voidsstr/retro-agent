/*
 * busytick.h - a "busy since" tick stamp whose elapsed time cannot underflow
 * (agent 1.91.0, log.c echo_busy_begin/echo_busy_end/echo_stall_check).
 * Win32-free: tests/native/test_busytick.c compiles it as is.
 *
 * WHY (2026-09-28, .124, agent 1.90.x): log.c stamped a console call as
 * GetTickCount() | 1, so that 0 could mean "idle", and took the elapsed time as
 * GetTickCount() - stamp. GetTickCount moves in 10-16 ms steps, so a write that
 * began on an EVEN tick and finished inside that tick read one millisecond in
 * the future: 0xFFFFFFFF ms, logged as "a console write returned after 4294967
 * s". That line is echoed to the console too, which can make another - 4,514
 * of them in ten minutes, ~100 KB of log a minute, and agent.log (512 KB x2)
 * rotated its whole history away every few minutes. The restart it should
 * have explained on .124 was nearly gone from the log by the time anyone
 * looked.
 *
 * The mark keeps its low bit set (0 still means idle); the elapsed time is
 * measured from the tick the mark was TAKEN on - `mark & ~1`, which is that
 * tick or 1 ms before it, never after - so it cannot go negative. Unsigned
 * 32-bit arithmetic keeps the 49.7-day GetTickCount wrap correct.
 */
#ifndef RETRO_BUSYTICK_H
#define RETRO_BUSYTICK_H

/* a busy stamp for tick t: never 0, so 0 can mean "not busy" */
static __inline unsigned int busytick_mark(unsigned int t)
{
    return t | 1u;
}

/* milliseconds since the stamp, from the tick it was taken on (or 1 ms early) */
static __inline unsigned int busytick_elapsed(unsigned int now, unsigned int mark)
{
    return now - (mark & ~1u);
}

#endif /* RETRO_BUSYTICK_H */
