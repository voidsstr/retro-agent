/* test_gs_resume_mtime.c - GAMESYNC must not treat "same size" as "same file".
 *
 * WHAT THIS PROTECTS, and it is not hypothetical.
 *
 * gs_copy_file() skips a destination it believes is already correct, so that a
 * sync interrupted by a power cut does not start over. Until 2026-08-29 that
 * test was SIZE ONLY, justified in the source as "these are immutable release
 * trees". That premise died the moment we began PATCHING staged games, because
 * a recompiled DLL very often lands on exactly the same file-alignment boundary
 * as the one it replaces - so for a patch, same-size-different-content is the
 * NORMAL case rather than an edge case.
 *
 * The outage it caused: applying the official Deus Ex 1.112fm patch changed 38
 * files in DeusEx\SYSTEM and SEVENTEEN of them kept their exact byte size,
 * Core.dll (790,528) and DeusEx.exe (253,952) among them. Every box that
 * already had the game therefore accepted the new Core.u - whose size DID
 * change - while keeping the retail Core.dll. That is a mixed-version Unreal
 * install, and it died during InitEngine with
 *     Can't find 'intUObjectexecGetConfig' in 'Core.dll'
 * while GAMESYNC reported state=done, titles 31/31, 0 skipped, 0 failed. The
 * report was truthful: the sync had genuinely decided those files were current.
 * Shogo lost 3 files to the same cause (ima.dll, DE_Msg.dll, cdaudio.dll) and
 * Descent 3 lost netgames\co-op.d3m.
 *
 * The fix compares size AND last-write time, and stamps the destination with
 * the source's time after a copy so that resume still costs nothing.
 *
 * The 2-second tolerance is load-bearing, not sloppiness: FAT32 records write
 * times with 2-second granularity, so a time set from an NTFS source is rounded
 * on a FAT volume. An exact comparison would never match there and every sync
 * would re-copy the whole library on the Win9x boxes. Same "modify window" idea
 * as rsync, same reason.
 */

#include "munit.h"
#include <stdio.h>

typedef unsigned int   DWORD32_;
typedef struct { DWORD32_ dwLowDateTime, dwHighDateTime; } FILETIME_;

/* --- verbatim from agent/src/gamesync.c (gs_same_mtime) ------------------- */
#define GS_MTIME_SLACK_100NS  (2 * 10000000LL)

static int gs_same_mtime(const FILETIME_ *a, const FILETIME_ *b)
{
    long long ta = ((long long)a->dwHighDateTime << 32) | a->dwLowDateTime;
    long long tb = ((long long)b->dwHighDateTime << 32) | b->dwLowDateTime;
    long long d  = ta - tb;
    if (d < 0) d = -d;
    return d <= GS_MTIME_SLACK_100NS;
}

/* --- the skip decision, mirroring gs_copy_file's early-out ---------------- */
static int skip_now(long long dst_size, long long src_size,
                    const FILETIME_ *src_ft, const FILETIME_ *dst_ft)
{
    return dst_size >= 0 && dst_size == src_size && gs_same_mtime(src_ft, dst_ft);
}

/* the OLD, buggy rule - kept so the regression is asserted in both directions */
static int skip_before_the_fix(long long dst_size, long long src_size)
{
    return dst_size >= 0 && dst_size == src_size;
}

static FILETIME_ ft_from(long long v)
{
    FILETIME_ f;
    f.dwLowDateTime  = (DWORD32_)(v & 0xFFFFFFFFLL);
    f.dwHighDateTime = (DWORD32_)((v >> 32) & 0xFFFFFFFFLL);
    return f;
}

#define SEC 10000000LL          /* one second in FILETIME units */
#define BASE 130000000000000000LL

int main(void)
{
    FILETIME_ a, b;
    int fails = 0;

    printf("== GAMESYNC resume: size alone must not mean 'already correct' ==\n");

    /* 1. THE DEUS EX CASE. Core.dll is 790,528 bytes before AND after the
     *    1.112fm patch, with different content and a different build time.
     *    The old rule skipped it - that is the whole bug. */
    a = ft_from(BASE);              /* library copy, patched  */
    b = ft_from(BASE - 400LL*24*3600*SEC);   /* box copy, retail, older */
    if (skip_now(790528, 790528, &a, &b)) {
        printf("  FAIL: patched Core.dll (same size, older box copy) would be SKIPPED\n");
        fails++;
    } else {
        printf("  ok: same-size-but-different Core.dll is copied\n");
    }
    if (!skip_before_the_fix(790528, 790528)) {
        printf("  FAIL: the old rule was supposed to skip it - test models it wrong\n");
        fails++;
    } else {
        printf("  ok: the OLD rule really did skip it (regression is real)\n");
    }

    /* 2. Resume must still work: same size AND same time = nothing to do. */
    a = ft_from(BASE); b = ft_from(BASE);
    if (!skip_now(790528, 790528, &a, &b)) {
        printf("  FAIL: an identical file is re-copied - resume is broken\n");
        fails++;
    } else printf("  ok: identical file (size + time) is skipped\n");

    /* 3. A truncated resume is still caught by size. */
    a = ft_from(BASE); b = ft_from(BASE);
    if (skip_now(123, 790528, &a, &b)) {
        printf("  FAIL: a half-copied file would be left truncated\n");
        fails++;
    } else printf("  ok: short file is re-copied\n");

    /* 4. FAT32 rounds to 2 s. Within the window must still count as equal, or
     *    every Win9x box re-copies the entire library on every sync. */
    a = ft_from(BASE); b = ft_from(BASE - 2*SEC);
    if (!gs_same_mtime(&a, &b)) {
        printf("  FAIL: 2 s apart must compare equal (FAT32 granularity)\n");
        fails++;
    } else printf("  ok: 2 s apart compares equal (FAT32 tolerance)\n");

    /* 5. ...but the window must not be so wide it hides a real rebuild. */
    a = ft_from(BASE); b = ft_from(BASE - 3*SEC);
    if (gs_same_mtime(&a, &b)) {
        printf("  FAIL: 3 s apart must NOT compare equal\n");
        fails++;
    } else printf("  ok: 3 s apart is a difference\n");

    /* 6. Order must not matter. */
    a = ft_from(BASE - 2*SEC); b = ft_from(BASE);
    if (!gs_same_mtime(&a, &b)) {
        printf("  FAIL: comparison is not symmetric\n");
        fails++;
    } else printf("  ok: comparison is symmetric\n");

    printf(fails ? "== FAILED (%d) ==\n" : "== all passed ==\n", fails);
    return fails ? 1 : 0;
}
