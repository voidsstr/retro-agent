/*
 * monpower.h - "the monitor never sleeps, the screensaver runs" (agent 1.96.0).
 *
 * User directive 2026-09-29: "update all of the retro computers so the monitor
 * does not go to sleep, it just has the screensaver activated. the agent
 * should set this up on startup".
 *
 * THE OLD BEHAVIOUR (<= 1.95.x): retrowall set the Starfield screensaver and
 * never looked at power management. Measured on the fleet that day:
 *   - .184 (XP)    "Turn off monitor (AC) After 20 mins", DC 5 mins;
 *   - .243 (Win98) USER_POWER_POLICY VideoTimeoutAc 900 s, VideoTimeoutDc 120 s,
 *                  and IdleTimeoutAc 1200 s with IdleAc = PowerActionSleep;
 *   - .133 / .143  already "Never" (someone had set it by hand).
 * The screensaver starts at 10 minutes and the monitor is switched off under
 * it later - the monitor "goes to sleep", which is what the user saw.
 *
 * NOW, on every agent start of a managed box (never a Windows 10/11 host, see
 * hostpolicy.h), monpower.c drives the ACTIVE power scheme's
 *   monitor timeout, system standby timeout, hibernate timeout  (AC and DC)
 * to 0 = never, and (pre-Vista only) SPI_SETPOWEROFFACTIVE /
 * SPI_SETLOWPOWERACTIVE to FALSE. The screensaver itself stays retrowall's.
 *
 * Mechanisms, by Windows:
 *   Win98 / XP  powrprof GetActivePwrScheme + ReadPwrScheme / WritePwrScheme /
 *               SetActivePwrScheme (Win98 has them too - its scheme blob is
 *               the same USER_POWER_POLICY, read off .243). All LoadLibrary'd:
 *               the agent imports nothing from powrprof.dll, see ntdyn.h.
 *   Win7 (6.x)  PowerGetActiveScheme + Power{Read,Write}{AC,DC}ValueIndex +
 *               PowerSetActiveScheme on the GUID settings below.
 *
 * Compare first, write only what differs, read every write back, and log a
 * failure loudly. A settled box writes nothing and logs one line.
 *
 * Win32-free so tests/native/test_monpower.c compiles exactly this.
 */
#ifndef RETRO_MONPOWER_H
#define RETRO_MONPOWER_H

#ifdef __GNUC__
#define MP_UNUSED __attribute__((unused))
#else
#define MP_UNUSED
#endif

/* HKLM\Software\RetroAgent\MonitorNeverSleep (DWORD). Absent or non-zero =
 * enforce; 0 = leave power management alone. */
#define MP_REG_SWITCH  "MonitorNeverSleep"
/* HKLM\Software\RetroAgent\MonitorPowerBoot (REG_SZ) - what the last pass did,
 * readable with REGREAD on a Win9x box where EXEC is unsafe. */
#define MP_REG_RESULT  "MonitorPowerBoot"

/* The value every enforced timeout must hold: 0 seconds = never. */
#define MP_NEVER 0UL

/* Enforce at all? `present` = the switch value exists as a DWORD. */
MP_UNUSED static int mp_enabled(int present, unsigned long value)
{
    return !present || value != 0;
}

/* Which scheme API to drive. */
#define MP_API_NONE 0
#define MP_API_OLD  1   /* GetActivePwrScheme/ReadPwrScheme/WritePwrScheme (98, XP) */
#define MP_API_NEW  2   /* PowerGetActiveScheme/Power*ValueIndex (Vista+) */

/* NT 6+ gets the GUID API: the XP functions still exist in Windows 7's
 * powrprof but are compatibility shims documented as "may be altered or
 * unavailable" - the GUID settings are what powercfg -q shows. A pre-Vista
 * box never has the new API; if an NT6 box somehow lacks it, the old one is
 * still better than nothing. */
MP_UNUSED static int mp_pick_api(unsigned long os_major, int is_nt,
                                 int have_new, int have_old)
{
    if (is_nt && os_major >= 6 && have_new)
        return MP_API_NEW;
    if (have_old)
        return MP_API_OLD;
    return MP_API_NONE;
}

/* SPI_{GET,SET}POWEROFFACTIVE / LOWPOWERACTIVE: set to FALSE only on a
 * Windows that still has them (9x and NT 5.x - Vista dropped both), only when
 * the GET worked (an unsupported index must not read as "on" and be re-set on
 * every boot), and only when it is on. */
MP_UNUSED static int mp_spi_should_clear(unsigned long os_major, int is_nt,
                                         int get_ok, int active)
{
    if (is_nt && os_major >= 6)
        return 0;
    return get_ok && active != 0;
}

/* One enforced timeout: does it need a write? A value that could not be read
 * is NOT written - the old-API write replaces the whole scheme, so a failed
 * read leaves nothing safe to write back. The caller logs it as unreadable. */
MP_UNUSED static int mp_needs_write(int known, unsigned long cur)
{
    return known && cur != MP_NEVER;
}

/* Zero every field of `f[0..n)` that is not already 0. Returns how many
 * changed - 0 on a settled box, which means "write nothing". */
MP_UNUSED static int mp_zero_fields(unsigned long *const *f, int n)
{
    int i, changed = 0;
    for (i = 0; i < n; i++)
        if (*f[i] != MP_NEVER) {
            *f[i] = MP_NEVER;
            changed++;
        }
    return changed;
}

/* The read-back verdict: every field 0. */
MP_UNUSED static int mp_all_never(const unsigned long *v, int n)
{
    int i;
    for (i = 0; i < n; i++)
        if (v[i] != MP_NEVER)
            return 0;
    return 1;
}

/* ---- USER_POWER_POLICY layout (winnt.h; identical on Win98 and XP) --------
 * Offsets pinned here so the native test can decode the real .243 blob and
 * monpower.c can static-assert the compiler's struct against the same numbers.
 * Revision(4) IdleAc(12) IdleDc(12) IdleTimeoutAc IdleTimeoutDc
 * IdleSensitivityAc/Dc ThrottlePolicyAc/Dc (4 x UCHAR) MaxSleepAc MaxSleepDc
 * Reserved[2] VideoTimeoutAc VideoTimeoutDc SpindownTimeoutAc/Dc ... */
#define MP_UPP_IDLE_AC    28
#define MP_UPP_IDLE_DC    32
#define MP_UPP_VIDEO_AC   56
#define MP_UPP_VIDEO_DC   60
#define MP_UPP_MIN_SIZE   64

MP_UNUSED static unsigned long mp_le32(const unsigned char *p)
{
    return (unsigned long)p[0] | ((unsigned long)p[1] << 8) |
           ((unsigned long)p[2] << 16) | ((unsigned long)p[3] << 24);
}

/* ---- Vista+ setting GUIDs (winnt.h / powrprof), spelled out so the agent
 * needs no GUID import library. Order = the order monpower.c reports them. */
typedef struct {
    unsigned long  d1;
    unsigned short d2, d3;
    unsigned char  d4[8];
} mp_guid_t;

/* GUID_VIDEO_SUBGROUP 7516b95f-f776-4464-8c53-06167f40cc99 */
#define MP_GUID_VIDEO_SUBGROUP { 0x7516b95fUL, 0xf776, 0x4464, \
    { 0x8c, 0x53, 0x06, 0x16, 0x7f, 0x40, 0xcc, 0x99 } }
/* GUID_VIDEO_POWERDOWN_TIMEOUT (VIDEOIDLE) 3c0bc021-c8a8-4e07-a973-6b14cbcb2b7e */
#define MP_GUID_VIDEOIDLE { 0x3c0bc021UL, 0xc8a8, 0x4e07, \
    { 0xa9, 0x73, 0x6b, 0x14, 0xcb, 0xcb, 0x2b, 0x7e } }
/* GUID_SLEEP_SUBGROUP 238c9fa8-0aad-41ed-83f4-97be242c8f20 */
#define MP_GUID_SLEEP_SUBGROUP { 0x238c9fa8UL, 0x0aad, 0x41ed, \
    { 0x83, 0xf4, 0x97, 0xbe, 0x24, 0x2c, 0x8f, 0x20 } }
/* GUID_STANDBY_TIMEOUT (STANDBYIDLE) 29f6c1db-86da-48c5-9fdb-f2b67b1f44da */
#define MP_GUID_STANDBYIDLE { 0x29f6c1dbUL, 0x86da, 0x48c5, \
    { 0x9f, 0xdb, 0xf2, 0xb6, 0x7b, 0x1f, 0x44, 0xda } }
/* GUID_HIBERNATE_TIMEOUT (HIBERNATEIDLE) 9d7815a6-7ee4-497e-8888-515a05f02364 */
#define MP_GUID_HIBERNATEIDLE { 0x9d7815a6UL, 0x7ee4, 0x497e, \
    { 0x88, 0x88, 0x51, 0x5a, 0x05, 0xf0, 0x23, 0x64 } }

#endif /* RETRO_MONPOWER_H */
