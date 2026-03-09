/*
 * autologon.c - Auto-dismiss Win98 network login dialog
 *
 * Runs via RunServices (before user logon). Watches for the MSNP32
 * "Enter Network Password" dialog, fills username/password from
 * registry (HKLM\Software\RetroAgent\AutoLogon), and clicks OK.
 *
 * This solves the Win98 limitation where Client for Microsoft Networks
 * always shows a login prompt at boot, which cannot be suppressed via
 * registry settings alone.
 *
 * Build: i686-w64-mingw32-gcc -Os -s -o autologon.exe autologon.c
 *        -luser32 -ladvapi32 -mwindows
 */

#include <windows.h>

#define AUTOLOGON_KEY "Software\\RetroAgent\\AutoLogon"
#define MAX_WAIT_MS   30000  /* give up after 30 seconds */
#define POLL_MS       200

/*
 * Read a string value from HKLM\Software\RetroAgent\AutoLogon.
 * Returns 0 on success.
 */
static int read_reg_str(const char *name, char *buf, DWORD bufsize)
{
    HKEY hKey;
    DWORD type, size;

    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, AUTOLOGON_KEY, 0,
                      KEY_READ, &hKey) != ERROR_SUCCESS)
        return -1;

    size = bufsize;
    if (RegQueryValueExA(hKey, name, NULL, &type,
                         (BYTE *)buf, &size) != ERROR_SUCCESS || type != REG_SZ) {
        RegCloseKey(hKey);
        return -1;
    }

    RegCloseKey(hKey);
    return 0;
}

/*
 * Check if a window is the MSNP32 "Enter Network Password" dialog.
 * It's a standard dialog (#32770) with specific child controls:
 * - Static text containing "password" (case-insensitive)
 * - At least one Edit control (username/password fields)
 */
static BOOL is_login_dialog(HWND hwnd)
{
    char cls[32], title[128];
    HWND child;
    int edit_count = 0;

    /* Must be a dialog box */
    GetClassNameA(hwnd, cls, sizeof(cls));
    if (lstrcmpA(cls, "#32770") != 0)
        return FALSE;

    /* Must be visible */
    if (!IsWindowVisible(hwnd))
        return FALSE;

    /* Check window title for "password" or "network" */
    GetWindowTextA(hwnd, title, sizeof(title));
    CharLowerA(title);
    if (strstr(title, "password") == NULL && strstr(title, "network") == NULL)
        return FALSE;

    /* Must have edit controls (username/password fields) */
    child = NULL;
    while ((child = FindWindowExA(hwnd, child, "Edit", NULL)) != NULL)
        edit_count++;

    return (edit_count >= 1);
}

/*
 * Find and fill the login dialog.
 * Sets username in first Edit, password in second Edit, clicks OK.
 */
static BOOL fill_and_submit(HWND hwnd, const char *username, const char *password)
{
    HWND edit1, edit2;

    /* First Edit = username */
    edit1 = FindWindowExA(hwnd, NULL, "Edit", NULL);
    if (!edit1) return FALSE;

    /* Second Edit = password */
    edit2 = FindWindowExA(hwnd, edit1, "Edit", NULL);

    /* Set username */
    SetWindowTextA(edit1, username);

    /* Set password (if there's a second edit field) */
    if (edit2)
        SetWindowTextA(edit2, password);

    /* Small delay to let the UI update */
    Sleep(100);

    /* Click OK (IDOK = 1) */
    PostMessageA(hwnd, WM_COMMAND, MAKEWPARAM(IDOK, BN_CLICKED), 0);

    return TRUE;
}

/*
 * Enumerate all top-level windows looking for the login dialog.
 */
typedef struct {
    HWND found;
} find_ctx_t;

static BOOL CALLBACK enum_windows_cb(HWND hwnd, LPARAM lParam)
{
    find_ctx_t *ctx = (find_ctx_t *)lParam;
    if (is_login_dialog(hwnd)) {
        ctx->found = hwnd;
        return FALSE;  /* stop enumerating */
    }
    return TRUE;
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR lpCmd, int nShow)
{
    char username[128] = "";
    char password[128] = "";
    DWORD elapsed = 0;
    find_ctx_t ctx;

    (void)hInst; (void)hPrev; (void)lpCmd; (void)nShow;

    /* Read credentials from registry */
    if (read_reg_str("username", username, sizeof(username)) != 0)
        return 1;  /* no config, exit silently */
    read_reg_str("password", password, sizeof(password));
    /* password can be empty — that's valid */

    /* Poll for the login dialog */
    while (elapsed < MAX_WAIT_MS) {
        ctx.found = NULL;
        EnumWindows(enum_windows_cb, (LPARAM)&ctx);

        if (ctx.found) {
            fill_and_submit(ctx.found, username, password);
            /* Wait a bit and check if another dialog appears (confirmation) */
            Sleep(1000);
            ctx.found = NULL;
            EnumWindows(enum_windows_cb, (LPARAM)&ctx);
            if (ctx.found) {
                /* Second dialog (e.g. "confirm password") — dismiss it too */
                fill_and_submit(ctx.found, username, password);
            }
            return 0;
        }

        Sleep(POLL_MS);
        elapsed += POLL_MS;
    }

    return 0;  /* timed out, exit silently */
}
