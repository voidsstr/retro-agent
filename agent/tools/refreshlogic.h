/* refreshlogic.h - pure decision logic shared by refreshkeep.c and its
 * regression test (tests/native/test_refreshkeep.c).
 *
 * Kept free of <windows.h> on purpose: the test compiles this header
 * natively on the Linux dev host, so the rule that decides "force the mode
 * to N Hz or leave it alone" is the SAME code that ships in the exe.
 *
 * Why this tool exists (verified on .124, 2026-08-25):
 *   A fullscreen game that calls ChangeDisplaySettings WITHOUT
 *   DM_DISPLAYFREQUENCY lands at 60Hz on XP + ForceWare, even when the
 *   desktop was at 100Hz and the registry default for that exact mode says
 *   100Hz. Measured: desktop 1024x768x32@100 -> Quake II fullscreen 1024x768
 *   -> DISPLAYCFG reports 60Hz. ForceWare 71.89's control panel has NO
 *   refresh-rate override page, so the only lever left is to re-apply the
 *   mode from outside with the frequency field set.
 */
#ifndef REFRESHLOGIC_H
#define REFRESHLOGIC_H

#define RK_HZ_SENTINEL_MAX 200   /* EnumDisplaySettings reports 0/1Hz sentinel
                                  * "use the driver default" modes; they are
                                  * not real rates and must never be chosen. */
#define RK_HZ_MIN          50

/* Is hz a real, usable refresh rate (not a sentinel)? */
static int rk_hz_is_real(int hz)
{
    return hz >= RK_HZ_MIN && hz < RK_HZ_SENTINEL_MAX;
}

/* Choose the rate to drive the current mode at.
 *
 *   avail[]/n  - refresh rates the driver enumerates for the CURRENT
 *                width x height x bpp (raw, sentinels included).
 *   want       - requested rate, or 0 for "max the monitor offers".
 *
 * Returns the chosen rate, or 0 when there is nothing sane to pick. An
 * explicit want is honoured ONLY if the driver actually offers it - asking
 * for a rate the monitor cannot do is how you get a black screen on a CRT.
 */
static int rk_pick_refresh(const int *avail, int n, int want)
{
    int i, best = 0;

    for (i = 0; i < n; i++) {
        if (!rk_hz_is_real(avail[i]))
            continue;
        if (want > 0 && avail[i] == want)
            return want;
        if (avail[i] > best)
            best = avail[i];
    }
    return want > 0 ? 0 : best;
}

/* Should we issue a ChangeDisplaySettings right now?
 * Only when we have a target, the display REPORTS a real rate, and it is not
 * already on the target.
 *
 * A current rate of 0 or 1 is "unknown", not "wrong": Windows 9x reports 0
 * for the live mode (measured on .243, 2026-09-29: DISPLAYCFG get -> refresh 0,
 * registry_refresh 75) and a driver can report its 1 Hz "default". Forcing on
 * an unknown re-applies the mode on EVERY 1 s poll, the re-apply cannot make
 * the reading real, and a CRT re-syncs once a second for the whole session. */
static int rk_should_force(int current_hz, int target_hz)
{
    return target_hz > 0 && rk_hz_is_real(current_hz) && current_hz != target_hz;
}

/* A re-apply BUDGET. A mode that keeps drifting off target is being set by
 * someone else - a Direct3D / DirectDraw title that loses its device when we
 * change the mode under it, resets it at no rate, and so loses it again: a
 * mode-switch ping-pong on a CRT. So at most RK_MAX_FORCES re-applies inside
 * any RK_FORCE_WINDOW_S seconds, and past that refreshkeep stops for good and
 * says so. times[] holds the seconds of the re-applies made so far. */
#define RK_MAX_FORCES     3
#define RK_FORCE_WINDOW_S 60

static int rk_budget_ok(const int *times, int n, int now_s)
{
    int i, recent = 0;
    for (i = 0; i < n; i++)
        if (now_s - times[i] < RK_FORCE_WINDOW_S)
            recent++;
    return recent < RK_MAX_FORCES;
}

#endif /* REFRESHLOGIC_H */
