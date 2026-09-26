/*
 * vcrdd_log.c - the display driver's side of the flight recorder: one IOCTL
 * per entry into the miniport's ring, so the DLL's steps interleave with the
 * miniport's in a single timeline. Never on a drawing path.
 */
#include "vcrdd.h"

HANDLE g_hDriver;

DWORD VcrIoctl(HANDLE h, DWORD code, PVOID in, DWORD cin, PVOID out, DWORD cout,
               DWORD *got)
{
    DWORD n = 0, rc;
    rc = EngDeviceIoControl(h, code, in, cin, out, cout, &n);
    if (got)
        *got = n;
    return rc;
}

void VcrDd(ULONG level, ULONG code, ULONG a, ULONG b, ULONG c, ULONG d,
           const char *fmt, ...)
{
    vcr_log_write_req r;
    va_list ap;
    if (!g_hDriver)
        return;
    r.code = code;
    r.level = level;
    r.src = VCR_SRC_DISPLAY;
    r.pid = (ULONG)(ULONG_PTR)EngGetCurrentProcessId();
    r.a = a;
    r.b = b;
    r.c = c;
    r.d = d;
    va_start(ap, fmt);
    vcr_vsnprintf(r.msg, sizeof r.msg, fmt, ap);
    va_end(ap);
    VcrIoctl(g_hDriver, IOCTL_VCR_LOG_WRITE, &r, sizeof r, NULL, 0, NULL);
}
