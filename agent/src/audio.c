/*
 * audio.c - Audio device information
 * Enumerates wave output devices via waveOutGetNumDevs/waveOutGetDevCapsA.
 * Reads driver info from the registry.
 * Requires -lwinmm for waveOut functions.
 */

#include "handlers.h"
#include "protocol.h"
#include "util.h"
#include "log.h"
#include <windows.h>
#include <mmsystem.h>
#include <stdio.h>
#include <string.h>

#define LOG_AUDIO "AUDIO"

/*
 * Build a human-readable string of supported wave formats.
 * dwFormats is a bitmask from WAVEOUTCAPS.
 */
static void format_wave_formats(DWORD fmt, char *buf, int bufsize)
{
    buf[0] = '\0';

    /* Check for key sample rates */
    if (fmt & (WAVE_FORMAT_1M08 | WAVE_FORMAT_1M16 |
               WAVE_FORMAT_1S08 | WAVE_FORMAT_1S16))
        strcat(buf, "11kHz ");
    if (fmt & (WAVE_FORMAT_2M08 | WAVE_FORMAT_2M16 |
               WAVE_FORMAT_2S08 | WAVE_FORMAT_2S16))
        strcat(buf, "22kHz ");
    if (fmt & (WAVE_FORMAT_4M08 | WAVE_FORMAT_4M16 |
               WAVE_FORMAT_4S08 | WAVE_FORMAT_4S16))
        strcat(buf, "44kHz ");

    /* Check for stereo */
    if (fmt & (WAVE_FORMAT_1S08 | WAVE_FORMAT_1S16 |
               WAVE_FORMAT_2S08 | WAVE_FORMAT_2S16 |
               WAVE_FORMAT_4S08 | WAVE_FORMAT_4S16))
        strcat(buf, "stereo ");

    /* Check for 16-bit */
    if (fmt & (WAVE_FORMAT_1M16 | WAVE_FORMAT_1S16 |
               WAVE_FORMAT_2M16 | WAVE_FORMAT_2S16 |
               WAVE_FORMAT_4M16 | WAVE_FORMAT_4S16))
        strcat(buf, "16bit ");

    /* Trim trailing space */
    {
        int len = (int)strlen(buf);
        if (len > 0 && buf[len - 1] == ' ')
            buf[len - 1] = '\0';
    }

    (void)bufsize;
}

/*
 * Read audio driver info from registry.
 * Win98: HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Drivers
 * Also try: HKLM\System\CurrentControlSet\Services\Class\MEDIA
 */
static void read_driver_info(json_t *j)
{
    HKEY hkey;
    char wavemapper[256] = "";
    char aux[256] = "";
    char midi[256] = "";

    /* Read Drivers key for current wave/midi driver names */
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE,
                      "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Drivers",
                      0, KEY_READ, &hkey) == ERROR_SUCCESS) {
        DWORD type, size;

        size = sizeof(wavemapper);
        if (RegQueryValueExA(hkey, "wavemapper", NULL, &type,
                             (BYTE *)wavemapper, &size) != ERROR_SUCCESS)
            wavemapper[0] = '\0';

        size = sizeof(aux);
        if (RegQueryValueExA(hkey, "aux", NULL, &type,
                             (BYTE *)aux, &size) != ERROR_SUCCESS)
            aux[0] = '\0';

        size = sizeof(midi);
        if (RegQueryValueExA(hkey, "midi", NULL, &type,
                             (BYTE *)midi, &size) != ERROR_SUCCESS)
            midi[0] = '\0';

        RegCloseKey(hkey);
    }

    json_key(j, "system_drivers");
    json_object_start(j);
    json_kv_str(j, "wavemapper", wavemapper);
    json_kv_str(j, "aux", aux);
    json_kv_str(j, "midi", midi);
    json_object_end(j);
}

/*
 * Enumerate MEDIA class entries from registry for driver descriptions.
 * Win98: HKLM\System\CurrentControlSet\Services\Class\MEDIA\*
 * WinXP: HKLM\SYSTEM\CurrentControlSet\Control\Class\{4D36E96C-...}\*
 */
static void add_media_class_info(json_t *j)
{
    HKEY hkey;
    OSVERSIONINFOA osvi;
    const char *base_path;
    char subkey_name[64];
    DWORD index, name_len;

    osvi.dwOSVersionInfoSize = sizeof(osvi);
    GetVersionExA(&osvi);

    if (osvi.dwPlatformId == VER_PLATFORM_WIN32_NT)
        base_path = "SYSTEM\\CurrentControlSet\\Control\\Class\\"
                    "{4D36E96C-E325-11CE-BFC1-08002BE10318}";
    else
        base_path = "System\\CurrentControlSet\\Services\\Class\\MEDIA";

    json_key(j, "media_class_entries");
    json_array_start(j);

    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, base_path, 0, KEY_READ, &hkey)
        == ERROR_SUCCESS) {

        for (index = 0; ; index++) {
            HKEY hsubkey;
            char full_path[512];
            char desc[256] = "";
            char drv[256] = "";
            char mfg[256] = "";

            name_len = sizeof(subkey_name);
            if (RegEnumKeyExA(hkey, index, subkey_name, &name_len,
                              NULL, NULL, NULL, NULL) != ERROR_SUCCESS)
                break;

            /* Skip non-numeric entries */
            if (subkey_name[0] < '0' || subkey_name[0] > '9')
                continue;

            _snprintf(full_path, sizeof(full_path), "%s\\%s",
                      base_path, subkey_name);

            if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, full_path, 0, KEY_READ,
                              &hsubkey) == ERROR_SUCCESS) {
                DWORD type, size;

                size = sizeof(desc);
                if (RegQueryValueExA(hsubkey, "DriverDesc", NULL, &type,
                                     (BYTE *)desc, &size) != ERROR_SUCCESS)
                    desc[0] = '\0';

                size = sizeof(drv);
                if (RegQueryValueExA(hsubkey, "Driver", NULL, &type,
                                     (BYTE *)drv, &size) != ERROR_SUCCESS)
                    drv[0] = '\0';

                size = sizeof(mfg);
                if (RegQueryValueExA(hsubkey, "ProviderName", NULL, &type,
                                     (BYTE *)mfg, &size) != ERROR_SUCCESS) {
                    size = sizeof(mfg);
                    if (RegQueryValueExA(hsubkey, "Mfg", NULL, &type,
                                         (BYTE *)mfg, &size) != ERROR_SUCCESS)
                        mfg[0] = '\0';
                }

                RegCloseKey(hsubkey);

                /* Only include entries with a driver description */
                if (desc[0]) {
                    json_object_start(j);
                    json_kv_str(j, "index", subkey_name);
                    json_kv_str(j, "description", desc);
                    json_kv_str(j, "driver", drv);
                    json_kv_str(j, "provider", mfg);
                    json_object_end(j);
                }
            }
        }

        RegCloseKey(hkey);
    }

    json_array_end(j);
}

void handle_audioinfo(SOCKET sock)
{
    json_t j;
    char *result;
    UINT num_devs;
    UINT i;

    json_init(&j);
    json_object_start(&j);

    /* Enumerate wave output devices */
    num_devs = waveOutGetNumDevs();
    log_msg(LOG_AUDIO, "waveOutGetNumDevs() = %u", num_devs);

    json_kv_uint(&j, "wave_out_count", num_devs);

    json_key(&j, "wave_out_devices");
    json_array_start(&j);

    for (i = 0; i < num_devs; i++) {
        WAVEOUTCAPSA caps;
        char format_str[128];

        ZeroMemory(&caps, sizeof(caps));
        if (waveOutGetDevCapsA(i, &caps, sizeof(caps)) == MMSYSERR_NOERROR) {
            log_msg(LOG_AUDIO, "  Device %u: \"%s\" channels=%u formats=0x%08lX",
                    i, caps.szPname, caps.wChannels,
                    (unsigned long)caps.dwFormats);

            format_wave_formats(caps.dwFormats, format_str, sizeof(format_str));

            json_object_start(&j);
            json_kv_uint(&j, "device_id", i);
            json_kv_str(&j, "name", caps.szPname);
            json_kv_uint(&j, "manufacturer_id", caps.wMid);
            json_kv_uint(&j, "product_id", caps.wPid);
            json_kv_uint(&j, "channels", caps.wChannels);

            /* Driver version: major.minor */
            {
                char ver[16];
                _snprintf(ver, sizeof(ver), "%u.%u",
                          (caps.vDriverVersion >> 8) & 0xFF,
                          caps.vDriverVersion & 0xFF);
                json_kv_str(&j, "driver_version", ver);
            }

            json_kv_str(&j, "formats", format_str);

            /* Capabilities flags */
            json_key(&j, "capabilities");
            json_object_start(&j);
            json_kv_bool(&j, "volume", (caps.dwSupport & WAVECAPS_VOLUME) != 0);
            json_kv_bool(&j, "lr_volume", (caps.dwSupport & WAVECAPS_LRVOLUME) != 0);
            json_kv_bool(&j, "pitch", (caps.dwSupport & WAVECAPS_PITCH) != 0);
            json_kv_bool(&j, "playback_rate",
                         (caps.dwSupport & WAVECAPS_PLAYBACKRATE) != 0);
            json_kv_bool(&j, "sync", (caps.dwSupport & WAVECAPS_SYNC) != 0);
            json_object_end(&j);

            json_object_end(&j);
        }
    }

    json_array_end(&j);

    /* System driver registry info */
    read_driver_info(&j);

    /* Media class entries */
    add_media_class_info(&j);

    json_object_end(&j);

    result = json_finish(&j);
    send_text_response(sock, result);
    json_free(&j);
}
