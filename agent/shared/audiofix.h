/*
 * audiofix.h - a sound card with a driver and no wave device: repair it?
 *
 * Found 2026-09-26 on a freshly PXE-imaged Dell Dimension 4600. Agent 1.85.1
 * installed its SoundMAX driver (smwdmCH5.inf, "installed - device working"),
 * Device Manager showed no problem code, the Windows Audio service ran - and
 * waveOutGetNumDevs() was 0, before and after a reboot. The card's INF
 * Needs=WDMAUDIO.Registration, which writes HKLM\...\RunOnce entries
 * ("rundll32 streamci.dll,StreamingDeviceSetup ...") that register XP's kernel
 * audio stack - sysaudio, kmixer, wdmaud - with the software-device
 * enumerator. RunOnce was empty afterwards, swenum\Devices held none of those
 * registrations, and sysaudio.sys/kmixer.sys/wdmaud.sys were not even on disk:
 * the entries had been consumed without running (by what is unproven). Running
 * the same commands from the box's own wdmaudio.inf installed eight software
 * devices and the wave device appeared at once, with no reboot.
 *
 * So the repair is exactly what Windows would have done, driven from the
 * box's own INF, and only when all of this is true:
 *   - NT (9x's audio stack is a different world),
 *   - no wave-out device at all,
 *   - a HARDWARE sound device carries a driver (a MEDIA-class instance that is
 *     not software/root-enumerated and has a Drivers subkey - a Voodoo 2 is
 *     class MEDIA too and has none),
 *   - sysaudio's registration is absent (if it is present, the fault is
 *     something else and this repair would change nothing),
 *   - fewer than AUDIOFIX_MAX_ATTEMPTS tries so far.
 *
 * Win32-free so tests/native/test_audiofix.c compiles the code the agent runs.
 */
#ifndef RETRO_AUDIOFIX_H
#define RETRO_AUDIOFIX_H

#ifdef __GNUC__
#define AF_UNUSED __attribute__((unused))
#else
#define AF_UNUSED
#endif

#define AUDIOFIX_MAX_ATTEMPTS 2

/* KSCATEGORY_SYSAUDIO's software-device registration, under
 * HKLM\SYSTEM\CurrentControlSet\Services\swenum\Devices. */
#define AUDIOFIX_SYSAUDIO_KEY \
    "SYSTEM\\CurrentControlSet\\Services\\swenum\\Devices\\{A7C7A5B0-5AF3-11D1-9CED-00A024BF0407}"

#define AUDIOFIX_NONE        0   /* nothing to do */
#define AUDIOFIX_REPAIR      1
#define AUDIOFIX_OTHER_FAULT 2   /* no wave device, but not this fault: say so */
#define AUDIOFIX_GAVE_UP     3   /* this fault, attempts exhausted: say so */

AF_UNUSED static int audiofix_decide(int is_nt, unsigned wave_devices, int hw_audio_bound,
                           int sysaudio_registered, unsigned attempts)
{
    if (!is_nt || wave_devices > 0 || !hw_audio_bound)
        return AUDIOFIX_NONE;
    if (sysaudio_registered)
        return AUDIOFIX_OTHER_FAULT;
    if (attempts >= AUDIOFIX_MAX_ATTEMPTS)
        return AUDIOFIX_GAVE_UP;
    return AUDIOFIX_REPAIR;
}

/* A MEDIA-class instance's MatchingDeviceId names hardware unless it is one of
 * XP's own software entries (ms_mmmci, ms_mmdrv, ...), a software-enumerated
 * device (sw\{...}) or a root-enumerated legacy one. Case-insensitive. */
AF_UNUSED static int audiofix_is_hw_id(const char *id)
{
    const char *p;
    char c0, c1, c2;
    if (!id || !*id)
        return 0;
    p = id;
    c0 = (char)(p[0] | 0x20);
    c1 = p[1] ? (char)(p[1] | 0x20) : 0;
    c2 = (p[1] && p[2]) ? (char)(p[2] | 0x20) : 0;
    if (c0 == 'm' && c1 == 's' && p[1] && p[2] == '_')
        return 0;
    if (c0 == 's' && c1 == 'w' && p[1] && p[2] == '\\')
        return 0;
    if (c0 == 'r' && c1 == 'o' && c2 == 'o' && p[3] && (p[3] | 0x20) == 't' && p[4] == '\\')
        return 0;
    return 1;
}

/* A RunOnce value this repair may run: wdmaudio.inf's streamci registrations
 * and nothing else. */
AF_UNUSED static int audiofix_is_streamci_cmd(const char *cmd)
{
    static const char needle[] = "streamingdevicesetup";
    const char *s;
    size_t i, n = sizeof(needle) - 1;
    if (!cmd)
        return 0;
    for (s = cmd; *s; s++) {
        for (i = 0; i < n && s[i]; i++)
            if ((char)(s[i] | 0x20) != needle[i])
                break;
        if (i == n)
            return 1;
    }
    return 0;
}

#endif
