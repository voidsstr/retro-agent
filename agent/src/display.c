/*
 * display.c - Display configuration (get/set resolution)
 * Uses EnumDisplaySettingsA and ChangeDisplaySettingsA.
 */

#include "handlers.h"
#include "protocol.h"
#include "util.h"
#include "log.h"
#include <windows.h>
#include <stdio.h>
#include <string.h>

#define LOG_DISPLAY "DISPLAY"

static void display_get(SOCKET sock)
{
    DEVMODEA dm;
    json_t j;
    char *result;

    ZeroMemory(&dm, sizeof(dm));
    dm.dmSize = sizeof(dm);

    if (!EnumDisplaySettingsA(NULL, ENUM_CURRENT_SETTINGS, &dm)) {
        send_error_response(sock, "EnumDisplaySettingsA failed");
        return;
    }

    log_msg(LOG_DISPLAY, "Current: %lux%lu %lubpp %luHz (dmFields=0x%08lX)",
            dm.dmPelsWidth, dm.dmPelsHeight,
            dm.dmBitsPerPel, dm.dmDisplayFrequency, dm.dmFields);

    json_init(&j);
    json_object_start(&j);
    json_kv_uint(&j, "width", dm.dmPelsWidth);
    json_kv_uint(&j, "height", dm.dmPelsHeight);
    json_kv_uint(&j, "bpp", dm.dmBitsPerPel);
    json_kv_uint(&j, "refresh", dm.dmDisplayFrequency);

    /* Also report the registry refresh rate if the driver reports 0 */
    if (dm.dmDisplayFrequency == 0) {
        DEVMODEA reg_dm;
        ZeroMemory(&reg_dm, sizeof(reg_dm));
        reg_dm.dmSize = sizeof(reg_dm);
        if (EnumDisplaySettingsA(NULL, ENUM_REGISTRY_SETTINGS, &reg_dm)) {
            json_kv_uint(&j, "registry_refresh", reg_dm.dmDisplayFrequency);
        }
    }

    json_object_end(&j);

    result = json_finish(&j);
    send_text_response(sock, result);
    json_free(&j);
}

static void display_set(SOCKET sock, const char *args)
{
    DEVMODEA dm;
    LONG cds_result;
    int width = 0, height = 0, bpp = 0, refresh = 0;
    int parsed;
    json_t j;
    char *result;

    /* Parse: <width> <height> <bpp> [refresh] */
    parsed = sscanf(args, "%d %d %d %d", &width, &height, &bpp, &refresh);
    if (parsed < 3 || width <= 0 || height <= 0 || bpp <= 0) {
        send_error_response(sock,
            "Usage: DISPLAYCFG set <width> <height> <bpp> [refresh]");
        return;
    }

    log_msg(LOG_DISPLAY, "Setting: %dx%d %dbpp %dHz",
            width, height, bpp, refresh);

    ZeroMemory(&dm, sizeof(dm));
    dm.dmSize = sizeof(dm);
    dm.dmPelsWidth = (DWORD)width;
    dm.dmPelsHeight = (DWORD)height;
    dm.dmBitsPerPel = (DWORD)bpp;
    dm.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_BITSPERPEL;

    if (refresh > 0) {
        dm.dmDisplayFrequency = (DWORD)refresh;
        dm.dmFields |= DM_DISPLAYFREQUENCY;
    }

    /* Save to registry first, then apply dynamically.
     * Some drivers (e.g. Voodoo3) only save with CDS_UPDATEREGISTRY
     * but don't force a mode change until a separate call with 0. */
    ChangeDisplaySettingsA(&dm, CDS_UPDATEREGISTRY);
    cds_result = ChangeDisplaySettingsA(&dm, 0);

    json_init(&j);
    json_object_start(&j);

    switch (cds_result) {
    case DISP_CHANGE_SUCCESSFUL:
        log_msg(LOG_DISPLAY, "ChangeDisplaySettings succeeded");
        json_kv_str(&j, "status", "ok");
        json_kv_uint(&j, "width", dm.dmPelsWidth);
        json_kv_uint(&j, "height", dm.dmPelsHeight);
        json_kv_uint(&j, "bpp", dm.dmBitsPerPel);
        if (refresh > 0)
            json_kv_uint(&j, "refresh", dm.dmDisplayFrequency);
        break;

    case DISP_CHANGE_RESTART:
        log_msg(LOG_DISPLAY, "ChangeDisplaySettings requires restart");
        json_kv_str(&j, "status", "restart_required");
        json_kv_uint(&j, "width", dm.dmPelsWidth);
        json_kv_uint(&j, "height", dm.dmPelsHeight);
        json_kv_uint(&j, "bpp", dm.dmBitsPerPel);
        if (refresh > 0)
            json_kv_uint(&j, "refresh", dm.dmDisplayFrequency);
        break;

    case DISP_CHANGE_BADFLAGS:
        log_msg(LOG_DISPLAY, "ChangeDisplaySettings: bad flags");
        json_kv_str(&j, "status", "error");
        json_kv_str(&j, "error", "bad flags");
        break;

    case DISP_CHANGE_BADPARAM:
        log_msg(LOG_DISPLAY, "ChangeDisplaySettings: bad parameter");
        json_kv_str(&j, "status", "error");
        json_kv_str(&j, "error", "bad parameter - mode may not be supported");
        break;

    case DISP_CHANGE_FAILED:
        log_msg(LOG_DISPLAY, "ChangeDisplaySettings: failed");
        json_kv_str(&j, "status", "error");
        json_kv_str(&j, "error", "display driver failed the mode change");
        break;

    case DISP_CHANGE_BADMODE:
        log_msg(LOG_DISPLAY, "ChangeDisplaySettings: bad mode");
        json_kv_str(&j, "status", "error");
        json_kv_str(&j, "error", "mode not supported by display driver");
        break;

    case DISP_CHANGE_NOTUPDATED:
        log_msg(LOG_DISPLAY, "ChangeDisplaySettings: not updated (registry)");
        json_kv_str(&j, "status", "error");
        json_kv_str(&j, "error", "unable to write settings to registry");
        break;

    default:
        log_msg(LOG_DISPLAY, "ChangeDisplaySettings: unknown result %ld",
                cds_result);
        json_kv_str(&j, "status", "error");
        {
            char err_msg[64];
            _snprintf(err_msg, sizeof(err_msg),
                      "unknown result code %ld", cds_result);
            json_kv_str(&j, "error", err_msg);
        }
        break;
    }

    json_object_end(&j);

    result = json_finish(&j);
    send_text_response(sock, result);
    json_free(&j);
}

void handle_displaycfg(SOCKET sock, const char *args)
{
    if (!args || !args[0]) {
        send_error_response(sock,
            "Usage: DISPLAYCFG get | DISPLAYCFG set <width> <height> <bpp> [refresh]");
        return;
    }

    if (str_starts_with(args, "get")) {
        display_get(sock);
    } else if (str_starts_with(args, "set")) {
        const char *params = args + 3;
        params = str_skip_spaces(params);
        display_set(sock, params);
    } else {
        send_error_response(sock,
            "Unknown subcommand. Use: DISPLAYCFG get | DISPLAYCFG set <width> <height> <bpp> [refresh]");
    }
}
