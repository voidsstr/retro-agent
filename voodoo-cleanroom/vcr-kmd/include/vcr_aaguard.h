/*
 * vcr_aaguard.h - the AA auto-disarm after a freeze (2026-09-30). Pure
 * decisions; no kernel, no Win32 (tests/native/test_vcr_aaguard.c).
 *
 * WHY. On 2026-09-29 Quake II at SSTH3_SLI_AA_CONFIGURATION = 6 (2x AA, four
 * chips) froze .124 hard. The setting and Diag\SliAA stayed armed, so the next
 * boot froze too, the moment a game was started from the desktop: vcrphases
 * --prev ended at SLI SET_DONE, 130 s into boot #46. A frozen box cannot
 * disarm itself - the NEXT boot must.
 *
 * HOW. The miniport keeps Diag\SliAALive (DWORD, flushed): 1 from the moment an
 * enable that includes AA succeeds, 0 again when the session is taken down
 * (VcrSliOff: Glide's disable, a re-enable, a mode set, a display reset). A
 * boot that finds it still 1 is a boot after one that ended with AA live -
 * a freeze or a power cut in the middle of AA. That boot:
 *   - sets Diag\SliAA = 0 (the kernel's AA kill switch; re-arming is then a
 *     deliberate act again),
 *   - puts Glide's SSTH3_SLI_AA_CONFIGURATION back to the board's non-AA SLI
 *     value, IF it holds an AA value (so games start normally instead of
 *     opening Glide into a refused AA request),
 *   - records Diag\SliAAAutoOff = the boot count of the boot that died, and a
 *     recorder event, so the operator and the control panel can say why.
 */
#ifndef VCR_AAGUARD_H
#define VCR_AAGUARD_H

#ifdef __GNUC__
#define VCR_AAG_UNUSED __attribute__((unused))
#else
#define VCR_AAG_UNUSED
#endif

/* Glide h5's SSTH3_SLI_AA_CONFIGURATION table (retro3dfx-glide
 * minihwc/h5sliaa.h h5SliAaConfigEnv): 1 = single chip + 2x, 3/6 = 2x,
 * 4/7 = 4x, 8 = 8x. Everything else asks no AA. */
VCR_AAG_UNUSED static int vcr_aag_cfg_is_aa(unsigned cfg)
{
    return cfg == 1 || cfg == 3 || cfg == 4 || cfg == 6 || cfg == 7 || cfg == 8;
}

/* The "all chips in SLI, no AA" value for a board of n chips - the value the
 * 3dfx Control Panel recommends (scripts/3dfx/3dfxctl ctl_logic.h). */
VCR_AAG_UNUSED static unsigned vcr_aag_safe_cfg(unsigned chips)
{
    return chips >= 4 ? 5u : chips == 2 ? 2u : 0u;
}

/* A REG_SZ digit string ("6", "6\0", " 6") -> value; anything else -> -1, so an
 * unreadable value is never "fixed" by a guess. */
VCR_AAG_UNUSED static long vcr_aag_parse(const unsigned short *s, unsigned nchars)
{
    unsigned i = 0;
    long v = 0;
    int digits = 0;
    while (i < nchars && s[i] == ' ')
        i++;
    for (; i < nchars && s[i]; i++) {
        if (s[i] < '0' || s[i] > '9')
            return -1;
        v = v * 10 + (s[i] - '0');
        if (++digits > 3)
            return -1;
    }
    return digits ? v : -1;
}

enum {
    VCR_AAG_NOTHING = 0,    /* the last boot ended with no AA live */
    VCR_AAG_DISARM = 1,     /* it did: SliAA = 0, record it */
};

VCR_AAG_UNUSED static int vcr_aag_boot(unsigned long live_marker)
{
    return live_marker == 1 ? VCR_AAG_DISARM : VCR_AAG_NOTHING;
}

#endif /* VCR_AAGUARD_H */
