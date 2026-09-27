/* stale_owner.c - bed-only probe for the `sliaa off` integration fix.
 *   stale_owner set   HWCSETEXCLUSIVE, then exit WITHOUT a release: the owner
 *                     a Glide program (or a successful `sliaa` enable) leaves
 *                     behind when it exits or is killed
 *   stale_owner rls   HWCRLSEXCLUSIVE from a process that is NOT the owner:
 *                     what `sliaa off` did before 5e4ca36 (must be refused)
 * No mode is switched by either (a refused release makes no mode set). */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "vcr_types.h"
#include "vcr_hwcext.h"

int main(int argc, char **argv)
{
    vcr_hwc_req rq;
    vcr_hwc_res rs;
    HDC dc = GetDC(NULL);
    int n, set = argc > 1 && !strcmp(argv[1], "set");
    memset(&rq, 0, sizeof rq);
    memset(&rs, 0, sizeof rs);
    rq.which = set ? VCR_HWC_HWCSETEXCLUSIVE : VCR_HWC_HWCRLSEXCLUSIVE;
    n = ExtEscape(dc, VCR_EXT_HWC, sizeof rq, (LPCSTR)&rq, sizeof rs, (LPSTR)&rs);
    printf("{\"cmd\":\"stale_owner %s\",\"pid\":%lu,\"ret\":%d,\"resStatus\":%d}\n",
           set ? "set" : "rls", GetCurrentProcessId(), n, (int)rs.resStatus);
    return n > 0 && rs.resStatus == VCR_HWC_OK ? 0 : 1;
}
