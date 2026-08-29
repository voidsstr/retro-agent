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

/*
 * ui_click_at - perform a mouse click at (x,y). Shared by UICLICK and CLICKSHOT.
 * right=1 -> right button; dbl=1 -> double click.
 */
void ui_click_at(int x, int y, int right, int dbl)
{
    DWORD down_flag = right ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_LEFTDOWN;
    DWORD up_flag   = right ? MOUSEEVENTF_RIGHTUP   : MOUSEEVENTF_LEFTUP;

    SetCursorPos(x, y);
    mouse_event(down_flag, 0, 0, 0, 0);
    mouse_event(up_flag, 0, 0, 0, 0);

    if (dbl) {
        mouse_event(down_flag, 0, 0, 0, 0);
        mouse_event(up_flag, 0, 0, 0, 0);
    }
}

void handle_uiclick(SOCKET sock, const char *args)
{
    int x, y;
    int right_click = 0;
    int double_click = 0;
    char buf[256];

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

    ui_click_at(x, y, right_click, double_click);

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
    int extended;   /* needs KEYEVENTF_EXTENDEDKEY - see send_key_press() */
} key_map_t;

/* The `extended` column is not cosmetic. The grey navigation cluster, the
 * arrows and PrintScreen all live on E0-prefixed scan codes; without
 * KEYEVENTF_EXTENDEDKEY a game reading the keyboard through DirectInput sees
 * the NUMPAD twin of the key instead (or nothing at all), which looks exactly
 * like UIKEY being ignored. */
static const key_map_t named_keys[] = {
    { "ENTER",       VK_RETURN,   0 },
    { "RETURN",      VK_RETURN,   0 },
    { "TAB",         VK_TAB,      0 },
    { "ESCAPE",      VK_ESCAPE,   0 },
    { "ESC",         VK_ESCAPE,   0 },
    { "SPACE",       VK_SPACE,    0 },
    { "BACKSPACE",   VK_BACK,     0 },
    { "DELETE",      VK_DELETE,   1 },
    { "DEL",         VK_DELETE,   1 },
    { "UP",          VK_UP,       1 },
    { "DOWN",        VK_DOWN,     1 },
    { "LEFT",        VK_LEFT,     1 },
    { "RIGHT",       VK_RIGHT,    1 },
    { "HOME",        VK_HOME,     1 },
    { "END",         VK_END,      1 },
    { "PAGEUP",      VK_PRIOR,    1 },
    { "PAGEDOWN",    VK_NEXT,     1 },
    { "INSERT",      VK_INSERT,   1 },
    { "F1",          VK_F1,       0 },
    { "F2",          VK_F2,       0 },
    { "F3",          VK_F3,       0 },
    { "F4",          VK_F4,       0 },
    { "F5",          VK_F5,       0 },
    { "F6",          VK_F6,       0 },
    { "F7",          VK_F7,       0 },
    { "F8",          VK_F8,       0 },
    { "F9",          VK_F9,       0 },
    { "F10",         VK_F10,      0 },
    { "F11",         VK_F11,      0 },
    { "F12",         VK_F12,      0 },

    /* PrintScreen: most games save their own screenshot on this key, which is
     * the only way to capture a D3D/OpenGL EXCLUSIVE FULLSCREEN frame - the
     * agent's own SCREENSHOT is a GDI BitBlt and returns pure black for those
     * surfaces. Its absence here is why fullscreen titles could not be
     * verified at all. */
    { "PRINTSCREEN", VK_SNAPSHOT, 1 },
    { "PRTSC",       VK_SNAPSHOT, 1 },
    { "PRTSCR",      VK_SNAPSHOT, 1 },
    { "SYSRQ",       VK_SNAPSHOT, 1 },

    /* The console key. Quake/GoldSrc/Unreal engines all open their console on
     * it, which is how a screenshot command gets typed at all. */
    { "TILDE",       VK_OEM_3,    0 },
    { "BACKQUOTE",   VK_OEM_3,    0 },
    { "GRAVE",       VK_OEM_3,    0 },
    { "CONSOLE",     VK_OEM_3,    0 },

    { "PAUSE",       VK_PAUSE,    0 },
    { "BREAK",       VK_PAUSE,    0 },
    { "CAPSLOCK",    VK_CAPITAL,  0 },
    { "NUMLOCK",     VK_NUMLOCK,  1 },
    { "SCROLLLOCK",  VK_SCROLL,   0 },
    { "WIN",         VK_LWIN,     1 },
    { "APPS",        VK_APPS,     1 },

    /* Bare modifiers, so a caller can press one on its own. The ALT+X combo
     * form is parsed separately in the UIKEY handler. */
    { "SHIFT",       VK_SHIFT,    0 },
    { "CTRL",        VK_CONTROL,  0 },
    { "CONTROL",     VK_CONTROL,  0 },
    { "ALT",         VK_MENU,     0 },

    { NULL, 0, 0 }
};

static const key_map_t *lookup_named_key_entry(const char *name)
{
    const key_map_t *k;

    for (k = named_keys; k->name; k++) {
        if (_stricmp(name, k->name) == 0)
            return k;
    }
    return NULL;
}

static BYTE lookup_named_key(const char *name)
{
    const key_map_t *k = lookup_named_key_entry(name);
    if (k) return k->vk;

    /* Single letter A-Z */
    if (name[0] && !name[1]) {
        char c = name[0];
        if (c >= 'a' && c <= 'z') return (BYTE)(c - 'a' + 0x41);
        if (c >= 'A' && c <= 'Z') return (BYTE)c;
        if (c >= '0' && c <= '9') return (BYTE)c;
    }

    return 0;
}

static int named_key_is_extended(const char *name)
{
    const key_map_t *k = lookup_named_key_entry(name);
    return k ? k->extended : 0;
}

static void send_key_press_ex(BYTE vk, int extended)
{
    BYTE sc = (BYTE)MapVirtualKey(vk, 0);  /* MAPVK_VK_TO_VSC */
    DWORD flags = extended ? KEYEVENTF_EXTENDEDKEY : 0;

    /* MapVirtualKey returns 0 for VK_SNAPSHOT on some XP keyboard layouts.
     * A zero scan code makes keybd_event take its documented special path and
     * copy the SCREEN to the clipboard instead of delivering a keystroke - so
     * the game never sees the key and never writes its screenshot. Fall back
     * to the real E0-37 PrintScreen scan code. */
    if (sc == 0 && vk == VK_SNAPSHOT)
        sc = 0x37;

    keybd_event(vk, sc, flags, 0);
    keybd_event(vk, sc, flags | KEYEVENTF_KEYUP, 0);
}

static void send_key_press(BYTE vk)
{
    send_key_press_ex(vk, 0);
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
        send_key_press_ex(vk, named_key_is_extended(last_part));

        /* Release modifiers in reverse order */
        for (i = mod_count - 1; i >= 0; i--)
            keybd_event(modifiers[i], (BYTE)MapVirtualKey(modifiers[i], 0), KEYEVENTF_KEYUP, 0);
    }

    send_text_response(sock, "OK");
}
