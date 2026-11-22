/*
 * stallrec.exe - sub-second stall recorder for a fleet box.
 *
 * WHY THIS EXISTS: typeperf samples once a second and AVERAGES, so a 200 ms
 * input hitch - the thing players actually feel - is completely invisible to
 * it. A 5-minute typeperf trace of CS 1.6 on .123 (2026-11-21) showed hl.exe
 * holding 92-100% CPU with no starvation and no DPC storm, while the player
 * was feeling a hard hitch every 5-30 s. The instrument was wrong, not the
 * complaint.
 *
 * HOW IT MEASURES: one thread at TIME_CRITICAL inside a HIGH_PRIORITY_CLASS
 * process, looping Sleep(1) and timing the gap with QueryPerformanceCounter.
 * At that priority the only things that can keep us off the CPU for tens of
 * milliseconds are the kernel itself - an ISR, a long DPC, a paging stall, an
 * SMI, or a clock/P-state transition. A game at NORMAL priority cannot cause
 * a reading here, which is the whole point: a hit means the STACK stalled,
 * not that the box is merely busy.
 *
 * It also brackets each stall with GetSystemTimes (XP SP1+) so the log says
 * whether the lost time was kernel or idle:
 *   kernel-heavy  -> a driver (video, audio, NIC, storage) ate the core
 *   idle-heavy    -> nothing ran at all: clock throttle / SMI / bus stall
 *
 * Build (on the dev host):
 *   i686-w64-mingw32-gcc -O2 -s -o stallrec.exe stallrec.c
 * No SSE/SSE2 is assumed, so it is safe on every box in the fleet.
 *
 * Usage:  stallrec.exe <seconds> <threshold_ms> <logfile>
 *   e.g.  stallrec.exe 300 20 C:\stalls.txt
 * Writes and flushes a line per stall, plus a summary, so a kill loses nothing.
 */
#include <windows.h>
#include <stdio.h>

typedef BOOL (WINAPI *GETSYSTIMES)(LPFILETIME, LPFILETIME, LPFILETIME);
typedef UINT (WINAPI *TIMEPERIOD)(UINT);

/*
 * THE CLOCK CAN MASQUERADE AS A STALL - this cost a 1.2 MB log of nothing.
 * XP's default timer tick is 15.6 ms, so Sleep(1) returns in ~15.6 ms and ANY
 * threshold at or below that flags every single loop iteration: a 25-minute
 * run on .123 produced 12,944 "stalls" of 15.3-15.9 ms on a box that had not
 * stalled once. So: pin the tick to 1 ms with timeBeginPeriod, MEASURE the
 * granularity we actually got, and refuse a threshold that sits inside it.
 */
#define GRAN_SAMPLES 200

static double ft_ms(FILETIME a, FILETIME b) /* b - a, in ms */
{
    ULONGLONG x = ((ULONGLONG)a.dwHighDateTime << 32) | a.dwLowDateTime;
    ULONGLONG y = ((ULONGLONG)b.dwHighDateTime << 32) | b.dwLowDateTime;
    return (double)(y - x) / 10000.0;
}

int main(int argc, char **argv)
{
    int secs = (argc > 1) ? atoi(argv[1]) : 300;
    double thresh = (argc > 2) ? atof(argv[2]) : 20.0;
    const char *path = (argc > 3) ? argv[3] : "C:\\stalls.txt";
    LARGE_INTEGER freq, prev, now, t0;
    GETSYSTIMES pGST;
    FILETIME i0, k0, u0, i1, k1, u1;
    FILE *f;
    SYSTEMTIME st;
    double gap, worst = 0.0, total_lost = 0.0, granularity = 0.0, floor_ms = 0.0;
    long hits = 0, loops = 0;
    int have_times = 0;

    if (secs <= 0) secs = 300;
    if (thresh <= 0.0) thresh = 20.0;

    if (!QueryPerformanceFrequency(&freq) || freq.QuadPart == 0) {
        printf("no high-resolution timer on this machine\n");
        return 2;
    }
    f = fopen(path, "w");
    if (!f) { printf("cannot write %s\n", path); return 2; }

    pGST = (GETSYSTIMES)GetProcAddress(GetModuleHandleA("kernel32.dll"),
                                       "GetSystemTimes");
    have_times = (pGST != NULL);

    /* Pin the timer tick to 1 ms if we can, then measure what we really got. */
    {
        HMODULE wm = LoadLibraryA("winmm.dll");
        TIMEPERIOD pBegin = wm ? (TIMEPERIOD)GetProcAddress(wm, "timeBeginPeriod")
                               : NULL;
        if (pBegin) pBegin(1);
    }
    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

    /* Measure Sleep(1) granularity so the floor below is a fact, not a guess. */
    {
        int i;
        double worst_gran = 0.0;
        LARGE_INTEGER a, b;
        QueryPerformanceCounter(&a);
        for (i = 0; i < GRAN_SAMPLES; i++) {
            Sleep(1);
            QueryPerformanceCounter(&b);
            gap = (double)(b.QuadPart - a.QuadPart) * 1000.0 / (double)freq.QuadPart;
            if (gap > worst_gran) worst_gran = gap;
            a = b;
        }
        granularity = worst_gran;
        floor_ms = granularity * 2.0 + 5.0;
        if (thresh < floor_ms) thresh = floor_ms;
    }

    GetLocalTime(&st);
    fprintf(f, "stallrec: started %04d-%02d-%02d %02d:%02d:%02d  "
               "run=%ds threshold=%.1fms systimes=%s\n",
            st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
            secs, thresh, have_times ? "yes" : "no");
    fprintf(f, "timer granularity measured at %.1f ms -> threshold floor %.1f ms"
               " (a threshold inside the tick flags every loop)\n",
            granularity, floor_ms);
    fprintf(f, "a hit = the KERNEL kept a TIME_CRITICAL thread off the CPU "
               "this long. A busy game cannot cause one.\n");
    fflush(f);

    QueryPerformanceCounter(&t0);
    prev = t0;
    if (have_times) pGST(&i0, &k0, &u0);

    for (;;) {
        Sleep(1);
        QueryPerformanceCounter(&now);
        gap = (double)(now.QuadPart - prev.QuadPart) * 1000.0
              / (double)freq.QuadPart;
        loops++;
        if (gap >= thresh) {
            hits++;
            total_lost += gap;
            if (gap > worst) worst = gap;
            GetLocalTime(&st);
            if (have_times) {
                double di, dk, du;
                pGST(&i1, &k1, &u1);
                di = ft_ms(i0, i1); dk = ft_ms(k0, k1); du = ft_ms(u0, u1);
                fprintf(f, "%02d:%02d:%02d.%03d  STALL %8.1f ms   "
                           "kernel %6.1f  user %6.1f  idle %6.1f  -> %s\n",
                        st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                        gap, dk, du, di,
                        (di > dk && di > du) ? "NOTHING RAN (throttle/SMI/bus)"
                                             : (dk >= du ? "KERNEL (driver)"
                                                         : "user code"));
                i0 = i1; k0 = k1; u0 = u1;
            } else {
                fprintf(f, "%02d:%02d:%02d.%03d  STALL %8.1f ms\n",
                        st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                        gap);
            }
            fflush(f);
        } else if (have_times && (loops % 1000) == 0) {
            pGST(&i0, &k0, &u0);   /* keep the bracket fresh between hits */
        }
        prev = now;
        if ((double)(now.QuadPart - t0.QuadPart) / (double)freq.QuadPart
            >= (double)secs) break;
    }

    GetLocalTime(&st);
    fprintf(f, "\nstallrec: finished %02d:%02d:%02d  %ld stall(s) over %ds  "
               "worst %.1f ms  total lost %.1f ms  (%.2f stalls/min)\n",
            st.wHour, st.wMinute, st.wSecond, hits, secs, worst, total_lost,
            secs ? (double)hits * 60.0 / (double)secs : 0.0);
    fclose(f);
    printf("stallrec done: %ld stall(s), worst %.1f ms -> %s\n",
           hits, worst, path);
    return 0;
}
