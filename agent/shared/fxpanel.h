/*
 * fxpanel.h - should this box have the 3dfx Control Panel, and is its copy
 * current? (agent 1.94.0)
 *
 * The panel (scripts/3dfx/3dfxctl) tunes a Banshee / Voodoo3 / Voodoo4 /
 * Voodoo5 on Windows XP-class NT: SLI and anti-aliasing, vsync, gamma, the
 * live core clock. The user asked for it to be on the desktop, with the 3dfx
 * logo, on every box that has such a card - deployed by the agent when it
 * loads, not by someone remembering push_3dfxctl.py. src/fxpanel.c does the
 * Win32 half; this is the decision, Win32-free so
 * tests/native/test_fxpanel.c compiles exactly what the agent runs.
 *
 * WHICH CARDS. The panel's two lanes are the VSA-100/Avenger display drivers
 * (vcr-kmd and the vintage H5 / AmigaMerlin one), so a card qualifies by its
 * PCI device id: 0003/0004 Banshee, 0005 Voodoo3, 0009 VSA-100 (Voodoo4 4500,
 * Voodoo5 5500/6000). A Voodoo 1/2 (0001/0002) is a 3D-only add-in with no
 * display driver and nothing for the panel to set, so it does not. The card
 * must also be PRESENT with a driver installed - a pulled card leaves its
 * Enum key behind (.124 and .133 "kept" Voodoos they no longer contained).
 *
 * WHICH WINDOWS. NT 5.x only (2000/XP/2003): the panel is built for XP SP3
 * (-D_WIN32_WINNT=0x0501, every import checked against XP's exports), no
 * 3dfx display driver exists for Vista or later, and on Windows 9x it would
 * not load. A modern host is refused before this is ever asked
 * (host_manages_this_box).
 *
 * WHEN TO COPY. A file is copied when it is missing on the box OR differs from
 * the share's copy in size or write time (a newer panel on the share reaches
 * the box at its next agent start). A matching file is left alone - so a
 * settled box does nothing.
 *
 * NOT A DRIVER CHANGE. Copying an application and a shortcut changes no
 * setting and no driver, so this sits outside the "3dfx drivers are never
 * changed automatically" rule (drvsafe.h). The panel itself changes nothing
 * until a person presses a button in it.
 */
#ifndef RETRO_FXPANEL_H
#define RETRO_FXPANEL_H

#ifdef __GNUC__
#define FXP_UNUSED __attribute__((unused))
#else
#define FXP_UNUSED
#endif

/* The 3dfx PCI device ids the panel serves. */
FXP_UNUSED static int fxpanel_dev_has_panel(unsigned dev)
{
    return dev == 0x0003 || dev == 0x0004 || dev == 0x0005 || dev == 0x0009;
}

/* NT 5.x only. is_nt: VER_PLATFORM_WIN32_NT. */
FXP_UNUSED static int fxpanel_os_ok(int is_nt, unsigned major)
{
    return is_nt && major == 5;
}

enum {
    FXP_OFF = 1,        /* HKLM\Software\RetroAgent\FxPanel = 0 */
    FXP_NOT_OS,         /* not NT 5.x */
    FXP_NO_CARD,        /* no present Banshee/V3/V4/V5 with a driver */
    FXP_DEPLOY          /* applicable: make sure the files are current */
};

FXP_UNUSED static int fxpanel_decide(int switched_off, int is_nt, unsigned major,
                                     int card_present)
{
    if (switched_off)
        return FXP_OFF;
    if (!fxpanel_os_ok(is_nt, major))
        return FXP_NOT_OS;
    if (!card_present)
        return FXP_NO_CARD;
    return FXP_DEPLOY;
}

/* Copy this file? Missing, or a different size or write time from the share's
 * copy. `*_time` is the FILETIME as one 64-bit number. A write time within
 * 2 s counts as equal: FAT keeps 2-second times, so a copy onto a FAT volume
 * can never match NTFS's to the tick and would be copied at every start. */
FXP_UNUSED static int fxpanel_need_copy(int have_local,
                                        unsigned long long local_size,
                                        unsigned long long local_time,
                                        unsigned long long src_size,
                                        unsigned long long src_time)
{
    unsigned long long d;
    if (!have_local)
        return 1;
    if (local_size != src_size)
        return 1;
    d = local_time > src_time ? local_time - src_time : src_time - local_time;
    return d > 20000000ULL;             /* 2 s in 100 ns units */
}

#endif /* RETRO_FXPANEL_H */
