/*
 * hwscan.c - deep hardware inventory for a retro PC, run via the agent's EXEC.
 *
 * Reads what the agent's built-in commands don't:
 *   - CPUID leaf 0/1: vendor, family/model/stepping, full feature-flag decode
 *     (confirms NO CMOV/MMX/SSE on a genuine Pentium).
 *   - Full PCI device list from HKLM\Enum\PCI (Win9x's PnP enumeration) -
 *     every function on the bus, incl. the host bridge (northbridge) whose
 *     device id identifies the Intel Triton chipset (430FX/HX/VX/TX), which
 *     decides the RAM ceiling and whether >64MB is even cacheable.
 *   - Memory totals + the system BIOS date string (from the F000 BIOS ROM).
 *
 * Built i586 + CMOV-free (same flags as the agent) so it runs on the P5 it's
 * inspecting. Output is plain text lines, easy to relay.
 */
#include <windows.h>
#include <stdio.h>
#include <string.h>

static void cpuid(unsigned leaf, unsigned *a, unsigned *b, unsigned *c, unsigned *d)
{
    unsigned ra, rb, rc, rd;
    __asm__ __volatile__("cpuid"
                         : "=a"(ra), "=b"(rb), "=c"(rc), "=d"(rd)
                         : "a"(leaf));
    *a = ra; *b = rb; *c = rc; *d = rd;
}

static const char *chipset_name(unsigned ven, unsigned dev)
{
    if (ven != 0x8086) return NULL;
    switch (dev) {
        case 0x122D: return "Intel 430FX (Triton) - caches <=64MB, max 128MB";
        case 0x1250: return "Intel 430HX (Triton II) - caches full RAM, max 512MB (business chipset)";
        case 0x7030: return "Intel 430VX - caches <=64MB, max 128MB";
        case 0x7100: return "Intel 430TX - caches <=64MB, max 256MB";
        case 0x1237: return "Intel 440FX (Natoma) - P6/PPro era";
        case 0x7190: return "Intel 440BX - P2/P3 era";
        default:     return NULL;
    }
}

static void decode_features(unsigned edx)
{
    printf("CPU features(edx=0x%08X):", edx);
    if (edx & (1u<<0))  printf(" FPU");
    if (edx & (1u<<4))  printf(" TSC");
    if (edx & (1u<<5))  printf(" MSR");
    if (edx & (1u<<8))  printf(" CX8");
    if (edx & (1u<<15)) printf(" CMOV"); else printf(" NO-CMOV");
    if (edx & (1u<<23)) printf(" MMX");  else printf(" NO-MMX");
    if (edx & (1u<<25)) printf(" SSE");  else printf(" NO-SSE");
    if (edx & (1u<<26)) printf(" SSE2");
    printf("\n");
}

static void read_desc(HKEY parent, const char *subkey, char *out, int cap)
{
    HKEY h, inst;
    char instname[256];
    DWORD idx = 0, n, type, sz;
    out[0] = '\0';
    if (RegOpenKeyExA(parent, subkey, 0, KEY_READ, &h) != ERROR_SUCCESS)
        return;
    /* first instance subkey */
    n = sizeof(instname);
    if (RegEnumKeyExA(h, 0, instname, &n, NULL, NULL, NULL, NULL) == ERROR_SUCCESS) {
        if (RegOpenKeyExA(h, instname, 0, KEY_READ, &inst) == ERROR_SUCCESS) {
            sz = cap; type = REG_SZ;
            RegQueryValueExA(inst, "DeviceDesc", NULL, &type, (BYTE *)out, &sz);
            out[cap - 1] = '\0';
            RegCloseKey(inst);
        }
    }
    (void)idx;
    RegCloseKey(h);
}

static void enum_pci(void)
{
    HKEY pci;
    char name[256], desc[256];
    DWORD i = 0, n;
    printf("--- PCI devices (HKLM\\Enum\\PCI) ---\n");
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "Enum\\PCI", 0, KEY_READ, &pci)
            != ERROR_SUCCESS) {
        printf("(cannot open HKLM\\Enum\\PCI)\n");
        return;
    }
    for (;; i++) {
        unsigned ven = 0, dev = 0;
        const char *cs;
        n = sizeof(name);
        if (RegEnumKeyExA(pci, i, name, &n, NULL, NULL, NULL, NULL)
                != ERROR_SUCCESS)
            break;
        /* name looks like VEN_8086&DEV_1250&SUBSYS_...&REV_.. */
        {
            char *v = strstr(name, "VEN_");
            char *d = strstr(name, "DEV_");
            if (v) sscanf(v + 4, "%4x", &ven);
            if (d) sscanf(d + 4, "%4x", &dev);
        }
        read_desc(pci, name, desc, sizeof(desc));
        printf("PCI %04X:%04X  %s\n", ven, dev, desc[0] ? desc : name);
        cs = chipset_name(ven, dev);
        if (cs)
            printf("   >>> CHIPSET: %s\n", cs);
    }
    RegCloseKey(pci);
}

static void bios_info(void)
{
    /* The BIOS date lives at physical F000:FFF5 (8 ASCII bytes mm/dd/yy).
     * Win9x maps the ROM readable at linear 0x000FFFF5. */
    const char *rom = (const char *)0x000FFFF5;
    char date[9];
    int i, ok = 1;
    for (i = 0; i < 8; i++) {
        char c = rom[i];
        if (c < 0x20 || c > 0x7E) { ok = 0; break; }
        date[i] = c;
    }
    date[8] = '\0';
    if (ok)
        printf("BIOS date (F000:FFF5): %s\n", date);
}

int main(void)
{
    unsigned a, b, c, d;
    char vendor[13];
    MEMORYSTATUS ms;

    printf("==== hwscan ====\n");

    cpuid(0, &a, &b, &c, &d);
    memcpy(vendor + 0, &b, 4);
    memcpy(vendor + 4, &d, 4);
    memcpy(vendor + 8, &c, 4);
    vendor[12] = '\0';
    printf("CPU vendor=%s cpuid_maxleaf=%u\n", vendor, a);

    cpuid(1, &a, &b, &c, &d);
    printf("CPU family=%u model=%u stepping=%u (raw eax=0x%08X)\n",
           (a >> 8) & 0xF, (a >> 4) & 0xF, a & 0xF, a);
    decode_features(d);

    ms.dwLength = sizeof(ms);
    GlobalMemoryStatus(&ms);
    printf("MEM total=%luMB avail=%luMB\n",
           (unsigned long)(ms.dwTotalPhys / (1024 * 1024)),
           (unsigned long)(ms.dwAvailPhys / (1024 * 1024)));

    bios_info();
    enum_pci();

    printf("==== end ====\n");
    return 0;
}
