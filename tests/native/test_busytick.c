/* agent/shared/busytick.h - a busy stamp's elapsed time never underflows
 * (agent 1.91.0, log.c echo_busy_end + echo_stall_check).
 *
 * 2026-09-28, .124 (agent 1.90.x): the stamp was GetTickCount() | 1 and the
 * elapsed time GetTickCount() - stamp. A console write that began on an even
 * tick and ended inside the same 10-16 ms tick read 0xFFFFFFFF ms and was
 * logged as "returned after 4294967 s" - echoed, it fed itself: 4,514 lines in
 * ten minutes and a log that rotated its history away every few minutes. */
#include "munit.h"
#include "../../agent/shared/busytick.h"

static unsigned int old_elapsed(unsigned int now, unsigned int mark) { return now - mark; }

TEST(a_write_inside_one_tick_took_no_time)
{
    unsigned int t = 1000;                        /* an EVEN tick */
    unsigned int mark = busytick_mark(t);
    CHECK_EQ_U(mark, 1001u);
    CHECK_EQ_U(busytick_elapsed(t, mark), 0u);
    CHECK_EQ_U(old_elapsed(t, mark), 0xFFFFFFFFu);       /* the 4294967 s bug */
    CHECK(busytick_elapsed(t, mark) < 30000u, "not a 30 s stall");
    CHECK(old_elapsed(t, mark) >= 30000u, "(1.90.x reported a stall here)");
}

TEST(an_odd_tick_is_at_most_one_ms_early)
{
    unsigned int t = 1001;
    CHECK_EQ_U(busytick_elapsed(t, busytick_mark(t)), 1u);
}

TEST(a_real_stall_still_measures_its_length)
{
    unsigned int t = 5000, mark = busytick_mark(t);
    CHECK_EQ_U(busytick_elapsed(t + 30000u, mark), 30000u);
    CHECK_EQ_U(busytick_elapsed(t + 16u, mark), 16u);
}

TEST(the_49_day_wrap_is_harmless)
{
    unsigned int t = 0xFFFFFFF0u, mark = busytick_mark(t);
    CHECK_EQ_U(busytick_elapsed(0x00000010u, mark), 0x20u);
    CHECK(busytick_mark(0) != 0, "a stamp is never the idle value 0");
}

MUNIT_MAIN("busytick (a console-write stamp cannot read in the future)",
    RUN(a_write_inside_one_tick_took_no_time);
    RUN(an_odd_tick_is_at_most_one_ms_early);
    RUN(a_real_stall_still_measures_its_length);
    RUN(the_49_day_wrap_is_harmless);
)
