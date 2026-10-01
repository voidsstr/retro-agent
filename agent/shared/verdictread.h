/*
 * verdictread.h - what a failed read of the published gate verdicts MEANS
 * (agent 1.97.1). Win32-free: tests/native/test_verdictread.c compiles it.
 *
 * GAMESYNC prefers the host's published verdicts (<library>\_gamegate\
 * <profile_hash>.txt) over its own rules: only that file carries the
 * operator's overrides - .243's curated "not wanted on this box" lines among
 * them. Until 1.97.1 ANY failure to read it read as "not published", and the
 * run fell back to the local rules, which cannot see an override: measured on
 * the W98BUILD VM 2026-10-01 (SMB to the NAS failing, error 53), the run that
 * should have copied 4 titles planned 18,104 MB of the library, every title the
 * user had ejected from that profile included - and GAMESYNC never deletes.
 *
 * So only "the file is not there" is "not published" (fail-open, as designed:
 * absent data never blocks a title). An unreachable share, a read that fails
 * or comes back empty (a file being replaced) is UNREADABLE: the run retries
 * and then refuses, loudly, copying nothing - the library is on the same share,
 * so that run could not have copied anything right anyway.
 */
#ifndef RETRO_VERDICTREAD_H
#define RETRO_VERDICTREAD_H

#ifndef VR_UNUSED
#  if defined(__GNUC__)
#    define VR_UNUSED __attribute__((unused))
#  else
#    define VR_UNUSED
#  endif
#endif

enum {
    VR_LOADED = 0,      /* read, non-empty: use it                               */
    VR_ABSENT = 1,      /* not published for this profile: local rules (as ever) */
    VR_UNREADABLE = 2   /* could not be read: retry, then refuse the run         */
};

/* Win32 error codes this needs, by value (no windows.h here). */
#define VR_ERROR_FILE_NOT_FOUND 2u
#define VR_ERROR_PATH_NOT_FOUND 3u

/* open_err: 0 when the open succeeded, else GetLastError() from the open.
 * size:     the file size when it opened (ignored otherwise).
 * read_ok:  1 when the whole file was read.
 * cap:      the largest file the agent accepts. */
VR_UNUSED static int vr_outcome(unsigned long open_err, unsigned long size,
                                int read_ok, unsigned long cap)
{
    if (open_err != 0)
        return (open_err == VR_ERROR_FILE_NOT_FOUND || open_err == VR_ERROR_PATH_NOT_FOUND)
                   ? VR_ABSENT : VR_UNREADABLE;
    if (size == 0 || !read_ok)
        return VR_UNREADABLE;           /* a file mid-replace, or the read dropped */
    if (size > cap)
        return VR_ABSENT;               /* a mistake, not a file - local rules     */
    return VR_LOADED;
}

/* How many times the run tries before it refuses (one per few seconds - the
 * listings use five as well). */
#define VR_TRIES 5

VR_UNUSED static const char *vr_name(int outcome)
{
    return outcome == VR_LOADED ? "loaded" : outcome == VR_ABSENT ? "absent" : "unreadable";
}

#endif /* RETRO_VERDICTREAD_H */
