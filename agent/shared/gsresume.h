/*
 * gsresume.h - GAMESYNC's resume test: is the destination file already the
 * library's file? (size AND last-write time since v1.62.0; how the time is
 * obtained changed in 1.85.0).
 *
 * WHY SIZE AND TIME, NOT SIZE: see the long comment above gs_copy_file() in
 * gamesync.c - the Deus Ex 1.112fm patch kept seventeen files at their exact
 * byte size, and a size-only test half-applied it on every box.
 *
 * WHY 2 SECONDS OF SLACK: FAT32 stores write times with 2-second granularity,
 * so a time copied from an NTFS source is rounded on a Win9x box. An exact
 * compare would never match there and every sync would re-copy the library.
 *
 * WHAT 1.85.0 CHANGED - WHERE THE TIMES COME FROM, NOT WHAT IS COMPARED.
 * Until then each file cost THREE metadata calls: FindFirstFileA(dst) for the
 * size, GetFileAttributesExA(dst) for its time, and GetFileAttributesExA(src)
 * - a round trip over SMB1 to the NAS - for the source's time, although the
 * FindFirstFile listing gs_copy_tree() was walking had just handed over that
 * very file's size AND time. Now:
 *   - the destination is read ONCE (GetFileAttributesExA gives size and time);
 *   - the source's time is the one the listing already returned;
 *   - and ONLY if that time disagrees with the destination's is the source
 *     asked directly, exactly as before. So a file the old test would copy is
 *     still copied; a file it would skip is skipped without the network call.
 *     The one case where the two differ - listing time equal, per-file query
 *     time not - is a source whose two APIs disagree, which the old test would
 *     have re-copied on every sync forever.
 *
 * Win32-free (times are 100 ns FILETIME ticks as 64-bit integers) so
 * tests/native/test_gs_resume_mtime.c and test_gs_resume_enum.c compile it.
 */
#ifndef RETRO_GSRESUME_H
#define RETRO_GSRESUME_H

#ifdef __GNUC__
#define GSR_UNUSED __attribute__((unused))
#else
#define GSR_UNUSED
#endif

#define GS_MTIME_SLACK_100NS  (2 * 10000000LL)   /* 2 s, in 100ns FILETIME units */

GSR_UNUSED static long long gsr_ft64(unsigned long hi, unsigned long lo)
{
    return (long long)(((unsigned long long)(hi & 0xFFFFFFFFUL) << 32) |
                       (unsigned long long)(lo & 0xFFFFFFFFUL));
}

GSR_UNUSED static int gsr_same_time(long long a, long long b)
{
    long long d = a - b;
    if (d < 0)
        d = -d;
    return d <= GS_MTIME_SLACK_100NS;
}

enum {
    GSR_COPY = 0,         /* copy the file                                  */
    GSR_SKIP = 1,         /* the destination is already the library's file  */
    GSR_ASK_SOURCE = 2    /* sizes agree, listing time does not: ask the    */
                          /* source for its time, as every agent before did */
};

/* Stage 1: decide from the listing and ONE read of the destination. */
GSR_UNUSED static int gsr_decide(int dst_exists, long long dst_size,
                                 long long dst_time, long long src_size,
                                 int have_src_list_time, long long src_list_time)
{
    if (!dst_exists || dst_size < 0 || dst_size != src_size)
        return GSR_COPY;
    if (have_src_list_time && gsr_same_time(src_list_time, dst_time))
        return GSR_SKIP;
    return GSR_ASK_SOURCE;
}

/* Stage 2: the source's own time, asked directly. */
GSR_UNUSED static int gsr_decide_source(int have_src_time, long long src_time,
                                        long long dst_time)
{
    return (have_src_time && gsr_same_time(src_time, dst_time)) ? GSR_SKIP
                                                                : GSR_COPY;
}

#endif /* RETRO_GSRESUME_H */
