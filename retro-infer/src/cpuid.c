/*
 * CPU capability detection for retro-infer.
 *
 * Fleet spread: Pentium/K6-2 (MMX, 3DNow!), P3 (SSE), P4/Athlon XP (SSE/SSE2).
 * Binary is compiled -march=i586, so nothing here may execute an SSE/MMX
 * instruction — CPUID bits only. OS support: Win98SE/2K/XP all enable FXSR
 * when the CPU has it, so CPUID is sufficient on this fleet.
 */
#include <string.h>
#include "infer.h"

#if !defined(__i386__)
/* Host (x86_64) build: capability set for local parity testing only.
 * 3DNow! doesn't exist on x86_64, so that path is never host-tested. */
void cpu_detect(cpu_caps_t *caps)
{
    memset(caps, 0, sizeof(*caps));
    caps->has_cpuid = 1;
    caps->mmx = caps->sse = caps->sse2 = 1;
    strcpy(caps->vendor, "HostBuild");
    strcpy(caps->brand, "linux host build (parity testing)");
}
#else

static void cpuid_raw(unsigned leaf, unsigned *a, unsigned *b, unsigned *c,
                      unsigned *d)
{
    __asm__ __volatile__(
        "cpuid"
        : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
        : "a"(leaf), "c"(0));
}

/* EFLAGS bit 21 (ID) is writable iff CPUID exists (486 vs 586 distinction). */
static int cpuid_present(void)
{
    unsigned before, after;
    __asm__ __volatile__(
        "pushfl\n\t"
        "popl %0\n\t"
        "movl %0, %1\n\t"
        "xorl $0x200000, %0\n\t"
        "pushl %0\n\t"
        "popfl\n\t"
        "pushfl\n\t"
        "popl %0\n\t"
        "pushl %1\n\t"          /* restore original EFLAGS */
        "popfl"
        : "=&r"(after), "=&r"(before));
    return ((before ^ after) & 0x200000u) != 0;
}

void cpu_detect(cpu_caps_t *caps)
{
    unsigned a, b, c, d, max_leaf, max_ext;

    memset(caps, 0, sizeof(*caps));
    caps->has_cpuid = cpuid_present();
    if (!caps->has_cpuid)
        return;

    cpuid_raw(0, &max_leaf, &b, &c, &d);
    memcpy(caps->vendor + 0, &b, 4);
    memcpy(caps->vendor + 4, &d, 4);
    memcpy(caps->vendor + 8, &c, 4);
    caps->vendor[12] = '\0';

    if (max_leaf >= 1) {
        cpuid_raw(1, &a, &b, &c, &d);
        caps->stepping = a & 0xF;
        caps->model    = (a >> 4) & 0xF;
        caps->family   = (a >> 8) & 0xF;
        if (caps->family == 0xF) {
            caps->family += (a >> 20) & 0xFF;
            caps->model  |= ((a >> 16) & 0xF) << 4;
        }
        caps->mmx  = (d >> 23) & 1;
        caps->sse  = (d >> 25) & 1;
        caps->sse2 = (d >> 26) & 1;
    }

    cpuid_raw(0x80000000u, &max_ext, &b, &c, &d);
    if (max_ext >= 0x80000001u) {
        cpuid_raw(0x80000001u, &a, &b, &c, &d);
        caps->amd3dnow    = (d >> 31) & 1;
        caps->amd3dnowext = (d >> 30) & 1;
        /* AMD MMX-ext / K6-2 report MMX here too */
        caps->mmx |= (d >> 23) & 1;
    }
    if (max_ext >= 0x80000004u) {
        unsigned *p = (unsigned *)caps->brand;
        cpuid_raw(0x80000002u, p + 0, p + 1, p + 2, p + 3);
        cpuid_raw(0x80000003u, p + 4, p + 5, p + 6, p + 7);
        cpuid_raw(0x80000004u, p + 8, p + 9, p + 10, p + 11);
        caps->brand[48] = '\0';
    }
}
#endif /* __i386__ */
