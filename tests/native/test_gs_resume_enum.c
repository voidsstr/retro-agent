/* test_gs_resume_enum.c - TRUE-SOURCE: agent/shared/gsresume.h, GAMESYNC's
 * resume test as of agent 1.85.0 - WHERE the source's time comes from.
 *
 * THE OLD COST (gamesync.c gs_copy_file, v1.62.0 - 1.84.x): per file,
 * FindFirstFileA(dst) for its size, GetFileAttributesExA(dst) for its time and
 * GetFileAttributesExA(src) - a round trip over SMB1 to the NAS - for the
 * source's time, although the listing gs_copy_tree() was walking had just
 * returned that file's size AND time. old_verdict() models those answers.
 *
 * THE FIX reads the destination once and takes the source's time from the
 * listing; only when that disagrees is the source asked, as before. What must
 * hold - the same-size-different-content outage (Deus Ex 1.112fm, Core.dll
 * 790,528 bytes before and after) must still be copied, and every file the old
 * test copied must still be copied.
 */
#include "munit.h"
#include <stdlib.h>

#include "../../agent/shared/gsresume.h"

#define SEC  10000000LL
#define BASE 130000000000000000LL

/* v1.62.0 - 1.84.x: size equal AND the source's own time within 2 s */
static int old_verdict(int dst_exists, long long dst_size, long long dst_t,
                       long long src_size, long long src_attr_t)
{
    return (dst_exists && dst_size == src_size &&
            gsr_same_time(src_attr_t, dst_t)) ? GSR_SKIP : GSR_COPY;
}

/* 1.85.0, and how many times it had to ask the NAS for the source's time */
static int new_verdict(int dst_exists, long long dst_size, long long dst_t,
                       long long src_size, long long src_list_t,
                       long long src_attr_t, int *asked)
{
    int v = gsr_decide(dst_exists, dst_size, dst_t, src_size, 1, src_list_t);
    if (v == GSR_ASK_SOURCE) {
        (*asked)++;
        v = gsr_decide_source(1, src_attr_t, dst_t);
    }
    return v;
}

TEST(a_settled_file_is_skipped_without_asking_the_nas)
{
    int asked = 0;
    CHECK_EQ_I(new_verdict(1, 790528, BASE, 790528, BASE, BASE, &asked), GSR_SKIP);
    CHECK_EQ_I(asked, 0);
    /* FAT32's 2 s rounding on the box still matches */
    CHECK_EQ_I(new_verdict(1, 790528, BASE - 2 * SEC, 790528, BASE, BASE, &asked),
               GSR_SKIP);
    CHECK_EQ_I(asked, 0);
}

TEST(the_deus_ex_patch_is_still_copied)
{
    /* same size, library copy patched (newer), box copy retail (older) */
    int asked = 0;
    long long lib = BASE, box = BASE - 400LL * 24 * 3600 * SEC;
    CHECK_EQ_I(new_verdict(1, 790528, box, 790528, lib, lib, &asked), GSR_COPY);
    CHECK_EQ_I(old_verdict(1, 790528, box, 790528, lib), GSR_COPY);
    CHECK_EQ_I(asked, 1);   /* the listing disagreed, so the source was asked */
}

TEST(a_missing_or_short_destination_is_copied_with_no_time_lookups)
{
    int asked = 0;
    CHECK_EQ_I(new_verdict(0, -1, 0, 100, BASE, BASE, &asked), GSR_COPY);
    CHECK_EQ_I(new_verdict(1, 50, BASE, 100, BASE, BASE, &asked), GSR_COPY);
    CHECK_EQ_I(asked, 0);
}

TEST(no_listing_time_falls_back_to_asking)
{
    CHECK_EQ_I(gsr_decide(1, 10, BASE, 10, 0, 0), GSR_ASK_SOURCE);
    CHECK_EQ_I(gsr_decide_source(0, 0, BASE), GSR_COPY);   /* could not ask */
}

TEST(every_file_the_old_test_copied_is_still_copied)
{
    /* Random destinations/sources where the listing and the source report the
     * same time (they are the same file on the same server): identical
     * verdicts, and the NAS is asked only for files that end up COPIED. */
    int i, differ = 0, asked_for_skips = 0;
    srand(99);
    for (i = 0; i < 20000; i++) {
        int dst_exists = rand() % 5 != 0;
        long long src_size = rand() % 4;
        long long dst_size = dst_exists ? rand() % 4 : -1;
        long long src_t = BASE + (long long)(rand() % 8) * SEC;
        long long dst_t = BASE + (long long)(rand() % 8) * SEC;
        int asked = 0;
        int nv = new_verdict(dst_exists, dst_size, dst_t, src_size, src_t, src_t, &asked);
        int ov = old_verdict(dst_exists, dst_size, dst_t, src_size, src_t);
        differ += nv != ov;
        if (nv == GSR_SKIP && asked)
            asked_for_skips++;
    }
    CHECK_EQ_I(differ, 0);
    CHECK_EQ_I(asked_for_skips, 0);
}

TEST(the_only_divergence_is_a_source_whose_two_apis_disagree)
{
    /* listing time == destination, per-file query time 10 s off: the OLD test
     * copied this file on EVERY sync (the destination is stamped from the
     * open handle, which agrees with the listing); the new one skips it. */
    int asked = 0;
    CHECK_EQ_I(old_verdict(1, 10, BASE, 10, BASE + 10 * SEC), GSR_COPY);
    CHECK_EQ_I(new_verdict(1, 10, BASE, 10, BASE, BASE + 10 * SEC, &asked), GSR_SKIP);
}

MUNIT_MAIN("GAMESYNC resume from the listing (agent/shared/gsresume.h, 1.85.0)",
    RUN(a_settled_file_is_skipped_without_asking_the_nas);
    RUN(the_deus_ex_patch_is_still_copied);
    RUN(a_missing_or_short_destination_is_copied_with_no_time_lookups);
    RUN(no_listing_time_falls_back_to_asking);
    RUN(every_file_the_old_test_copied_is_still_copied);
    RUN(the_only_divergence_is_a_source_whose_two_apis_disagree);
)
