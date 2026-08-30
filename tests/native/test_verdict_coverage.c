/* test_verdict_coverage.c - TRUE-SOURCE: gg_verdict_count() and
 * gg_verdict_declared() in agent/shared/gamegate.h, the guard that makes a
 * SHRUNKEN verdict file visible.
 *
 * THE FAILURE THIS EXISTS FOR, which really happened on 2026-08-30.
 * `gamegate.py publish --title Halo` rendered only the named title and wrote it
 * over the whole per-box file. Seven of eight boxes were left with a one-row
 * verdict file where a 38-row one had been. The survivor was PERFECTLY WELL
 * FORMED - same "# gamegate v1" header, same four columns, one valid row - so
 * nothing anywhere reported it, and the ollama adjudications that only the host
 * can produce were simply gone. Every box went on gating correctly by its own
 * arithmetic, which is exactly what made it invisible: the artefact looked fine.
 *
 * It was found by counting rows by hand. That is not a mechanism.
 *
 * A file cannot defend itself against being replaced. What it CAN do is state
 * how much it claims to cover, so the next reader notices the claim shrank -
 * `# titles=N` in the header, versus the rows actually present. This test pins
 * the two halves of that comparison, because a guard against an invisible
 * failure is worth exactly nothing if it is itself unverified.
 *
 * THE 0 CASE IS THE SUBTLE ONE. A file written before the header existed
 * carries no `# titles=`, and gg_verdict_declared() returns 0 for it. 0 must
 * mean "did not say", NEVER "covers nothing" - the eight files on the share at
 * the time this was written are all headerless, and a reader that treated them
 * as empty would raise a fleet-wide alarm about the one thing that is fine.
 */

#include "munit.h"
#include <string.h>

#include "../../agent/shared/gamegate.h"

/* The real .171 file as the --title clobber left it: valid, complete-looking,
 * and missing 37 titles. */
static const char *HALO_ONLY =
    "# gamegate v1 profile=efb240b3f32bb482 host=NSC-5B996B81319 "
    "generated=2026-08-30T13:18:35\n"
    "# Intel(R) Pentium(R) 4 CPU 2.80GHz 2793 MHz x1, 509 MB RAM, "
    "Intel(R) 82865G Graphics Controller (8086:2572, 96 MB, fixed), Windows XP\n"
    "# model=qwen3:14b\n"
    "# titles=38\n"
    "# <verdict>\t<title>\t<limiting>\t<reason>\n"
    "no\tHalo\tgpu_feature_level\tGPU too old for this title's renderer "
    "(have fixed, needs sm1.x) [rule]\n";

static const char *THREE_ROWS =
    "# gamegate v1 profile=deadbeefdeadbeef host=BOX generated=x\n"
    "# titles=3\n"
    "# <verdict>\t<title>\t<limiting>\t<reason>\n"
    "run\tQuake1\t-\tmeets requirements [rule]\n"
    "no\tFarCry\tvram_mb\tnot enough video RAM (have 32 MB, needs 64) [rule]\n"
    "marginal\tUT2004\tgpu_feature_level\tGPU lacks T&L [llm]\n";

/* What every file on the share looked like before the header shipped. */
static const char *HEADERLESS =
    "# gamegate v1 profile=deadbeefdeadbeef host=BOX generated=x\n"
    "# model=qwen3:14b\n"
    "# <verdict>\t<title>\t<limiting>\t<reason>\n"
    "run\tQuake1\t-\tmeets requirements [rule]\n"
    "run\tQuake2Complete\t-\tmeets requirements [rule]\n";

TEST(the_real_clobber_is_detectable_as_a_shrunken_file)
{
    /* THE WHOLE POINT. One row present, thirty-eight declared. Before the
     * header existed these two numbers could not be compared at all, and the
     * only way to notice was to count rows against the library by hand. */
    CHECK_EQ_I(gg_verdict_count(HALO_ONLY), 1);
    CHECK_EQ_I(gg_verdict_declared(HALO_ONLY), 38);
    CHECK(gg_verdict_count(HALO_ONLY) < gg_verdict_declared(HALO_ONLY),
          "a clobbered file must read as covering less than it claims");
}

TEST(an_intact_file_agrees_with_itself)
{
    /* The negative case matters as much: a guard that fires on healthy files
     * trains everyone to ignore it, which this project has already learned the
     * hard way with the REGEDIT5 validator check. */
    CHECK_EQ_I(gg_verdict_count(THREE_ROWS), 3);
    CHECK_EQ_I(gg_verdict_declared(THREE_ROWS), 3);
    CHECK(gg_verdict_count(THREE_ROWS) == gg_verdict_declared(THREE_ROWS),
          "an intact file must not look shrunken");
}

TEST(no_header_means_did_not_say_never_covers_nothing)
{
    /* Every file on the share when the header shipped was headerless. A reader
     * treating declared==0 as "covers nothing" would report a fleet-wide
     * outage about the one thing that is actually fine. */
    CHECK_EQ_I(gg_verdict_declared(HEADERLESS), 0);
    CHECK_EQ_I(gg_verdict_count(HEADERLESS), 2);
    CHECK(gg_verdict_count(HEADERLESS) > 0,
          "the rows are still readable without a header");
}

TEST(comments_and_blanks_are_not_verdicts)
{
    /* The row count must count VERDICTS. If a comment line counted, a
     * clobbered file's four header lines would pad it back up to five and
     * disguise the very shrinkage this exists to expose. */
    static const char *only_comments =
        "# gamegate v1 profile=x host=y generated=z\n"
        "# titles=38\n"
        "\n"
        "; a semicolon comment\n"
        "# <verdict>\t<title>\t<limiting>\t<reason>\n";
    CHECK_EQ_I(gg_verdict_count(only_comments), 0);
    CHECK_EQ_I(gg_verdict_declared(only_comments), 38);
    /* An all-header file is the most extreme shrinkage there is, and it must
     * be visible rather than reading as a valid empty file. */
    CHECK(gg_verdict_count(only_comments) < gg_verdict_declared(only_comments),
          "a file with no verdicts at all must still read as shrunken");
}

TEST(a_half_written_file_does_not_over_count)
{
    /* A publish caught mid-write. gg_verdict_parse only accepts a row with a
     * title, so a truncated last line must not be counted as a verdict - an
     * over-count would hide a shrunken file by making it match its claim. */
    static const char *torn =
        "# gamegate v1 profile=x host=y generated=z\n"
        "# titles=38\n"
        "run\tQuake1\t-\tmeets requirements [rule]\n"
        "no\tFarC";                       /* cut off, no tabs, no newline */
    int n = gg_verdict_count(torn);
    CHECK(n >= 1, "the complete row before the tear still counts");
    CHECK(n <= 2, "a torn line must not inflate the count");
    CHECK(n < gg_verdict_declared(torn),
          "a torn file still reads as covering less than it claims");
}

TEST(degenerate_input_is_safe)
{
    CHECK_EQ_I(gg_verdict_count(NULL), 0);
    CHECK_EQ_I(gg_verdict_declared(NULL), 0);
    CHECK_EQ_I(gg_verdict_count(""), 0);
    CHECK_EQ_I(gg_verdict_declared(""), 0);
    /* A header-like string that is not the key must not be mistaken for it. */
    CHECK_EQ_I(gg_verdict_declared("# titles missing\nrun\tQ\t-\tok [rule]\n"), 0);
    CHECK_EQ_I(gg_verdict_declared("# titlesX=38\n"), 0);
}

MUNIT_MAIN("verdict-file coverage guard (agent/shared/gamegate.h)",
    RUN(the_real_clobber_is_detectable_as_a_shrunken_file);
    RUN(an_intact_file_agrees_with_itself);
    RUN(no_header_means_did_not_say_never_covers_nothing);
    RUN(comments_and_blanks_are_not_verdicts);
    RUN(a_half_written_file_does_not_over_count);
    RUN(degenerate_input_is_safe);
)
