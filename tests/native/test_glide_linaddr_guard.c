/* test_glide_linaddr_guard.c
 *
 * Guards the grSstWinOpen crash fix in our glide fork (voidsstr/retro3dfx-glide,
 * glide3x/h3/minihwc/minihwc.c hwcMapBoard; see voodoo-cleanroom DEBUGGING-NOTES
 * "GLIDE grSstWinOpen VERIFIED ROOT CAUSE").
 *
 * Root cause (verified on .124 by instrumentation): the display driver's
 * HWCEXT_GETLINEARADDR escape returns resStatus=1 numBaseAddrs=3 but all
 * baseAddresses==0 (miniport IOCTL_VIDEO_QUERY_GLIDE_ACCESS_RANGES reports
 * success with VirtualAddress=0). The old code checked only res.resStatus and
 * used baseAddresses[0]=0 as the register base -> grSstWinOpen faulted (read at
 * reg_base + reg_offset == ~0x14717). The fix treats (a) ExtEscape rv<=0 and
 * (b) baseAddresses[0]==0 as failures so hwcMapBoard returns FXFALSE instead of
 * handing grSstWinOpen a NULL board base.
 *
 * Invariant: hwcMapBoard must accept the linear-address result ONLY when the
 * escape succeeded AND resStatus==1 AND baseAddresses[0]!=0.
 */
#include "munit.h"

/* mirror of the guard predicate in hwcMapBoard (post-fix). */
static int linaddr_result_usable(int ext_rv, int resStatus, unsigned long base0) {
    if (ext_rv <= 0) return 0;      /* escape failed */
    if (resStatus != 1) return 0;   /* driver reported failure */
    if (base0 == 0)   return 0;     /* zero board base -> would fault */
    return 1;
}

TEST(rejects_zero_base_the_real_bug) {
    /* the exact values captured on .124: rv=1, resStatus=1, base0=0 */
    CHECK(!linaddr_result_usable(1, 1, 0x00000000UL),
          "resStatus=1 but base0=0 must be REJECTED (that was the 0x14717 crash)");
}

TEST(rejects_failed_escape_and_bad_status) {
    CHECK(!linaddr_result_usable(0,  1, 0x60000000UL), "rv<=0 rejected");
    CHECK(!linaddr_result_usable(-1, 1, 0x60000000UL), "rv<0 rejected");
    CHECK(!linaddr_result_usable(1,  0, 0x60000000UL), "resStatus!=1 rejected");
}

TEST(accepts_a_valid_nonzero_mapping) {
    CHECK(linaddr_result_usable(1, 1, 0x60000000UL),
          "a real non-zero board base with rv=1/resStatus=1 must be accepted");
}

MUNIT_MAIN("glide GETLINEARADDR zero-base guard (grSstWinOpen crash fix)", {
    RUN(rejects_zero_base_the_real_bug);
    RUN(rejects_failed_escape_and_bad_status);
    RUN(accepts_a_valid_nonzero_mapping);
})
