/*
 * clockguard.h - may the agent move this box's clock?
 *
 * Found 2026-09-26 on a freshly PXE-imaged Dell Dimension 4600: its CMOS clock
 * read 2004-02-21 all through setup, so XP recorded the install (and started
 * the 30-day activation grace) in February 2004. At first logon clockfix.c saw
 * a year before 2024, asked the NAS, and set the clock to 2026-09-26 - which
 * is 22 years after the install. WMI then read ActivationRequired=1,
 * RemainingGracePeriod=0: the box worked while logged in and would have come
 * back from its next reboot to XP's activation lockout, where logon is refused,
 * the Run-key agent never starts, and recovery needs a person at the keyboard.
 * safe-reboot.py refused the reboot (wpabaln.exe was running), which is the
 * only reason it was found before it cost the box.
 *
 * A wrong clock is cosmetic and fixable remotely at any time; a locked box is
 * neither. So on the two Windows versions with product activation that blocks
 * LOGON (XP = NT 5.1, Server 2003 = NT 5.2), a correction is applied only when
 * Windows says it cannot use up the grace:
 *
 *   - activated                          -> allow
 *   - the move is backwards (or none)    -> allow (it only adds grace)
 *   - not activated, grace > jump + 1d   -> allow (e.g. the install date was
 *                                           right and a dead battery reset the
 *                                           clock to the past at a cold boot:
 *                                           moving forward restores the truth)
 *   - not activated, otherwise           -> REFUSE
 *   - activation state unknown           -> REFUSE (fail safe: see above)
 *
 * Every other Windows allows: 9x/ME and 2000 have no activation, and Vista/7
 * degrade to a notification rather than refusing logon.
 *
 * Win32-free so tests/native/test_clockguard.c compiles the code the agent runs.
 */
#ifndef RETRO_CLOCKGUARD_H
#define RETRO_CLOCKGUARD_H

#ifdef __GNUC__
#define CG_UNUSED __attribute__((unused))
#else
#define CG_UNUSED
#endif

#define CLOCKGUARD_ALLOW               0
#define CLOCKGUARD_REFUSE_UNACTIVATED  1
#define CLOCKGUARD_REFUSE_UNKNOWN      2

/* One day of margin: WMI reports whole days, and "grace 1, jump 1" must not
 * be the move that lands exactly on the expiry. */
#define CLOCKGUARD_MARGIN_DAYS 1

/* Product activation that blocks logon: NT 5.1 (XP) and 5.2 (Server 2003 /
 * XP x64). */
CG_UNUSED static int clockguard_wpa_os(int is_nt, unsigned long major, unsigned long minor)
{
    return is_nt && major == 5 && (minor == 1 || minor == 2);
}

/* wpa_known: the activation state was read (WMI answered).
 * activation_required / grace_days: Win32_WindowsProductActivation's
 *   ActivationRequired and RemainingGracePeriod (days).
 * jump_days: how far the correction moves the clock, target minus now, in
 *   whole days (negative = backwards). */
CG_UNUSED static int clockguard_decide(int wpa_os, int wpa_known,
                             unsigned long activation_required,
                             unsigned long grace_days, long long jump_days)
{
    if (!wpa_os)
        return CLOCKGUARD_ALLOW;
    if (!wpa_known)
        return CLOCKGUARD_REFUSE_UNKNOWN;
    if (!activation_required)
        return CLOCKGUARD_ALLOW;
    if (jump_days <= 0)
        return CLOCKGUARD_ALLOW;
    if ((long long)grace_days > jump_days + CLOCKGUARD_MARGIN_DAYS)
        return CLOCKGUARD_ALLOW;
    return CLOCKGUARD_REFUSE_UNACTIVATED;
}

CG_UNUSED static const char *clockguard_reason(int verdict)
{
    switch (verdict) {
    case CLOCKGUARD_ALLOW:              return "allowed";
    case CLOCKGUARD_REFUSE_UNACTIVATED: return "Windows is not activated and the move would use up its activation grace";
    case CLOCKGUARD_REFUSE_UNKNOWN:     return "could not read the activation state (WMI Win32_WindowsProductActivation)";
    }
    return "?";
}

#endif
