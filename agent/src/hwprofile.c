/*
 * hwprofile.c - HWPROFILE: what this machine actually IS, in one stable JSON.
 *
 * WHY THIS EXISTS RATHER THAN "just use SYSINFO". SYSINFO reports what
 * GetSystemInfo/GlobalMemoryStatus hand out, and for deciding whether a box can
 * run a game that is not enough:
 *
 *   - NO CLOCK SPEED. wProcessorLevel says "family 6"; a 500 MHz Pentium III
 *     and an 1100 MHz Pentium III are both family 6, and the whole question is
 *     which one this is.
 *   - NO VENDOR. Family 6 / model 2 is an AMD Athlon; family 15 / model 39 is
 *     an Athlon 64. Both were being read here as Intel P6 and Pentium 4 purely
 *     from the family number. Only the CPUID vendor string settles it.
 *   - NO INSTRUCTION SET. SSE2 absent is #UD on the first vectorised
 *     instruction - a hard, immediate crash, not a slow frame rate - and
 *     nothing in SYSINFO can tell you.
 *   - NO GPU AT ALL. Not the name, not the PCI ids, not the video RAM.
 *   - RAM CLAMPED. GlobalMemoryStatus saturates at 2047 MB, which is what
 *     three fleet boxes report; GlobalMemoryStatusEx gives the real figure.
 *
 * THE ADAPTER TRAP. VIDEODIAG enumerates every Class\{4D36E968-...}\NNNN
 * subkey, so its adapters[0] is the first REGISTRY KEY - which on a box that
 * has ever had a card swapped is a stale entry for hardware that is no longer
 * fitted. (It has already caused one wrong report that a box had no video
 * driver.) This file asks EnumDisplayDevices for the adapter that is ATTACHED
 * TO THE DESKTOP - the one actually drawing the screen - and follows its own
 * DeviceKey into the registry, so a dead key cannot be picked up. The PCI ids
 * are then cross-checked against the same DeviceID string.
 *
 * PROFILE HASH. gg_profile_hash() (agent/shared/gamegate.h) folds only the
 * hardware fields, so it is identical across reboots and identical between two
 * boxes built the same. That is what makes the host-side verdict cache hit.
 */

#include "handlers.h"
#include "protocol.h"
#include "util.h"
#include "log.h"
#include "hwextra.h"
#include "../shared/hwpub.h"
#include "../shared/gamegate.h"
#include "../shared/edid.h"

/* ENUM_REGISTRY_SETTINGS: the PERSISTED display mode. Absent from the old
 * mingw headers this agent is built against, so define it if need be. */
#ifndef ENUM_REGISTRY_SETTINGS
#define ENUM_REGISTRY_SETTINGS ((DWORD)-2)
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The agent is built at WINVER=0x0410 for Win98SE, where these display-device
 * constants are not declared. They are plain bit values in the ABI, so define
 * them rather than raising WINVER for the whole binary. */
#ifndef DISPLAY_DEVICE_ATTACHED_TO_DESKTOP
#define DISPLAY_DEVICE_ATTACHED_TO_DESKTOP 0x00000001
#endif
#ifndef DISPLAY_DEVICE_PRIMARY_DEVICE
#define DISPLAY_DEVICE_PRIMARY_DEVICE      0x00000004
#endif

#define LOG_HW "HWPROFILE"

/* ------------------------------------------------------------------ */
/* CPUID                                                                */
/* ------------------------------------------------------------------ */

/* Does this CPU have CPUID at all? A 486 below the SL-enhanced steppings does
 * not, and executing CPUID there is an invalid opcode. The fleet is Pentium
 * and up, but the agent also has to not crash on whatever gets plugged in
 * next, and the EFLAGS ID-bit toggle is the documented way to ask. */
static int cpu_has_cpuid(void)
{
    unsigned a = 0, b = 0;
    __asm__ __volatile__(
        "pushfl\n\t"
        "pushfl\n\t"
        "popl %0\n\t"
        "movl %0, %1\n\t"
        "xorl $0x00200000, %0\n\t"
        "pushl %0\n\t"
        "popfl\n\t"
        "pushfl\n\t"
        "popl %0\n\t"
        "popfl"
        : "=&r" (a), "=&r" (b)
        :
        : "cc");
    return ((a ^ b) & 0x00200000u) != 0;
}

/* CPUID with EBX saved around it. EBX is the PIC register on i386 and GCC will
 * not let it be clobbered directly; the xchg pair is the portable idiom. */
static void cpuid(unsigned leaf, unsigned sub, unsigned r[4])
{
    __asm__ __volatile__(
        "xchgl %%ebx, %1\n\t"
        "cpuid\n\t"
        "xchgl %%ebx, %1"
        : "=a" (r[0]), "=&r" (r[1]), "=c" (r[2]), "=d" (r[3])
        : "0" (leaf), "1" (0), "2" (sub));
}

static unsigned long long read_tsc(void)
{
    unsigned long long v;
    __asm__ __volatile__("rdtsc" : "=A" (v));
    return v;
}

/* Fill the CPU half of the profile. Returns 0 when CPUID is unavailable, in
 * which case the caller falls back to GetSystemInfo's coarse family number. */
static int cpu_identify(gg_profile_t *p, char *brand, DWORD brand_cch,
                        int *has_tsc)
{
    unsigned r[4];
    unsigned maxleaf, maxext, family, model;

    *has_tsc = 0;
    brand[0] = 0;
    if (!cpu_has_cpuid())
        return 0;

    cpuid(0, 0, r);
    maxleaf = r[0];
    memcpy(p->cpu_vendor + 0, &r[1], 4);
    memcpy(p->cpu_vendor + 4, &r[3], 4);
    memcpy(p->cpu_vendor + 8, &r[2], 4);
    p->cpu_vendor[12] = 0;

    if (maxleaf < 1)
        return 1;

    cpuid(1, 0, r);
    family = (r[0] >> 8) & 0xF;
    model  = (r[0] >> 4) & 0xF;
    /* Extended family/model. Only meaningful for family 15 (and 6 for the
     * extended model), which is how a Sandy Bridge reports 6/42 rather than
     * 6/10 - and 6/10 is a Pentium III Katmai. Getting this wrong misreads a
     * 2011 quad core as a 1999 uniprocessor. */
    if (family == 15)
        family += (r[0] >> 20) & 0xFF;
    if (family == 6 || family == 15)
        model |= ((r[0] >> 16) & 0xF) << 4;

    p->cpu_family   = family;
    p->cpu_model    = model;
    p->cpu_stepping = r[0] & 0xF;

    if (r[3] & (1u << 0))  p->cpu_features |= GG_CPU_FPU;
    if (r[3] & (1u << 4))  *has_tsc = 1;
    if (r[3] & (1u << 15)) p->cpu_features |= GG_CPU_CMOV;
    if (r[3] & (1u << 23)) p->cpu_features |= GG_CPU_MMX;
    if (r[3] & (1u << 25)) p->cpu_features |= GG_CPU_SSE;
    if (r[3] & (1u << 26)) p->cpu_features |= GG_CPU_SSE2;
    if (r[2] & (1u << 0))  p->cpu_features |= GG_CPU_SSE3;
    if (r[2] & (1u << 9))  p->cpu_features |= GG_CPU_SSSE3;
    if (r[2] & (1u << 19)) p->cpu_features |= GG_CPU_SSE41;

    cpuid(0x80000000u, 0, r);
    maxext = r[0];
    if (maxext >= 0x80000001u) {
        cpuid(0x80000001u, 0, r);
        if (r[3] & (1u << 31)) p->cpu_features |= GG_CPU_3DNOW;
    }
    /* The brand string is an extended leaf the Coppermine Pentium III does not
     * implement, so an empty result here is normal on half the fleet and the
     * caller falls back to the registry's ProcessorNameString. */
    if (maxext >= 0x80000004u && brand_cch >= 49) {
        unsigned leaf;
        char *w = brand;
        for (leaf = 0x80000002u; leaf <= 0x80000004u; leaf++) {
            cpuid(leaf, 0, r);
            memcpy(w, r, 16);
            w += 16;
        }
        brand[48] = 0;
        /* Intel pads the brand string with leading spaces. */
        while (*brand == ' ')
            memmove(brand, brand + 1, strlen(brand));
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* Clock speed                                                          */
/* ------------------------------------------------------------------ */

/* NT writes the nominal clock into the hardware description tree at boot.
 * PREFER THIS: it is the rated speed, it never jitters, and reading it costs
 * nothing - a measured value that wobbles by 3 MHz between polls would change
 * the profile hash and defeat the cache. */
static DWORD cpu_mhz_from_registry(char *name, DWORD name_cch)
{
    HKEY h;
    DWORD type, mhz = 0, size = sizeof(mhz);

    if (name && name_cch)
        name[0] = 0;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE,
                      "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
                      0, KEY_QUERY_VALUE, &h) != ERROR_SUCCESS)
        return 0;
    if (RegQueryValueExA(h, "~MHz", NULL, &type, (BYTE *)&mhz, &size)
            != ERROR_SUCCESS || type != REG_DWORD)
        mhz = 0;
    if (name && name_cch) {
        DWORD ncch = name_cch;
        if (RegQueryValueExA(h, "ProcessorNameString", NULL, &type,
                             (BYTE *)name, &ncch) != ERROR_SUCCESS)
            name[0] = 0;
        else
            name[name_cch - 1] = 0;
        while (name[0] == ' ')
            memmove(name, name + 1, strlen(name));
    }
    RegCloseKey(h);
    return mhz;
}

/* Win9x has no such registry value, so there the clock has to be MEASURED:
 * count TSC ticks across a known wall-clock interval. Deliberately short (the
 * agent is answering a command) and deliberately rounded to 5 MHz, because the
 * raw figure moves by a few MHz between runs and the profile hash must not. */
static DWORD cpu_mhz_measure(void)
{
    LARGE_INTEGER f, t0, t1;
    unsigned long long c0, c1, cycles;
    double secs, mhz;

    if (!QueryPerformanceFrequency(&f) || f.QuadPart == 0)
        return 0;
    QueryPerformanceCounter(&t0);
    c0 = read_tsc();
    Sleep(120);
    c1 = read_tsc();
    QueryPerformanceCounter(&t1);

    if (t1.QuadPart <= t0.QuadPart || c1 <= c0)
        return 0;
    cycles = c1 - c0;
    secs = (double)(t1.QuadPart - t0.QuadPart) / (double)f.QuadPart;
    if (secs <= 0.0)
        return 0;
    mhz = ((double)cycles / secs) / 1000000.0;
    if (mhz < 20.0 || mhz > 12000.0)
        return 0;
    return (DWORD)(((unsigned)(mhz + 2.5)) / 5u * 5u);
}

/* ------------------------------------------------------------------ */
/* Memory                                                               */
/* ------------------------------------------------------------------ */

typedef struct {
    DWORD          dwLength;
    DWORD          dwMemoryLoad;
    unsigned __int64 ullTotalPhys;
    unsigned __int64 ullAvailPhys;
    unsigned __int64 ullTotalPageFile;
    unsigned __int64 ullAvailPageFile;
    unsigned __int64 ullTotalVirtual;
    unsigned __int64 ullAvailVirtual;
    unsigned __int64 ullAvailExtendedVirtual;
} HW_MEMORYSTATUSEX;

typedef BOOL (WINAPI *PFN_GMSEx)(HW_MEMORYSTATUSEX *);

/* GlobalMemoryStatus saturates every field at 2 GB, so .123, .145 and .246 all
 * report exactly 2047 MB whatever they really have. GlobalMemoryStatusEx (2000
 * and later, absent on 98) reports the truth; fall back where it is missing. */
static DWORD ram_total_mb(void)
{
    PFN_GMSEx ex = (PFN_GMSEx)GetProcAddress(GetModuleHandleA("kernel32.dll"),
                                             "GlobalMemoryStatusEx");
    if (ex) {
        HW_MEMORYSTATUSEX m;
        memset(&m, 0, sizeof(m));
        m.dwLength = sizeof(m);
        if (ex(&m) && m.ullTotalPhys)
            return (DWORD)(m.ullTotalPhys / (1024 * 1024));
    }
    {
        MEMORYSTATUS ms;
        ms.dwLength = sizeof(ms);
        GlobalMemoryStatus(&ms);
        return (DWORD)(ms.dwTotalPhys / (1024 * 1024));
    }
}

/* ------------------------------------------------------------------ */
/* Display adapter - the ACTIVE one                                     */
/* ------------------------------------------------------------------ */

typedef BOOL (WINAPI *PFN_EnumDisplayDevicesA)(LPCSTR, DWORD,
                                               PDISPLAY_DEVICEA, DWORD);

/*
 * Read a registry value as a string, INCLUDING when it is not stored as one.
 *
 * FOUND ON .246 (Windows 7, 2026-08-30): the display class key's DriverDesc is
 * a **REG_BINARY** holding UTF-16LE. RegQueryValueExA converts REG_SZ for you
 * and hands REG_BINARY back RAW, so as a C string it ends at the first NUL -
 * after ONE character. The box reported its graphics card as "A": short,
 * printable, entirely plausible, and flagged by nothing. hwpub_utf16le_narrow()
 * detects that layout; anything that is neither a string nor UTF-16 in
 * disguise yields nothing rather than a byte soup that reads as a short name.
 */
static void reg_str(HKEY h, const char *name, char *buf, DWORD cch)
{
    DWORD type = REG_SZ, size;
    BYTE  raw[512];

    buf[0] = 0;
    size = sizeof(raw);
    if (RegQueryValueExA(h, name, NULL, &type, raw, &size) != ERROR_SUCCESS)
        return;

    if (type == REG_SZ || type == REG_EXPAND_SZ) {
        if (size >= cch)
            size = cch - 1;
        memcpy(buf, raw, size);
        buf[size] = 0;
    } else if (!hwpub_utf16le_narrow(raw, (int)size, buf, (int)cch)) {
        buf[0] = 0;
    }
    buf[cch - 1] = 0;
}

/* HardwareInformation.MemorySize is REG_BINARY on XP (4 bytes) and can be
 * REG_QWORD or an 8-byte REG_BINARY on Win7. Take whichever, little-endian. */
static DWORD reg_memsize_mb(HKEY h)
{
    BYTE   buf[16];
    DWORD  type = 0, size = sizeof(buf);
    unsigned __int64 bytes = 0;

    if (RegQueryValueExA(h, "HardwareInformation.MemorySize", NULL, &type,
                         buf, &size) != ERROR_SUCCESS)
        return 0;
    if (size >= 8)
        memcpy(&bytes, buf, 8);
    else if (size >= 4) {
        DWORD d = 0;
        memcpy(&d, buf, 4);
        bytes = d;
    } else
        return 0;
    /* Some drivers store megabytes here rather than bytes. A value under 4096
     * cannot be a byte count for a card that exists, so read it as MB. */
    if (bytes && bytes < 4096)
        return (DWORD)bytes;
    return (DWORD)(bytes / (1024 * 1024));
}

/* Pull VEN/DEV out of a hardware id / device id string, case-insensitively.
 * Windows writes "PCI\VEN_10DE&DEV_0150&..." in the enum and
 * "pci\ven_10de&dev_0150" in MatchingDeviceId - the same fact in two cases,
 * and a case-sensitive search finds only one of them. */
static int parse_ven_dev(const char *s, unsigned *ven, unsigned *dev)
{
    const char *p;
    char up[512];
    DWORD i;

    *ven = *dev = 0;
    if (!s || !*s)
        return 0;
    for (i = 0; i < sizeof(up) - 1 && s[i]; i++)
        up[i] = (char)((s[i] >= 'a' && s[i] <= 'z') ? s[i] - 32 : s[i]);
    up[i] = 0;

    p = strstr(up, "VEN_");
    if (p)
        *ven = (unsigned)strtoul(p + 4, NULL, 16);
    p = strstr(up, "DEV_");
    if (p)
        *dev = (unsigned)strtoul(p + 4, NULL, 16);
    return (*ven != 0);
}

typedef struct {
    char     name[256];          /* adapter description as Windows shows it */
    char     hwid[256];          /* the DeviceID / MatchingDeviceId string   */
    char     driver_version[64];
    char     driver_date[64];
    char     source[48];         /* how we found it - so a wrong answer is traceable */
    unsigned ven, dev;
    DWORD    vram_mb;
} hw_gpu_t;

/* Walk the display class key and read DriverVersion for the card whose
 * MatchingDeviceId matches the ids we already have from the live adapter.
 * NOTE the direction: the live adapter is the source of truth and this only
 * ANNOTATES it. Searching the class key first is what makes VIDEODIAG pick
 * stale hardware. */
static void gpu_annotate_from_class(hw_gpu_t *g, int is_nt)
{
    HKEY  hbase;
    const char *base = is_nt
        ? "SYSTEM\\CurrentControlSet\\Control\\Class\\"
          "{4D36E968-E325-11CE-BFC1-08002BE10318}"
        : "System\\CurrentControlSet\\Services\\Class\\Display";
    DWORD index;

    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, base, 0, KEY_READ, &hbase)
            != ERROR_SUCCESS)
        return;

    for (index = 0; ; index++) {
        char  sub[64], full[512], match[256], desc[256];
        DWORD cch = sizeof(sub);
        HKEY  h;
        unsigned ven = 0, dev = 0;

        if (RegEnumKeyExA(hbase, index, sub, &cch, NULL, NULL, NULL, NULL)
                != ERROR_SUCCESS)
            break;
        _snprintf(full, sizeof(full) - 1, "%s\\%s", base, sub);
        full[sizeof(full) - 1] = 0;
        if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, full, 0, KEY_READ, &h)
                != ERROR_SUCCESS)
            continue;

        reg_str(h, "MatchingDeviceId", match, sizeof(match));
        reg_str(h, "DriverDesc", desc, sizeof(desc));
        parse_ven_dev(match, &ven, &dev);

        if (ven && g->ven && ven == g->ven && dev == g->dev) {
            reg_str(h, "DriverVersion", g->driver_version,
                    sizeof(g->driver_version));
            reg_str(h, "DriverDate", g->driver_date, sizeof(g->driver_date));
            if (!g->vram_mb)
                g->vram_mb = reg_memsize_mb(h);
            if (!g->name[0] && desc[0])
                safe_strncpy(g->name, desc, sizeof(g->name));
            RegCloseKey(h);
            break;
        }
        RegCloseKey(h);
    }
    RegCloseKey(hbase);
}


/*
 * WIN9x ONLY: find the display adapter's PCI ids by walking HKLM\Enum\PCI.
 *
 * THIS IS THE ONE PATH NT NEVER NEEDS, AND IT IS THE ONE THAT DECIDES THE
 * GATE. On the NT family EnumDisplayDevices hands back a DeviceID string with
 * VEN_/DEV_ in it and everything above works. On Windows 98 it very often does
 * not: the API exists but its DeviceID is frequently empty, and the display
 * CLASS key there (System\CurrentControlSet\Services\Class\Display\NNNN) has no
 * MatchingDeviceId value at all - that is an NT invention - so
 * gpu_annotate_from_class finds nothing either. The old fallback read
 * DriverDesc and stopped, which left gpu_ven/gpu_dev at 0.
 *
 * Zero is not a harmless "unknown" here. gg_gpu_level_from_pci(0, 0) returns
 * GG_GPU_UNKNOWN, the gate FAILS OPEN on unknown by design, and the result is
 * that the machine with the weakest graphics in the entire fleet is the one
 * machine whose GPU is never gated - every Direct3D-only title approved onto a
 * Pentium-1 with a 2D-only VGA. The fail-open default is right; being unable
 * to answer on the box that needs the answer is not.
 *
 * Win9x binds the two halves the other way round from NT: the PCI instance
 * key carries "Driver" = "Display\0000", pointing AT the class key. So walk
 * Enum\PCI\<VEN_xxxx&DEV_xxxx>\<instance>, take the first one whose Driver
 * binding starts with "Display\", and read the ids out of the device key's own
 * NAME - which is where Windows put them.
 */
static int gpu_ids_from_win9x_enum(hw_gpu_t *g)
{
    static const char *const roots[] = {
        "Enum\\PCI",                                /* Win95/98/ME */
        "System\\CurrentControlSet\\Enum\\PCI",     /* belt and braces */
        0
    };
    int ri;

    for (ri = 0; roots[ri]; ri++) {
        HKEY hpci;
        DWORD di;

        if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, roots[ri], 0, KEY_READ, &hpci)
                != ERROR_SUCCESS)
            continue;

        for (di = 0; ; di++) {
            char  dev[128], devpath[512];
            DWORD cch = sizeof(dev);
            HKEY  hdev;
            DWORD ii;
            unsigned ven = 0, id = 0;

            if (RegEnumKeyExA(hpci, di, dev, &cch, NULL, NULL, NULL, NULL)
                    != ERROR_SUCCESS)
                break;
            /* parse_ven_dev upper-cases first, so a lower-case Win98 key name
             * ("ven_5333&dev_8901") matches as well as an upper-case one. */
            if (!parse_ven_dev(dev, &ven, &id) || !ven)
                continue;

            _snprintf(devpath, sizeof(devpath) - 1, "%s\\%s", roots[ri], dev);
            devpath[sizeof(devpath) - 1] = 0;
            if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, devpath, 0, KEY_READ, &hdev)
                    != ERROR_SUCCESS)
                continue;

            for (ii = 0; ; ii++) {
                char  inst[64], instpath[640], binding[64], desc[256];
                DWORD icch = sizeof(inst);
                HKEY  hinst;

                if (RegEnumKeyExA(hdev, ii, inst, &icch, NULL, NULL, NULL, NULL)
                        != ERROR_SUCCESS)
                    break;
                _snprintf(instpath, sizeof(instpath) - 1, "%s\\%s", devpath,
                          inst);
                instpath[sizeof(instpath) - 1] = 0;
                if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, instpath, 0, KEY_READ,
                                  &hinst) != ERROR_SUCCESS)
                    continue;
                reg_str(hinst, "Driver", binding, sizeof(binding));
                reg_str(hinst, "DeviceDesc", desc, sizeof(desc));
                RegCloseKey(hinst);

                /* "Display\0000" - case-insensitively, because this is a
                 * Windows registry value and we are a Linux-built binary
                 * reasoning about one. */
                if (_strnicmp(binding, "Display\\", 8) != 0)
                    continue;

                g->ven = ven;
                g->dev = id;
                if (!g->name[0] && desc[0])
                    safe_strncpy(g->name, desc, sizeof(g->name));
                if (!g->hwid[0]) {
                    _snprintf(g->hwid, sizeof(g->hwid) - 1, "PCI\\%s", dev);
                    g->hwid[sizeof(g->hwid) - 1] = 0;
                }
                safe_strncpy(g->source, "Enum\\PCI(Driver=Display)",
                             sizeof(g->source));

                /* The class key the binding names carries the video RAM and
                 * the driver description on 9x, where there is no
                 * MatchingDeviceId to find it by. */
                {
                    char ckey[320];
                    HKEY hc;
                    _snprintf(ckey, sizeof(ckey) - 1,
                              "System\\CurrentControlSet\\Services\\Class\\%s",
                              binding);
                    ckey[sizeof(ckey) - 1] = 0;
                    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, ckey, 0, KEY_READ,
                                      &hc) == ERROR_SUCCESS) {
                        if (!g->vram_mb)
                            g->vram_mb = reg_memsize_mb(hc);
                        if (!g->driver_version[0])
                            reg_str(hc, "DriverVersion", g->driver_version,
                                    sizeof(g->driver_version));
                        if (!g->name[0])
                            reg_str(hc, "DriverDesc", g->name,
                                    sizeof(g->name));
                        RegCloseKey(hc);
                    }
                }
                RegCloseKey(hdev);
                RegCloseKey(hpci);
                return 1;
            }
            RegCloseKey(hdev);
        }
        RegCloseKey(hpci);
    }
    return 0;
}

/*
 * Identify the adapter that is DRAWING THE SCREEN.
 *
 * EnumDisplayDevices is enumerated in full and filtered on
 * DISPLAY_DEVICE_ATTACHED_TO_DESKTOP, preferring the primary. Index 0 is not
 * good enough on a box with a mirroring driver or a leftover secondary: the
 * first entry can be a "RDPDD Chained DD" or a disconnected head.
 */
static void gpu_identify(hw_gpu_t *g, int is_nt)
{
    PFN_EnumDisplayDevicesA pfn;
    DISPLAY_DEVICEA dd, best;
    DWORD i;
    int found = 0;

    memset(g, 0, sizeof(*g));
    memset(&best, 0, sizeof(best));

    pfn = (PFN_EnumDisplayDevicesA)GetProcAddress(
              GetModuleHandleA("user32.dll"), "EnumDisplayDevicesA");
    if (pfn) {
        for (i = 0; i < 16; i++) {
            memset(&dd, 0, sizeof(dd));
            dd.cb = sizeof(dd);
            if (!pfn(NULL, i, &dd, 0))
                break;
            if (!(dd.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP))
                continue;
            if (!found || (dd.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE)) {
                best = dd;
                found = 1;
                if (dd.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE)
                    break;
            }
        }
    }

    if (found) {
        safe_strncpy(g->name, best.DeviceString, sizeof(g->name));
        safe_strncpy(g->hwid, best.DeviceID, sizeof(g->hwid));
        parse_ven_dev(best.DeviceID, &g->ven, &g->dev);
        safe_strncpy(g->source, "EnumDisplayDevices(active)",
                     sizeof(g->source));

        /* Follow the adapter's OWN registry key for its video RAM. DeviceKey
         * is an NT-object path; the registry API wants the part after
         * \Registry\Machine\. */
        if (best.DeviceKey[0]) {
            const char *k = best.DeviceKey;
            const char *pfx = "\\Registry\\Machine\\";
            HKEY h;
            if (_strnicmp(k, pfx, (int)strlen(pfx)) == 0)
                k += strlen(pfx);
            if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, k, 0, KEY_READ, &h)
                    == ERROR_SUCCESS) {
                g->vram_mb = reg_memsize_mb(h);
                if (!g->name[0])
                    reg_str(h, "Device Description", g->name, sizeof(g->name));
                RegCloseKey(h);
            }
        }
    } else {
        safe_strncpy(g->source, "registry(class key)", sizeof(g->source));
    }

    gpu_annotate_from_class(g, is_nt);

    /* Last resort on Win9x, where EnumDisplayDevices may be absent entirely
     * or may hand back an empty DeviceID. The PCI ids matter more than the
     * name: without them the gate cannot classify the adapter and fails open,
     * which on this fleet means the weakest graphics in it is the one card
     * never gated. */
    if (!g->ven && !is_nt)
        gpu_ids_from_win9x_enum(g);
    if (!g->name[0] && !is_nt) {
        HKEY h;
        if (RegOpenKeyExA(HKEY_LOCAL_MACHINE,
                          "System\\CurrentControlSet\\Services\\Class\\"
                          "Display\\0000", 0, KEY_READ, &h) == ERROR_SUCCESS) {
            reg_str(h, "DriverDesc", g->name, sizeof(g->name));
            RegCloseKey(h);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Machine capabilities - software state that can be FIXED               */
/* ------------------------------------------------------------------ */
/*
 * Seven already-staged titles mount a disc image at launch (SystemShock2,
 * Shogo, RedFaction, StarCraft, Descent2, Descent3, SoldierOfFortune2), and a
 * survey found .123 and .246 have no virtual mounter at all - so on those
 * boxes those titles have never worked and nothing anywhere said so. That is a
 * fleet-management gap, not a hardware limit, so it is reported as a
 * capability rather than folded into the gate's run/no verdict.
 *
 * Detection is by DRIVER SERVICE KEY rather than by looking for an executable:
 * every mounter installs a virtual SCSI/bus driver, the key survives the
 * program directory being moved, and it costs one registry open each.
 */
static int service_exists(const char *name)
{
    HKEY  h;
    char  path[256];
    _snprintf(path, sizeof(path) - 1,
              "SYSTEM\\CurrentControlSet\\Services\\%s", name);
    path[sizeof(path) - 1] = 0;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, path, 0, KEY_QUERY_VALUE, &h)
            != ERROR_SUCCESS)
        return 0;
    RegCloseKey(h);
    return 1;
}

static int key_exists(const char *path)
{
    HKEY h;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, path, 0, KEY_QUERY_VALUE, &h)
            != ERROR_SUCCESS)
        return 0;
    RegCloseKey(h);
    return 1;
}

static unsigned caps_detect(char *why, DWORD why_cch)
{
    /* Virtual bus/port drivers, newest naming first. d346/d347 are Daemon
     * Tools 3.x/4.x; sptd is Daemon Tools 4+ and Alcohol; ElbyCDIO/VClone is
     * Virtual CloneDrive; mcdbus is MagicDisc; the rest are self-explanatory. */
    static const char *const mount_svc[] = {
        "d347bus", "d347prt", "d346bus", "d346prt", "sptd", "sptd2",
        "ElbyCDIO", "VClone", "mcdbus", "ImDisk", "WinCDEmu", "PxHelp20",
        "dtscsibus", "dtsoftbus01", "VCdRom", 0
    };
    static const char *const mount_key[] = {
        "SOFTWARE\\DT Soft\\DAEMON Tools",
        "SOFTWARE\\Daemon Tools",
        "SOFTWARE\\DAEMON Tools",
        "SOFTWARE\\Alcohol Soft",
        "SOFTWARE\\Elaborate Bytes\\VirtualCloneDrive", 0
    };
    unsigned caps = 0;
    int i;

    if (why && why_cch)
        why[0] = 0;

    for (i = 0; mount_svc[i]; i++) {
        if (!service_exists(mount_svc[i]))
            continue;
        caps |= GG_CAP_DISC_MOUNT;
        if (why && why_cch)
            safe_strncpy(why, mount_svc[i], (int)why_cch);
        break;
    }
    if (!(caps & GG_CAP_DISC_MOUNT)) {
        for (i = 0; mount_key[i]; i++) {
            if (!key_exists(mount_key[i]))
                continue;
            caps |= GG_CAP_DISC_MOUNT;
            if (why && why_cch)
                safe_strncpy(why, mount_key[i], (int)why_cch);
            break;
        }
    }
    return caps;
}

/* ------------------------------------------------------------------ */
/* OS + DirectX                                                         */
/* ------------------------------------------------------------------ */

static const char *os_product_name(const OSVERSIONINFOA *o)
{
    if (o->dwPlatformId == VER_PLATFORM_WIN32_WINDOWS) {
        if (o->dwMinorVersion == 0)  return "Windows 95";
        if (o->dwMinorVersion == 10) return "Windows 98";
        if (o->dwMinorVersion == 90) return "Windows Me";
        return "Windows 9x";
    }
    if (o->dwMajorVersion == 4)                            return "Windows NT 4.0";
    if (o->dwMajorVersion == 5 && o->dwMinorVersion == 0)  return "Windows 2000";
    if (o->dwMajorVersion == 5 && o->dwMinorVersion == 1)  return "Windows XP";
    if (o->dwMajorVersion == 5 && o->dwMinorVersion == 2)  return "Windows Server 2003";
    if (o->dwMajorVersion == 6 && o->dwMinorVersion == 0)  return "Windows Vista";
    if (o->dwMajorVersion == 6 && o->dwMinorVersion == 1)  return "Windows 7";
    if (o->dwMajorVersion == 6 && o->dwMinorVersion == 2)  return "Windows 8";
    if (o->dwMajorVersion == 6 && o->dwMinorVersion == 3)  return "Windows 8.1";
    if (o->dwMajorVersion >= 10)                           return "Windows 10/11";
    return "Unknown";
}

/* HKLM\SOFTWARE\Microsoft\DirectX\Version is "4.09.00.0904" for DX9. The
 * second field is the DirectX major. Vista and 7 keep reporting 4.09 while
 * shipping DX10/11, so the OS level raises the floor. */
static unsigned directx_major(char *ver, DWORD cch, int os_level)
{
    HKEY h;
    unsigned major = 0;

    ver[0] = 0;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\DirectX", 0,
                      KEY_READ, &h) == ERROR_SUCCESS) {
        reg_str(h, "Version", ver, cch);
        RegCloseKey(h);
    }
    if (ver[0]) {
        const char *dot = strchr(ver, '.');
        if (dot)
            major = (unsigned)strtoul(dot + 1, NULL, 10);
    }
    if (os_level >= GG_OS_WIN7 && major < 11) major = 11;
    else if (os_level == GG_OS_VISTA && major < 10) major = 10;
    return major;
}

/* ------------------------------------------------------------------ */
/* Build the profile                                                    */
/* ------------------------------------------------------------------ */

typedef BOOL (WINAPI *PFN_GDFSEx)(LPCSTR, PULARGE_INTEGER, PULARGE_INTEGER,
                                  PULARGE_INTEGER);

/* Everything the JSON needs that the gate itself does not. Kept separate so
 * gamesync can build a gg_profile_t without paying for the extras. */
typedef struct {
    char     hostname[128];
    char     cpu_brand[80];
    char     mhz_source[16];
    char     os_product[64];
    char     os_version[32];
    char     os_sp[64];
    char     dx_version[64];
    char     mount_evidence[64];  /* which key proved the mounter exists */
    hw_gpu_t gpu;
    int      screen_w, screen_h, screen_bpp;
    edid_panel_t panel;
    char     target_source[16];
} hw_extra_t;

static void hwprofile_collect(gg_profile_t *p, hw_extra_t *x)
{
    OSVERSIONINFOA osvi;
    SYSTEM_INFO si;
    int is_nt, has_tsc = 0;
    char regname[128];

    memset(p, 0, sizeof(*p));
    memset(x, 0, sizeof(*x));

    {
        DWORD cch = sizeof(x->hostname);
        if (!GetComputerNameA(x->hostname, &cch))
            x->hostname[0] = 0;
    }

    osvi.dwOSVersionInfoSize = sizeof(osvi);
    GetVersionExA(&osvi);
    is_nt = (osvi.dwPlatformId == VER_PLATFORM_WIN32_NT);
    p->os_major = osvi.dwMajorVersion;
    p->os_minor = osvi.dwMinorVersion;
    p->os_level = gg_os_level_from_version(osvi.dwMajorVersion,
                                           osvi.dwMinorVersion, is_nt);
    safe_strncpy(x->os_product, os_product_name(&osvi), sizeof(x->os_product));
    _snprintf(x->os_version, sizeof(x->os_version) - 1, "%lu.%lu.%lu",
              (unsigned long)osvi.dwMajorVersion,
              (unsigned long)osvi.dwMinorVersion,
              (unsigned long)(osvi.dwBuildNumber & 0xFFFF));
    x->os_version[sizeof(x->os_version) - 1] = 0;
    safe_strncpy(x->os_sp, osvi.szCSDVersion, sizeof(x->os_sp));

    GetSystemInfo(&si);
    p->cpu_count = si.dwNumberOfProcessors;

    if (!cpu_identify(p, x->cpu_brand, sizeof(x->cpu_brand), &has_tsc)) {
        /* No CPUID: all we have is the coarse family GetSystemInfo reports. */
        safe_strncpy(p->cpu_vendor, "unknown", sizeof(p->cpu_vendor));
        p->cpu_family   = si.wProcessorLevel;
        p->cpu_model    = (si.wProcessorRevision >> 8) & 0xFF;
        p->cpu_stepping = si.wProcessorRevision & 0xFF;
    }

    p->cpu_mhz = cpu_mhz_from_registry(regname, sizeof(regname));
    if (p->cpu_mhz)
        safe_strncpy(x->mhz_source, "registry", sizeof(x->mhz_source));
    if (!x->cpu_brand[0] && regname[0])
        safe_strncpy(x->cpu_brand, regname, sizeof(x->cpu_brand));
    if (!p->cpu_mhz && has_tsc) {
        p->cpu_mhz = cpu_mhz_measure();
        if (p->cpu_mhz)
            safe_strncpy(x->mhz_source, "tsc", sizeof(x->mhz_source));
    }
    if (!p->cpu_mhz)
        safe_strncpy(x->mhz_source, "unknown", sizeof(x->mhz_source));

    p->ram_mb = ram_total_mb();

    gpu_identify(&x->gpu, is_nt);
    p->gpu_ven   = x->gpu.ven;
    p->gpu_dev   = x->gpu.dev;
    p->vram_mb   = x->gpu.vram_mb;
    p->gpu_level = gg_gpu_level_from_pci(p->gpu_ven, p->gpu_dev);

    p->dx_major = directx_major(x->dx_version, sizeof(x->dx_version),
                                p->os_level);

    p->caps = caps_detect(x->mount_evidence, sizeof(x->mount_evidence));

    {
        HDC hdc = GetDC(NULL);
        if (hdc) {
            x->screen_w   = GetDeviceCaps(hdc, HORZRES);
            x->screen_h   = GetDeviceCaps(hdc, VERTRES);
            x->screen_bpp = GetDeviceCaps(hdc, BITSPIXEL) *
                            GetDeviceCaps(hdc, PLANES);
            ReleaseDC(NULL, hdc);
        }
    }

    /*
     * THE TARGET MODE - the resolution the games will actually run at. This is
     * a third fact, distinct from both of the two above, and getting it from
     * the wrong one of them has a different failure in each direction:
     *
     *  - The LIVE mode (screen_w/h) is wrong because a game that exits without
     *    restoring leaves it behind. .123 and .240 were both found sitting at
     *    640x480 from a DOSBox leftover while driving 1080p panels; a gate
     *    trusting that would conclude they were 640x480 machines.
     *  - The EDID NATIVE mode is wrong for a CRT, where the "preferred" timing
     *    is the tube's MAXIMUM, not anything the box is set to. Measured on
     *    .171: its Gateway VX1120 reports 1920x1440, while the fleet actually
     *    runs it at 1280x1024. Feeding the larger number to the adjudicator
     *    made it refuse four titles the box runs perfectly well - a box
     *    silently losing games to a number nothing uses.
     *
     * So: prefer the PERSISTED registry mode, which is what the machine is
     * configured to present and which a game's temporary ChangeDisplaySettings
     * does NOT alter, and fall back to the EDID native mode when there is no
     * usable registry mode. Both are reported; only this one is hashed.
     * Everything failing leaves 0/0, which fails open.
     */
    edid_probe_panel(&x->panel);
    {
        DEVMODEA dm;
        memset(&dm, 0, sizeof(dm));
        dm.dmSize = sizeof(dm);
        if (EnumDisplaySettingsA(NULL, ENUM_REGISTRY_SETTINGS, &dm)
            && dm.dmPelsWidth >= 640 && dm.dmPelsHeight >= 480) {
            p->panel_w = (unsigned)dm.dmPelsWidth;
            p->panel_h = (unsigned)dm.dmPelsHeight;
            safe_strncpy(x->target_source, "registry", sizeof(x->target_source));
        } else if (x->panel.ok) {
            p->panel_w = (unsigned)x->panel.native_w;
            p->panel_h = (unsigned)x->panel.native_h;
            safe_strncpy(x->target_source, "edid", sizeof(x->target_source));
        } else {
            safe_strncpy(x->target_source, "unknown", sizeof(x->target_source));
        }
    }
}

/* Public: build just the gate-relevant profile. gamesync uses this, so the
 * numbers the gate runs on are the same numbers HWPROFILE reports. */
void hwprofile_build(gg_profile_t *p)
{
    hw_extra_t x;
    hwprofile_collect(p, &x);
}

/* ------------------------------------------------------------------ */
/* HWPROFILE                                                            */
/* ------------------------------------------------------------------ */

static void add_features(json_t *j, unsigned bits)
{
    static const unsigned order[] = {
        GG_CPU_FPU, GG_CPU_MMX, GG_CPU_CMOV, GG_CPU_3DNOW, GG_CPU_SSE,
        GG_CPU_SSE2, GG_CPU_SSE3, GG_CPU_SSSE3, GG_CPU_SSE41
    };
    unsigned i;
    json_key(j, "features");
    json_array_start(j);
    for (i = 0; i < sizeof(order) / sizeof(order[0]); i++)
        if (bits & order[i])
            json_str(j, gg_feature_name(order[i]));
    json_array_end(j);
}

static void add_disk(json_t *j)
{
    PFN_GDFSEx ex = (PFN_GDFSEx)GetProcAddress(
        GetModuleHandleA("kernel32.dll"), "GetDiskFreeSpaceExA");
    char root[4] = "A:\\";
    char letter;

    json_key(j, "disk");
    json_array_start(j);
    for (letter = 'A'; letter <= 'Z'; letter++) {
        root[0] = letter;
        if (GetDriveTypeA(root) != DRIVE_FIXED)
            continue;
        json_object_start(j);
        json_kv_str(j, "root", root);
        if (ex) {
            ULARGE_INTEGER avail, total, freeb;
            avail.QuadPart = total.QuadPart = freeb.QuadPart = 0;
            if (ex(root, &avail, &total, &freeb)) {
                json_kv_uint(j, "free_mb",
                             (DWORD)(avail.QuadPart / (1024 * 1024)));
                json_kv_uint(j, "total_mb",
                             (DWORD)(total.QuadPart / (1024 * 1024)));
            }
        } else {
            DWORD spc, bps, fc, tc;
            if (GetDiskFreeSpaceA(root, &spc, &bps, &fc, &tc)) {
                DWORD bpc = spc * bps;
                json_kv_uint(j, "free_mb",
                             (DWORD)(((__int64)fc * bpc) / (1024 * 1024)));
                json_kv_uint(j, "total_mb",
                             (DWORD)(((__int64)tc * bpc) / (1024 * 1024)));
            }
        }
        json_object_end(j);
    }
    json_array_end(j);
}

/*
 * Build the whole HWPROFILE document and hand back the buffer.
 *
 * Split out of handle_hwprofile() so that hwpublish.c - which writes this same
 * record onto the share on every startup, so the fleet documentation is
 * measured rather than remembered - runs THE SAME PROBE. A second prober
 * would be a second answer, and the whole problem being solved here is two
 * descriptions of one machine disagreeing.
 *
 * Caller must HeapFree() the result. Returns NULL only if the builder failed.
 */
char *hwprofile_json(void)
{
    gg_profile_t p;
    hw_extra_t   x;
    json_t       j;
    char         hash[17], buf[64];
    SYSTEMTIME   st;

    hwprofile_collect(&p, &x);
    gg_profile_hash(&p, hash);

    json_init(&j);
    json_object_start(&j);

    json_kv_str(&j, "profile_hash", hash);
    json_kv_uint(&j, "profile_version", 1);
    json_kv_str(&j, "hostname", x.hostname);
#ifdef AGENT_VERSION
    json_kv_str(&j, "agent_version", AGENT_VERSION);
#endif

    /* WHEN THE BOX THINKS IT MEASURED ITSELF. Reported, never trusted for
     * staleness: a retro machine's RTC is frequently years out, so the
     * renderer judges age by when the record landed on the host and shows a
     * disagreement as clock skew rather than silently changing the answer. */
    GetLocalTime(&st);
    _snprintf(buf, sizeof(buf) - 1, "%04d-%02d-%02d %02d:%02d:%02d",
              st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    buf[sizeof(buf) - 1] = 0;
    json_kv_str(&j, "reported_at", buf);

    json_key(&j, "cpu");
    json_object_start(&j);
    json_kv_str(&j, "vendor", p.cpu_vendor);
    json_kv_str(&j, "brand", x.cpu_brand);
    json_kv_uint(&j, "family", p.cpu_family);
    json_kv_uint(&j, "model", p.cpu_model);
    json_kv_uint(&j, "stepping", p.cpu_stepping);
    json_kv_uint(&j, "mhz", p.cpu_mhz);
    json_kv_str(&j, "mhz_source", x.mhz_source);
    json_kv_uint(&j, "count", p.cpu_count);
    json_kv_uint(&j, "feature_bits", p.cpu_features);
    add_features(&j, p.cpu_features);
    json_object_end(&j);

    json_kv_uint(&j, "ram_mb", p.ram_mb);

    json_key(&j, "gpu");
    json_object_start(&j);
    json_kv_str(&j, "name", x.gpu.name);
    _snprintf(buf, sizeof(buf) - 1, "0x%04X", p.gpu_ven);
    buf[sizeof(buf) - 1] = 0;
    json_kv_str(&j, "pci_ven", buf);
    _snprintf(buf, sizeof(buf) - 1, "0x%04X", p.gpu_dev);
    buf[sizeof(buf) - 1] = 0;
    json_kv_str(&j, "pci_dev", buf);
    json_kv_str(&j, "hardware_id", x.gpu.hwid);
    json_kv_uint(&j, "vram_mb", p.vram_mb);
    json_kv_str(&j, "driver_version", x.gpu.driver_version);
    json_kv_str(&j, "driver_date", x.gpu.driver_date);
    json_kv_str(&j, "feature_level", gg_gpu_level_name(p.gpu_level));
    json_kv_int(&j, "feature_level_num", p.gpu_level);
    json_kv_str(&j, "source", x.gpu.source);
    json_object_end(&j);

    json_key(&j, "os");
    json_object_start(&j);
    json_kv_str(&j, "product", x.os_product);
    json_kv_str(&j, "version", x.os_version);
    json_kv_str(&j, "service_pack", x.os_sp);
    json_kv_str(&j, "level", gg_os_level_name(p.os_level));
    json_kv_int(&j, "level_num", p.os_level);
    json_object_end(&j);

    json_key(&j, "directx");
    json_object_start(&j);
    json_kv_str(&j, "version", x.dx_version);
    json_kv_uint(&j, "major", p.dx_major);
    json_object_end(&j);

    json_key(&j, "capabilities");
    json_object_start(&j);
    json_kv_bool(&j, "disc_mount", (p.caps & GG_CAP_DISC_MOUNT) ? 1 : 0);
    json_kv_str(&j, "disc_mount_evidence", x.mount_evidence);
    json_kv_uint(&j, "bits", p.caps);
    json_object_end(&j);

    json_key(&j, "display");
    json_object_start(&j);
    json_kv_int(&j, "width", x.screen_w);
    json_kv_int(&j, "height", x.screen_h);
    json_kv_int(&j, "bpp", x.screen_bpp);
    /* The panel's own native mode, distinct from the three fields above: those
     * say what the desktop is showing, these say what the hardware can do and
     * therefore what the games get configured to. */
    /* panel_w/h is the TARGET mode the gate hashes and judges against;
     * edid_* is the panel's own native timing, reported for diagnosis. */
    json_kv_uint(&j, "panel_w", p.panel_w);
    json_kv_uint(&j, "panel_h", p.panel_h);
    json_kv_str(&j, "panel_source", x.target_source);
    json_kv_int(&j, "edid_w", x.panel.native_w);
    json_kv_int(&j, "edid_h", x.panel.native_h);
    json_kv_int(&j, "panel_hz", x.panel.native_hz);
    json_kv_int(&j, "panel_digital", x.panel.digital);
    json_kv_str(&j, "panel_name", x.panel.name);
    json_object_end(&j);

    add_disk(&j);

    /* The two facts the single ACTIVE-adapter answer above deliberately does
     * not carry, and which the fleet documentation needs (hwextra.c):
     *
     *   video_cards[]   EVERY display-class instance, each marked with whether
     *                   it is the one attached to the desktop. .143 renders on
     *                   a GeForce 6800 with its Voodoo5 5500 sitting behind it
     *                   as a second adapter; reporting only the active one is
     *                   how a Voodoo5 test matrix got sized at two boxes when
     *                   only one of them had the card.
     *   accelerators[]  every VEN_121A device in the PCI enumerator. A VOODOO 2
     *                   IS NOT A DISPLAY ADAPTER AT ALL - its INF is
     *                   Class=MEDIA - so .171's card appears in no
     *                   display-class scan anywhere. Enum\PCI lists a fitted
     *                   card even with no driver bound to it, which is also
     *                   what proved .133's V5 6000 is genuinely gone rather
     *                   than merely undriven.
     *
     * Neither is folded into gg_profile_hash: the gate decides on the card
     * that draws the screen, and a second adapter must not invalidate its
     * verdict cache.
     */
    hwextra_emit_video_cards(&j);
    hwextra_emit_accelerators(&j);
    hwextra_emit_network(&j);

    json_object_end(&j);

    log_msg(LOG_HW, "hash=%s cpu=\"%s\" %uMHz(%s) x%u ram=%uMB gpu=%04X:%04X "
                    "\"%s\" vram=%uMB level=%s os=%s caps=%u",
            hash, p.cpu_vendor, p.cpu_mhz, x.mhz_source, p.cpu_count,
            p.ram_mb, p.gpu_ven, p.gpu_dev, x.gpu.name, p.vram_mb,
            gg_gpu_level_name(p.gpu_level), gg_os_level_name(p.os_level),
            p.caps);

    /* json_finish() hands the buffer itself to the caller, so json_free()
     * must NOT be called here - it would free the very buffer being returned.
     * Ownership transfers; the caller HeapFree()s it. */
    return json_finish(&j);
}

void handle_hwprofile(SOCKET sock, const char *args)
{
    char *result;

    (void)args;
    result = hwprofile_json();
    if (!result) {
        send_error_response(sock, "could not build the hardware profile");
        return;
    }
    send_text_response(sock, result);
    HeapFree(GetProcessHeap(), 0, result);
}
