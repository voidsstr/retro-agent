/*
 * gamegate.h - the hardware capability gate: can THIS machine run THAT game?
 *
 * Header-only and free of Win32 so the code the agent runs is exactly the code
 * the regression test compiles (tests/native/test_gamegate.c) and exactly the
 * code the host mirrors (scripts/gamegate/rules.py, pinned by
 * tests/python/test_gamegate_mirror.py, which compiles THIS header and compares
 * every answer against the Python copy).
 *
 * WHY IT EXISTS. The fleet spans a 1999 Pentium III with a Voodoo and a 2011
 * Sandy Bridge quad. GAMESYNC copied the whole staged library onto every box,
 * so a machine that cannot run a title still spent an hour of SMB1 bandwidth on
 * it and then wore a desktop icon that launches into a black screen or an
 * "illegal instruction". The gate stops the copy instead.
 *
 * THE DIVISION OF LABOUR - deterministic rules decide alone wherever they can:
 *
 *   hard NO      an OS floor unmet (an XP-only PE will not load on 98), a
 *                required CPU instruction absent (SSE2 on a Pentium III is
 *                #UD, not "slow"), a GPU two whole feature levels short.
 *   MARGINAL     a shortfall inside GG_MARGIN_PCT of a published minimum, or
 *                a GPU exactly one level short. Period minimum specs were
 *                guesses; this is the band where a judgement call is worth
 *                asking for, and the only band that reaches an LLM.
 *   RUN          everything met.
 *
 * A gate that phones a language model to conclude "a Pentium III cannot run
 * Doom 3" is a bad gate, so the rules must be able to answer that themselves.
 *
 * FAIL-OPEN, DELIBERATELY. Absent data never blocks a title: no requires.json,
 * an unparsable one, a field left at 0, or a GPU this table cannot classify all
 * yield RUN. The gate blocks only on positive evidence. The failure mode of
 * fail-closed is a box that silently receives no games and says nothing about
 * why, which is far worse than a box that receives one game too many.
 */
#ifndef RETRO_GAMEGATE_H
#define RETRO_GAMEGATE_H

#include <string.h>

/* Every definition here is `static`, and a translation unit that needs only
 * one of them would otherwise draw a -Wunused-function for the rest. The
 * agent builds with -Wall -Wextra and a wall of noise is how a real warning
 * gets missed, so mark them all deliberately unused. */
#if defined(__GNUC__)
#  define GG_FN   static __attribute__((unused))
#  define GG_DATA static __attribute__((unused))
#else
#  define GG_FN   static
#  define GG_DATA static
#endif


/* ------------------------------------------------------------------ */
/* CPU feature bits                                                     */
/* ------------------------------------------------------------------ */
#define GG_CPU_FPU    0x0001u
#define GG_CPU_MMX    0x0002u
#define GG_CPU_CMOV   0x0004u
#define GG_CPU_SSE    0x0008u
#define GG_CPU_SSE2   0x0010u
#define GG_CPU_SSE3   0x0020u
#define GG_CPU_SSSE3  0x0040u
#define GG_CPU_SSE41  0x0080u
#define GG_CPU_3DNOW  0x0100u

/* ------------------------------------------------------------------ */
/* GPU feature levels. ORDERED: a bigger number is strictly more capable. */
/* ------------------------------------------------------------------ */
#define GG_GPU_UNKNOWN (-1)  /* not classifiable - NEVER used to reject */
#define GG_GPU_NONE      0   /* no 3D acceleration at all (plain VGA/VESA) */
#define GG_GPU_FIXED     1   /* fixed-function rasteriser, no HW T&L:
                              * every 3dfx Voodoo, RIVA 128/TNT/TNT2, i810/i815,
                              * i865G, S3 Virge/Savage, Rage Pro/128 */
#define GG_GPU_TNL       2   /* HW transform & lighting, DX7 class:
                              * GeForce 256/2/4MX, Radeon 7x00 */
#define GG_GPU_SM1       3   /* shader model 1.x, DX8: GeForce3/4Ti, Radeon 8500 */
#define GG_GPU_SM2       4   /* shader model 2.0, DX9: GeForce FX, Radeon 9500+ */
#define GG_GPU_SM3       5   /* shader model 3.0 and everything after it */

/* ------------------------------------------------------------------ */
/* OS floors. ORDERED.                                                  */
/* ------------------------------------------------------------------ */
#define GG_OS_UNKNOWN 0
#define GG_OS_WIN9X   1   /* 95 / 98 / ME */
#define GG_OS_WIN2K   2
#define GG_OS_WINXP   3
#define GG_OS_VISTA   4
#define GG_OS_WIN7    5
#define GG_OS_WIN8    6
#define GG_OS_WIN10   7

/* ------------------------------------------------------------------ */
/* Machine CAPABILITIES - software state, not hardware                  */
/* ------------------------------------------------------------------ */
/*
 * These are deliberately kept OUT of the run/marginal/no verdict, because they
 * are a different kind of fact and want a different response.
 *
 * A GeForce2 GTS will never gain a pixel shader. A box with no virtual disc
 * mounter is ten minutes and a Daemon Tools installer away from having one -
 * and seven already-staged titles (SystemShock2, Shogo, RedFaction, StarCraft,
 * Descent2, Descent3, SoldierOfFortune2) mount a disc image at launch, so on
 * .123 and .246 they have never once worked and nothing said so.
 *
 * Reporting that as "this machine cannot run the game" would be wrong in the
 * way that matters: it tells you to give up rather than to install a mounter.
 * So gg_decide() records unmet capabilities in `missing_caps` and leaves the
 * verdict alone; the copy decision is hardware-only, and a capability gap
 * instead suppresses the individual SHORTCUT that needs it (and says why).
 * GAMESYNC re-runs on every boot, so the shortcut reappears by itself once the
 * box is fixed.
 */
#define GG_CAP_DISC_MOUNT 0x0001u   /* a virtual disc/CD image mounter exists */

/* ------------------------------------------------------------------ */
/* Verdicts                                                             */
/* ------------------------------------------------------------------ */
#define GG_V_RUN      0
#define GG_V_MARGINAL 1
#define GG_V_NO       2

/* How far below a published minimum still counts as "worth asking about"
 * rather than a flat no, in percent of the requirement. A 2001 title asking
 * for 700 MHz is generally playable on 550; at 400 it is not. */
#define GG_MARGIN_PCT 25

/* ------------------------------------------------------------------ */
/* The two records                                                      */
/* ------------------------------------------------------------------ */

/* What a machine IS. Every field is stable across reboots - nothing here may
 * depend on uptime, free RAM or free disk, or the profile hash changes on
 * every poll and the host-side cache never hits. */
typedef struct gg_profile_s {
    char          cpu_vendor[16];   /* CPUID vendor string, e.g. GenuineIntel */
    unsigned      cpu_family;
    unsigned      cpu_model;
    unsigned      cpu_stepping;
    unsigned      cpu_mhz;
    unsigned      cpu_count;
    unsigned      cpu_features;     /* GG_CPU_* bitmask */
    unsigned      ram_mb;
    unsigned      gpu_ven;          /* PCI vendor id, e.g. 0x10DE */
    unsigned      gpu_dev;          /* PCI device id, e.g. 0x0150 */
    unsigned      vram_mb;
    int           gpu_level;        /* GG_GPU_* */
    int           os_level;         /* GG_OS_* */
    unsigned      os_major;
    unsigned      os_minor;
    unsigned      dx_major;         /* DirectX runtime major, 0 = unknown */
    unsigned      caps;             /* GG_CAP_* bitmask - software state */
    /*
     * Free space on the volume the games land on, in MB. 0 = could not be
     * measured, which FAILS OPEN like every other absent field.
     *
     * DELIBERATELY NOT IN gg_profile_hash(). Free space changes every time
     * anything is written, so hashing it would mint a new profile - and
     * therefore miss every cached verdict - on essentially every run, which is
     * the same as having no cache at all. The hash names the MACHINE; this
     * field is a fact about the moment.
     */
    unsigned      free_mb;
} gg_profile_t;
/* The tag exists so handlers.h can forward-declare this without pulling the
 * whole gate header into every translation unit. */

/* What a title NEEDS. A zero/unset field is "no opinion" and never blocks. */
typedef struct {
    unsigned      version;          /* requirements_version, for cache keying */
    unsigned      min_cpu_mhz;
    unsigned      min_ram_mb;
    unsigned      min_vram_mb;
    unsigned      disk_mb;
    unsigned      req_features;     /* GG_CPU_* bitmask that MUST be present */
    int           min_gpu_level;    /* GG_GPU_* ; GG_GPU_UNKNOWN = no opinion */
    int           min_os_level;     /* GG_OS_*  ; GG_OS_UNKNOWN  = no opinion */
    int           max_os_level;     /* GG_OS_*  ; 0 = no ceiling */
    unsigned      req_caps;         /* GG_CAP_* the title needs at RUNTIME */
} gg_req_t;

/* The answer. `limiting` names the single field that decided it, so a skip can
 * be reported as a cause rather than as a shrug. */
typedef struct {
    int           verdict;          /* GG_V_* - HARDWARE only */
    const char   *limiting;         /* "cpu_mhz", "ram_mb", ... or "" */
    char          reason[160];
    /* Capabilities the title needs that this box does not have. NEVER folded
     * into `verdict`: these are remediable, and the caller must be able to
     * tell "buy a graphics card" from "run an installer". */
    unsigned      missing_caps;
} gg_decision_t;

/* ------------------------------------------------------------------ */
/* GPU classification from the PCI ids                                  */
/* ------------------------------------------------------------------ */

/* One row of the table: [lo,hi] device ids of one vendor share a level. */
typedef struct {
    unsigned short ven, lo, hi;
    signed char    level;
} gg_gpu_row_t;

/*
 * The table. NVIDIA device ids are NOT monotonic by generation - 0x0150 is a
 * GeForce2 GTS (2000) while 0x0160 is a GeForce 6200 (2004) - so this is
 * explicit ranges with no "anything above X" fallback. An id that matches no
 * row is GG_GPU_UNKNOWN, which never rejects anything.
 */
GG_DATA const gg_gpu_row_t gg_gpu_table[] = {
    /* 3dfx - no Voodoo ever had hardware T&L, the whole line is fixed-function */
    { 0x121A, 0x0001, 0x0009, GG_GPU_FIXED },   /* Voodoo 1/2/Banshee/3/4/5 */

    /* NVIDIA */
    { 0x10DE, 0x0018, 0x0019, GG_GPU_FIXED },   /* NV3  RIVA 128 */
    { 0x10DE, 0x0020, 0x0020, GG_GPU_FIXED },   /* NV4  RIVA TNT */
    { 0x10DE, 0x0028, 0x002F, GG_GPU_FIXED },   /* NV5  RIVA TNT2 / Vanta */
    { 0x10DE, 0x00A0, 0x00A0, GG_GPU_FIXED },   /* Aladdin TNT2 */
    { 0x10DE, 0x0100, 0x0103, GG_GPU_TNL   },   /* NV10 GeForce 256 */
    { 0x10DE, 0x0110, 0x0113, GG_GPU_TNL   },   /* NV11 GeForce2 MX */
    { 0x10DE, 0x0150, 0x0153, GG_GPU_TNL   },   /* NV15 GeForce2 GTS/Ti/Ultra */
    { 0x10DE, 0x01A0, 0x01A0, GG_GPU_TNL   },   /* nForce IGP (GeForce2 MX core) */
    { 0x10DE, 0x0170, 0x018F, GG_GPU_TNL   },   /* NV17/18 GeForce4 MX - DX7, no shaders */
    { 0x10DE, 0x0200, 0x0203, GG_GPU_SM1   },   /* NV20 GeForce3 */
    { 0x10DE, 0x0250, 0x0253, GG_GPU_SM1   },   /* NV25 GeForce4 Ti */
    { 0x10DE, 0x0280, 0x0289, GG_GPU_SM1   },   /* NV28 GeForce4 Ti AGP8x */
    { 0x10DE, 0x0300, 0x0334, GG_GPU_SM2   },   /* NV3x GeForce FX 5xxx */
    { 0x10DE, 0x0040, 0x004F, GG_GPU_SM3   },   /* NV40 GeForce 6800 */
    { 0x10DE, 0x0090, 0x009F, GG_GPU_SM3   },   /* G70  GeForce 7800 */
    { 0x10DE, 0x00C0, 0x00CF, GG_GPU_SM3   },   /* NV41/42 GeForce 6800 */
    { 0x10DE, 0x00F0, 0x00FF, GG_GPU_SM3   },   /* NV4x PCIe bridged */
    { 0x10DE, 0x0140, 0x014F, GG_GPU_SM3   },   /* NV43 GeForce 6600 */
    { 0x10DE, 0x0160, 0x016F, GG_GPU_SM3   },   /* NV44 GeForce 6200 */
    { 0x10DE, 0x01D0, 0x01DF, GG_GPU_SM3   },   /* G72  GeForce 7300/7400 */
    { 0x10DE, 0x0290, 0x029F, GG_GPU_SM3   },   /* G71  GeForce 7900 */
    { 0x10DE, 0x0390, 0x039F, GG_GPU_SM3   },   /* G73  GeForce 7600 */
    { 0x10DE, 0x0400, 0x0429, GG_GPU_SM3   },   /* G84/G86 GeForce 8500/8600/8400 */
    { 0x10DE, 0x05E0, 0x05FF, GG_GPU_SM3   },   /* GT200 */
    { 0x10DE, 0x0600, 0x06FF, GG_GPU_SM3   },   /* G92/G94/G98 - incl. 8400 GS (06E4) */
    { 0x10DE, 0x0A00, 0x0FFF, GG_GPU_SM3   },   /* GT21x / GF1xx and later */
    { 0x10DE, 0x1000, 0x3FFF, GG_GPU_SM3   },   /* Fermi and everything after */

    /* ATI / AMD. The 2D-era parts are fixed-function; R100 brought T&L. */
    { 0x1002, 0x4742, 0x4744, GG_GPU_FIXED },   /* Rage Pro */
    { 0x1002, 0x4C42, 0x4C4D, GG_GPU_FIXED },   /* Rage LT/Mobility */
    { 0x1002, 0x5041, 0x5046, GG_GPU_FIXED },   /* Rage 128 */
    { 0x1002, 0x5245, 0x524C, GG_GPU_FIXED },   /* Rage 128 Pro */
    { 0x1002, 0x5144, 0x5157, GG_GPU_TNL   },   /* R100 Radeon 7x00 / RV100 / RV200 */
    { 0x1002, 0x514C, 0x514D, GG_GPU_SM1   },   /* R200 Radeon 8500/9100 */
    { 0x1002, 0x4242, 0x4242, GG_GPU_SM1   },   /* R200 All-in-Wonder 8500 */
    { 0x1002, 0x4966, 0x496E, GG_GPU_SM1   },   /* RV250 Radeon 9000 */
    { 0x1002, 0x4144, 0x4154, GG_GPU_SM2   },   /* R300 Radeon 9500/9700 */
    { 0x1002, 0x4164, 0x4174, GG_GPU_SM2   },   /* R300 secondary */
    { 0x1002, 0x4E44, 0x4E56, GG_GPU_SM2   },   /* R350/R360 Radeon 9800 */
    { 0x1002, 0x5960, 0x5965, GG_GPU_SM2   },   /* RV280 Radeon 9200 */
    { 0x1002, 0x5B60, 0x5B7F, GG_GPU_SM2   },   /* RV370 Radeon X300/X550 */
    { 0x1002, 0x5D48, 0x5D6F, GG_GPU_SM3   },   /* R423/R480 Radeon X800 */
    { 0x1002, 0x7100, 0x71FF, GG_GPU_SM3   },   /* R520/RV530 Radeon X1000 */
    { 0x1002, 0x7240, 0x729F, GG_GPU_SM3   },   /* R580 Radeon X1900 */
    { 0x1002, 0x9400, 0x9FFF, GG_GPU_SM3   },   /* R600 (HD 2000) and later */
    { 0x1002, 0x6600, 0x68FF, GG_GPU_SM3   },   /* Evergreen / NI / SI */

    /* Intel integrated. i8xx and i865G have no hardware T&L at all - the
     * driver does it on the CPU - so they are fixed-function, not TNL. The
     * 915/945 generation gained SM2.0 pixel shaders (vertex still on CPU). */
    { 0x8086, 0x7121, 0x7125, GG_GPU_FIXED },   /* i810 */
    { 0x8086, 0x1132, 0x1132, GG_GPU_FIXED },   /* i815 */
    { 0x8086, 0x2562, 0x2572, GG_GPU_FIXED },   /* i845G / i865G / i830M */
    { 0x8086, 0x2582, 0x2592, GG_GPU_SM2   },   /* i915G / i910GML */
    { 0x8086, 0x2772, 0x27AE, GG_GPU_SM2   },   /* i945G / i945GM */
    { 0x8086, 0x29A2, 0x29D2, GG_GPU_SM3   },   /* G965 / G33 / G35 */
    { 0x8086, 0x2E00, 0x2E92, GG_GPU_SM3   },   /* G4x */
    { 0x8086, 0x0042, 0x0126, GG_GPU_SM3   },   /* Ironlake / Sandy Bridge HD */

    /* S3, Matrox, SiS, Trident - the 2D-with-a-bit-of-3D crowd */
    { 0x5333, 0x0000, 0xFFFF, GG_GPU_FIXED },   /* S3 Virge/Trio/Savage */
    { 0x102B, 0x0000, 0xFFFF, GG_GPU_FIXED },   /* Matrox */
    { 0x1039, 0x0000, 0xFFFF, GG_GPU_FIXED },   /* SiS */
    { 0x1023, 0x0000, 0xFFFF, GG_GPU_FIXED },   /* Trident */
    { 0x100C, 0x0000, 0xFFFF, GG_GPU_FIXED },   /* Tseng */
    { 0x1013, 0x0000, 0xFFFF, GG_GPU_FIXED },   /* Cirrus Logic */

    /* Virtual adapters. A VM's display is not a game GPU; classify it as
     * fixed-function so a shader title is gated rather than crashing. */
    { 0x15AD, 0x0000, 0xFFFF, GG_GPU_FIXED },   /* VMware SVGA */
    { 0x80EE, 0x0000, 0xFFFF, GG_GPU_FIXED },   /* VirtualBox */
    { 0x1234, 0x0000, 0xFFFF, GG_GPU_FIXED }    /* QEMU/Bochs */
};

#define GG_GPU_TABLE_ROWS ((int)(sizeof(gg_gpu_table) / sizeof(gg_gpu_table[0])))

/* Classify a display adapter. GG_GPU_UNKNOWN when no row matches - the caller
 * must treat that as "no opinion", never as "no 3D". */
GG_FN int gg_gpu_level_from_pci(unsigned ven, unsigned dev)
{
    int i;
    if (!ven)
        return GG_GPU_UNKNOWN;
    for (i = 0; i < GG_GPU_TABLE_ROWS; i++) {
        if (gg_gpu_table[i].ven != (unsigned short)ven)
            continue;
        if (dev < gg_gpu_table[i].lo || dev > gg_gpu_table[i].hi)
            continue;
        return gg_gpu_table[i].level;
    }
    return GG_GPU_UNKNOWN;
}

/* ------------------------------------------------------------------ */
/* Name <-> level                                                       */
/* ------------------------------------------------------------------ */

GG_FN const char *gg_gpu_level_name(int lvl)
{
    switch (lvl) {
    case GG_GPU_NONE:  return "none";
    case GG_GPU_FIXED: return "fixed";
    case GG_GPU_TNL:   return "tnl";
    case GG_GPU_SM1:   return "sm1.x";
    case GG_GPU_SM2:   return "sm2.0";
    case GG_GPU_SM3:   return "sm3.0";
    default:           return "unknown";
    }
}

GG_FN int gg_gpu_level_parse(const char *s)
{
    if (!s || !*s)              return GG_GPU_UNKNOWN;
    if (!strcmp(s, "none"))     return GG_GPU_NONE;
    if (!strcmp(s, "fixed"))    return GG_GPU_FIXED;
    if (!strcmp(s, "tnl"))      return GG_GPU_TNL;
    if (!strcmp(s, "sm1.x"))    return GG_GPU_SM1;
    if (!strcmp(s, "sm1"))      return GG_GPU_SM1;
    if (!strcmp(s, "sm2.0"))    return GG_GPU_SM2;
    if (!strcmp(s, "sm2"))      return GG_GPU_SM2;
    if (!strcmp(s, "sm3.0"))    return GG_GPU_SM3;
    if (!strcmp(s, "sm3"))      return GG_GPU_SM3;
    return GG_GPU_UNKNOWN;
}

GG_FN const char *gg_os_level_name(int lvl)
{
    switch (lvl) {
    case GG_OS_WIN9X:  return "win9x";
    case GG_OS_WIN2K:  return "win2k";
    case GG_OS_WINXP:  return "winxp";
    case GG_OS_VISTA:  return "vista";
    case GG_OS_WIN7:   return "win7";
    case GG_OS_WIN8:   return "win8";
    case GG_OS_WIN10:  return "win10";
    default:           return "unknown";
    }
}

GG_FN int gg_os_level_parse(const char *s)
{
    if (!s || !*s)             return GG_OS_UNKNOWN;
    if (!strcmp(s, "win9x"))   return GG_OS_WIN9X;
    if (!strcmp(s, "win95"))   return GG_OS_WIN9X;
    if (!strcmp(s, "win98"))   return GG_OS_WIN9X;
    if (!strcmp(s, "win2k"))   return GG_OS_WIN2K;
    if (!strcmp(s, "winxp"))   return GG_OS_WINXP;
    if (!strcmp(s, "xp"))      return GG_OS_WINXP;
    if (!strcmp(s, "vista"))   return GG_OS_VISTA;
    if (!strcmp(s, "win7"))    return GG_OS_WIN7;
    if (!strcmp(s, "win8"))    return GG_OS_WIN8;
    if (!strcmp(s, "win10"))   return GG_OS_WIN10;
    return GG_OS_UNKNOWN;
}

/* Map a Windows major.minor to a level. Win9x is platform-id driven, so the
 * caller passes is_nt; 4.x on NT is NT4, which the fleet does not run but
 * which must not be mistaken for Windows 95. */
GG_FN int gg_os_level_from_version(unsigned major, unsigned minor, int is_nt)
{
    if (!is_nt)
        return GG_OS_WIN9X;                     /* 95 / 98 / 98SE / ME */
    if (major == 5 && minor == 0) return GG_OS_WIN2K;
    if (major == 5)               return GG_OS_WINXP;   /* 5.1 XP, 5.2 2003/x64 */
    if (major == 6 && minor == 0) return GG_OS_VISTA;
    if (major == 6 && minor == 1) return GG_OS_WIN7;
    if (major == 6)               return GG_OS_WIN8;    /* 6.2 / 6.3 */
    if (major >= 10)              return GG_OS_WIN10;
    return GG_OS_UNKNOWN;
}

GG_FN unsigned gg_feature_parse(const char *s)
{
    if (!s || !*s)             return 0;
    if (!strcmp(s, "fpu"))     return GG_CPU_FPU;
    if (!strcmp(s, "mmx"))     return GG_CPU_MMX;
    if (!strcmp(s, "cmov"))    return GG_CPU_CMOV;
    if (!strcmp(s, "sse"))     return GG_CPU_SSE;
    if (!strcmp(s, "sse2"))    return GG_CPU_SSE2;
    if (!strcmp(s, "sse3"))    return GG_CPU_SSE3;
    if (!strcmp(s, "ssse3"))   return GG_CPU_SSSE3;
    if (!strcmp(s, "sse4.1"))  return GG_CPU_SSE41;
    if (!strcmp(s, "3dnow"))   return GG_CPU_3DNOW;
    return 0;
}

GG_FN const char *gg_feature_name(unsigned bit)
{
    switch (bit) {
    case GG_CPU_FPU:   return "fpu";
    case GG_CPU_MMX:   return "mmx";
    case GG_CPU_CMOV:  return "cmov";
    case GG_CPU_SSE:   return "sse";
    case GG_CPU_SSE2:  return "sse2";
    case GG_CPU_SSE3:  return "sse3";
    case GG_CPU_SSSE3: return "ssse3";
    case GG_CPU_SSE41: return "sse4.1";
    case GG_CPU_3DNOW: return "3dnow";
    default:           return "?";
    }
}

GG_FN unsigned gg_capability_parse(const char *s)
{
    if (!s || !*s)                 return 0;
    if (!strcmp(s, "disc_mount"))  return GG_CAP_DISC_MOUNT;
    return 0;
}

GG_FN const char *gg_capability_name(unsigned bit)
{
    switch (bit) {
    case GG_CAP_DISC_MOUNT: return "disc_mount";
    default:                return "?";
    }
}

/* What to actually DO about a missing capability, so the message a box prints
 * is a next step rather than a complaint. */
GG_FN const char *gg_capability_remedy(unsigned bit)
{
    switch (bit) {
    case GG_CAP_DISC_MOUNT:
        return "install a virtual disc mounter (Daemon Tools)";
    default:
        return "unknown remedy";
    }
}

/* ------------------------------------------------------------------ */
/* requires.json - a deliberately tiny scanner                          */
/* ------------------------------------------------------------------ */
/*
 * We author every requires.json ourselves, so a full JSON parser would be
 * 400 lines of attack surface protecting us from a file we wrote. This scans
 * for "key" then takes the value after the colon. It is string-position based
 * and therefore does not understand nesting - which is exactly why the schema
 * is FLAT. tests/native/test_gamegate.c pins the shapes it must survive.
 */

/*
 * Find the value that follows "key": within [json, end).
 *
 * `end` is what makes the flat scanner safe next to the nested "shortcuts"
 * object. Without it, a title-level lookup of "min_cpu_mhz" would happily
 * find one written inside a SHORTCUT and adopt it as the title's own - a
 * silent wrong answer, which is the worst kind. Callers that mean the whole
 * string pass end = 0.
 */
GG_FN const char *gg__find_key_n(const char *json, const char *key,
                                 const char *end)
{
    size_t klen;
    const char *p;

    if (!json || !key)
        return 0;
    klen = strlen(key);
    for (p = json; (p = strchr(p, '"')) != 0; p++) {
        if (end && p >= end)
            return 0;
        if (strncmp(p + 1, key, klen) != 0 || p[1 + klen] != '"')
            continue;
        p += 2 + klen;
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
            p++;
        if (*p != ':')
            continue;
        p++;
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
            p++;
        if (end && p >= end)
            return 0;
        return p;
    }
    return 0;
}

GG_FN const char *gg__find_key(const char *json, const char *key)
{
    return gg__find_key_n(json, key, 0);
}

/* Unsigned integer value for `key`, or `def` when absent/non-numeric.
 * A float ("2.5") reads as its integer part; we have no fractional needs. */
GG_FN unsigned gg_json_uint_n(const char *json, const char *key, unsigned def,
                              const char *end)
{
    const char *p = gg__find_key_n(json, key, end);
    unsigned v = 0;
    int any = 0;

    if (!p)
        return def;
    while (*p >= '0' && *p <= '9') {
        v = v * 10u + (unsigned)(*p - '0');
        p++;
        any = 1;
    }
    return any ? v : def;
}

/* String value for `key` into buf (always NUL-terminated). Returns 1 on hit.
 * Only the escapes our own files can contain (\" and \\) are honoured. */
GG_FN int gg_json_str_n(const char *json, const char *key, char *buf,
                        size_t cap, const char *end)
{
    const char *p = gg__find_key_n(json, key, end);
    size_t n = 0;

    if (!buf || !cap)
        return 0;
    buf[0] = 0;
    if (!p || *p != '"')
        return 0;
    p++;
    while (*p && *p != '"') {
        if (*p == '\\' && p[1])
            p++;
        if (n + 1 < cap)
            buf[n++] = *p;
        p++;
    }
    buf[n] = 0;
    return 1;
}

/* OR together the GG_CPU_* bits named in the string array at `key`.
 * ["sse", "sse2"] -> GG_CPU_SSE|GG_CPU_SSE2. Absent key -> 0. */
/* OR together the bits named in a string array, mapping each element with
 * `map`. Shared by cpu_features and requires_capabilities so the two lists
 * cannot drift into different tolerances for whitespace and junk. */
GG_FN unsigned gg_json_strlist_n(const char *json, const char *key,
                                 unsigned (*map)(const char *),
                                 const char *end)
{
    const char *p = gg__find_key_n(json, key, end);
    unsigned bits = 0;
    char word[24];
    size_t n;

    if (!p || *p != '[')
        return 0;
    p++;
    while (*p && *p != ']') {
        if (*p != '"') {
            p++;
            continue;
        }
        p++;
        n = 0;
        while (*p && *p != '"') {
            if (n + 1 < sizeof(word))
                word[n++] = *p;
            p++;
        }
        word[n] = 0;
        if (*p == '"')
            p++;
        bits |= map(word);
    }
    return bits;
}

GG_FN unsigned gg_json_uint(const char *json, const char *key, unsigned def)
{
    return gg_json_uint_n(json, key, def, 0);
}

GG_FN int gg_json_str(const char *json, const char *key, char *buf, size_t cap)
{
    return gg_json_str_n(json, key, buf, cap, 0);
}

GG_FN unsigned gg_json_features(const char *json, const char *key)
{
    return gg_json_strlist_n(json, key, gg_feature_parse, 0);
}

/*
 * Overlay whatever `[json, end)` states onto `r`, leaving unstated fields
 * alone. PRESENCE, not value, decides: a shortcut writing
 * "requires_capabilities": [] must be able to CLEAR a title-level requirement,
 * and a value-based test could not tell that from "not mentioned". Returns 1
 * if at least one gate-relevant key was present.
 */
GG_FN int gg_req_overlay(const char *json, const char *end, gg_req_t *r)
{
    char word[24];
    int any = 0;

    if (!json || !r)
        return 0;

    if (gg__find_key_n(json, "requirements_version", end)) {
        r->version = gg_json_uint_n(json, "requirements_version", 0, end);
    }
    if (gg__find_key_n(json, "min_cpu_mhz", end)) {
        r->min_cpu_mhz = gg_json_uint_n(json, "min_cpu_mhz", 0, end); any = 1;
    }
    if (gg__find_key_n(json, "min_ram_mb", end)) {
        r->min_ram_mb = gg_json_uint_n(json, "min_ram_mb", 0, end); any = 1;
    }
    if (gg__find_key_n(json, "min_vram_mb", end)) {
        r->min_vram_mb = gg_json_uint_n(json, "min_vram_mb", 0, end); any = 1;
    }
    if (gg__find_key_n(json, "disk_mb", end)) {
        r->disk_mb = gg_json_uint_n(json, "disk_mb", 0, end);
    }
    if (gg__find_key_n(json, "cpu_features", end)) {
        r->req_features = gg_json_strlist_n(json, "cpu_features",
                                            gg_feature_parse, end);
        any = 1;
    }
    if (gg__find_key_n(json, "requires_capabilities", end)) {
        r->req_caps = gg_json_strlist_n(json, "requires_capabilities",
                                        gg_capability_parse, end);
        any = 1;
    }
    if (gg_json_str_n(json, "gpu_feature_level", word, sizeof(word), end)) {
        r->min_gpu_level = gg_gpu_level_parse(word); any = 1;
    }
    if (gg_json_str_n(json, "min_os", word, sizeof(word), end)) {
        r->min_os_level = gg_os_level_parse(word); any = 1;
    }
    if (gg_json_str_n(json, "max_os", word, sizeof(word), end)) {
        r->max_os_level = gg_os_level_parse(word); any = 1;
    }
    return any;
}

GG_FN void gg_req_init(gg_req_t *r)
{
    memset(r, 0, sizeof(*r));
    r->min_gpu_level = GG_GPU_UNKNOWN;
    r->min_os_level  = GG_OS_UNKNOWN;
    r->max_os_level  = GG_OS_UNKNOWN;
}

/*
 * Locate one shortcut's object inside the optional "shortcuts" map. Returns a
 * pointer to the '{' and sets *end just past its matching '}'.
 *
 * WHY PER-SHORTCUT AT ALL. launch.txt is already one line per shortcut and the
 * halves of a title do not always need the same machine. Battlefield 1942's
 * single player wants a mounted disc; its dedicated-server and join launchers
 * check neither disc nor CD key. Gating the whole title on the harder half
 * would take working LAN play off most of the fleet to protect a shortcut
 * nobody could use anyway.
 *
 * The target is a Windows filename, so the match is CASE-INSENSITIVE - we are
 * a Linux host reasoning about a case-insensitive filesystem, and a
 * case-sensitive compare here would report "no per-shortcut rule" for a rule
 * sitting right there.
 */
GG_FN const char *gg_req_shortcut_slice(const char *json, const char *target,
                                        const char **end_out)
{
    const char *sc, *p;
    size_t tlen;

    if (end_out)
        *end_out = 0;
    if (!json || !target || !*target)
        return 0;
    sc = gg__find_key(json, "shortcuts");
    if (!sc || *sc != '{')
        return 0;
    tlen = strlen(target);

    for (p = sc; (p = strchr(p, '"')) != 0; p++) {
        size_t i;
        const char *v;
        int depth;

        for (i = 0; i < tlen; i++) {
            char a = p[1 + i], b = target[i];
            if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
            if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
            if (a != b)
                break;
        }
        if (i != tlen || p[1 + tlen] != '"')
            continue;
        v = p + 2 + tlen;
        while (*v == ' ' || *v == '\t' || *v == '\r' || *v == '\n')
            v++;
        if (*v != ':')
            continue;
        v++;
        while (*v == ' ' || *v == '\t' || *v == '\r' || *v == '\n')
            v++;
        if (*v != '{')
            continue;
        depth = 0;
        for (p = v; *p; p++) {
            if (*p == '{') depth++;
            else if (*p == '}') {
                depth--;
                if (!depth) {
                    if (end_out)
                        *end_out = p + 1;
                    return v;
                }
            }
        }
        return 0;                   /* unterminated - treat as absent */
    }
    return 0;
}

/*
 * Parse a whole requires.json at TITLE level. Returns 1 if anything at all was
 * understood; 0 means "no opinion" and the caller must let the title through.
 *
 * The scan stops at "shortcuts": the flat scanner has no idea about nesting,
 * so without the limit a per-shortcut minimum would be adopted as the title's.
 */
GG_FN int gg_req_parse(const char *json, gg_req_t *r)
{
    const char *sc;

    if (!r)
        return 0;
    gg_req_init(r);
    if (!json || !*json)
        return 0;
    sc = gg__find_key(json, "shortcuts");
    return gg_req_overlay(json, sc, r);
}

/*
 * Parse for ONE shortcut: the title-level requirements, then that shortcut's
 * own object overlaid on top. `target` is the launch.txt first column (the
 * relative exe or .bat). A title with no "shortcuts" map behaves exactly as
 * gg_req_parse, so adding the key is opt-in per title.
 */
GG_FN int gg_req_parse_shortcut(const char *json, const char *target,
                                gg_req_t *r)
{
    const char *slice, *slice_end = 0;
    int any;

    any = gg_req_parse(json, r);
    if (!json || !*json)
        return any;
    slice = gg_req_shortcut_slice(json, target, &slice_end);
    if (slice)
        any |= gg_req_overlay(slice, slice_end, r);
    return any;
}

/* ------------------------------------------------------------------ */
/* The decision                                                         */
/* ------------------------------------------------------------------ */

/* Is `have` short of `need` by more than the marginal band? Both in the same
 * unit. A `have` of 0 means "unmeasured" and never triggers anything. */
GG_FN int gg__hard_short(unsigned have, unsigned need)
{
    if (!need || !have)
        return 0;
    if (have >= need)
        return 0;
    /* short by more than GG_MARGIN_PCT of the requirement */
    return (have * 100u) < (need * (100u - GG_MARGIN_PCT));
}

GG_FN int gg__soft_short(unsigned have, unsigned need)
{
    if (!need || !have)
        return 0;
    return have < need;
}

GG_FN void gg__say(gg_decision_t *d, int verdict, const char *field,
                    const char *what, unsigned have, unsigned need,
                    const char *unit)
{
    char num[64];
    size_t n;

    d->verdict  = verdict;
    d->limiting = field;
    /* Hand-rolled rather than snprintf: this header is compiled into the agent,
     * which routes printf to the box's own msvcrt for Pentium-1 safety, and
     * into a host test. Keeping it free of stdio keeps both honest. */
    n = 0;
    while (*what && n + 1 < sizeof(d->reason))
        d->reason[n++] = *what++;
    d->reason[n] = 0;

    num[0] = 0;
    {
        char tmp[24];
        int  i = 0, j;
        unsigned v = have;
        do { tmp[i++] = (char)('0' + (v % 10u)); v /= 10u; } while (v && i < 20);
        j = 0;
        while (i > 0) num[j++] = tmp[--i];
        num[j++] = ' ';
        num[j] = 0;
        while (*unit && j + 1 < (int)sizeof(num)) num[j++] = *unit++;
        num[j] = 0;
    }
    /* " (have <n> <unit>, needs <m>)" */
    {
        const char *lead = " (have ";
        char tmp[24];
        int i = 0, j;
        unsigned v = need;
        while (*lead && n + 1 < sizeof(d->reason)) d->reason[n++] = *lead++;
        for (j = 0; num[j] && n + 1 < sizeof(d->reason); j++) d->reason[n++] = num[j];
        lead = ", needs ";
        while (*lead && n + 1 < sizeof(d->reason)) d->reason[n++] = *lead++;
        do { tmp[i++] = (char)('0' + (v % 10u)); v /= 10u; } while (v && i < 20);
        while (i > 0 && n + 1 < sizeof(d->reason)) d->reason[n++] = tmp[--i];
        if (n + 1 < sizeof(d->reason)) d->reason[n++] = ')';
        d->reason[n] = 0;
    }
}

GG_FN void gg__say_plain(gg_decision_t *d, int verdict, const char *field,
                          const char *text)
{
    size_t n = 0;
    d->verdict  = verdict;
    d->limiting = field;
    while (*text && n + 1 < sizeof(d->reason))
        d->reason[n++] = *text++;
    d->reason[n] = 0;
}

/*
 * Decide whether `p` can run a title needing `r`.
 *
 * ORDER MATTERS: the hard, binary disqualifications are tested first, so the
 * reported limiting factor is the one that actually stops the game rather than
 * whichever happened to be checked first. A box that is both short of RAM and
 * running the wrong OS should be told about the OS.
 */
GG_FN void gg_decide(const gg_profile_t *p, const gg_req_t *r,
                      gg_decision_t *d)
{
    int soft = 0;
    const char *soft_field = "";
    char soft_text[160];

    memset(d, 0, sizeof(*d));
    d->verdict  = GG_V_RUN;
    d->limiting = "";
    soft_text[0] = 0;
    if (!p || !r) {
        gg__say_plain(d, GG_V_RUN, "", "no data - not gated");
        return;
    }

    /* Recorded, never folded into the verdict. See the GG_CAP_* block: a
     * missing mounter is an installer away, and calling that "cannot run"
     * tells the operator to give up instead of to fix the box. */
    d->missing_caps = r->req_caps & ~p->caps;

    /* 1. OS floor. Binary: XP's loader refuses a Vista-subsystem PE before a
     *    single instruction runs, and a Win32 NT-only binary does not start on
     *    98 at all. There is no "marginal" here. */
    if (r->min_os_level != GG_OS_UNKNOWN && p->os_level != GG_OS_UNKNOWN &&
        p->os_level < r->min_os_level) {
        gg__say_plain(d, GG_V_NO, "os",
                      "OS too old for this title");
        return;
    }
    if (r->max_os_level != GG_OS_UNKNOWN && p->os_level != GG_OS_UNKNOWN &&
        p->os_level > r->max_os_level) {
        gg__say_plain(d, GG_V_NO, "os",
                      "OS too new for this title");
        return;
    }

    /* 2. Required CPU instructions. Also binary: an absent SSE2 is #UD on the
     *    first vectorised instruction, not a slow frame rate. */
    if (r->req_features && p->cpu_features) {
        unsigned missing = r->req_features & ~p->cpu_features;
        if (missing) {
            unsigned bit;
            char msg[80];
            size_t n = 0;
            const char *lead = "CPU lacks ";
            const char *nm;
            while (*lead) msg[n++] = *lead++;
            for (bit = 1; bit; bit <<= 1) {
                if (!(missing & bit))
                    continue;
                nm = gg_feature_name(bit);
                while (*nm && n + 2 < sizeof(msg)) msg[n++] = *nm++;
                break;                          /* name the first one only */
            }
            msg[n] = 0;
            gg__say_plain(d, GG_V_NO, "cpu_features", msg);
            return;
        }
    }

    /* 3. GPU feature level. One level short is a judgement call (many titles
     *    of that era ship a lower-detail path); two or more is not. An
     *    unclassifiable adapter never rejects. */
    if (r->min_gpu_level != GG_GPU_UNKNOWN && p->gpu_level != GG_GPU_UNKNOWN) {
        int gap = r->min_gpu_level - p->gpu_level;
        if (gap >= 2) {
            gg__say_plain(d, GG_V_NO, "gpu_feature_level",
                          "GPU too old for this title's renderer");
            return;
        }
        if (gap == 1) {
            soft = 1;
            soft_field = "gpu_feature_level";
            {
                const char *t = "GPU one feature level short";
                size_t n = 0;
                while (*t) soft_text[n++] = *t++;
                soft_text[n] = 0;
            }
        }
    }

    /* 4. CPU clock, RAM, VRAM: hard short past the margin, soft inside it. */
    if (gg__hard_short(p->cpu_mhz, r->min_cpu_mhz)) {
        gg__say(d, GG_V_NO, "cpu_mhz", "CPU too slow", p->cpu_mhz,
                r->min_cpu_mhz, "MHz");
        return;
    }
    if (gg__hard_short(p->ram_mb, r->min_ram_mb)) {
        gg__say(d, GG_V_NO, "ram_mb", "not enough RAM", p->ram_mb,
                r->min_ram_mb, "MB");
        return;
    }
    if (gg__hard_short(p->vram_mb, r->min_vram_mb)) {
        gg__say(d, GG_V_NO, "vram_mb", "not enough video RAM", p->vram_mb,
                r->min_vram_mb, "MB");
        return;
    }

    /* 5. Free disk. HARD, and with NO margin band: a tree either fits or it
     *    does not, and 90% of Far Cry is not a playable game. Checked AFTER
     *    the cpu/ram/vram floors so a box that genuinely cannot run the title
     *    is told that, rather than sent to free up space it would then waste.
     *
     *    FAILS OPEN on free_mb == 0 - what the agent reports when it could not
     *    measure the volume - the same direction as every other absent field.
     *    GAMESYNC's own room check is the backstop and sees the REAL tree size;
     *    this refuses the copy BEFORE an hour of SMB1 bandwidth is spent on a
     *    title that was never going to fit. */
    if (r->disk_mb && p->free_mb && p->free_mb < r->disk_mb) {
        gg__say(d, GG_V_NO, "disk", "not enough free disk", p->free_mb,
                r->disk_mb, "MB");
        return;
    }

    if (!soft && gg__soft_short(p->cpu_mhz, r->min_cpu_mhz)) {
        gg__say(d, GG_V_MARGINAL, "cpu_mhz", "CPU below minimum", p->cpu_mhz,
                r->min_cpu_mhz, "MHz");
        return;
    }
    if (!soft && gg__soft_short(p->ram_mb, r->min_ram_mb)) {
        gg__say(d, GG_V_MARGINAL, "ram_mb", "RAM below minimum", p->ram_mb,
                r->min_ram_mb, "MB");
        return;
    }
    if (!soft && gg__soft_short(p->vram_mb, r->min_vram_mb)) {
        gg__say(d, GG_V_MARGINAL, "vram_mb", "video RAM below minimum",
                p->vram_mb, r->min_vram_mb, "MB");
        return;
    }

    if (soft) {
        gg__say_plain(d, GG_V_MARGINAL, soft_field, soft_text);
        return;
    }

    gg__say_plain(d, GG_V_RUN, "", "meets requirements");
}

/* ------------------------------------------------------------------ */
/* Profile hash                                                         */
/* ------------------------------------------------------------------ */
/*
 * A 64-bit FNV-1a over the HARDWARE fields only, rendered as 16 lower-case hex
 * digits. Two identical machines therefore share one cache entry and one
 * published verdict file.
 *
 * NOTHING VARIABLE MAY ENTER THIS. Uptime, free RAM, free disk, the running
 * process list - any of them would change the hash on every poll and the
 * host-side cache would never hit once, which is the whole point of the cache.
 * MHz is bucketed to 25 MHz and RAM to 16 MB because both are measured, not
 * declared: a timed rdtsc loop reads 863 one minute and 866 the next, and a
 * machine reporting 511 MB after a video BIOS steals a megabyte is the same
 * machine as one reporting 512.
 */
GG_FN void gg_profile_hash(const gg_profile_t *p, char out[17])
{
    unsigned long long h = 1469598103934665603ULL;   /* FNV-1a 64 offset */
    unsigned fields[10];
    const char *v;
    int i, k;

    if (!out)
        return;
    out[0] = 0;
    if (!p)
        return;

    for (v = p->cpu_vendor; *v; v++) {
        h ^= (unsigned char)*v;
        h *= 1099511628211ULL;
    }

    fields[0] = p->cpu_family;
    fields[1] = p->cpu_model;
    fields[2] = p->cpu_stepping;
    fields[3] = p->cpu_mhz / 25u;          /* 25 MHz buckets */
    fields[4] = p->cpu_count;
    fields[5] = p->cpu_features;
    fields[6] = p->ram_mb / 16u;           /* 16 MB buckets */
    fields[7] = (p->gpu_ven << 16) | (p->gpu_dev & 0xFFFFu);
    fields[8] = p->vram_mb;
    fields[9] = (p->os_major << 8) | (p->os_minor & 0xFFu);

    for (i = 0; i < 10; i++) {
        unsigned f = fields[i];
        for (k = 0; k < 4; k++) {
            h ^= (unsigned char)(f & 0xFFu);
            h *= 1099511628211ULL;
            f >>= 8;
        }
    }

    for (i = 0; i < 16; i++) {
        int nib = (int)((h >> (60 - i * 4)) & 0xFULL);
        out[i] = (char)(nib < 10 ? '0' + nib : 'a' + nib - 10);
    }
    out[16] = 0;
}

/* ------------------------------------------------------------------ */
/* The published verdict file                                           */
/* ------------------------------------------------------------------ */
/*
 * <library>\_gamegate\<profile_hash>.txt, one decision per line:
 *
 *     <verdict>\t<title>\t<limiting>\t<reason>
 *
 * Tab-separated for the same reason PREFER.TXT is: a title name can contain
 * spaces and a reason certainly does. Blank lines and #/; comments ignored.
 * ONLY a "no" line blocks a title - "run" and "marginal" both deploy, so a
 * truncated or half-written file can never do worse than deploy everything.
 */
GG_FN int gg_verdict_parse(char *line, char **title, char **limiting,
                            char **reason)
{
    char *tab, *p = line;
    int verdict;

    if (!line || !title)
        return -1;
    *title = 0;
    if (limiting) *limiting = 0;
    if (reason)   *reason = 0;

    tab = strchr(p, '\r');
    if (tab) *tab = 0;
    tab = strchr(p, '\n');
    if (tab) *tab = 0;
    while (*p == ' ' || *p == '\t')
        p++;
    if (!*p || *p == '#' || *p == ';')
        return -1;

    tab = strchr(p, '\t');
    if (!tab)
        return -1;
    *tab = 0;
    if      (!strcmp(p, "no"))       verdict = GG_V_NO;
    else if (!strcmp(p, "marginal")) verdict = GG_V_MARGINAL;
    else if (!strcmp(p, "run"))      verdict = GG_V_RUN;
    else                             return -1;

    p = tab + 1;
    if (!*p)
        return -1;
    tab = strchr(p, '\t');
    if (tab) {
        *tab = 0;
        *title = p;
        p = tab + 1;
        tab = strchr(p, '\t');
        if (tab) {
            *tab = 0;
            if (limiting) *limiting = p;
            if (reason)   *reason = tab + 1;
        } else if (limiting) {
            *limiting = p;
        }
    } else {
        *title = p;
    }
    return verdict;
}

#endif /* RETRO_GAMEGATE_H */
