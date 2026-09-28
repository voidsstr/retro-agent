/*
 * drvplan.h - what state is each device's driver in? (agent 1.88.0,
 * DRIVERS STATUS / PLAN). Win32-free: tests/native/test_drvplan.c compiles it.
 *
 * User directive 2026-09-27: the agent should "make sure to update all of the
 * drivers on the box except for 3dfx". Before anything installs, every device
 * gets exactly one state, decided in this order:
 *
 *   excluded_3dfx - agent/shared/drvsafe.h says so: never touched automatically
 *   disabled      - problem 22/29, or Win9x ConfigFlags bit 0: the operator's
 *                   (or hardware's) choice, left alone (.243's NEC USB card)
 *   missing       - no driver bound, a problem a driver can fix (1 10 18 28
 *                   31 37 39 - drvmatch_problem_driver_fixable), or bound to
 *                   Win9x's "Unknown" class (.243's SB16 "Unsupported Device")
 *   problem       - any other problem code: not a driver's fault (12 = a
 *                   resource conflict), so no driver change is proposed
 *   generic       - a DISPLAY adapter on the stub driver ("Standard VGA",
 *                   "VGA Compatible", or bound through a PCI\CC_03xx family id)
 *                   - the case .124 hit: a GeForce2 at 800x600x16 on Windows'
 *                   own driver with the right one unused on its disk
 *   ok            - everything else. A USB hub or a PCI bridge bound through a
 *                   family id is ok: that IS its right driver.
 */
#ifndef RETRO_DRVPLAN_H
#define RETRO_DRVPLAN_H

#include <string.h>

#if defined(__GNUC__)
#define DRVPLAN_API static __attribute__((unused))
#else
#define DRVPLAN_API static
#endif

enum {
    DRVST_OK = 0,
    DRVST_MISSING,
    DRVST_GENERIC,
    DRVST_PROBLEM,
    DRVST_DISABLED,
    DRVST_EXCLUDED,
    DRVST_COUNT
};

DRVPLAN_API const char *drvst_name(int s)
{
    switch (s) {
    case DRVST_OK:       return "ok";
    case DRVST_MISSING:  return "missing";
    case DRVST_GENERIC:  return "generic";
    case DRVST_PROBLEM:  return "problem";
    case DRVST_DISABLED: return "disabled";
    case DRVST_EXCLUDED: return "excluded_3dfx";
    default:             return "?";
    }
}

DRVPLAN_API int drvplan_up(int c) { return (c >= 'a' && c <= 'z') ? c - 32 : c; }

DRVPLAN_API int drvplan_ieq(const char *a, const char *b)
{
    if (!a || !b) return 0;
    for (; *a && *b; a++, b++)
        if (drvplan_up((unsigned char)*a) != drvplan_up((unsigned char)*b)) return 0;
    return *a == *b;
}

DRVPLAN_API int drvplan_istarts(const char *s, const char *pre)
{
    if (!s || !pre) return 0;
    for (; *pre; s++, pre++)
        if (!*s || drvplan_up((unsigned char)*s) != drvplan_up((unsigned char)*pre)) return 0;
    return 1;
}

DRVPLAN_API int drvplan_icontains(const char *s, const char *needle)
{
    size_t n, i;
    if (!s || !needle || !(n = strlen(needle))) return 0;
    for (; *s; s++) {
        for (i = 0; i < n && s[i] && drvplan_up((unsigned char)s[i]) == drvplan_up((unsigned char)needle[i]); i++)
            ;
        if (i == n) return 1;
    }
    return 0;
}

/* A display adapter on Windows' stub driver. */
DRVPLAN_API int drvplan_display_stub(const char *cls, const char *matching, const char *desc)
{
    if (!drvplan_ieq(cls, "Display")) return 0;
    if (drvplan_istarts(matching, "PCI\\CC_03") || drvplan_istarts(matching, "*PNP09")) return 1;
    return drvplan_icontains(desc, "Standard VGA") || drvplan_icontains(desc, "VGA Compatible")
        || drvplan_icontains(desc, "Standard PCI Graphics Adapter");
}

/* Can installing a driver clear this problem code? (the same set as
 * drvmatch_problem_driver_fixable, kept here so this header stands alone) */
DRVPLAN_API int drvplan_problem_driver_fixable(unsigned long p)
{
    return p == 1 || p == 10 || p == 18 || p == 28 || p == 31 || p == 37 || p == 39;
}

/* The one state of a device.
 *   problem       CM problem code (0 = none)
 *   driver_bound  a class/driver key is bound (NT SPDRP_DRIVER, 9x Enum Driver)
 *   disabled      Win9x ConfigFlags bit 0 (NT: pass 0; problem 22 covers it)
 *   excl3dfx      a DRVSAFE_* reason, 0 = none */
DRVPLAN_API int drvplan_state(unsigned long problem, int driver_bound, int disabled, int excl3dfx,
                              const char *cls, const char *matching, const char *desc)
{
    if (excl3dfx) return DRVST_EXCLUDED;
    if (disabled || problem == 22 || problem == 29) return DRVST_DISABLED;
    if (!driver_bound || drvplan_problem_driver_fixable(problem)) return DRVST_MISSING;
    if (drvplan_ieq(cls, "Unknown")) return DRVST_MISSING;  /* 9x: "Unsupported Device" etc. */
    if (problem) return DRVST_PROBLEM;
    if (drvplan_display_stub(cls, matching, desc)) return DRVST_GENERIC;
    return DRVST_OK;
}

#endif
