#include "port.h"

#ifdef _WIN32
#include <windows.h>

double ri_now(void)
{
    LARGE_INTEGER f, t;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart / (double)f.QuadPart;
}

unsigned long ri_ram_mb(int avail)
{
    MEMORYSTATUS ms;
    ms.dwLength = sizeof(ms);
    GlobalMemoryStatus(&ms);
    return (unsigned long)((avail ? ms.dwAvailPhys : ms.dwTotalPhys) /
                           (1024 * 1024));
}

#else /* Linux host build */
#include <time.h>
#include <unistd.h>

double ri_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

unsigned long ri_ram_mb(int avail)
{
    long pages = sysconf(avail ? _SC_AVPHYS_PAGES : _SC_PHYS_PAGES);
    long psize = sysconf(_SC_PAGESIZE);
    if (pages < 0 || psize < 0)
        return 0;
    return (unsigned long)((unsigned long long)pages * psize / (1024 * 1024));
}
#endif
