/*
 * input.c - UI automation: window enumeration, mouse clicks, keyboard input.
 * Used for driving installer wizards via screenshot-based LLM orchestration.
 */

#include "handlers.h"
#include "protocol.h"
#include "util.h"
#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define LOG_INPUT "INPUT"
#define MAX_WINDOWS 64

/* ---------- WINLIST ---------- */

typedef struct {
    HWND  hwnd;
    char  title[256];
    char  classname[128];
    RECT  rect;
} win_info_t;

static win_info_t g_windows[MAX_WINDOWS];
static int g_window_count;

static BOOL CALLBACK enum_windows_cb(HWND hwnd, LPARAM lParam)
{
    win_info_t *w;
    (void)lParam;

    if (!IsWindowVisible(hwnd))
        return TRUE;

    if (g_window_count >= MAX_WINDOWS)
        return FALSE;

    w = &g_windows[g_window_count];
    w->hwnd = hwnd;
    w->title[0] = '\0';
    w->classname[0] = '\0';

    GetWindowTextA(hwnd, w->title, sizeof(w->title));

    /* Skip windows with empty titles (reduces noise) */
    if (w->title[0] == '\0')
        return TRUE;

    GetClassNameA(hwnd, w->classname, sizeof(w->classname));
    GetWindowRect(hwnd, &w->rect);

    g_window_count++;
    return TRUE;
}

void handle_winlist(SOCKET sock)
{
    json_t j;
    int i;
    char hwnd_hex[16];

    g_window_count = 0;
    EnumWindows(enum_windows_cb, 0);

    log_msg(LOG_INPUT, "WINLIST: found %d visible windows", g_window_count);

    json_init(&j);
    json_object_start(&j);
    json_key(&j, "windows");
    json_array_start(&j);

    for (i = 0; i < g_window_count; i++) {
        win_info_t *w = &g_windows[i];
        _snprintf(hwnd_hex, sizeof(hwnd_hex), "%08lX", (unsigned long)(DWORD)(DWORD_PTR)w->hwnd);

        json_object_start(&j);
        json_kv_str(&j, "hwnd", hwnd_hex);
        json_kv_str(&j, "title", w->title);
        json_kv_str(&j, "class", w->classname);

        json_key(&j, "rect");
        json_object_start(&j);
        json_kv_int(&j, "left", (int)w->rect.left);
        json_kv_int(&j, "top", (int)w->rect.top);
        json_kv_int(&j, "right", (int)w->rect.right);
        json_kv_int(&j, "bottom", (int)w->rect.bottom);
        json_object_end(&j);

        json_kv_bool(&j, "visible", 1);
        json_object_end(&j);
    }

    json_array_end(&j);
    json_object_end(&j);

    {
        char *result = json_finish(&j);
        if (result) {
            send_text_response(sock, result);
            HeapFree(GetProcessHeap(), 0, result);
        } else {
            send_error_response(sock, "Out of memory");
        }
    }
}

/* ---------- UICLICK ---------- */

void handle_uiclick(SOCKET sock, const char *args)
{
    int x, y;
    int right_click = 0;
    int double_click = 0;
    char buf[256];
    DWORD down_flag, up_flag;

    if (!args || !args[0]) {
        send_error_response(sock, "UICLICK requires: <x> <y> [right] [dblclick]");
        return;
    }

    /* Parse x y */
    safe_strncpy(buf, args, sizeof(buf));
    {
        char *p = buf;
        char *tok;

        tok = p;
        while (*p && *p != ' ') p++;
        if (*p) *p++ = '\0';
        x = atoi(tok);

        while (*p == ' ') p++;
        tok = p;
        while (*p && *p != ' ') p++;
        if (*p) *p++ = '\0';
        y = atoi(tok);

        /* Parse optional flags */
        while (*p) {
            while (*p == ' ') p++;
            if (_strnicmp(p, "right", 5) == 0) {
                right_click = 1;
                p += 5;
            } else if (_strnicmp(p, "dblclick", 8) == 0) {
                double_click = 1;
                p += 8;
            } else {
                while (*p && *p != ' ') p++;
            }
        }
    }

    log_msg(LOG_INPUT, "UICLICK: x=%d y=%d right=%d dbl=%d", x, y, right_click, double_click);

    down_flag = right_click ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_LEFTDOWN;
    up_flag   = right_click ? MOUSEEVENTF_RIGHTUP   : MOUSEEVENTF_LEFTUP;

    SetCursorPos(x, y);
    mouse_event(down_flag, 0, 0, 0, 0);
    mouse_event(up_flag, 0, 0, 0, 0);

    if (double_click) {
        mouse_event(down_flag, 0, 0, 0, 0);
        mouse_event(up_flag, 0, 0, 0, 0);
    }

    send_text_response(sock, "OK");
}

/* ---------- UIDRAG ---------- */

/*
 * UIDRAG <x1> <y1> <x2> <y2>
 * Mouse-down at (x1,y1), smooth move to (x2,y2), mouse-up.
 * Interpolates intermediate positions so Win9x apps see WM_MOUSEMOVE.
 */
void handle_uidrag(SOCKET sock, const char *args)
{
    int x1, y1, x2, y2;
    int dx, dy, steps, i;
    char buf[256];

    if (!args || !args[0]) {
        send_error_response(sock, "UIDRAG requires: <x1> <y1> <x2> <y2>");
        return;
    }

    safe_strncpy(buf, args, sizeof(buf));
    {
        char *p = buf;
        char *tok;

        tok = p;
        while (*p && *p != ' ') p++;
        if (*p) *p++ = '\0';
        x1 = atoi(tok);

        while (*p == ' ') p++;
        tok = p;
        while (*p && *p != ' ') p++;
        if (*p) *p++ = '\0';
        y1 = atoi(tok);

        while (*p == ' ') p++;
        tok = p;
        while (*p && *p != ' ') p++;
        if (*p) *p++ = '\0';
        x2 = atoi(tok);

        while (*p == ' ') p++;
        tok = p;
        while (*p && *p != ' ') p++;
        if (*p) *p++ = '\0';
        y2 = atoi(tok);
    }

    log_msg(LOG_INPUT, "UIDRAG: (%d,%d) -> (%d,%d)", x1, y1, x2, y2);

    /* Calculate steps — one step per ~4 pixels, capped at 100 */
    dx = x2 - x1;
    dy = y2 - y1;
    {
        double dist = sqrt((double)(dx * dx + dy * dy));
        steps = (int)(dist / 4.0);
        if (steps < 5) steps = 5;
        if (steps > 100) steps = 100;
    }

    /* Mouse-down at start position */
    SetCursorPos(x1, y1);
    Sleep(30);
    mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0);
    Sleep(30);

    /* Interpolate movement */
    for (i = 1; i <= steps; i++) {
        int cx = x1 + (dx * i) / steps;
        int cy = y1 + (dy * i) / steps;
        SetCursorPos(cx, cy);
        Sleep(5);
    }

    /* Ensure exact endpoint and release */
    SetCursorPos(x2, y2);
    Sleep(30);
    mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0);

    send_text_response(sock, "OK");
}

/* ---------- UIKEY ---------- */

/* Named key lookup table */
typedef struct {
    const char *name;
    BYTE vk;
} key_map_t;

static const key_map_t named_keys[] = {
    { "ENTER",     VK_RETURN },
    { "RETURN",    VK_RETURN },
    { "TAB",       VK_TAB },
    { "ESCAPE",    VK_ESCAPE },
    { "ESC",       VK_ESCAPE },
    { "SPACE",     VK_SPACE },
    { "BACKSPACE", VK_BACK },
    { "DELETE",    VK_DELETE },
    { "DEL",       VK_DELETE },
    { "UP",        VK_UP },
    { "DOWN",      VK_DOWN },
    { "LEFT",      VK_LEFT },
    { "RIGHT",     VK_RIGHT },
    { "HOME",      VK_HOME },
    { "END",       VK_END },
    { "PAGEUP",    VK_PRIOR },
    { "PAGEDOWN",  VK_NEXT },
    { "INSERT",    VK_INSERT },
    { "F1",        VK_F1 },
    { "F2",        VK_F2 },
    { "F3",        VK_F3 },
    { "F4",        VK_F4 },
    { "F5",        VK_F5 },
    { "F6",        VK_F6 },
    { "F7",        VK_F7 },
    { "F8",        VK_F8 },
    { "F9",        VK_F9 },
    { "F10",       VK_F10 },
    { "F11",       VK_F11 },
    { "F12",       VK_F12 },
    { NULL, 0 }
};

static BYTE lookup_named_key(const char *name)
{
    const key_map_t *k;

    for (k = named_keys; k->name; k++) {
        if (_stricmp(name, k->name) == 0)
            return k->vk;
    }

    /* Single letter A-Z */
    if (name[0] && !name[1]) {
        char c = name[0];
        if (c >= 'a' && c <= 'z') return (BYTE)(c - 'a' + 0x41);
        if (c >= 'A' && c <= 'Z') return (BYTE)c;
        if (c >= '0' && c <= '9') return (BYTE)c;
    }

    return 0;
}

static void send_key_press(BYTE vk)
{
    BYTE sc = (BYTE)MapVirtualKey(vk, 0);  /* MAPVK_VK_TO_VSC */
    keybd_event(vk, sc, 0, 0);
    keybd_event(vk, sc, KEYEVENTF_KEYUP, 0);
}

static void send_text_input(const char *text)
{
    while (*text) {
        SHORT vks = VkKeyScanA(*text);
        if (vks != -1) {
            BYTE vk = (BYTE)(vks & 0xFF);
            BYTE shift_state = (BYTE)((vks >> 8) & 0xFF);

            BYTE sc = (BYTE)MapVirtualKey(vk, 0);
            if (shift_state & 1) keybd_event(VK_SHIFT, MapVirtualKey(VK_SHIFT, 0), 0, 0);
            if (shift_state & 2) keybd_event(VK_CONTROL, MapVirtualKey(VK_CONTROL, 0), 0, 0);
            if (shift_state & 4) keybd_event(VK_MENU, MapVirtualKey(VK_MENU, 0), 0, 0);

            keybd_event(vk, sc, 0, 0);
            keybd_event(vk, sc, KEYEVENTF_KEYUP, 0);

            if (shift_state & 4) keybd_event(VK_MENU, MapVirtualKey(VK_MENU, 0), KEYEVENTF_KEYUP, 0);
            if (shift_state & 2) keybd_event(VK_CONTROL, MapVirtualKey(VK_CONTROL, 0), KEYEVENTF_KEYUP, 0);
            if (shift_state & 1) keybd_event(VK_SHIFT, MapVirtualKey(VK_SHIFT, 0), KEYEVENTF_KEYUP, 0);
        }
        text++;
    }
}

void handle_uikey(SOCKET sock, const char *args)
{
    char buf[512];
    BYTE modifiers[3];
    int mod_count = 0;
    int i;

    if (!args || !args[0]) {
        send_error_response(sock, "UIKEY requires a key spec");
        return;
    }

    log_msg(LOG_INPUT, "UIKEY: spec=\"%s\"", args);

    /* TEXT: mode - type each character */
    if (_strnicmp(args, "TEXT:", 5) == 0) {
        send_text_input(args + 5);
        send_text_response(sock, "OK");
        return;
    }

    /* Parse modifier+key combos (e.g., ALT+N, CTRL+SHIFT+A) */
    safe_strncpy(buf, args, sizeof(buf));
    {
        char *p = buf;
        char *last_part = buf;
        BYTE vk;

        /* Walk through +-separated parts; last part is the key, rest are modifiers */
        while (*p) {
            if (*p == '+') {
                *p = '\0';
                /* Check if this part is a modifier */
                if (_stricmp(last_part, "ALT") == 0) {
                    modifiers[mod_count++] = VK_MENU;
                } else if (_stricmp(last_part, "CTRL") == 0) {
                    modifiers[mod_count++] = VK_CONTROL;
                } else if (_stricmp(last_part, "SHIFT") == 0) {
                    modifiers[mod_count++] = VK_SHIFT;
                }
                last_part = p + 1;
            }
            p++;
        }

        /* last_part is the final key name */
        vk = lookup_named_key(last_part);
        if (vk == 0) {
            char err[128];
            _snprintf(err, sizeof(err), "Unknown key: %s", last_part);
            send_error_response(sock, err);
            return;
        }

        /* Press modifiers down */
        for (i = 0; i < mod_count; i++)
            keybd_event(modifiers[i], (BYTE)MapVirtualKey(modifiers[i], 0), 0, 0);

        /* Press and release the key */
        send_key_press(vk);

        /* Release modifiers in reverse order */
        for (i = mod_count - 1; i >= 0; i--)
            keybd_event(modifiers[i], (BYTE)MapVirtualKey(modifiers[i], 0), KEYEVENTF_KEYUP, 0);
    }

    send_text_response(sock, "OK");
}
